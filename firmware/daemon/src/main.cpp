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
#include <PubSubClient.h>
#include <WiFi.h>

#include "cc1101.h"
#include "cover_state.h"
#include "daemon_config.h"
#include "ev1527.h"
#include "pergola_codes.h"
#include "pins.h"

static constexpr const char *FIRMWARE_VERSION = "0.1.0";

static CC1101 radio;
static CoverState_t cover;
static WiFiClient net;
static PubSubClient mqtt(net);

static uint32_t lastPublishMs = 0;
static uint32_t lastMqttAttemptMs = 0;
static uint32_t lastWifiAttemptMs = 0;
static bool radioReady = false;

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
static void transmitCode(uint32_t code) {
	if (!radioReady) {
		Serial.println(F("# tx: radio not ready, dropping command"));
		return;
	}
	uint32_t durations[ev1527PulseCount(32)];
	const uint16_t n = ev1527BuildWord(code, PERGOLA_CODE_BITS, PERGOLA_ALPHA_US,
	                                   durations, sizeof(durations) / sizeof(durations[0]));
	if (n == 0) {
		Serial.println(F("# tx: could not build word"));
		return;
	}
	if (!radio.beginTransmitRaw()) {
		Serial.println(F("# tx: chip would not enter TX"));
		return;
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

	// Diagnostics: without feedback from the pergola, "what did we last actually
	// send, and when" is the only ground truth available for debugging.
	if (mqtt.connected()) {
		const char *name = "unknown";
		if (code == PERGOLA_CODE_OPEN) {
			name = "open";
		} else if (code == PERGOLA_CODE_STOP) {
			name = "stop";
		} else if (code == PERGOLA_CODE_CLOSE) {
			name = "close";
		}
		char json[128];
		snprintf(json, sizeof(json),
		         "{\"command\":\"%s\",\"code\":\"0x%06lX\",\"repeats\":%u,"
		         "\"uptime_ms\":%lu}",
		         name, static_cast<unsigned long>(code), PERGOLA_TX_REPEATS,
		         static_cast<unsigned long>(millis()));
		mqtt.publish(TOPIC_LAST_COMMAND, json, true);
	}
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

// The device block is repeated in each payload so all three entities land under
// one Home Assistant device.
#define HA_DEVICE_JSON                                                      \
	"\"device\":{\"identifiers\":[\"" PERGOLA_ID "\"],\"name\":\"Pergola\"," \
	"\"manufacturer\":\"Green Outside\",\"model\":\"Actual 3x4\","          \
	"\"sw_version\":\"" "0.1.0" "\"}"

static void publishDiscovery() {
	static char payload[MQTT_BUFFER_BYTES];

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
	         HA_DEVICE_JSON "}");
	mqtt.publish(HA_ROOF_CONFIG, payload, true);

	// A real light, not a diagnostic readout: with the roof closed, a `close`
	// lights the bar without moving anything and a `stop` clears it.
	snprintf(payload, sizeof(payload),
	         "{\"name\":\"Light bar\",\"unique_id\":\"" PERGOLA_ID "_light\","
	         "\"command_topic\":\"" TOPIC_LIGHT_SET "\","
	         "\"state_topic\":\"" TOPIC_LIGHT_STATE "\","
	         "\"availability_topic\":\"" TOPIC_AVAILABILITY "\","
	         "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
	         HA_DEVICE_JSON "}");
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
	         HA_DEVICE_JSON "}");
	mqtt.publish(HA_LAST_COMMAND_CONFIG, payload, true);

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
	mqtt.subscribe(TOPIC_ROOF_SET);
	mqtt.subscribe(TOPIC_ROOF_POSITION_SET);
	mqtt.subscribe(TOPIC_LIGHT_SET);
	publishDiscovery();
	publishState(true);
	return true;
}

// ---------------------------------------------------------------------------

void setup() {
	Serial.begin(115200);
	delay(200);
	Serial.printf("\n# pergola-to-mqtt daemon %s\n", FIRMWARE_VERSION);

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

	radioReady = radio.begin();
	if (!radioReady) {
		Serial.println(F("# radio: VERSION did not read back -- check wiring/power"));
	} else {
		radio.setFrequencyMHz(PERGOLA_FREQ_MHZ);
		Serial.printf("# radio: %.3f MHz, VERSION=0x%02X\n", PERGOLA_FREQ_MHZ,
		              radio.version());
	}

	// Assume closed on boot. It is a guess, and positionTrusted() reports false
	// until a full open or close proves it.
	cover.begin(0);

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
	mqtt.setServer(MQTT_HOST, MQTT_PORT);
	mqtt.setCallback(onMessage);
}

void loop() {
	const uint32_t now = millis();

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

	// Sending is blocking (12 words, ~45 ms each) so only one per pass.
	uint32_t code = 0;
	if (cover.nextTx(now, &code)) {
		transmitCode(code);
		publishState(true);
	}

	publishState(false);
}
