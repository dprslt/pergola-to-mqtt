// State that has to survive a power cut, in NVS.
//
// Everything else this daemon knows can be rebuilt on a fresh boot. These two
// facts cannot:
//
//   stop-owed  A move has been commanded and the stop that terminates it has
//              not gone out yet. docs/behaviour.md: a full open that is never
//              followed by a stop latches the roof open, and only a stop clears
//              it. CoverState_t schedules that stop when the move starts, so a
//              crash or a busy loop cannot skip it -- but the schedule lives in
//              RAM, so a reset inside the ~6.8 s window between the open and
//              its stop used to lose the obligation outright. That window is
//              also when the radio and WiFi draw their transmit current, which
//              is exactly when a marginal supply browns out (docs/hardware.md,
//              section Power). Firmware cannot fix the supply; it can make sure
//              the obligation outlives the reset.
//
// The position estimate is deliberately NOT here. Storing it was tried and
// reverted: the boot-time reset to "closed" is the only thing that clears a belief
// that has drifted from reality, and persisting the estimate made a wrong one
// survive every reboot. An owed stop is a fact about what this daemon did; a
// position is a guess about the world. Keep the first, never the second.
//
// The setter caches in RAM and touches flash only when the value really changes,
// so calling it from loop() costs nothing in the common case.
#pragma once

#include <stdint.h>

// Opens the namespace and loads both values. Safe to call once, from setup().
void durableBegin();

// True if a stop is still owed to the pergola. Survives a reset.
bool durableStopOwed();
void durableSetStopOwed(bool owed);
