// Dead-reckoned cover position, plus the two behavioural rules this pergola
// imposes. See docs/behaviour.md.
//
// Deliberately knows nothing about radios or MQTT: it consumes commands and time,
// and produces codes to transmit. That keeps the two rules that actually matter
// -- the mandatory stop after a full open, and the light coupling -- in one place
// that can be reasoned about without a network stack in the way.
#pragma once

#include <stdint.h>

#include "pergola_codes.h"

enum class CoverState : uint8_t {
	Closed,
	Open,
	Opening,
	Closing,
	Stopped,  // part-way, not moving
};

// Codes queued for transmission, each with an earliest-send time. A direction
// reversal enqueues a stop and then the new command, so two pending entries plus
// the scheduled auto-stop is the realistic worst case.
static constexpr uint8_t COVER_TX_QUEUE_LEN = 4;

class CoverState_t {
public:
	void begin(uint8_t assumedPosition = 0);

	// Advance the model. Call often; it is cheap and drives the auto-stop.
	void tick(uint32_t nowMs);

	void commandOpen(uint32_t nowMs);
	void commandClose(uint32_t nowMs);
	void commandStop(uint32_t nowMs);
	// target 0 = closed, 100 = fully open.
	void commandSetPosition(uint32_t nowMs, uint8_t target);

	// The light bar has no code of its own. A `close` turns it on and a `stop`
	// turns it off, so with the roof already at its closed end stop a `close`
	// lights it without moving anything -- that is the whole trick.
	//
	// Unconditional on purpose. An earlier version refused unless state_ said the
	// roof was closed, which was theatre: state_ is a dead-reckoned BELIEF, and the
	// pergola's wired wall button moves the roof without emitting any RF, so the
	// belief can be stale in either direction. Such a guard would refuse when the
	// roof really is closed and permit when it is not -- failing exactly when it
	// mattered. Nothing here may gate behaviour on a position we cannot verify.
	//
	// The honest consequence: if the roof is not actually closed, this closes it.
	// That is the same thing pressing `close` on the remote would do.
	void commandLightOn(uint32_t nowMs);
	// A stop against an already-stopped motor does nothing but clear the light.
	void commandLightOff(uint32_t nowMs);

	// Pop the next code whose send time has arrived. Returns false if there is
	// nothing to send yet.
	bool nextTx(uint32_t nowMs, uint32_t *codeOut);

	uint8_t position() const { return position_; }
	CoverState state() const { return state_; }
	const char *stateName() const;
	// Inferred, never measured: a close turns the light on, a stop turns it off.
	bool lightOn() const { return lightOn_; }

	// True while a scheduled stop is still outstanding. Exposed so the caller
	// can persist that obligation somewhere that survives a reset -- this class
	// deliberately owns no storage, and autoStopAtMs_ is RAM only. Not a
	// position, so nothing here invites gating behaviour on a believed position.
	bool movePending() const { return autoStopAtMs_ != 0; }

private:
	struct Pending {
		uint32_t code;
		uint32_t notBeforeMs;
		bool used;
	};

	void enqueue(uint32_t code, uint32_t notBeforeMs);
	void startMove(uint32_t nowMs, bool opening, uint8_t target);
	void settle(uint8_t position);

	uint8_t position_ = 0;
	uint8_t target_ = 0;
	CoverState state_ = CoverState::Closed;
	bool lightOn_ = false;

	uint32_t moveStartMs_ = 0;
	uint8_t moveStartPos_ = 0;
	// When the mandatory stop must go out. 0 = nothing scheduled.
	uint32_t autoStopAtMs_ = 0;

	Pending queue_[COVER_TX_QUEUE_LEN] = {};
};
