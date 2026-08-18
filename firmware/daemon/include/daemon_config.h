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

// Home Assistant MQTT discovery.
#define HA_DISCOVERY_PREFIX "homeassistant"
#define HA_ROOF_CONFIG HA_DISCOVERY_PREFIX "/cover/" PERGOLA_ID "_roof/config"
#define HA_LIGHT_CONFIG HA_DISCOVERY_PREFIX "/light/" PERGOLA_ID "_light/config"
#define HA_LAST_COMMAND_CONFIG \
	HA_DISCOVERY_PREFIX "/sensor/" PERGOLA_ID "_last_command/config"
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
