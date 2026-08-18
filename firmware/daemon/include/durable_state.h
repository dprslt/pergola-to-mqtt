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
//   position   The dead-reckoned estimate. Still a guess, because a wired wall
//              press is undetectable either way, but a stale guess beats the
//              hardcoded 0 that a reboot used to assert.
//
// Both setters cache in RAM and touch flash only when the value really changes,
// so calling them from loop() costs nothing in the common case.
#pragma once

#include <stdint.h>

// Opens the namespace and loads both values. Safe to call once, from setup().
void durableBegin();

// True if a stop is still owed to the pergola. Survives a reset.
bool durableStopOwed();
void durableSetStopOwed(bool owed);

// 0 = closed, 100 = fully open. Defaults to 0 on a first ever boot.
uint8_t durablePosition();
void durableSetPosition(uint8_t position);
