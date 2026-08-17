// Edge-timing front end: turns the CC1101's raw OOK baseband on GDO0 into frames
// of pulse durations.
//
// A GPIO interrupt timestamps every transition into a lock-free ring buffer; the
// main loop drains it, splits the stream into frames on silence, removes the
// spikes that asynchronous serial mode is documented to produce [p.63], and hands
// out whole frames.
//
// See docs/cc1101/04-async-serial-ook.md for why framing on silence is the right
// call, and why one button press deliberately arrives as one frame.
#pragma once

#include <stdint.h>

#include "cc1101_config.h"

// A long press of an EV1527-class remote repeats its ~50-pulse burst every 10 ms
// or so. 1024 pulses is around 20 repeats -- enough to see the repetition
// structure, which is what tells a fixed code from a rolling one.
static constexpr uint16_t SNIFFER_MAX_PULSES = 1024;

struct PulseFrame {
	uint32_t seq;          // monotonic, starts at 1
	uint32_t startMillis;  // millis() when the first pulse was queued
	uint16_t count;        // number of valid entries in durations[]
	uint8_t firstLevel;    // level of durations[0]; 1 = carrier on
	bool truncated;        // ran into SNIFFER_MAX_PULSES; more of this burst follows
	uint32_t durations[SNIFFER_MAX_PULSES];  // microseconds, alternating levels
};

class PulseSniffer {
   public:
	void begin(uint8_t pin);

	// Attaching and detaching matters: during transmit the ESP32 drives GDO0
	// itself, and every generated edge would otherwise fire the ISR and fill the
	// ring with our own output.
	void enable();
	void disable();
	bool enabled() const { return enabled_; }

	// Drains the edge queue and assembles frames. Returns a completed frame, or
	// nullptr. The pointer stays valid until the next call.
	const PulseFrame *poll();

	// Silence that ends a frame.
	uint32_t gapUs = CAPTURE_GAP_US_DEFAULT;
	// Pulses shorter than this are spikes: they and their partner get folded into
	// the preceding pulse. Set to 0 to keep everything.
	uint32_t glitchUs = CAPTURE_GLITCH_US_DEFAULT;
	// Frames with fewer pulses than this are noise and get dropped.
	uint16_t minPulses = CAPTURE_MIN_PULSES_DEFAULT;

	uint32_t droppedEdges() const;
	uint32_t rejectedFrames() const { return rejected_; }
	uint32_t emittedFrames() const { return emitted_; }
	void resetCounters();

   private:
	bool closeFrame();
	void deglitch(PulseFrame *f);
	void resetWork();

	PulseFrame bufA_{};
	PulseFrame bufB_{};
	PulseFrame *work_ = &bufA_;
	PulseFrame *out_ = &bufB_;

	uint8_t pin_ = 0;
	bool enabled_ = false;
	uint32_t seq_ = 0;
	uint32_t rejected_ = 0;
	uint32_t emitted_ = 0;
};
