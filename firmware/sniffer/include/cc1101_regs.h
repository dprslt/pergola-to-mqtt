// CC1101 register addresses, command strobes and header-byte encoding.
//
// Mirrors docs/cc1101/02-registers.md. Datasheet references are to TI SWRS061I.
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Header byte [p.30]
//
//   bit 7   R/W    1 = read, 0 = write
//   bit 6   BURST  1 = auto-increment address (or, for 0x30..0x3D, select the
//                  status register rather than the command strobe)
//   bits 5:0       address
// ---------------------------------------------------------------------------
static constexpr uint8_t CC_WRITE_SINGLE = 0x00;
static constexpr uint8_t CC_WRITE_BURST = 0x40;
static constexpr uint8_t CC_READ_SINGLE = 0x80;
static constexpr uint8_t CC_READ_BURST = 0xC0;

// Configuration registers 0x00..0x2E [p.68]
static constexpr uint8_t CC_IOCFG2 = 0x00;
static constexpr uint8_t CC_IOCFG1 = 0x01;
static constexpr uint8_t CC_IOCFG0 = 0x02;
static constexpr uint8_t CC_FIFOTHR = 0x03;
static constexpr uint8_t CC_SYNC1 = 0x04;
static constexpr uint8_t CC_SYNC0 = 0x05;
static constexpr uint8_t CC_PKTLEN = 0x06;
static constexpr uint8_t CC_PKTCTRL1 = 0x07;
static constexpr uint8_t CC_PKTCTRL0 = 0x08;
static constexpr uint8_t CC_ADDR = 0x09;
static constexpr uint8_t CC_CHANNR = 0x0A;
static constexpr uint8_t CC_FSCTRL1 = 0x0B;
static constexpr uint8_t CC_FSCTRL0 = 0x0C;
static constexpr uint8_t CC_FREQ2 = 0x0D;
static constexpr uint8_t CC_FREQ1 = 0x0E;
static constexpr uint8_t CC_FREQ0 = 0x0F;
static constexpr uint8_t CC_MDMCFG4 = 0x10;
static constexpr uint8_t CC_MDMCFG3 = 0x11;
static constexpr uint8_t CC_MDMCFG2 = 0x12;
static constexpr uint8_t CC_MDMCFG1 = 0x13;
static constexpr uint8_t CC_MDMCFG0 = 0x14;
static constexpr uint8_t CC_DEVIATN = 0x15;
static constexpr uint8_t CC_MCSM2 = 0x16;
static constexpr uint8_t CC_MCSM1 = 0x17;
static constexpr uint8_t CC_MCSM0 = 0x18;
static constexpr uint8_t CC_FOCCFG = 0x19;
static constexpr uint8_t CC_BSCFG = 0x1A;
static constexpr uint8_t CC_AGCCTRL2 = 0x1B;
static constexpr uint8_t CC_AGCCTRL1 = 0x1C;
static constexpr uint8_t CC_AGCCTRL0 = 0x1D;
static constexpr uint8_t CC_WOREVT1 = 0x1E;
static constexpr uint8_t CC_WOREVT0 = 0x1F;
static constexpr uint8_t CC_WORCTRL = 0x20;
static constexpr uint8_t CC_FREND1 = 0x21;
static constexpr uint8_t CC_FREND0 = 0x22;
static constexpr uint8_t CC_FSCAL3 = 0x23;
static constexpr uint8_t CC_FSCAL2 = 0x24;
static constexpr uint8_t CC_FSCAL1 = 0x25;
static constexpr uint8_t CC_FSCAL0 = 0x26;
static constexpr uint8_t CC_RCCTRL1 = 0x27;
static constexpr uint8_t CC_RCCTRL0 = 0x28;
static constexpr uint8_t CC_FSTEST = 0x29;
static constexpr uint8_t CC_PTEST = 0x2A;
static constexpr uint8_t CC_AGCTEST = 0x2B;
static constexpr uint8_t CC_TEST2 = 0x2C;
static constexpr uint8_t CC_TEST1 = 0x2D;
static constexpr uint8_t CC_TEST0 = 0x2E;

static constexpr uint8_t CC_CONFIG_REG_COUNT = 0x2F;  // 0x00..0x2E inclusive

// Command strobes 0x30..0x3D, burst bit clear [p.67]
static constexpr uint8_t CC_SRES = 0x30;      // reset chip
static constexpr uint8_t CC_SFSTXON = 0x31;   // enable + calibrate synthesiser
static constexpr uint8_t CC_SXOFF = 0x32;     // turn off crystal oscillator
static constexpr uint8_t CC_SCAL = 0x33;      // calibrate synthesiser, then off
static constexpr uint8_t CC_SRX = 0x34;       // enter RX
static constexpr uint8_t CC_STX = 0x35;       // enter TX
static constexpr uint8_t CC_SIDLE = 0x36;     // leave RX/TX, synthesiser off
static constexpr uint8_t CC_SWOR = 0x38;      // start wake-on-radio
static constexpr uint8_t CC_SPWD = 0x39;      // power down when CSn goes high
static constexpr uint8_t CC_SFRX = 0x3A;      // flush RX FIFO
static constexpr uint8_t CC_SFTX = 0x3B;      // flush TX FIFO
static constexpr uint8_t CC_SWORRST = 0x3C;   // reset WOR real-time clock
static constexpr uint8_t CC_SNOP = 0x3D;      // no-op; fetches the status byte

// Status registers 0x30..0x3D, burst bit set [p.69]
static constexpr uint8_t CC_PARTNUM = 0x30;
static constexpr uint8_t CC_VERSION = 0x31;
static constexpr uint8_t CC_FREQEST = 0x32;
static constexpr uint8_t CC_LQI = 0x33;
static constexpr uint8_t CC_RSSI = 0x34;
static constexpr uint8_t CC_MARCSTATE = 0x35;
static constexpr uint8_t CC_WORTIME1 = 0x36;
static constexpr uint8_t CC_WORTIME0 = 0x37;
static constexpr uint8_t CC_PKTSTATUS = 0x38;
static constexpr uint8_t CC_VCO_VC_DAC = 0x39;
static constexpr uint8_t CC_TXBYTES = 0x3A;
static constexpr uint8_t CC_RXBYTES = 0x3B;
static constexpr uint8_t CC_RCCTRL1_STATUS = 0x3C;
static constexpr uint8_t CC_RCCTRL0_STATUS = 0x3D;

// Multi-byte areas
static constexpr uint8_t CC_PATABLE = 0x3E;
static constexpr uint8_t CC_FIFO = 0x3F;

// MARCSTATE values we actually test against [p.93]
static constexpr uint8_t CC_MARC_IDLE = 0x01;
static constexpr uint8_t CC_MARC_RX = 0x0D;
static constexpr uint8_t CC_MARC_RXFIFO_OVERFLOW = 0x11;
static constexpr uint8_t CC_MARC_TX = 0x13;
static constexpr uint8_t CC_MARC_TXFIFO_UNDERFLOW = 0x16;
static constexpr uint8_t CC_MARC_MASK = 0x1F;

// Chip status byte fields [p.31]
static constexpr uint8_t CC_STATUS_CHIP_RDYn = 0x80;
static constexpr uint8_t CC_STATUS_STATE_MASK = 0x70;
static constexpr uint8_t CC_STATUS_FIFO_MASK = 0x0F;

// IOCFGx.GDOx_CFG values used here [p.62]
static constexpr uint8_t CC_GDO_CFG_ASYNC_DATA_OUT = 0x0D;  // serial data out
static constexpr uint8_t CC_GDO_CFG_CARRIER_SENSE = 0x0E;
static constexpr uint8_t CC_GDO_CFG_HIGH_Z = 0x2E;  // release the pin

// Expected VERSION readings. Anything else -- especially 0x00 or 0xFF -- means
// the SPI link is not working [p.92].
static constexpr uint8_t CC_VERSION_EXPECTED = 0x14;
static constexpr uint8_t CC_VERSION_LEGACY = 0x04;

// RSSI conversion offset for 433 MHz, in dB [p.45].
static constexpr int CC_RSSI_OFFSET_DB = 74;
