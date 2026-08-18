#include "ev1527.h"

uint16_t ev1527BuildWord(uint32_t code, uint8_t bitCount, uint32_t alphaUs,
                         uint32_t *durations, uint16_t capacity) {
	if (durations == nullptr || bitCount == 0 || bitCount > 32) {
		return 0;
	}
	// Below ~50 us the CC1101's 8x oversampling of the async input cannot
	// reproduce the edge; above 20 ms nothing sane is being described.
	if (alphaUs < 50 || alphaUs > 20000) {
		return 0;
	}
	const uint16_t needed = ev1527PulseCount(bitCount);
	if (capacity < needed) {
		return 0;
	}

	uint16_t n = 0;
	durations[n++] = alphaUs;       // sync pulse, carrier on
	durations[n++] = 31 * alphaUs;  // sync gap, carrier off

	// Most significant bit first.
	for (int8_t b = static_cast<int8_t>(bitCount) - 1; b >= 0; b--) {
		const bool one = ((code >> b) & 1u) != 0u;
		durations[n++] = one ? 3 * alphaUs : alphaUs;  // carrier on
		durations[n++] = one ? alphaUs : 3 * alphaUs;  // carrier off
	}
	return n;
}

bool ev1527Decode(const uint32_t *durations, uint16_t count, uint8_t firstLevel,
                  uint8_t bitCount, uint32_t alphaUs, uint32_t *codeOut) {
	if (durations == nullptr || codeOut == nullptr || bitCount == 0 || bitCount > 32) {
		return false;
	}
	// Halfway between alpha and 3*alpha. Anything below is "short", above is
	// "long". A frame whose pulses do not fall cleanly either side of this is
	// rejected further down.
	const uint32_t split = alphaUs * 2;
	// Anything at or beyond the sync low is a frame boundary, not a data pulse.
	const uint32_t syncLow = alphaUs * 16;

	uint16_t i = 0;
	// A capture segmented on the sync low starts on data. One taken mid-stream
	// may start with the sync pulse pair; skip it.
	if (count >= 2 && durations[1] >= syncLow) {
		i = 2;
	}
	// durations[i] must be a carrier-ON period for the pairing below to mean
	// anything. firstLevel tells us the parity of index 0.
	const bool onAtEven = (firstLevel != 0);
	if (((i % 2) == 0) != onAtEven) {
		i++;
	}

	uint32_t code = 0;
	uint8_t bits = 0;
	for (; i + 1 < count && bits < bitCount; i += 2) {
		const uint32_t on = durations[i];
		const uint32_t off = durations[i + 1];
		if (on >= syncLow || off >= syncLow) {
			break;  // hit the next word
		}
		const bool onLong = on >= split;
		const bool offLong = off >= split;
		if (onLong == offLong) {
			return false;  // both short or both long: not a valid bit
		}
		code = (code << 1) | (onLong ? 1u : 0u);
		bits++;
	}
	if (bits != bitCount) {
		return false;
	}
	*codeOut = code;
	return true;
}
