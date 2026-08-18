#include "pulse_sniffer.h"

#include <Arduino.h>

#ifndef ARDUINO_ISR_ATTR
#define ARDUINO_ISR_ATTR
#endif

namespace {

// Power of two so the wrap is a mask. Sized to absorb a whole button press while
// the main loop is busy printing the previous frame over a 115200 baud link.
constexpr uint16_t RING_SIZE = 2048;
constexpr uint16_t RING_MASK = RING_SIZE - 1;

volatile uint32_t ringDur[RING_SIZE];
volatile uint8_t ringLvl[RING_SIZE];
volatile uint16_t ringHead = 0;  // producer: the ISR
volatile uint16_t ringTail = 0;  // consumer: poll()
volatile uint32_t ringDropped = 0;
volatile uint32_t lastEdgeUs = 0;

uint8_t isrPin = 0;

// Single producer, single consumer, 32-bit aligned slots: no locking needed on
// Xtensa, where aligned word access is atomic.
//
// micros() and digitalRead() both live in flash rather than IRAM. That is fine
// here because this firmware never writes flash, so the cache is never disabled
// underneath the handler.
void ARDUINO_ISR_ATTR edgeIsr() {
	uint32_t now = micros();
	uint32_t dt = now - lastEdgeUs;
	lastEdgeUs = now;

	uint16_t next = static_cast<uint16_t>((ringHead + 1) & RING_MASK);
	if (next == ringTail) {
		ringDropped++;
		return;
	}

	// The pulse that just ended held the level opposite to the one we read now.
	ringDur[ringHead] = dt;
	ringLvl[ringHead] = digitalRead(isrPin) ? 0 : 1;
	ringHead = next;
}

}  // namespace

void PulseSniffer::begin(uint8_t pin) {
	pin_ = pin;
	isrPin = pin;
	pinMode(pin_, INPUT);
	resetWork();
}

void PulseSniffer::enable() {
	if (enabled_) {
		return;
	}
	// Start from a clean slate: anything queued before now predates this session.
	ringHead = 0;
	ringTail = 0;
	lastEdgeUs = micros();
	resetWork();

	attachInterrupt(digitalPinToInterrupt(pin_), edgeIsr, CHANGE);
	enabled_ = true;
}

void PulseSniffer::disable() {
	if (!enabled_) {
		return;
	}
	detachInterrupt(digitalPinToInterrupt(pin_));
	enabled_ = false;
	resetWork();
}

uint32_t PulseSniffer::droppedEdges() const { return ringDropped; }

void PulseSniffer::resetCounters() {
	ringDropped = 0;
	rejected_ = 0;
	emitted_ = 0;
}

void PulseSniffer::resetWork() {
	work_->count = 0;
	work_->truncated = false;
	work_->firstLevel = 1;
}

const PulseFrame *PulseSniffer::poll() {
	while (ringTail != ringHead) {
		uint32_t dur = ringDur[ringTail];
		uint8_t lvl = ringLvl[ringTail];
		ringTail = static_cast<uint16_t>((ringTail + 1) & RING_MASK);

		if (dur >= gapUs) {
			// Silence. It terminates whatever we were collecting and is not itself
			// part of any frame.
			if (closeFrame()) {
				return out_;
			}
			continue;
		}

		if (work_->count >= SNIFFER_MAX_PULSES) {
			// Out of room mid-burst. Emit what we have, flagged, and let the next
			// pulses start a fresh frame. This one pulse is lost at the seam --
			// `truncated` is how the host tools know not to trust the boundary.
			work_->truncated = true;
			if (closeFrame()) {
				return out_;
			}
			continue;
		}

		if (work_->count == 0) {
			work_->firstLevel = lvl;
			work_->startMillis = millis();
		}
		work_->durations[work_->count++] = dur;
	}

	// The trailing silence after the last burst produces no edge, so no ring entry
	// will ever report it. Close on elapsed time instead.
	if (work_->count > 0 && (micros() - lastEdgeUs) >= gapUs) {
		if (closeFrame()) {
			return out_;
		}
	}

	return nullptr;
}

bool PulseSniffer::closeFrame() {
	if (work_->count == 0) {
		resetWork();
		return false;
	}

	deglitch(work_);

	if (work_->count < minPulses) {
		rejected_++;
		resetWork();
		return false;
	}

	work_->seq = ++seq_;
	emitted_++;

	// Swap rather than copy: these buffers are 4 KB each.
	PulseFrame *finished = work_;
	work_ = out_;
	out_ = finished;
	resetWork();
	return true;
}

void PulseSniffer::deglitch(PulseFrame *f) {
	if (glitchUs == 0 || f->count == 0) {
		return;
	}

	uint16_t i = 0;

	// A leading spike has no predecessor to fold into, so it can only be dropped
	// -- which inverts the level of everything that follows.
	while (i < f->count && f->durations[i] < glitchUs) {
		i++;
		f->firstLevel = f->firstLevel ? 0 : 1;
	}

	uint16_t out = 0;
	while (i < f->count) {
		uint32_t d = f->durations[i];

		if (d < glitchUs) {
			if (out > 0 && i + 1 < f->count) {
				// Spike. The pulse before it, the spike, and the pulse after it are
				// really one pulse at the earlier level.
				f->durations[out - 1] += d + f->durations[i + 1];
				i += 2;
				continue;
			}
			// Trailing spike with nothing to merge into: drop it.
			i++;
			continue;
		}

		f->durations[out++] = d;
		i++;
	}

	f->count = out;
}
