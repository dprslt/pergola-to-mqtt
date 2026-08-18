// Default CC1101 configuration: 315 MHz OOK, asynchronous serial mode, raw
// baseband on GDO0.
//
// 315 MHz, not 433.92: this pergola's remote is a 315 MHz OOK transmitter. See
// docs/remote-protocol.md. Retune at runtime with `freq 433.92` when working on
// a different remote.
//
// Every byte here is justified in docs/cc1101/05-recipes.md (recipe 1). Change
// one, change the other.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cc1101_regs.h"

struct CC1101RegValue {
	uint8_t addr;
	uint8_t value;
};

// Registers written at start-up. Anything absent stays at its reset value --
// notably SYNC1/SYNC0, ADDR and the wake-on-radio block, none of which do
// anything in asynchronous serial mode.
static const CC1101RegValue CC1101_DEFAULT_CONFIG[] = {
    {CC_IOCFG2, 0x2E},    // GDO2 high-impedance
    {CC_IOCFG1, 0x2E},    // GDO1/SO 3-state, normal SPI behaviour
    {CC_IOCFG0, 0x0D},    // GDO0 = async serial data out. The whole point.
    {CC_FIFOTHR, 0x47},   // ADC_RETENTION=1 (RX BW < 325 kHz), FIFO_THR=7
    {CC_PKTLEN, 0xFF},    // unused in async mode
    {CC_PKTCTRL1, 0x04},  // PQT=0, APPEND_STATUS=1; both inert here
    {CC_PKTCTRL0, 0x32},  // PKT_FORMAT=3 async, whitening off, CRC off
    {CC_ADDR, 0x00},
    {CC_CHANNR, 0x00},   // single channel
    {CC_FSCTRL1, 0x06},  // IF = 152 kHz, suits a 203 kHz channel filter
    {CC_FSCTRL0, 0x00},
    // 315.000 MHz with a 26 MHz crystal:
    //   FREQ = round(315e6 * 2^16 / 26e6) = 793994 = 0x0C1D8A -> 315.000061 MHz
    {CC_FREQ2, 0x0C},
    {CC_FREQ1, 0x1D},
    {CC_FREQ0, 0x8A},
    {CC_MDMCFG4, 0x87},   // channel BW 203 kHz, DRATE_E=7
    {CC_MDMCFG3, 0x83},   // DRATE_M=131 -> 4798 Baud
    {CC_MDMCFG2, 0x30},   // ASK/OOK, no Manchester, SYNC_MODE=0
    {CC_MDMCFG1, 0x22},   // reset value; channel spacing unused
    {CC_MDMCFG0, 0xF8},   // reset value
    {CC_DEVIATN, 0x00},   // no effect in OOK/ASK [p.43]
    {CC_MCSM2, 0x07},     // reset value; no RX timeout
    {CC_MCSM1, 0x3C},     // RXOFF_MODE=3: stay in RX forever
    {CC_MCSM0, 0x18},     // FS_AUTOCAL on IDLE->RX/TX, PO_TIMEOUT=2
    {CC_FOCCFG, 0x16},    // inert in OOK [p.36]
    {CC_BSCFG, 0x6C},     // reset value; no bit sync in async mode
    {CC_AGCCTRL2, 0x07},  // max DVGA + LNA gain, MAGN_TARGET = 42 dB
    {CC_AGCCTRL1, 0x00},  // carrier-sense thresholds disabled
    // 12 dB, not the 8 dB of 0x91. Async serial mode has no squelch, so the data
    // line always carries something and a sensitive decision boundary turns the
    // noise floor into a flood of junk frames that buries real output. The remote
    // arrives at about -23 dBm, roughly 70 dB above the floor, so it loses nothing.
    // Raise to 0x93 (16 dB) if still noisy. [p.87]
    {CC_AGCCTRL0, 0x92},
    {CC_FREND1, 0x56},    // RX front-end currents for a low data rate
    {CC_FREND0, 0x11},    // PA_POWER=1: OOK '1' from PATABLE[1], '0' from [0]
    {CC_FSCAL3, 0xE9},
    {CC_FSCAL2, 0x2A},
    {CC_FSCAL1, 0x00},
    {CC_FSCAL0, 0x1F},
    {CC_TEST2, 0x81},  // matches FIFOTHR.ADC_RETENTION=1 [p.72]
    {CC_TEST1, 0x35},  // ditto
    {CC_TEST0, 0x09},  // VCO_SEL_CAL_EN=0, we sit on one frequency
};

static constexpr size_t CC1101_DEFAULT_CONFIG_LEN =
    sizeof(CC1101_DEFAULT_CONFIG) / sizeof(CC1101_DEFAULT_CONFIG[0]);

// PATABLE. In OOK, index 0 is the power for a '0' and index FREND0.PA_POWER (=1)
// the power for a '1' [p.59]. 0xC0 is +10 dBm at 433 MHz with multi-layer
// inductors, drawing ~29 mA [p.60]; drop to 0x60 for 0 dBm if the board browns
// out on transmit.
static constexpr uint8_t CC1101_PATABLE_OFF = 0x00;
static constexpr uint8_t CC1101_PATABLE_ON_DEFAULT = 0xC0;

// Defaults for the pulse capture front end. All adjustable over serial.
//
// gap: silence that ends a frame. 20 ms is deliberately longer than the ~10 ms
//      sync gap an EV1527-class remote puts between repeats, so one button press
//      arrives as one frame containing every repeat -- which is exactly what lets
//      the host tools tell a fixed code from a rolling one.
// glitch: async serial mode emits 37-38.5 ns spikes at random [p.63]. Anything
//      this short is a spike, and the pulses either side of it get merged.
// 5000 us, not 20000. The remote's sync low is 31 * alpha = ~10.9 ms, so a
// 20 ms threshold does NOT split consecutive words: repeats merge into one frame
// and the repeat comparison straddles partial words, reporting identical=NO on a
// code that is perfectly fixed. That cost an evening. See
// docs/remote-protocol.md. Anything comfortably below the sync low works.
static constexpr uint32_t CAPTURE_GAP_US_DEFAULT = 5000;
static constexpr uint32_t CAPTURE_GLITCH_US_DEFAULT = 50;
// A 24-bit word is 48 data pulses plus a sync pulse, and observed noise frames
// run to about 36 pulses, so 40 separates them cleanly. Lower this when working on
// a remote that sends shorter words.
static constexpr uint16_t CAPTURE_MIN_PULSES_DEFAULT = 40;
