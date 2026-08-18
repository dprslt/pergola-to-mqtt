// Everything specific to THIS pergola's remote, in one place.
//
// Measured and verified on hardware 2026-08-18 -- all three codes were confirmed
// by forging the word and watching the roof respond. Full derivation, the
// waveform spec and the reason replay does not work: docs/remote-protocol.md.
#pragma once

#include <stdint.h>

// --- Radio ------------------------------------------------------------------

// The remote is a 315 MHz OOK transmitter, NOT 433.92. Its SAW resonator is
// marked R315 and a sweep of 433.0-434.8 MHz found nothing at all.
static constexpr float PERGOLA_FREQ_MHZ = 315.0f;

// --- Encoding ---------------------------------------------------------------

// 24 bits, one bit per pulse pair, 1:3 short:long. The encoder is silkscreened
// 2262, but the half-bits do not pair into PT2262 tri-state symbols, so this is
// decoded as 24 independent bits (the EV1527 scheme).
static constexpr uint8_t PERGOLA_CODE_BITS = 24;

// The short pulse. Measured 344-362 us across ~300 frames; 351 is the mean and
// is what the working transmissions used.
static constexpr uint32_t PERGOLA_ALPHA_US = 351;

// --- Codes ------------------------------------------------------------------

// 20-bit address shared by every button, then a 4-bit field with one bit per
// button.
static constexpr uint32_t PERGOLA_ADDRESS = 0xF3A75;

static constexpr uint32_t PERGOLA_CODE_OPEN = 0xF3A758;   // nibble 1000
static constexpr uint32_t PERGOLA_CODE_STOP = 0xF3A754;   // nibble 0100
static constexpr uint32_t PERGOLA_CODE_CLOSE = 0xF3A752;  // nibble 0010

// A fourth code, 0xF3A751 (nibble 0001), is predicted but has never been
// transmitted or verified. It is documented in docs/remote-protocol.md rather than
// defined here, deliberately: an unused constant sitting next to three working ones
// is an invitation to try it, and an unknown command to a motor controller is not a
// free experiment.

// --- Transmit shape ---------------------------------------------------------

// The remote sends its word about 12 times per press. Matching that is both
// what the receiver expects and a reasonable duty cycle.
static constexpr uint8_t PERGOLA_TX_REPEATS = 12;

// 1 ms, because the 31*alpha sync low is already inside each forged word. Do not
// raise this to "be safe": a longer gap makes the receiver measure a sync low far
// longer than 31*alpha and it rejects the word. That is exactly why replaying a
// captured frame with the old 20 ms default never worked.
static constexpr uint16_t PERGOLA_TX_GAP_MS = 1;

// --- Travel and behaviour ----------------------------------------------------

// Measured with a stopwatch on 2026-08-18. See docs/behaviour.md.
//
// There is NO position feedback anywhere in this system. The remote's encoder is
// transmit-only, and the pergola also has a PHYSICAL WIRED BUTTON that moves the
// roof without emitting any RF at all -- so even sniffing 315 MHz would not catch
// every change. Position is dead-reckoned from these two numbers and is always an
// estimate that any wired press silently invalidates.
static constexpr uint32_t PERGOLA_TRAVEL_OPEN_MS = 6300;
static constexpr uint32_t PERGOLA_TRAVEL_CLOSE_MS = 6000;

// !!! A FULL OPEN MUST BE FOLLOWED BY A STOP !!!
//
// If the roof reaches fully open and no stop is sent, it cannot be closed again.
// Not a tidiness preference, a lockout: every open that runs to completion must be
// terminated with a stop or the pergola is stuck open.
//
// CoverState_t::startMove() schedules that stop unconditionally, at the moment the
// move starts rather than when travel ends, so a busy loop or a dropped connection
// cannot skip it. There is deliberately no flag to turn it off.

// How long after travel should complete before the stop goes out. Enough that the
// roof has certainly reached the end stop; a stop sent to an already-stopped motor
// is harmless.
static constexpr uint32_t PERGOLA_AUTOSTOP_MARGIN_MS = 500;

// The light bar is wired into the motion commands: starting a CLOSE turns it on, a
// STOP turns it off. So its state is inferred from what was last sent, never
// measured. With the roof already closed a CLOSE lights it without moving anything,
// which is enough for real on/off control -- see docs/behaviour.md.
