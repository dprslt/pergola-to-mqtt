// Minimal CC1101 driver, scoped to what OOK sniffing and replay need.
//
// No FIFO handling, no packet handler, no wake-on-radio: asynchronous serial mode
// bypasses all of it. See docs/cc1101/04-async-serial-ook.md.
#pragma once

#include <stdint.h>

#include "cc1101_regs.h"

class CC1101 {
   public:
	// Brings up SPI, performs the documented power-on reset, writes the default
	// configuration and enters RX. Returns false if VERSION does not read back
	// plausibly, which always means wiring or power.
	bool begin();

	// Re-applies the default configuration table and PATABLE, then returns to RX.
	void applyDefaultConfig();

	uint8_t version() { return readStatusReg(CC_VERSION); }
	uint8_t marcState() { return readStatusReg(CC_MARCSTATE) & CC_MARC_MASK; }
	uint8_t pktStatus() { return readStatusReg(CC_PKTSTATUS); }

	// Signed RSSI in dBm, using the 433 MHz offset of 74 dB [p.45].
	float rssiDbm();

	uint8_t readReg(uint8_t addr);
	uint8_t readStatusReg(uint8_t addr);
	void writeReg(uint8_t addr, uint8_t value);
	void writeBurst(uint8_t addr, const uint8_t *data, uint8_t len);
	uint8_t strobe(uint8_t cmd);

	// SIDLE, then wait for MARCSTATE to actually reach IDLE. Necessary before
	// touching frequency registers, and before any further strobe: SIDLE discards
	// pending strobes until IDLE is reached [p.32]. Returns false on timeout.
	bool idle();

	// IOCFG0 back to async data out, GDO0 back to an ESP32 input, then SRX.
	bool receive();

	// Hands GDO0 over to the ESP32 and enters TX. The caller then drives the pin
	// directly; see docs/cc1101/05-recipes.md recipe 2. Returns false if the chip
	// would not go idle first.
	bool beginTransmitRaw();

	// Undoes beginTransmitRaw() and goes back to receiving.
	void endTransmitRaw();

	// Frequency programming must only happen in IDLE [p.57]; this handles the
	// IDLE/restore dance itself. Returns the frequency actually programmed, which
	// is quantised to f_xosc / 2^16 = 396.7 Hz steps.
	float setFrequencyMHz(float mhz);
	float frequencyMHz() const { return freqMhz_; }

	// Closest achievable data rate to the request. In async serial mode this only
	// sets the demodulator's data filter bandwidth -- no bit synchroniser runs.
	uint32_t setDataRateBaud(uint32_t baud);
	uint32_t dataRateBaud() const { return baud_; }

	// Closest achievable receiver channel filter bandwidth, in kHz.
	uint32_t setChannelBwKhz(uint32_t khz);
	uint32_t channelBwKhz() const { return bwKhz_; }

	// PATABLE[1]: the power used for an OOK '1'. See the table in
	// docs/cc1101/03-frequency-and-modulation.md#output-power.
	void setPaLevel(uint8_t patableByte);
	uint8_t paLevel() const { return paLevel_; }

   private:
	void select();
	void deselect();
	bool waitMisoLow(uint32_t timeoutUs);
	void hardReset();
	void writeFreqRegisters(uint32_t freqWord);
	void writePatable();

	// Placeholder only. Overwritten from the chip's own FREQ registers in
	// applyDefaultConfig(); never trust it before begin() has run.
	float freqMhz_ = 0.0f;
	uint32_t baud_ = 4798;
	uint32_t bwKhz_ = 203;
	uint8_t paLevel_ = 0xC0;
};
