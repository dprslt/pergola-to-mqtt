// Wiring for ESP32 <-> CC1101. See docs/hardware.md for the rationale behind
// each choice; in short: 18/19/23/5 are the default VSPI pins, and GPIO4 is
// neither a strapping pin, nor wired to flash, nor input-only -- GDO0 has to be
// driven as well as read, so an input-only GPIO cannot be used.
#pragma once

#include <stdint.h>

static constexpr uint8_t PIN_SCK = 18;
static constexpr uint8_t PIN_MISO = 19;
static constexpr uint8_t PIN_MOSI = 23;
static constexpr uint8_t PIN_CSN = 5;

// Asynchronous serial data. CC1101 drives it in RX, the ESP32 drives it in TX.
static constexpr uint8_t PIN_GDO0 = 4;

// GDO2 (module pin 8) is left floating -- the sniffer never needs it, since
// asynchronous serial mode routes the baseband out on GDO0 alone. IOCFG2 is
// configured to high-impedance so the pin drives nothing.
//
// If you ever do wire it, do not use GPIO2: it is a strapping pin, and GDO2
// powers up driving CHIP_RDYn high, which would change the ESP32's boot mode.
// PIN_GDO2 is deliberately absent rather than set to an unconnected GPIO.

// SPI limits from the datasheet [p.30]: 10 MHz with a 100 ns gap between address
// and data, 9 MHz single access, 6.5 MHz burst. 4 MHz is comfortably inside all
// three and short dupont wires stay happy.
static constexpr uint32_t SPI_CLOCK_HZ = 4000000;

// Crystal on the module. Every frequency, data rate and bandwidth formula scales
// with this -- check your board before trusting any computed value.
static constexpr uint32_t CC1101_XOSC_HZ = 26000000;
