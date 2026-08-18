#include "cover_state.h"

namespace {

// Interpolate travel. Position is 0-100; the two travel times differ, so the
// direction matters.
uint32_t travelMsFor(uint8_t fromPos, uint8_t toPos) {
	const int32_t delta = static_cast<int32_t>(toPos) - static_cast<int32_t>(fromPos);
	const uint32_t span = static_cast<uint32_t>(delta < 0 ? -delta : delta);
	const uint32_t full = (delta > 0) ? PERGOLA_TRAVEL_OPEN_MS : PERGOLA_TRAVEL_CLOSE_MS;
	return (full * span) / 100u;
}

}  // namespace

void CoverState_t::begin(uint8_t assumedPosition) {
	position_ = assumedPosition > 100 ? 100 : assumedPosition;
	target_ = position_;
	state_ = (position_ == 0) ? CoverState::Closed
	                          : (position_ >= 100 ? CoverState::Open : CoverState::Stopped);
	lightOn_ = false;
	// On boot we have no idea where the roof is; whatever was passed in is a guess,
	// and a wall press while we were off is undetectable. Nothing is published about
	// how much to believe it: an indicator whose "good" state can be silently false
	// is worse than no indicator, because it invites trust it cannot earn.
	autoStopAtMs_ = 0;
	for (uint8_t i = 0; i < COVER_TX_QUEUE_LEN; i++) {
		queue_[i].used = false;
	}
}

void CoverState_t::enqueue(uint32_t code, uint32_t notBeforeMs) {
	for (uint8_t i = 0; i < COVER_TX_QUEUE_LEN; i++) {
		if (!queue_[i].used) {
			queue_[i] = {code, notBeforeMs, true};
			return;
		}
	}
	// Full queue means commands are arriving faster than they can be sent. Drop
	// the new one rather than overwrite a pending stop, which might be the
	// mandatory one.
}

bool CoverState_t::nextTx(uint32_t nowMs, uint32_t *codeOut) {
	for (uint8_t i = 0; i < COVER_TX_QUEUE_LEN; i++) {
		if (queue_[i].used &&
		    static_cast<int32_t>(nowMs - queue_[i].notBeforeMs) >= 0) {
			*codeOut = queue_[i].code;
			queue_[i].used = false;
			// The light follows the motion commands; see docs/behaviour.md.
			if (*codeOut == PERGOLA_CODE_CLOSE) {
				lightOn_ = true;
			} else if (*codeOut == PERGOLA_CODE_STOP) {
				lightOn_ = false;
			}
			return true;
		}
	}
	return false;
}

void CoverState_t::settle(uint8_t position) {
	position_ = position;
	target_ = position;
	state_ = (position == 0) ? CoverState::Closed
	                         : (position >= 100 ? CoverState::Open : CoverState::Stopped);
}

void CoverState_t::startMove(uint32_t nowMs, bool opening, uint8_t target) {
	// Reversing direction mid-travel: stop first, then start the new move a beat
	// later so the controller sees two distinct commands.
	if (state_ == CoverState::Opening || state_ == CoverState::Closing) {
		enqueue(PERGOLA_CODE_STOP, nowMs);
		nowMs += 300;
	}
	moveStartMs_ = nowMs;
	moveStartPos_ = position_;
	target_ = target;
	state_ = opening ? CoverState::Opening : CoverState::Closing;
	enqueue(opening ? PERGOLA_CODE_OPEN : PERGOLA_CODE_CLOSE, nowMs);

	// The stop is scheduled up front, not when travel finishes, so that a crash
	// or a busy loop cannot leave a full open un-terminated. See
	// docs/behaviour.md: an un-stopped full open locks the roof open.
	//
	// For a move to an end stop the duration is the FULL travel time, not the
	// interpolation from the believed position. The belief can be wrong in either
	// direction -- a wall press, a remote press, a stale estimate -- and deriving
	// the delay from it is the same trap as gating on it, just quieter. Believing
	// we were already at 100 made travelMsFor(100, 100) return 0, so an open was
	// followed by its stop 500 ms later; the roof twitched and settled, and it
	// presented as "the pergola does not move at all".
	//
	// Erring long is safe: a stop against a motor already at its end stop does
	// nothing but clear the light. Erring short halts the roof part way and looks
	// exactly like a dropped command.
	uint32_t duration;
	if (target == 0 || target >= 100) {
		duration = opening ? PERGOLA_TRAVEL_OPEN_MS : PERGOLA_TRAVEL_CLOSE_MS;
	} else {
		duration = travelMsFor(moveStartPos_, target);
	}
	moveDurationMs_ = duration;
	autoStopAtMs_ = nowMs + duration + PERGOLA_AUTOSTOP_MARGIN_MS;
	if (autoStopAtMs_ == 0) {
		autoStopAtMs_ = 1;  // 0 is the "nothing scheduled" sentinel
	}
}

void CoverState_t::tick(uint32_t nowMs) {
	if (state_ == CoverState::Opening || state_ == CoverState::Closing) {
		const uint32_t elapsed = nowMs - moveStartMs_;
		// The same span startMove() scheduled the stop against, not a fresh
		// interpolation: the two must not disagree, or the position settles before
		// the stop it belongs to has gone out.
		const uint32_t duration = moveDurationMs_;
		if (duration == 0) {
			settle(target_);
		} else {
			const uint32_t capped = elapsed > duration ? duration : elapsed;
			const int32_t delta = static_cast<int32_t>(target_) -
			                      static_cast<int32_t>(moveStartPos_);
			position_ = static_cast<uint8_t>(
			    static_cast<int32_t>(moveStartPos_) +
			    (delta * static_cast<int32_t>(capped)) / static_cast<int32_t>(duration));
		}
	}

	if (autoStopAtMs_ != 0 && static_cast<int32_t>(nowMs - autoStopAtMs_) >= 0) {
		autoStopAtMs_ = 0;
		enqueue(PERGOLA_CODE_STOP, nowMs);
		settle(target_);
	}
}

// No "already there, nothing to do" short-circuit in either of these.
//
// They used to early-return when the believed position was already at the target,
// which is the same unverifiable-belief trap as the old light guard: the belief is
// dead-reckoned, a wired wall press can change the roof without us knowing, and a
// reboot resets it to "closed" outright. The observed consequence was a command
// being silently DROPPED -- boot thinking the roof is closed while it is really
// open, then ignoring every close command because position_ == 0.
//
// A redundant command costs one ~540 ms burst and moves nothing. A dropped command
// leaves the user pressing a button that does nothing. Always transmit.
void CoverState_t::commandOpen(uint32_t nowMs) {
	startMove(nowMs, true, 100);
}

void CoverState_t::commandClose(uint32_t nowMs) {
	startMove(nowMs, false, 0);
}

void CoverState_t::commandStop(uint32_t nowMs) {
	autoStopAtMs_ = 0;
	enqueue(PERGOLA_CODE_STOP, nowMs);
	// Freeze wherever tick() last put us; that interpolated position is the best
	// estimate available.
	target_ = position_;
	state_ = (position_ == 0) ? CoverState::Closed
	                         : (position_ >= 100 ? CoverState::Open : CoverState::Stopped);
}

void CoverState_t::commandSetPosition(uint32_t nowMs, uint8_t target) {
	if (target > 100) {
		target = 100;
	}
	if (target == position_) {
		// Believed to be there already. Unlike open/close there is no safe direction
		// to pick and no travel to run, so this one genuinely cannot act -- but say so
		// rather than failing silently, and re-anchor with a full open or close.
		return;
	}
	startMove(nowMs, target > position_, target);
}

void CoverState_t::commandLightOn(uint32_t nowMs) {
	// Deliberately NOT startMove(): no travel to model, and above all no auto-stop
	// scheduled -- the auto-stop is what would immediately switch the light back
	// off. nextTx() sets lightOn_ when the CLOSE actually goes out.
	//
	// If the roof happens to be open, this closes it. There is no auto-stop, so it
	// closes fully and the light stays on. The mandatory-stop rule applies to a full
	// OPEN, not a close, so that is safe -- see docs/behaviour.md.
	enqueue(PERGOLA_CODE_CLOSE, nowMs);
}

void CoverState_t::commandLightOff(uint32_t nowMs) {
	// Do not cancel a scheduled auto-stop here: if a move is in flight, its
	// mandatory stop must still happen. This stop is additive and harmless.
	enqueue(PERGOLA_CODE_STOP, nowMs);
}

const char *CoverState_t::stateName() const {
	switch (state_) {
		case CoverState::Closed: return "closed";
		case CoverState::Open: return "open";
		case CoverState::Opening: return "opening";
		case CoverState::Closing: return "closing";
		case CoverState::Stopped: return "stopped";
	}
	return "unknown";
}
