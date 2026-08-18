// MQTT identity and topics.
#pragma once

// One place to rename everything if a second pergola ever appears.
#define PERGOLA_ID "pergola"

#define TOPIC_AVAILABILITY PERGOLA_ID "/availability"
// Topic names come from docs/home-assistant.md, which is the spec. "roof", not
// "cover": the doc was written first and there is no reason to diverge from it.
#define TOPIC_ROOF_SET PERGOLA_ID "/roof/set"
#define TOPIC_ROOF_POSITION_SET PERGOLA_ID "/roof/position/set"
#define TOPIC_ROOF_STATE PERGOLA_ID "/roof/state"
#define TOPIC_ROOF_POSITION PERGOLA_ID "/roof/position"
#define TOPIC_LIGHT_SET PERGOLA_ID "/light/set"
#define TOPIC_LIGHT_STATE PERGOLA_ID "/light/state"
#define TOPIC_LAST_COMMAND PERGOLA_ID "/last_command"
#define TOPIC_IP PERGOLA_ID "/ip"
// What the boot-time owed-stop check did. Retained, set once per boot on the first
// broker connect. Not a live "is a stop owed" readout: that flips true for a few
// seconds on every ordinary move and would be pure noise. This reports the one case
// worth seeing, which is a reset that left an obligation behind.
#define TOPIC_RECOVERY PERGOLA_ID "/recovery"
// Only subscribed by the esp32dev-selftest build, which can be told to wedge
// loop() so the task watchdog can be proved rather than assumed. Its own topic
// rather than a payload on roof/set, so a stray publish cannot be confused with a
// movement command.
#define TOPIC_SELFTEST PERGOLA_ID "/selftest/set"

// Home Assistant MQTT discovery.
#define HA_DISCOVERY_PREFIX "homeassistant"
#define HA_ROOF_CONFIG HA_DISCOVERY_PREFIX "/cover/" PERGOLA_ID "_roof/config"
#define HA_LIGHT_CONFIG HA_DISCOVERY_PREFIX "/light/" PERGOLA_ID "_light/config"
#define HA_LAST_COMMAND_CONFIG \
	HA_DISCOVERY_PREFIX "/sensor/" PERGOLA_ID "_last_command/config"
#define HA_IP_CONFIG HA_DISCOVERY_PREFIX "/sensor/" PERGOLA_ID "_ip/config"
#define HA_RECOVERY_CONFIG \
	HA_DISCOVERY_PREFIX "/sensor/" PERGOLA_ID "_recovery/config"
// No position-confidence entity is published. Both of these names existed briefly
// and are cleared at boot so Home Assistant drops the orphaned entities: an
// indicator whose "position is fine" state can be silently false -- a wired wall
// press is undetectable -- invites exactly the trust it cannot earn.
#define HA_SYNCED_CONFIG_OBSOLETE \
	HA_DISCOVERY_PREFIX "/binary_sensor/" PERGOLA_ID "_position_synced/config"
#define HA_TRUSTED_CONFIG_OBSOLETE \
	HA_DISCOVERY_PREFIX "/binary_sensor/" PERGOLA_ID "_position_trusted/config"
#define TOPIC_SYNCED_OBSOLETE PERGOLA_ID "/position_synced"
#define TOPIC_TRUSTED_OBSOLETE PERGOLA_ID "/position_trusted"

static constexpr uint32_t MQTT_RECONNECT_MS = 5000;
static constexpr uint32_t WIFI_RETRY_MS = 500;
// setup() waits this long for an association, then gives up and enters loop()
// anyway. It must NOT block for ever: a board wedged in setup() reports nothing,
// serves nothing, and looks identical to a hardware fault.
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
// How often loop() retries a dropped association. Calling reconnect() every pass
// is counterproductive -- it restarts the attempt before it can finish.
static constexpr uint32_t WIFI_RECONNECT_MS = 10000;
// While moving, position changes fast enough to be worth streaming.
static constexpr uint32_t PUBLISH_MOVING_MS = 500;
static constexpr uint32_t PUBLISH_IDLE_MS = 30000;

// PubSubClient's default 256-byte buffer cannot hold a discovery payload.
static constexpr uint16_t MQTT_BUFFER_BYTES = 1024;

// PubSubClient defaults to 15 s, which on its own is half the watchdog budget
// below: a broker that accepts the TCP connection and then says nothing would
// park loop() long enough to look like a hang. Five seconds is generous on a LAN.
static constexpr uint16_t MQTT_SOCKET_TIMEOUT_S = 5;

// The task watchdog has to outlast the longest blocking stretch in loop(), which
// is a transmit: 12 words at ~45 ms is ~540 ms, plus a possible MQTT connect at
// MQTT_SOCKET_TIMEOUT_S. 30 s clears both with room to spare and still recovers
// a genuinely wedged loop inside half a minute.
//
// It is safe to let this reboot mid-move only because the owed stop is in NVS --
// see durable_state.h. Before that it would have traded a hang for a latched
// roof.
static constexpr uint32_t WDT_TIMEOUT_MS = 30000;

// The status and firmware-upload page. Port 80 so the device page's "Visit device"
// link needs no port suffix. Started only when an OTA password is set, and the
// upload form behind it is HTTP-basic authenticated with that same password.
static constexpr uint16_t HTTP_PORT = 80;
#define HTTP_UPDATE_PATH "/update"
#define HTTP_UPDATE_USER PERGOLA_ID
