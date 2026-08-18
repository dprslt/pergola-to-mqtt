#include "cc1101.h"

#include <Arduino.h>
#include <SPI.h>
#include <math.h>

#include "cc1101_config.h"
#include "pins.h"

namespace {

// How long to wait for the state machine to settle. Calibration on IDLE->RX/TX
// takes ~712 us [p.64], so 20 ms is many times over.
constexpr uint32_t STATE_TIMEOUT_US = 20000;

constexpr double XOSC = static_cast<double>(CC1101_XOSC_HZ);

}  // namespace

// ---------------------------------------------------------------------------
// SPI plumbing
// ---------------------------------------------------------------------------

void CC1101::select() {
	SPI.beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
	digitalWrite(PIN_CSN, LOW);
	// No CHIP_RDYn wait here: this firmware never puts the chip in SLEEP or XOFF,
	// so the crystal is always running and SO is already low. hardReset() does
	// wait, because there it genuinely matters.
}

void CC1101::deselect() {
	digitalWrite(PIN_CSN, HIGH);
	SPI.endTransaction();
}

bool CC1101::waitMisoLow(uint32_t timeoutUs) {
	uint32_t start = micros();
	while (digitalRead(PIN_MISO)) {
		if (micros() - start > timeoutUs) {
			return false;
		}
	}
	return true;
}

void CC1101::writeReg(uint8_t addr, uint8_t value) {
	select();
	SPI.transfer(addr | CC_WRITE_SINGLE);
	SPI.transfer(value);
	deselect();
}

uint8_t CC1101::readReg(uint8_t addr) {
	select();
	SPI.transfer(addr | CC_READ_SINGLE);
	uint8_t value = SPI.transfer(0x00);
	deselect();
	return value;
}

uint8_t CC1101::readStatusReg(uint8_t addr) {
	// For 0x30..0x3D the burst bit selects the status register rather than the
	// command strobe, so status reads use 0xC0 and cannot be bursted [p.32].
	select();
	SPI.transfer(addr | CC_READ_BURST);
	uint8_t value = SPI.transfer(0x00);
	deselect();
	return value;
}

void CC1101::writeBurst(uint8_t addr, const uint8_t *data, uint8_t len) {
	select();
	SPI.transfer(addr | CC_WRITE_BURST);
	for (uint8_t i = 0; i < len; i++) {
		SPI.transfer(data[i]);
	}
	deselect();
}

uint8_t CC1101::strobe(uint8_t cmd) {
	select();
	uint8_t status = SPI.transfer(cmd);
	deselect();
	return status;
}

// ---------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------

void CC1101::hardReset() {
	// Manual power-on reset [p.51]. Step one -- SCLK high, SI low -- exists to
	// avoid tripping the optional pin-control mode, which reuses CSn/SCLK/SI to
	// command state changes [p.34].
	digitalWrite(PIN_CSN, HIGH);
	pinMode(PIN_SCK, OUTPUT);
	digitalWrite(PIN_SCK, HIGH);
	pinMode(PIN_MOSI, OUTPUT);
	digitalWrite(PIN_MOSI, LOW);

	delayMicroseconds(5);
	digitalWrite(PIN_CSN, LOW);
	delayMicroseconds(10);
	digitalWrite(PIN_CSN, HIGH);
	delayMicroseconds(45);  // datasheet asks for at least 40 us

	// Hand the pins back to the SPI peripheral before talking.
	SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

	select();
	waitMisoLow(10000);  // CHIP_RDYn: crystal running
	SPI.transfer(CC_SRES);
	waitMisoLow(10000);  // reset complete, chip is IDLE
	deselect();
}

bool CC1101::begin() {
	pinMode(PIN_CSN, OUTPUT);
	digitalWrite(PIN_CSN, HIGH);
	pinMode(PIN_GDO0, INPUT);

	hardReset();

	uint8_t v = version();
	bool linkOk = (v == CC_VERSION_EXPECTED || v == CC_VERSION_LEGACY);

	applyDefaultConfig();
	return linkOk;
}

void CC1101::applyDefaultConfig() {
	idle();

	for (size_t i = 0; i < CC1101_DEFAULT_CONFIG_LEN; i++) {
		writeReg(CC1101_DEFAULT_CONFIG[i].addr, CC1101_DEFAULT_CONFIG[i].value);
	}

	paLevel_ = CC1101_PATABLE_ON_DEFAULT;
	writePatable();

	// Keep the cached view of the tunables in step with what we just wrote.
	//
	// Read FREQ back from the chip rather than restating the config table's value
	// as a literal here. A hardcoded 433.92f survived the switch to 315 MHz and
	// made `status` report a carrier the radio was not on -- and a status line that
	// lies about the frequency is the single most expensive kind of bug in this
	// project. Deriving it cannot drift.
	{
		const uint32_t freqWord = (static_cast<uint32_t>(readReg(CC_FREQ2)) << 16) |
		                          (static_cast<uint32_t>(readReg(CC_FREQ1)) << 8) |
		                          static_cast<uint32_t>(readReg(CC_FREQ0));
		freqMhz_ = static_cast<float>(freqWord * XOSC / 65536.0 / 1e6);
	}
	uint8_t mdmcfg4 = readReg(CC_MDMCFG4);
	uint8_t mdmcfg3 = readReg(CC_MDMCFG3);
	uint8_t drateE = mdmcfg4 & 0x0F;
	uint8_t chanbwE = (mdmcfg4 >> 6) & 0x03;
	uint8_t chanbwM = (mdmcfg4 >> 4) & 0x03;
	baud_ = static_cast<uint32_t>(
	    ((256.0 + mdmcfg3) * pow(2.0, drateE) / 268435456.0) * XOSC);
	bwKhz_ = static_cast<uint32_t>(
	    XOSC / (8.0 * (4 + chanbwM) * pow(2.0, chanbwE)) / 1000.0 + 0.5);

	receive();
}

void CC1101::writePatable() {
	// Only index 0 and 1 matter for OOK, but PATABLE must be burst-written to
	// reach anything past index 0 [p.59].
	const uint8_t table[8] = {CC1101_PATABLE_OFF, paLevel_, 0, 0, 0, 0, 0, 0};
	writeBurst(CC_PATABLE, table, sizeof(table));
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

bool CC1101::idle() {
	strobe(CC_SIDLE);

	uint32_t start = micros();
	for (;;) {
		uint8_t state = marcState();
		if (state == CC_MARC_IDLE) {
			return true;
		}
		// The FIFOs are unused in async serial mode, but an overflow state would
		// wedge us here forever, and it only clears via a flush strobe [p.33].
		if (state == CC_MARC_RXFIFO_OVERFLOW) {
			strobe(CC_SFRX);
		} else if (state == CC_MARC_TXFIFO_UNDERFLOW) {
			strobe(CC_SFTX);
		}
		if (micros() - start > STATE_TIMEOUT_US) {
			return false;
		}
		delayMicroseconds(50);
	}
}

bool CC1101::receive() {
	if (!idle()) {
		return false;
	}

	writeReg(CC_IOCFG0, CC_GDO_CFG_ASYNC_DATA_OUT);
	pinMode(PIN_GDO0, INPUT);
	strobe(CC_SRX);

	uint32_t start = micros();
	while (marcState() != CC_MARC_RX) {
		if (micros() - start > STATE_TIMEOUT_US) {
			return false;
		}
		delayMicroseconds(50);
	}
	return true;
}

bool CC1101::beginTransmitRaw() {
	if (!idle()) {
		return false;
	}

	// Release GDO0 before driving it, or the CC1101 output and the ESP32 output
	// fight over the same net.
	writeReg(CC_IOCFG0, CC_GDO_CFG_HIGH_Z);
	pinMode(PIN_GDO0, OUTPUT);
	digitalWrite(PIN_GDO0, LOW);

	strobe(CC_STX);

	uint32_t start = micros();
	while (marcState() != CC_MARC_TX) {
		if (micros() - start > STATE_TIMEOUT_US) {
			// Put the pin back before giving up, so we do not leave two drivers on
			// the net.
			pinMode(PIN_GDO0, INPUT);
			writeReg(CC_IOCFG0, CC_GDO_CFG_ASYNC_DATA_OUT);
			return false;
		}
		delayMicroseconds(50);
	}
	delayMicroseconds(200);  // let the PA settle before keying it
	return true;
}

void CC1101::endTransmitRaw() {
	digitalWrite(PIN_GDO0, LOW);
	idle();
	receive();
}

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

void CC1101::writeFreqRegisters(uint32_t freqWord) {
	writeReg(CC_FREQ2, static_cast<uint8_t>((freqWord >> 16) & 0xFF));
	writeReg(CC_FREQ1, static_cast<uint8_t>((freqWord >> 8) & 0xFF));
	writeReg(CC_FREQ0, static_cast<uint8_t>(freqWord & 0xFF));
}

float CC1101::setFrequencyMHz(float mhz) {
	// The chip tunes three bands: 300-348, 387-464 and 779-928 MHz [p.64]. This
	// project uses the lower two: 433.92 for generic OOK remotes, and 315 for
	// PT2262-class remotes built for that band. A 433 MHz module's matching
	// network and antenna make 315 comparatively deaf, but a remote held against
	// the board still sits tens of dB above the noise floor.
	if (mhz < 300.0f) {
		mhz = 300.0f;
	}
	if (mhz > 464.0f) {
		mhz = 464.0f;
	}
	// 348-387 MHz is a gap between bands where the synthesiser cannot lock.
	// Snap to whichever edge is nearer rather than pretending it tuned.
	if (mhz > 348.0f && mhz < 387.0f) {
		mhz = (mhz - 348.0f < 387.0f - mhz) ? 348.0f : 387.0f;
	}

	// f_carrier = (f_xosc / 2^16) * FREQ[23:0]   [p.75]
	uint32_t freqWord =
	    static_cast<uint32_t>((static_cast<double>(mhz) * 1e6 * 65536.0) / XOSC + 0.5);

	// Frequency programming registers must only change in IDLE [p.57].
	idle();
	writeFreqRegisters(freqWord);
	freqMhz_ = static_cast<float>(freqWord * XOSC / 65536.0 / 1e6);
	receive();

	return freqMhz_;
}

uint32_t CC1101::setDataRateBaud(uint32_t baud) {
	if (baud < 600) {
		baud = 600;
	}
	if (baud > 500000) {
		baud = 500000;
	}

	// R = ((256 + DRATE_M) * 2^DRATE_E / 2^28) * f_xosc   [p.35]
	int e = static_cast<int>(floor(log2(static_cast<double>(baud) * 1048576.0 / XOSC)));
	if (e < 0) {
		e = 0;
	}
	if (e > 15) {
		e = 15;
	}

	double m = floor(static_cast<double>(baud) * 268435456.0 / (XOSC * pow(2.0, e)) + 0.5) - 256.0;
	if (m > 255.0) {
		// Rounded up out of the mantissa; carry into the exponent [p.35].
		m = 0.0;
		if (e < 15) {
			e++;
		} else {
			m = 255.0;
		}
	}
	if (m < 0.0) {
		m = 0.0;
	}

	idle();
	uint8_t mdmcfg4 = (readReg(CC_MDMCFG4) & 0xF0) | static_cast<uint8_t>(e & 0x0F);
	writeReg(CC_MDMCFG4, mdmcfg4);
	writeReg(CC_MDMCFG3, static_cast<uint8_t>(m));
	baud_ = static_cast<uint32_t>(((256.0 + m) * pow(2.0, e) / 268435456.0) * XOSC);
	receive();

	return baud_;
}

uint32_t CC1101::setChannelBwKhz(uint32_t khz) {
	// BW = f_xosc / (8 * (4 + CHANBW_M) * 2^CHANBW_E)   [p.35]
	// Only 16 combinations exist, so just pick the closest.
	uint8_t bestE = 0;
	uint8_t bestM = 0;
	double bestBw = 0.0;
	double bestErr = 1e12;

	for (uint8_t e = 0; e < 4; e++) {
		for (uint8_t m = 0; m < 4; m++) {
			double bw = XOSC / (8.0 * (4 + m) * pow(2.0, e)) / 1000.0;
			double err = fabs(bw - static_cast<double>(khz));
			if (err < bestErr) {
				bestErr = err;
				bestE = e;
				bestM = m;
				bestBw = bw;
			}
		}
	}

	idle();
	uint8_t mdmcfg4 = static_cast<uint8_t>((bestE << 6) | (bestM << 4) | (readReg(CC_MDMCFG4) & 0x0F));
	writeReg(CC_MDMCFG4, mdmcfg4);
	bwKhz_ = static_cast<uint32_t>(bestBw + 0.5);
	receive();

	return bwKhz_;
}

void CC1101::setPaLevel(uint8_t patableByte) {
	paLevel_ = patableByte;
	writePatable();
}

float CC1101::rssiDbm() {
	// RSSI is a 2's-complement value in half-dB steps; the 433 MHz offset is
	// 74 dB [p.44, p.45].
	int raw = readStatusReg(CC_RSSI);
	if (raw >= 128) {
		raw -= 256;
	}
	return static_cast<float>(raw) / 2.0f - static_cast<float>(CC_RSSI_OFFSET_DB);
}
