// Build a canonical EV1527 word as a list of alternating pulse durations.
//
// This exists because REPLAYING A CAPTURED FRAME DOES NOT WORK, for two
// independent reasons documented in docs/remote-protocol.md:
//
//   1. The sniffer segments frames *on* the sync low, so a capture holds
//      [24 data bits][sync high] and the 31*alpha sync low -- the separator --
//      is gone. Replay puts the sync in the wrong place.
//   2. Captured pulses carry the CC1101's +/-1/8-bit sampling jitter and
//      glitch-merged edges. Widths of 675 and 1393 us have been observed, which
//      are neither alpha nor 3*alpha, and a decoder that width-checks every
//      pulse discards the whole word.
//
// Forging gives exact timings and the right structure. It is also the only
// option for a daemon, which has no capture to replay.
#pragma once

#include <stdint.h>

// Layout, alpha = the short pulse:
//   sync:  alpha high, 31*alpha low
//   bit 1: 3*alpha high, alpha low
//   bit 0: alpha high, 3*alpha low
//
// durations[0] is always a carrier-ON period. That polarity is not an
// assumption: a forged non-inverted frame moved the roof on 2026-08-18.
//
// Returns the number of pulses written, or 0 if the arguments are out of range
// or the buffer is too small. A word needs 2 + 2*bitCount entries.
uint16_t ev1527BuildWord(uint32_t code, uint8_t bitCount, uint32_t alphaUs,
                         uint32_t *durations, uint16_t capacity);

// Pulses a word of bitCount bits will occupy.
static inline uint16_t ev1527PulseCount(uint8_t bitCount) {
	return static_cast<uint16_t>(2 + 2 * bitCount);
}

// --- Decoding ---------------------------------------------------------------

// Decode a captured frame into a code word.
//
// `durations` alternate carrier-on / carrier-off starting with `firstLevel`.
// Each bit is one pulse pair: (3*alpha on, alpha off) = 1, (alpha on, 3*alpha
// off) = 0. A leading sync pulse pair is skipped if present.
//
// Deliberately tolerant, because real captures are messy: pulse widths are
// classified against a threshold rather than matched exactly, since the CC1101's
// 8x oversampling of the async data adds +/-1/8 of a bit period of jitter to
// every edge.
//
// Returns true and sets *codeOut if exactly `bitCount` bits decoded cleanly.
// Returns false on a short, corrupt or ambiguous frame -- which is the common
// case for noise, and is how the daemon tells a real press from the noise floor.
bool ev1527Decode(const uint32_t *durations, uint16_t count, uint8_t firstLevel,
                  uint8_t bitCount, uint32_t alphaUs, uint32_t *codeOut);
