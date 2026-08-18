#include "durable_state.h"

#include <Arduino.h>
#include <Preferences.h>

namespace {

// NVS keys are capped at 15 characters, so these are already as long as they
// can safely get.
constexpr const char *NVS_NAMESPACE = "pergola";
constexpr const char *KEY_STOP_OWED = "stop_owed";
// Left behind by a reverted experiment in storing the position estimate. Cleared
// at boot so an old value cannot be picked up by a future change that reintroduces
// the key without knowing why it went away.
constexpr const char *KEY_POSITION_OBSOLETE = "position";

Preferences prefs;
bool ready = false;

// The RAM mirror. Reads never go to flash, and writes only do when the value
// differs from what is already there.
bool stopOwed = false;

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
	if (prefs.isKey(KEY_POSITION_OBSOLETE)) {
		prefs.remove(KEY_POSITION_OBSOLETE);
		Serial.println(F("# nvs: dropped a stored position from an older build"));
	}
	Serial.printf("# nvs: stop_owed=%s\n", stopOwed ? "YES" : "no");
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
