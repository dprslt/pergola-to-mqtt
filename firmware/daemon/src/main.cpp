// pergola-to-mqtt -- MQTT daemon.
//
// Exposes the pergola to Home Assistant as a cover. Transmit only: it never
// listens. See docs/behaviour.md for why that is the honest choice -- the pergola
// has a physical wired button that moves the roof without emitting any RF, so a
// receiver could catch remote presses but would still miss wall presses, and a
// position estimate that looks authoritative while being quietly wrong is worse
// than one that admits it is an estimate.
//
// Two rules from docs/behaviour.md are enforced here and must not be relaxed:
//   1. Every open that runs to completion is terminated by a stop, or the roof
//      locks open and cannot be closed.
//   2. The light bar is wired to the motion commands, so its state is inferred.
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <HTTPUpdateServer.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "cc1101.h"
#include "cover_state.h"
#include "daemon_config.h"
#include "durable_state.h"
#include "ev1527.h"
#include "pergola_codes.h"
#include "pins.h"

static constexpr const char *FIRMWARE_VERSION = "0.1.0";

static CC1101 radio;
static CoverState_t cover;
static WiFiClient net;
static PubSubClient mqtt(net);
static WebServer http(HTTP_PORT);
static HTTPUpdateServer httpUpdater;

static uint32_t lastPublishMs = 0;
static uint32_t lastMqttAttemptMs = 0;
static uint32_t lastWifiAttemptMs = 0;
static bool radioReady = false;
static bool otaEnabled = false;

// What the boot-time owed-stop check found, reported to the broker once it is
// reachable. Failed is the one that matters: a stop was owed, could not be sent, and
// the roof may be latched open with nothing else about to notice.
enum class Recovery : uint8_t { None, StopSent, Failed };
static Recovery recoveryOutcome = Recovery::None;

static const char *recoveryName() {
	switch (recoveryOutcome) {
		case Recovery::StopSent: return "stop-sent";
		case Recovery::Failed: return "FAILED";
		case Recovery::None: break;
	}
	return "none";
}

// A transmit can happen with no broker to tell: the boot-time recovery stop always
// does, and a WiFi drop can too. Buffering it means the record of the most
// consequential transmit this daemon makes is not the one that gets thrown away.
static char pendingLastCommand[224] = {0};
// Distinct from !otaEnabled: the bring-up needs an association, so it is deferred to
// loop() and this stops it being retried once it has settled either way.
static bool otaResolved = false;

// ---------------------------------------------------------------------------
// WiFi diagnostics
// ---------------------------------------------------------------------------

// "Failed to associate" covers causes with completely different fixes: a wrong
// passphrase, an AP that is not visible, and an auth mode the ESP32 cannot do all
// look identical from WiFi.status(). The reason code separates them.
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
	if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
		return;
	}
	const uint8_t reason = info.wifi_sta_disconnected.reason;
	const char *hint = "";
	switch (reason) {
		case WIFI_REASON_NO_AP_FOUND:
			hint = " (SSID not seen: wrong name, out of range, or 5 GHz-only)";
			break;
		case WIFI_REASON_AUTH_FAIL:
		case WIFI_REASON_HANDSHAKE_TIMEOUT:
		case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
			hint = " (authentication rejected: check the passphrase)";
			break;
		case WIFI_REASON_ASSOC_FAIL:
			hint = " (association refused: MAC filter, or the AP is full)";
			break;
		default:
			break;
	}
	Serial.printf("# wifi: disconnected, reason=%u%s\n", reason, hint);
}

// Report what is actually on the air, so a wrong SSID cannot be mistaken for a
// wrong password. Blocking, and only ever called after a failure.
static void scanForSsid(const char *wanted) {
	Serial.println(F("# wifi: scanning..."));
	const int n = WiFi.scanNetworks();
	if (n <= 0) {
		// NOT an antenna fault. A scan issued while WiFi.begin() is still retrying
		// routinely returns zero, because the connection attempts pre-empt it. This
		// message used to blame the antenna and sent me chasing hardware while the
		// real problem was a one-character typo in the SSID. Trust the pre-radio
		// scan in setup() for the "can this board hear anything at all" question.
		Serial.println(F("# wifi: scan returned nothing -- inconclusive while a"
		                 " connect is retrying; see the pre-radio scan above"));
		return;
	}
	bool found = false;
	for (int i = 0; i < n; i++) {
		const bool match = WiFi.SSID(i) == wanted;
		if (match) {
			found = true;
		}
		// Print the target plus anything strong, to keep the log short but useful.
		if (match || WiFi.RSSI(i) > -70) {
			Serial.printf("#   %s'%s'  %d dBm  ch%d  auth=%d\n",
			              match ? "-> " : "   ", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
			              WiFi.channel(i), static_cast<int>(WiFi.encryptionType(i)));
		}
	}
	if (!found) {
		Serial.printf("# wifi: '%s' is NOT among the %d visible networks --"
		              " the SSID is wrong or the AP is out of range\n", wanted, n);
	} else {
		Serial.printf("# wifi: '%s' IS visible, so the SSID is right --"
		              " suspect the passphrase or the auth mode\n", wanted);
		Serial.println(F("#       auth=4 is WPA2_PSK. The original ESP32 cannot do"
		                 " WPA3-only (auth=6/7); a mixed WPA2/WPA3 AP is fine."));
	}
	WiFi.scanDelete();
}

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------

// Forge and send one code, imitating the remote's own repeat count and gap.
// Returns false if nothing reached the air, which is what lets the caller keep an
// owed stop owed rather than assuming it was delivered.
static bool transmitCode(uint32_t code, bool isRecovery = false) {
	if (!radioReady) {
		Serial.println(F("# tx: radio not ready, dropping command"));
		return false;
	}
	uint32_t durations[ev1527PulseCount(32)];
	const uint16_t n = ev1527BuildWord(code, PERGOLA_CODE_BITS, PERGOLA_ALPHA_US,
	                                   durations, sizeof(durations) / sizeof(durations[0]));
	if (n == 0) {
		Serial.println(F("# tx: could not build word"));
		return false;
	}
	if (!radio.beginTransmitRaw()) {
		Serial.println(F("# tx: chip would not enter TX"));
		return false;
	}
	for (uint8_t r = 0; r < PERGOLA_TX_REPEATS; r++) {
		uint8_t level = 1;  // durations[0] is a carrier-ON period
		for (uint16_t i = 0; i < n; i++) {
			digitalWrite(PIN_GDO0, level ? HIGH : LOW);
			delayMicroseconds(durations[i]);
			level = level ? 0 : 1;
		}
		digitalWrite(PIN_GDO0, LOW);
		if (r + 1 < PERGOLA_TX_REPEATS) {
			delay(PERGOLA_TX_GAP_MS);
		}
	}
	radio.endTransmitRaw();
	Serial.printf("# tx: 0x%06lX x%u\n", static_cast<unsigned long>(code),
	              PERGOLA_TX_REPEATS);

	// The obligation is discharged here and nowhere else: not when the stop is
	// scheduled, not when it is queued, but once it has actually been on the air.
	// The movePending() check keeps it owed through a reversal, where the stop
	// that just went out is followed by a fresh move carrying a fresh obligation.
	if (code == PERGOLA_CODE_STOP && !cover.movePending()) {
		durableSetStopOwed(false);
	}

	// Diagnostics: without feedback from the pergola, "what did we last actually
	// send, and when" is the only ground truth available for debugging.
	const char *name = "unknown";
	if (code == PERGOLA_CODE_OPEN) {
		name = "open";
	} else if (code == PERGOLA_CODE_STOP) {
		name = "stop";
	} else if (code == PERGOLA_CODE_CLOSE) {
		name = "close";
	}
	char json[sizeof(pendingLastCommand)];
	snprintf(json, sizeof(json),
	         "{\"command\":\"%s\",\"code\":\"0x%06lX\",\"repeats\":%u,"
	         "\"uptime_ms\":%lu,\"recovery\":%s}",
	         name, static_cast<unsigned long>(code), PERGOLA_TX_REPEATS,
	         static_cast<unsigned long>(millis()), isRecovery ? "true" : "false");
	if (mqtt.connected()) {
		mqtt.publish(TOPIC_LAST_COMMAND, json, true);
	} else {
		// Flushed by mqttConnect(). Overwriting a previous pending entry is correct:
		// this topic has only ever reported the *last* command.
		snprintf(pendingLastCommand, sizeof(pendingLastCommand), "%s", json);
	}
	return true;
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------

static void publishState(bool force) {
	if (!mqtt.connected()) {
		return;
	}
	const bool moving = cover.state() == CoverState::Opening ||
	                    cover.state() == CoverState::Closing;
	const uint32_t interval = moving ? PUBLISH_MOVING_MS : PUBLISH_IDLE_MS;
	if (!force && millis() - lastPublishMs < interval) {
		return;
	}
	lastPublishMs = millis();

	char buf[8];
	mqtt.publish(TOPIC_ROOF_STATE, cover.stateName(), true);
	snprintf(buf, sizeof(buf), "%u", cover.position());
	mqtt.publish(TOPIC_ROOF_POSITION, buf, true);
	mqtt.publish(TOPIC_LIGHT_STATE, cover.lightOn() ? "ON" : "OFF", true);
}

// The device block is repeated in each payload so every entity lands under one Home
// Assistant device. Built at runtime rather than as a string literal because two of
// its fields are only known once there is an association, and because the previous
// literal carried its own copy of the version number that could drift from
// FIRMWARE_VERSION.
static char deviceJson[288];

static void buildDeviceJson() {
	// configuration_url is what makes "Visit device" on the Home Assistant device
	// page open the firmware upload form. Only emitted when that page actually
	// exists: a dead link on the device page is worse than no link, and the web
	// server is not started at all without an OTA password.
	char configUrl[64] = {0};
	if (otaEnabled) {
		snprintf(configUrl, sizeof(configUrl), ",\"configuration_url\":\"http://%s/\"",
		         WiFi.localIP().toString().c_str());
	}
	snprintf(deviceJson, sizeof(deviceJson),
	         "\"device\":{\"identifiers\":[\"" PERGOLA_ID "\"],\"name\":\"Pergola\","
	         "\"manufacturer\":\"Green Outside\",\"model\":\"Actual 3x4\","
	         "\"sw_version\":\"%s\"%s}",
	         FIRMWARE_VERSION, configUrl);
}

static void publishDiscovery() {
	static char payload[MQTT_BUFFER_BYTES];

	// Rebuilt on every connect, so a new DHCP lease refreshes the device page link
	// without needing a reflash.
	buildDeviceJson();

	snprintf(payload, sizeof(payload),
	         "{\"name\":\"Roof\",\"unique_id\":\"" PERGOLA_ID "_roof\","
	         "\"device_class\":\"awning\","
	         "\"command_topic\":\"" TOPIC_ROOF_SET "\","
	         "\"state_topic\":\"" TOPIC_ROOF_STATE "\","
	         "\"position_topic\":\"" TOPIC_ROOF_POSITION "\","
	         "\"set_position_topic\":\"" TOPIC_ROOF_POSITION_SET "\","
	         "\"availability_topic\":\"" TOPIC_AVAILABILITY "\","
	         "\"payload_open\":\"OPEN\",\"payload_close\":\"CLOSE\","
	         "\"payload_stop\":\"STOP\","
	         "\"state_open\":\"open\",\"state_closed\":\"closed\","
	         "\"state_opening\":\"opening\",\"state_closing\":\"closing\","
	         "\"state_stopped\":\"stopped\","
	         "\"position_open\":100,\"position_closed\":0,"
	         "%s}", deviceJson);
	mqtt.publish(HA_ROOF_CONFIG, payload, true);

	// A real light, not a diagnostic readout: with the roof closed, a `close`
	// lights the bar without moving anything and a `stop` clears it.
	snprintf(payload, sizeof(payload),
	         "{\"name\":\"Light bar\",\"unique_id\":\"" PERGOLA_ID "_light\","
	         "\"command_topic\":\"" TOPIC_LIGHT_SET "\","
	         "\"state_topic\":\"" TOPIC_LIGHT_STATE "\","
	         "\"availability_topic\":\"" TOPIC_AVAILABILITY "\","
	         "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
	         "%s}", deviceJson);
	mqtt.publish(HA_LIGHT_CONFIG, payload, true);

	// Retire both short-lived position-confidence entities. An empty retained
	// payload on a discovery topic removes the entity in Home Assistant.
	mqtt.publish(HA_SYNCED_CONFIG_OBSOLETE, "", true);
	mqtt.publish(HA_TRUSTED_CONFIG_OBSOLETE, "", true);
	mqtt.publish(TOPIC_SYNCED_OBSOLETE, "", true);
	mqtt.publish(TOPIC_TRUSTED_OBSOLETE, "", true);

	snprintf(payload, sizeof(payload),
	         "{\"name\":\"Last command\","
	         "\"unique_id\":\"" PERGOLA_ID "_last_command\","
	         "\"state_topic\":\"" TOPIC_LAST_COMMAND "\","
	         "\"value_template\":\"{{ value_json.command }}\","
	         "\"json_attributes_topic\":\"" TOPIC_LAST_COMMAND "\","
	         "\"availability_topic\":\"" TOPIC_AVAILABILITY "\","
	         "\"entity_category\":\"diagnostic\","
	         "%s}", deviceJson);
	mqtt.publish(HA_LAST_COMMAND_CONFIG, payload, true);

	// The address, as an entity rather than only as a link. Makes the board
	// findable from a template or an automation, and tells you where it went after
	// a DHCP change without opening a serial monitor -- which would reset it.
	snprintf(payload, sizeof(payload),
	         "{\"name\":\"IP address\",\"unique_id\":\"" PERGOLA_ID "_ip\","
	         "\"state_topic\":\"" TOPIC_IP "\","
	         "\"availability_topic\":\"" TOPIC_AVAILABILITY "\","
	         "\"entity_category\":\"diagnostic\","
	         "\"icon\":\"mdi:ip-network\","
	         "%s}", deviceJson);
	mqtt.publish(HA_IP_CONFIG, payload, true);

	// Diagnostic, and the only place an unattended recovery leaves a trace: the stop
	// itself goes out before there is a broker to tell.
	snprintf(payload, sizeof(payload),
	         "{\"name\":\"Last boot recovery\","
	         "\"unique_id\":\"" PERGOLA_ID "_recovery\","
	         "\"state_topic\":\"" TOPIC_RECOVERY "\","
	         "\"availability_topic\":\"" TOPIC_AVAILABILITY "\","
	         "\"entity_category\":\"diagnostic\","
	         "\"icon\":\"mdi:lifebuoy\","
	         "%s}", deviceJson);
	mqtt.publish(HA_RECOVERY_CONFIG, payload, true);

	Serial.println(F("# mqtt: discovery published"));
}

static void onMessage(char *topic, uint8_t *payload, unsigned int len) {
	char body[16] = {0};
	const unsigned int n = len < sizeof(body) - 1 ? len : sizeof(body) - 1;
	memcpy(body, payload, n);
	const uint32_t now = millis();

	if (!strcmp(topic, TOPIC_ROOF_SET)) {
		if (!strcmp(body, "OPEN")) {
			cover.commandOpen(now);
		} else if (!strcmp(body, "CLOSE")) {
			cover.commandClose(now);
		} else if (!strcmp(body, "STOP")) {
			cover.commandStop(now);
		} else {
			Serial.printf("# mqtt: unknown command '%s'\n", body);
			return;
		}
		Serial.printf("# mqtt: %s\n", body);
	} else if (!strcmp(topic, TOPIC_LIGHT_SET)) {
		if (!strcmp(body, "ON")) {
			cover.commandLightOn(now);
			Serial.println(F("# mqtt: light ON (sends close)"));
		} else if (!strcmp(body, "OFF")) {
			cover.commandLightOff(now);
			Serial.println(F("# mqtt: light OFF"));
		} else {
			Serial.printf("# mqtt: unknown light command '%s'\n", body);
			return;
		}
#ifdef PERGOLA_WDT_SELFTEST
	} else if (!strcmp(topic, TOPIC_SELFTEST)) {
		// Present only in the esp32dev-selftest build. There is no way to reach this
		// from a normal image: the macro is undefined, so the branch and the topic
		// subscription both vanish at compile time.
		//
		// Wedging loop() on purpose is the only way to prove the task watchdog is
		// really armed rather than merely configured. Nothing else in the daemon
		// blocks long enough -- the longest stretch is a ~540 ms transmit.
		if (!strcmp(body, "HANG")) {
			Serial.println(F("# selftest: hanging loop() with nothing owed -- the task"
			                 " watchdog must reboot this board"));
			Serial.flush();
			for (;;) {
			}
		}
		if (!strcmp(body, "OWE-AND-HANG")) {
			// Books the obligation directly instead of sending an open, so the pair
			// "watchdog reboots a wedged loop" and "the reboot discharges the owed
			// stop" can be tested together without moving the roof. The recovery stop
			// on the next boot is real RF, and against a stopped motor it does
			// nothing but clear the light.
			durableSetStopOwed(true);
			Serial.println(F("# selftest: hanging loop() with a stop owed -- the reboot"
			                 " must transmit it"));
			Serial.flush();
			for (;;) {
			}
		}
		Serial.printf("# selftest: unknown payload '%s'\n", body);
		return;
#endif
	} else if (!strcmp(topic, TOPIC_ROOF_POSITION_SET)) {
		const long target = strtol(body, nullptr, 10);
		if (target < 0 || target > 100) {
			Serial.printf("# mqtt: position '%s' out of range\n", body);
			return;
		}
		cover.commandSetPosition(now, static_cast<uint8_t>(target));
		Serial.printf("# mqtt: set_position %ld\n", target);
	} else {
		return;
	}
	publishState(true);
}

static bool mqttConnect() {
	char clientId[32];
	snprintf(clientId, sizeof(clientId), PERGOLA_ID "-%06llX",
	         ESP.getEfuseMac() & 0xFFFFFFULL);

	const char *user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
	const char *pass = strlen(MQTT_PASSWORD) ? MQTT_PASSWORD : nullptr;

	// Last will so Home Assistant marks the device unavailable if this drops off
	// rather than showing a stale position for ever.
	if (!mqtt.connect(clientId, user, pass, TOPIC_AVAILABILITY, 1, true, "offline")) {
		Serial.printf("# mqtt: connect failed, state=%d\n", mqtt.state());
		return false;
	}
	Serial.printf("# mqtt: connected as %s\n", clientId);
	mqtt.publish(TOPIC_AVAILABILITY, "online", true);
	mqtt.publish(TOPIC_IP, WiFi.localIP().toString().c_str(), true);
	mqtt.publish(TOPIC_RECOVERY, recoveryName(), true);
	if (pendingLastCommand[0]) {
		mqtt.publish(TOPIC_LAST_COMMAND, pendingLastCommand, true);
		pendingLastCommand[0] = '\0';
		Serial.println(F("# mqtt: flushed the transmit made while offline"));
	}
	mqtt.subscribe(TOPIC_ROOF_SET);
	mqtt.subscribe(TOPIC_ROOF_POSITION_SET);
	mqtt.subscribe(TOPIC_LIGHT_SET);
#ifdef PERGOLA_WDT_SELFTEST
	mqtt.subscribe(TOPIC_SELFTEST);
#endif
	publishDiscovery();
	publishState(true);
	return true;
}

// ---------------------------------------------------------------------------
// Over-the-air updates
// ---------------------------------------------------------------------------

// The page behind configuration_url. Deliberately plain: it exists so that a
// reboot, a DHCP change or a stale position can be diagnosed without opening a
// serial monitor, because doing that resets the board.
static void handleRoot() {
	char body[768];
	const uint32_t up = millis() / 1000;
	snprintf(body, sizeof(body),
	         "<!doctype html><meta name=viewport content=\"width=device-width\">"
	         "<title>Pergola</title>"
	         "<style>body{font:16px/1.5 system-ui,sans-serif;margin:2rem auto;"
	         "max-width:30rem;padding:0 1rem}dt{font-weight:600;margin-top:.5rem}"
	         "dd{margin:0}a{display:inline-block;margin-top:1.5rem}</style>"
	         "<h1>Pergola</h1><dl>"
	         "<dt>Firmware<dd>%s"
	         "<dt>Address<dd>%s"
	         "<dt>Uptime<dd>%lu h %lu m"
	         "<dt>Roof<dd>%s, position %u%% <em>(dead reckoned, not measured)</em>"
	         "<dt>Light bar<dd>%s"
	         "<dt>Radio<dd>%s"
	         "<dt>Broker<dd>%s"
	         "<dt>Stop owed<dd>%s"
	         "</dl><a href=\"" HTTP_UPDATE_PATH "\">Update firmware</a>",
	         FIRMWARE_VERSION, WiFi.localIP().toString().c_str(),
	         static_cast<unsigned long>(up / 3600),
	         static_cast<unsigned long>((up % 3600) / 60), cover.stateName(),
	         cover.position(), cover.lightOn() ? "on" : "off",
	         radioReady ? "ready" : "NOT READY", mqtt.connected() ? "connected" : "offline",
	         durableStopOwed() ? "YES -- a stop is still outstanding" : "no");
	http.send(200, "text/html", body);
}

static void setupOta() {
	otaResolved = true;
	// No password, no OTA, and no web server either. An unauthenticated flash
	// endpoint on a device that moves a heavy roof with pinch points is not a
	// convenience worth having, and failing closed beats printing a warning nobody
	// reads. Set PERGOLA_OTA_PASSWORD in .env to turn both on.
	if (!strlen(OTA_PASSWORD)) {
		Serial.println(F("# ota: DISABLED -- no password set"));
		Serial.println(F("#      set PERGOLA_OTA_PASSWORD in firmware/daemon/.env"
		                 " to enable it"));
		return;
	}
	ArduinoOTA.setHostname(PERGOLA_ID);
	ArduinoOTA.setPassword(OTA_PASSWORD);
	ArduinoOTA.onStart([]() { Serial.println(F("# ota: start")); });
	ArduinoOTA.onProgress([](unsigned int, unsigned int) {
		// Erasing and writing flash is slow enough to trip the watchdog on its own,
		// and a watchdog reboot part way through writing the image is the one reset
		// the NVS flag cannot help with.
		esp_task_wdt_reset();
	});
	ArduinoOTA.onEnd([]() { Serial.println(F("# ota: written, rebooting")); });
	ArduinoOTA.onError([](ota_error_t err) {
		Serial.printf("# ota: error %u\n", static_cast<unsigned>(err));
	});
	ArduinoOTA.begin();

	// Same password, two routes in: espota for `pio run -t upload`, and a browser
	// form for the link on the Home Assistant device page.
	httpUpdater.setup(&http, HTTP_UPDATE_PATH, HTTP_UPDATE_USER, OTA_PASSWORD);
	http.on("/", handleRoot);
	http.begin();

	otaEnabled = true;
	Serial.printf("# ota: espota on '%s.local', web on http://%s/\n", PERGOLA_ID,
	              WiFi.localIP().toString().c_str());
}

// ---------------------------------------------------------------------------

void setup() {
	Serial.begin(115200);
	delay(200);
	Serial.printf("\n# pergola-to-mqtt daemon %s\n", FIRMWARE_VERSION);
#ifdef PERGOLA_WDT_SELFTEST
	Serial.println(F("# ***** SELFTEST BUILD -- can be told to hang on purpose."));
	Serial.println(F("# ***** Do not leave this on the pergola. Reflash with"
	                 " -e esp32dev-ota."));
#endif
	Serial.printf("# boot: reset reason %d\n", static_cast<int>(esp_reset_reason()));

	durableBegin();

	// Recovery, before anything slow or power-hungry runs. If a stop is owed the
	// roof may be sitting latched open right now, and the supply that caused the
	// reset is most likely to fail again during the WiFi bring-up below -- so get
	// the stop out first and let the rest of setup() follow.
	//
	// This is the one path that touches the CC1101 before the pre-radio scan,
	// which costs that scan its "SPI and GPIO4 untouched" guarantee. A latched
	// roof outranks a diagnostic. The normal boot path is unchanged, because the
	// flag is clear unless a move was actually interrupted.
	if (durableStopOwed()) {
		Serial.println(F("# recovery: a stop was owed when the last reset happened"));
		radioReady = radio.begin();
		if (!radioReady) {
			recoveryOutcome = Recovery::Failed;
			Serial.println(F("# recovery: radio did not init -- the stop is STILL owed"
			                 " and will be retried on the next boot"));
		} else {
			radio.setFrequencyMHz(PERGOLA_FREQ_MHZ);
			if (transmitCode(PERGOLA_CODE_STOP, true)) {
				recoveryOutcome = Recovery::StopSent;
				Serial.println(F("# recovery: stop sent, roof is free to close again"));
			} else {
				recoveryOutcome = Recovery::Failed;
				Serial.println(F("# recovery: stop FAILED to reach the air -- still"
				                 " owed, retried on the next boot"));
			}
		}
	}

	// A scan BEFORE the CC1101 is touched. If this sees nothing either, the 2.4 GHz
	// side is broken independently of anything this project does to the SPI bus or
	// GPIO4 -- which points at the module's own antenna or its supply, not at us.
	Serial.printf("# chip: %s rev%d, %d MHz, %u KB free heap\n", ESP.getChipModel(),
	              ESP.getChipRevision(), ESP.getCpuFreqMHz(),
	              static_cast<unsigned>(ESP.getFreeHeap() / 1024));
	WiFi.mode(WIFI_STA);
	// A MAC address proves the WiFi peripheral initialised at all.
	Serial.printf("# wifi: mac %s\n", WiFi.macAddress().c_str());
	const int preScan = WiFi.scanNetworks();
	Serial.printf("# wifi: pre-radio scan sees %d network(s)\n", preScan);
	for (int i = 0; i < preScan && i < 8; i++) {
		Serial.printf("#   '%s'  %d dBm  ch%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
		              WiFi.channel(i));
	}
	WiFi.scanDelete();

	// Already up if the recovery path above ran, and begin() is not worth
	// repeating just for symmetry.
	if (!radioReady) {
		radioReady = radio.begin();
		if (radioReady) {
			radio.setFrequencyMHz(PERGOLA_FREQ_MHZ);
		}
	}
	if (!radioReady) {
		Serial.println(F("# radio: VERSION did not read back -- check wiring/power"));
	} else {
		Serial.printf("# radio: %.3f MHz, VERSION=0x%02X\n", PERGOLA_FREQ_MHZ,
		              radio.version());
	}

	// Restore the last settled estimate instead of asserting "closed". Both are
	// guesses -- a wired wall press while we were off is undetectable either way --
	// but the stored one was true at some point, and 0 is true only by luck. No
	// confidence value is published about it; see daemon_config.h.
	cover.begin(durablePosition());

	WiFi.mode(WIFI_STA);
	WiFi.setSleep(false);
	WiFi.onEvent(onWiFiEvent);
	WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
	Serial.printf("# wifi: connecting to '%s'\n", WIFI_SSID);
	// Bounded, not an unbounded spin. An earlier version looped here for ever, so a
	// wrong SSID or a placeholder credential produced a board that printed dots and
	// nothing else -- indistinguishable from an antenna or power fault. Give up and
	// let loop() keep retrying, so `status` output and the radio stay alive.
	const uint32_t wifiStart = millis();
	while (WiFi.status() != WL_CONNECTED &&
	       millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS) {
		delay(WIFI_RETRY_MS);
	}
	if (WiFi.status() == WL_CONNECTED) {
		Serial.printf("# wifi: %s\n", WiFi.localIP().toString().c_str());
	} else {
		// Name the SSID being attempted: the overwhelmingly likely cause is a wrong
		// or unfilled credential, not radio trouble.
		Serial.printf("# wifi: FAILED to associate with '%s' after %lu ms\n",
		              WIFI_SSID, static_cast<unsigned long>(WIFI_CONNECT_TIMEOUT_MS));
		Serial.println(F("# wifi: check PERGOLA_WIFI_SSID / PERGOLA_WIFI_PASSWORD"
		                 " in firmware/daemon/.env, then rebuild"));
		scanForSsid(WIFI_SSID);
		Serial.println(F("# wifi: retrying in the background"));
	}

	mqtt.setBufferSize(MQTT_BUFFER_BYTES);
	mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
	mqtt.setServer(MQTT_HOST, MQTT_PORT);
	mqtt.setCallback(onMessage);

	// setupOta() is NOT called here. It needs an association for the web server to
	// bind and for the printed URL to be real, and the wait above is bounded and may
	// have timed out. loop() calls it once WiFi is actually up.

	// The watchdog goes on last, so nothing in the bounded bring-up above -- the
	// two blocking scans, the 15 s association wait -- can trip it before loop()
	// starts feeding it.
#if ESP_IDF_VERSION_MAJOR >= 5
	const esp_task_wdt_config_t wdtConfig = {
	    .timeout_ms = WDT_TIMEOUT_MS,
	    .idle_core_mask = 0,
	    .trigger_panic = true,
	};
	// Arduino core 3 initialises the timer itself, so reconfigure it; on a build
	// that did not, initialise it here.
	if (esp_task_wdt_reconfigure(&wdtConfig) != ESP_OK) {
		esp_task_wdt_init(&wdtConfig);
	}
#else
	esp_task_wdt_init(WDT_TIMEOUT_MS / 1000, true);
#endif
	esp_task_wdt_add(nullptr);
	Serial.printf("# wdt: armed, %lu ms\n",
	              static_cast<unsigned long>(WDT_TIMEOUT_MS));
}

void loop() {
	const uint32_t now = millis();

	esp_task_wdt_reset();

	// Before the MQTT block below, not after it. mqttConnect() publishes discovery,
	// and the device block it builds carries configuration_url only when the web
	// server is already up -- so bringing OTA up second meant Home Assistant got a
	// device page with no link to the update form until the next reconnect.
	if (!otaResolved && WiFi.status() == WL_CONNECTED) {
		setupOta();
	}

	if (WiFi.status() != WL_CONNECTED) {
		// The radio keeps working without a network, and a pending mandatory stop
		// still has to go out, so do not block here. Rate-limited, because calling
		// reconnect() every pass restarts the attempt before it can succeed.
		if (now - lastWifiAttemptMs > WIFI_RECONNECT_MS) {
			lastWifiAttemptMs = now;
			Serial.println(F("# wifi: reconnecting"));
			WiFi.reconnect();
		}
	} else if (!mqtt.connected() && now - lastMqttAttemptMs > MQTT_RECONNECT_MS) {
		lastMqttAttemptMs = now;
		mqttConnect();
	}
	mqtt.loop();

	cover.tick(now);

	// Record the obligation BEFORE the open that creates it can go out. A command
	// that arrived via mqtt.loop() above is already reflected in movePending(), and
	// nextTx() below is what actually puts it on the air -- so a reset between the
	// two leaves the flag set, which is the safe direction to fail in. Reversals
	// briefly clear and re-set it; that is two flash writes on a rare event, and
	// the alternative is a window with the flag wrongly clear.
	if (cover.movePending()) {
		durableSetStopOwed(true);
	}

	// Sending is blocking (12 words, ~45 ms each) so only one per pass.
	uint32_t code = 0;
	if (cover.nextTx(now, &code)) {
		transmitCode(code);
		publishState(true);
	}

	// Persist only once the roof has settled. Writing every interpolated step
	// would be hundreds of flash writes per open, for an estimate superseded a few
	// hundred milliseconds later.
	const CoverState settledState = cover.state();
	if (settledState != CoverState::Opening && settledState != CoverState::Closing) {
		durableSetPosition(cover.position());
	}

	// Neither is serviced while a stop is owed. An update ends in a reboot, and the
	// window where that reboot would matter is exactly the window this flag marks.
	// The NVS flag would recover it, but declining for a few seconds is free and
	// keeps the recovery path for faults rather than for something we chose to do.
	if (otaEnabled && !durableStopOwed()) {
		ArduinoOTA.handle();
		http.handleClient();
	}

	publishState(false);
}
