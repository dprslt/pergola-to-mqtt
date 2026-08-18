#include "durable_state.h"

#include <Arduino.h>
#include <Preferences.h>

namespace {

// NVS keys are capped at 15 characters, so these are already as long as they
// can safely get.
constexpr const char *NVS_NAMESPACE = "pergola";
constexpr const char *KEY_STOP_OWED = "stop_owed";
constexpr const char *KEY_POSITION = "position";

Preferences prefs;
bool ready = false;

// The RAM mirror. Reads never go to flash, and writes only do when the value
// differs from what is already there.
bool stopOwed = false;
uint8_t position = 0;

}  // namespace

void durableBegin() {
	// false = read/write. A first ever boot has no namespace yet; Preferences
	// creates it and the getters below fall back to the defaults passed in.
	ready = prefs.begin(NVS_NAMESPACE, false);
	if (!ready) {
		// Not fatal. The daemon still works, it just loses the two guarantees
		// above -- so say so loudly rather than degrading in silence.
		Serial.println(F("# nvs: could not open namespace, durable state is OFF"));
		Serial.println(F("#      an interrupted open will not self-clear on the next boot"));
		return;
	}
	stopOwed = prefs.getBool(KEY_STOP_OWED, false);
	position = prefs.getUChar(KEY_POSITION, 0);
	if (position > 100) {
		position = 100;
	}
	Serial.printf("# nvs: stop_owed=%s position=%u%%\n", stopOwed ? "YES" : "no",
	              position);
}

bool durableStopOwed() {
	return stopOwed;
}

void durableSetStopOwed(bool owed) {
	if (owed == stopOwed) {
		return;
	}
	stopOwed = owed;
	if (ready) {
		prefs.putBool(KEY_STOP_OWED, owed);
	}
}

uint8_t durablePosition() {
	return position;
}

void durableSetPosition(uint8_t pos) {
	if (pos > 100 || pos == position) {
		return;
	}
	position = pos;
	if (ready) {
		prefs.putUChar(KEY_POSITION, pos);
	}
}
