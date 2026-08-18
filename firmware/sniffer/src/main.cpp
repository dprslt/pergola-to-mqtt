// pergola-to-mqtt -- 433 MHz OOK sniffer and replayer.
//
// Prints every OOK burst it hears as a line of pulse durations, and can replay a
// captured burst back out. Everything is tunable over the serial console, so
// chasing a stubborn remote never needs a reflash: type `?` for the command list.
//
// Output grammar, consumed by tools/pergola_capture.py:
//
//   #...                          human-readable comment, ignore when parsing
//   F,seq,t_ms,rssi,lvl,trunc,n,d0,d1,...,dn-1     a captured burst
//   S,mhz,rssi                                     one row of a frequency scan
//
// Durations are microseconds, alternating in level, starting at level `lvl`
// (1 = carrier on).

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "cc1101.h"
#include "cc1101_config.h"
#include "pins.h"
#include "pulse_sniffer.h"
#include "ev1527.h"
#include "pergola_codes.h"

static constexpr const char *FIRMWARE_VERSION = "0.1.0";

static CC1101 radio;
static PulseSniffer sniffer;

static bool frameOutput = true;

// Peak RSSI seen recently, sampled slowly in the main loop. A frame only closes
// `gapUs` after its last edge, so reading RSSI at that moment would report the
// noise floor rather than the burst -- this keeps the peak from just before.
static constexpr uint32_t RSSI_SAMPLE_MS = 5;
static constexpr uint32_t RSSI_PEAK_HOLD_MS = 250;
static float rssiPeak = -200.0f;
static uint32_t rssiPeakAt = 0;
static uint32_t rssiSampledAt = 0;

// Most recent emitted frame, kept so `keep <slot>` still works after the sniffer
// has moved on.
static PulseFrame lastFrame;
static bool haveLastFrame = false;

// Replay slots.
static constexpr uint8_t SLOT_COUNT = 4;
struct ReplaySlot {
	bool used = false;
	uint16_t count = 0;
	uint8_t firstLevel = 1;
	uint32_t durations[SNIFFER_MAX_PULSES];
};
static ReplaySlot slots[SLOT_COUNT];

// ---------------------------------------------------------------------------
// Register names, for the `reg` dump
// ---------------------------------------------------------------------------

static const char *const REG_NAMES[CC_CONFIG_REG_COUNT] = {
    "IOCFG2", "IOCFG1", "IOCFG0", "FIFOTHR", "SYNC1", "SYNC0", "PKTLEN",
    "PKTCTRL1", "PKTCTRL0", "ADDR", "CHANNR", "FSCTRL1", "FSCTRL0", "FREQ2",
    "FREQ1", "FREQ0", "MDMCFG4", "MDMCFG3", "MDMCFG2", "MDMCFG1", "MDMCFG0",
    "DEVIATN", "MCSM2", "MCSM1", "MCSM0", "FOCCFG", "BSCFG", "AGCCTRL2",
    "AGCCTRL1", "AGCCTRL0", "WOREVT1", "WOREVT0", "WORCTRL", "FREND1", "FREND0",
    "FSCAL3", "FSCAL2", "FSCAL1", "FSCAL0", "RCCTRL1", "RCCTRL0", "FSTEST",
    "PTEST", "AGCTEST", "TEST2", "TEST1", "TEST0",
};

// ---------------------------------------------------------------------------
// Frame analysis -- just enough to tell at a glance whether a capture is real
// ---------------------------------------------------------------------------

// Greedy width clustering. Remotes use two or three distinct pulse widths plus a
// long sync gap, so a handful of buckets covers everything we expect to see.
static constexpr uint8_t MAX_CLUSTERS = 6;
static constexpr float CLUSTER_TOLERANCE = 0.25f;  // +/- 25% of the centre

struct ClusterSet {
	uint8_t n = 0;
	uint32_t centre[MAX_CLUSTERS] = {0};
	uint16_t population[MAX_CLUSTERS] = {0};
};

static void buildClusters(const PulseFrame *f, ClusterSet *cs) {
	cs->n = 0;
	for (uint16_t i = 0; i < f->count; i++) {
		uint32_t d = f->durations[i];
		bool placed = false;
		for (uint8_t c = 0; c < cs->n; c++) {
			float delta = fabsf(static_cast<float>(d) - static_cast<float>(cs->centre[c]));
			if (delta <= CLUSTER_TOLERANCE * static_cast<float>(cs->centre[c])) {
				// Running mean, so the centre settles on the real pulse width rather
				// than on whichever sample happened to arrive first.
				uint32_t total = cs->centre[c] * cs->population[c] + d;
				cs->population[c]++;
				cs->centre[c] = total / cs->population[c];
				placed = true;
				break;
			}
		}
		if (!placed && cs->n < MAX_CLUSTERS) {
			cs->centre[cs->n] = d;
			cs->population[cs->n] = 1;
			cs->n++;
		}
	}

	// Sort ascending by centre; insertion sort on at most six entries.
	for (uint8_t i = 1; i < cs->n; i++) {
		uint32_t c = cs->centre[i];
		uint16_t p = cs->population[i];
		int8_t j = static_cast<int8_t>(i) - 1;
		while (j >= 0 && cs->centre[j] > c) {
			cs->centre[j + 1] = cs->centre[j];
			cs->population[j + 1] = cs->population[j];
			j--;
		}
		cs->centre[j + 1] = c;
		cs->population[j + 1] = p;
	}
}

static uint8_t classify(const ClusterSet *cs, uint32_t d) {
	uint8_t best = 0;
	uint32_t bestErr = UINT32_MAX;
	for (uint8_t c = 0; c < cs->n; c++) {
		uint32_t err = (d > cs->centre[c]) ? (d - cs->centre[c]) : (cs->centre[c] - d);
		if (err < bestErr) {
			bestErr = err;
			best = c;
		}
	}
	return best;
}

// Prints a one-line verdict on a frame. The interesting part is `same`: a remote
// repeats its burst several times per press, so if the repeats within one press
// are identical, the code is at least stable inside a press. Comparing that hex
// across separate presses is what distinguishes a fixed code from a rolling one --
// and that comparison is the host tools' job.
static void summariseFrame(const PulseFrame *f, float rssi) {
	ClusterSet cs;
	buildClusters(f, &cs);

	// The most populous cluster is the symbol width; anything several times longer
	// is a sync gap between repeats.
	uint32_t symbol = cs.n ? cs.centre[0] : 0;
	uint16_t bestPop = 0;
	for (uint8_t c = 0; c < cs.n; c++) {
		if (cs.population[c] > bestPop) {
			bestPop = cs.population[c];
			symbol = cs.centre[c];
		}
	}
	uint32_t syncThreshold = symbol * 4;

	uint32_t totalUs = 0;
	for (uint16_t i = 0; i < f->count; i++) {
		totalUs += f->durations[i];
	}

	// Split on sync gaps and compare each repeat against the first.
	uint16_t repeats = 0;
	bool allSame = true;
	uint16_t firstStart = 0, firstLen = 0;
	uint16_t segStart = 0;
	for (uint16_t i = 0; i <= f->count; i++) {
		bool boundary = (i == f->count) || (f->durations[i] >= syncThreshold);
		if (!boundary) {
			continue;
		}
		uint16_t segLen = i - segStart;
		if (segLen >= 8) {
			repeats++;
			if (repeats == 1) {
				firstStart = segStart;
				firstLen = segLen;
			} else if (segLen != firstLen) {
				allSame = false;
			} else {
				for (uint16_t k = 0; k < segLen; k++) {
					if (classify(&cs, f->durations[segStart + k]) !=
					    classify(&cs, f->durations[firstStart + k])) {
						allSame = false;
						break;
					}
				}
			}
		}
		segStart = i + 1;
	}

	Serial.printf("# frame %u: n=%u %.1fms rssi=%.1fdBm lvl=%u%s widths=",
	              f->seq, f->count, totalUs / 1000.0f, rssi, f->firstLevel,
	              f->truncated ? " TRUNCATED" : "");
	for (uint8_t c = 0; c < cs.n; c++) {
		Serial.printf("%s%ux%u", c ? " " : "", cs.centre[c], cs.population[c]);
	}
	if (repeats > 1) {
		Serial.printf(" repeats=%u identical=%s", repeats, allSame ? "yes" : "NO");
	} else if (repeats == 1) {
		Serial.print(" repeats=1");
	}
	// Decode on the device rather than making the operator run a host script.
	// Failure is the normal case for noise, so say nothing when it fails -- a
	// printed code is a positive signal that a real remote was heard.
	uint32_t code = 0;
	if (ev1527Decode(f->durations, f->count, f->firstLevel, PERGOLA_CODE_BITS,
	                 PERGOLA_ALPHA_US, &code)) {
		Serial.printf(" code=0x%06lX", static_cast<unsigned long>(code));
		if ((code >> 4) == PERGOLA_ADDRESS) {
			const char *name = "?";
			if (code == PERGOLA_CODE_OPEN) {
				name = "OPEN";
			} else if (code == PERGOLA_CODE_STOP) {
				name = "STOP";
			} else if (code == PERGOLA_CODE_CLOSE) {
				name = "CLOSE";
			}
			Serial.printf(" (this pergola: %s)", name);
		}
	}
	Serial.println();
}

static void printFrame(const PulseFrame *f, float rssi) {
	Serial.printf("F,%u,%u,%.1f,%u,%u,%u", f->seq, f->startMillis, rssi,
	              f->firstLevel, f->truncated ? 1u : 0u, f->count);
	for (uint16_t i = 0; i < f->count; i++) {
		Serial.print(',');
		Serial.print(f->durations[i]);
	}
	Serial.println();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static void printHelp() {
	Serial.println(F("# commands:"));
	Serial.println(F("#   ?                    this help"));
	Serial.println(F("#   status               chip + capture state"));
	Serial.println(F("#   reg                  dump every configuration register"));
	Serial.println(F("#   rssi [ms]            watch RSSI (default 5000 ms, any key stops)"));
	Serial.println(F("#   scan [lo hi kHz ms]  sweep RSSI (default 433.0 434.8 50 40)"));
	Serial.println(F("#   freq <MHz>           set carrier, e.g. freq 433.42"));
	Serial.println(F("#   rate <baud>          set data rate, e.g. rate 2400"));
	Serial.println(F("#   bw <kHz>             set channel filter, e.g. bw 102"));
	Serial.println(F("#   gap <us>             silence that ends a frame"));
	Serial.println(F("#   glitch <us>          pulses shorter than this are spikes"));
	Serial.println(F("#   minp <n>             minimum pulses for a frame to count"));
	Serial.println(F("#   power <hex>          PATABLE[1], e.g. power C0 (+10dBm) or 60 (0dBm)"));
	Serial.println(F("#   w <addr> <val>       write a register, hex"));
	Serial.println(F("#   x <addr>             read a config register, hex"));
	Serial.println(F("#   xs <addr>            read a status register, hex"));
	Serial.println(F("#   out on|off           frame printing"));
	Serial.println(F("#   open | stop | close  send this pergola's own command"));
	Serial.println(F("#   forge <slot> <hex> [bits] [alpha_us]"));
	Serial.println(F("#                        synthesise an EV1527 word (default 24 bits, 351 us)"));
	Serial.println(F("#   keep <slot>          store the last frame in slot 0-3"));
	Serial.println(F("#   slots                list stored frames"));
	Serial.println(F("#   tx <slot> <rep> [ms] replay a slot (rep 1-20, gap default 20 ms)"));
	Serial.println(F("#   defaults             re-apply the default configuration"));
	Serial.println(F("#   zero                 reset the counters"));
}

static void printStatus() {
	uint8_t v = radio.version();
	Serial.printf("# chip: VERSION=0x%02X PARTNUM=0x%02X MARCSTATE=0x%02X PKTSTATUS=0x%02X%s\n",
	              v, radio.readStatusReg(CC_PARTNUM), radio.marcState(),
	              radio.pktStatus(),
	              (v == CC_VERSION_EXPECTED || v == CC_VERSION_LEGACY)
	                  ? ""
	                  : "   <-- BAD, check wiring and 3V3");
	Serial.printf("# radio: %.3f MHz  %u baud  BW %u kHz  PATABLE[1]=0x%02X  RSSI %.1f dBm\n",
	              radio.frequencyMHz(), radio.dataRateBaud(), radio.channelBwKhz(),
	              radio.paLevel(), radio.rssiDbm());
	Serial.printf("# capture: gap=%uus glitch=%uus minp=%u out=%s\n", sniffer.gapUs,
	              sniffer.glitchUs, sniffer.minPulses, frameOutput ? "on" : "off");
	Serial.printf("# counters: frames=%u rejected=%u droppedEdges=%u\n",
	              sniffer.emittedFrames(), sniffer.rejectedFrames(),
	              sniffer.droppedEdges());
}

static void dumpRegisters() {
	for (uint8_t a = 0; a < CC_CONFIG_REG_COUNT; a++) {
		Serial.printf("# 0x%02X %-9s 0x%02X\n", a, REG_NAMES[a], radio.readReg(a));
	}
	Serial.printf("# --- PARTNUM 0x%02X VERSION 0x%02X MARCSTATE 0x%02X PKTSTATUS 0x%02X LQI 0x%02X\n",
	              radio.readStatusReg(CC_PARTNUM), radio.readStatusReg(CC_VERSION),
	              radio.marcState(), radio.pktStatus(),
	              radio.readStatusReg(CC_LQI));
}

static void watchRssi(uint32_t ms) {
	Serial.printf("# watching RSSI for %u ms -- press a remote button now\n", ms);
	uint32_t start = millis();
	while (millis() - start < ms) {
		Serial.printf("# rssi %.1f dBm  marc=0x%02X\n", radio.rssiDbm(), radio.marcState());
		delay(200);
		if (Serial.available()) {
			break;
		}
	}
	Serial.println(F("# done"));
}

static void doScan(float lo, float hi, float stepKhz, uint32_t dwellMs) {
	if (lo < 387.0f) lo = 387.0f;
	if (hi > 464.0f) hi = 464.0f;
	if (stepKhz < 5.0f) stepKhz = 5.0f;
	if (dwellMs < 5) dwellMs = 5;
	if (dwellMs > 500) dwellMs = 500;

	float original = radio.frequencyMHz();
	sniffer.disable();

	Serial.printf("# scan %.3f-%.3f MHz, %.0f kHz steps, %u ms dwell\n", lo, hi,
	              stepKhz, dwellMs);
	Serial.println(F("# hold the remote button down for the whole sweep; run it"));
	Serial.println(F("# once with the remote silent to get a noise floor"));

	for (float f = lo; f <= hi + 1e-4f; f += stepKhz / 1000.0f) {
		float actual = radio.setFrequencyMHz(f);
		delay(3);  // RSSI takes a moment to become valid after entering RX [p.44]

		float peak = -200.0f;
		uint32_t start = millis();
		while (millis() - start < dwellMs) {
			float r = radio.rssiDbm();
			if (r > peak) {
				peak = r;
			}
			delay(1);
		}
		Serial.printf("S,%.3f,%.1f\n", actual, peak);

		if (Serial.available()) {
			Serial.println(F("# scan aborted"));
			break;
		}
	}

	radio.setFrequencyMHz(original);
	sniffer.enable();
	Serial.printf("# scan done, back to %.3f MHz\n", radio.frequencyMHz());
}

static void keepFrame(uint8_t slot) {
	if (slot >= SLOT_COUNT) {
		Serial.printf("# keep: slot must be 0-%u\n", SLOT_COUNT - 1);
		return;
	}
	if (!haveLastFrame) {
		Serial.println(F("# keep: no frame captured yet"));
		return;
	}
	slots[slot].used = true;
	slots[slot].count = lastFrame.count;
	slots[slot].firstLevel = lastFrame.firstLevel;
	memcpy(slots[slot].durations, lastFrame.durations,
	       sizeof(uint32_t) * lastFrame.count);
	Serial.printf("# slot %u <- frame %u (%u pulses)\n", slot, lastFrame.seq,
	              lastFrame.count);
}

static void listSlots() {
	for (uint8_t s = 0; s < SLOT_COUNT; s++) {
		if (!slots[s].used) {
			Serial.printf("# slot %u: empty\n", s);
			continue;
		}
		uint32_t total = 0;
		for (uint16_t i = 0; i < slots[s].count; i++) {
			total += slots[s].durations[i];
		}
		Serial.printf("# slot %u: %u pulses, %.1f ms, first level %u\n", s,
		              slots[s].count, total / 1000.0f, slots[s].firstLevel);
	}
}

// Synthesise a canonical EV1527 word into a slot, rather than replaying a
// capture. Two reasons this is not just a convenience:
//
//   1. A captured frame is segmented ON the sync low, so it holds
//      [24 data bits][sync high] and loses the 31-alpha sync low entirely.
//      Replaying it puts the sync in the wrong place and substitutes the tx
//      gap for it. A forged frame emits sync-then-data, as the encoder does.
//   2. Captured pulses carry the CC1101's +/-1/8-bit sampling jitter and any
//      glitch-merged edges -- widths of 675 or 1393 us have been observed,
//      which are neither alpha nor 3*alpha. A decoder that width-checks every
//      pulse rejects the whole word. Forged pulses are exact.
//
// Layout, alpha = the short pulse in microseconds:
//   sync:   alpha high, 31*alpha low
//   bit 1:  3*alpha high, alpha low
//   bit 0:  alpha high, 3*alpha low
static void forgeSlot(uint8_t slot, uint32_t code, uint8_t bitCount, uint32_t alpha) {
	if (slot >= SLOT_COUNT) {
		Serial.println(F("# forge: slot must be 0-3"));
		return;
	}
	ReplaySlot &s = slots[slot];
	const uint16_t n = ev1527BuildWord(code, bitCount, alpha, s.durations,
	                                   SNIFFER_MAX_PULSES);
	if (n == 0) {
		Serial.println(F("# forge: bad arguments (bits 1-32, alpha 50-20000 us)"));
		return;
	}
	s.count = n;
	// firstLevel 1 = carrier on for durations[0]. Verified on hardware, not assumed:
	// a forged non-inverted frame moved the roof. See docs/remote-protocol.md.
	s.firstLevel = 1;
	s.used = true;

	uint32_t total = 0;
	for (uint16_t i = 0; i < n; i++) {
		total += s.durations[i];
	}
	Serial.printf("# forge: slot %u <- 0x%0*lX, %u bits, alpha %lu us, %u pulses,"
	              " %.1f ms\n",
	              slot, (bitCount + 3) / 4, static_cast<unsigned long>(code),
	              bitCount, static_cast<unsigned long>(alpha), n, total / 1000.0f);
}

static void transmitSlot(uint8_t slot, uint8_t repeats, uint16_t gapMs) {
	if (slot >= SLOT_COUNT || !slots[slot].used) {
		Serial.println(F("# tx: empty or invalid slot"));
		return;
	}
	if (repeats < 1) repeats = 1;
	if (repeats > 20) repeats = 20;
	if (gapMs < 1) gapMs = 1;
	if (gapMs > 1000) gapMs = 1000;

	const ReplaySlot &s = slots[slot];

	// Our own edges would otherwise pour straight back into the capture ring.
	sniffer.disable();

	if (!radio.beginTransmitRaw()) {
		Serial.println(F("# tx: chip would not enter TX -- check power and antenna"));
		sniffer.enable();
		return;
	}

	for (uint8_t r = 0; r < repeats; r++) {
		uint8_t level = s.firstLevel;
		for (uint16_t i = 0; i < s.count; i++) {
			digitalWrite(PIN_GDO0, level ? HIGH : LOW);
			delayMicroseconds(s.durations[i]);
			level = level ? 0 : 1;
		}
		digitalWrite(PIN_GDO0, LOW);
		if (r + 1 < repeats) {
			delay(gapMs);  // also yields, which keeps the watchdog happy
		}
	}

	radio.endTransmitRaw();
	sniffer.enable();

	Serial.printf("# tx: slot %u x%u sent. EN 300 220 caps 433 MHz at 10%% duty cycle --\n",
	              slot, repeats);
	Serial.println(F("# tx: keep bursts short, do not hold a carrier."));
}

// Slot 3 is the scratch slot for the named commands below. Anything a user has
// stored there is overwritten by `open`/`stop`/`close`.
static constexpr uint8_t NAMED_COMMAND_SLOT = 3;

// One-shot: forge this pergola's code and send it with the remote's own repeat
// count and inter-word gap. This is the path the MQTT daemon uses too.
static void sendNamed(const char *name, uint32_t code) {
	forgeSlot(NAMED_COMMAND_SLOT, code, PERGOLA_CODE_BITS, PERGOLA_ALPHA_US);
	if (!slots[NAMED_COMMAND_SLOT].used) {
		return;
	}
	Serial.printf("# %s: sending 0x%06lX\n", name, static_cast<unsigned long>(code));
	transmitSlot(NAMED_COMMAND_SLOT, PERGOLA_TX_REPEATS, PERGOLA_TX_GAP_MS);
}

static void execute(char *line) {
	char *cmd = strtok(line, " \t");
	if (!cmd) {
		return;
	}
	char *a1 = strtok(nullptr, " \t");
	char *a2 = strtok(nullptr, " \t");
	char *a3 = strtok(nullptr, " \t");
	char *a4 = strtok(nullptr, " \t");

	if (!strcmp(cmd, "?") || !strcmp(cmd, "help")) {
		printHelp();
	} else if (!strcmp(cmd, "status") || !strcmp(cmd, "st")) {
		printStatus();
	} else if (!strcmp(cmd, "reg")) {
		dumpRegisters();
	} else if (!strcmp(cmd, "rssi")) {
		watchRssi(a1 ? strtoul(a1, nullptr, 10) : 5000);
	} else if (!strcmp(cmd, "scan")) {
		doScan(a1 ? atof(a1) : 433.0f, a2 ? atof(a2) : 434.8f,
		       a3 ? atof(a3) : 50.0f, a4 ? strtoul(a4, nullptr, 10) : 40);
	} else if (!strcmp(cmd, "freq") && a1) {
		Serial.printf("# frequency now %.3f MHz\n", radio.setFrequencyMHz(atof(a1)));
	} else if (!strcmp(cmd, "rate") && a1) {
		Serial.printf("# data rate now %u baud\n",
		              radio.setDataRateBaud(strtoul(a1, nullptr, 10)));
	} else if (!strcmp(cmd, "bw") && a1) {
		Serial.printf("# channel bandwidth now %u kHz\n",
		              radio.setChannelBwKhz(strtoul(a1, nullptr, 10)));
	} else if (!strcmp(cmd, "open")) {
		sendNamed("open", PERGOLA_CODE_OPEN);
	} else if (!strcmp(cmd, "stop")) {
		sendNamed("stop", PERGOLA_CODE_STOP);
	} else if (!strcmp(cmd, "close")) {
		sendNamed("close", PERGOLA_CODE_CLOSE);
	} else if (!strcmp(cmd, "forge") && a1 && a2) {
		forgeSlot(static_cast<uint8_t>(strtoul(a1, nullptr, 10)),
		          strtoul(a2, nullptr, 16),
		          a3 ? static_cast<uint8_t>(strtoul(a3, nullptr, 10)) : PERGOLA_CODE_BITS,
		          a4 ? strtoul(a4, nullptr, 10) : PERGOLA_ALPHA_US);
	} else if (!strcmp(cmd, "gap") && a1) {
		sniffer.gapUs = strtoul(a1, nullptr, 10);
		Serial.printf("# gap now %u us\n", sniffer.gapUs);
	} else if (!strcmp(cmd, "glitch") && a1) {
		sniffer.glitchUs = strtoul(a1, nullptr, 10);
		Serial.printf("# glitch now %u us\n", sniffer.glitchUs);
	} else if (!strcmp(cmd, "minp") && a1) {
		sniffer.minPulses = static_cast<uint16_t>(strtoul(a1, nullptr, 10));
		Serial.printf("# minp now %u\n", sniffer.minPulses);
	} else if (!strcmp(cmd, "power") && a1) {
		uint8_t v = static_cast<uint8_t>(strtoul(a1, nullptr, 16));
		if (v >= 0x61 && v <= 0x6F) {
			Serial.println(F("# power: 0x61-0x6F are not allowed PA settings [p.59]"));
		} else {
			radio.setPaLevel(v);
			Serial.printf("# PATABLE[1] now 0x%02X\n", radio.paLevel());
		}
	} else if (!strcmp(cmd, "w") && a1 && a2) {
		uint8_t addr = static_cast<uint8_t>(strtoul(a1, nullptr, 16));
		uint8_t val = static_cast<uint8_t>(strtoul(a2, nullptr, 16));
		if (addr >= CC_CONFIG_REG_COUNT) {
			Serial.println(F("# w: address must be 0x00-0x2E"));
		} else {
			radio.writeReg(addr, val);
			Serial.printf("# 0x%02X %s <- 0x%02X (read back 0x%02X)\n", addr,
			              REG_NAMES[addr], val, radio.readReg(addr));
		}
	} else if (!strcmp(cmd, "x") && a1) {
		uint8_t addr = static_cast<uint8_t>(strtoul(a1, nullptr, 16));
		if (addr >= CC_CONFIG_REG_COUNT) {
			Serial.println(F("# x: address must be 0x00-0x2E, use xs for status"));
		} else {
			Serial.printf("# 0x%02X %s = 0x%02X\n", addr, REG_NAMES[addr],
			              radio.readReg(addr));
		}
	} else if (!strcmp(cmd, "xs") && a1) {
		uint8_t addr = static_cast<uint8_t>(strtoul(a1, nullptr, 16));
		Serial.printf("# status 0x%02X = 0x%02X\n", addr, radio.readStatusReg(addr));
	} else if (!strcmp(cmd, "out") && a1) {
		frameOutput = !strcmp(a1, "on");
		Serial.printf("# frame output %s\n", frameOutput ? "on" : "off");
	} else if (!strcmp(cmd, "keep") && a1) {
		keepFrame(static_cast<uint8_t>(strtoul(a1, nullptr, 10)));
	} else if (!strcmp(cmd, "slots")) {
		listSlots();
	} else if (!strcmp(cmd, "tx") && a1) {
		transmitSlot(static_cast<uint8_t>(strtoul(a1, nullptr, 10)),
		             a2 ? static_cast<uint8_t>(strtoul(a2, nullptr, 10)) : 4,
		             a3 ? static_cast<uint16_t>(strtoul(a3, nullptr, 10)) : 20);
	} else if (!strcmp(cmd, "defaults")) {
		radio.applyDefaultConfig();
		sniffer.gapUs = CAPTURE_GAP_US_DEFAULT;
		sniffer.glitchUs = CAPTURE_GLITCH_US_DEFAULT;
		sniffer.minPulses = CAPTURE_MIN_PULSES_DEFAULT;
		Serial.println(F("# default configuration re-applied"));
		printStatus();
	} else if (!strcmp(cmd, "zero")) {
		sniffer.resetCounters();
		Serial.println(F("# counters reset"));
	} else {
		Serial.printf("# unknown command '%s' -- type ? for help\n", cmd);
	}
}

static void pollSerial() {
	static char buf[96];
	static uint8_t len = 0;

	while (Serial.available()) {
		char c = static_cast<char>(Serial.read());
		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			buf[len] = '\0';
			if (len > 0) {
				execute(buf);
			}
			len = 0;
			continue;
		}
		if (len < sizeof(buf) - 1) {
			buf[len++] = c;
		}
	}
}

// ---------------------------------------------------------------------------

void setup() {
	Serial.begin(115200);
	delay(300);

	Serial.println();
	Serial.printf("# pergola-to-mqtt sniffer %s\n", FIRMWARE_VERSION);

	bool ok = radio.begin();
	sniffer.begin(PIN_GDO0);
	sniffer.enable();

	printStatus();
	if (!ok) {
		Serial.println(F("# ---------------------------------------------------------"));
		Serial.println(F("# CC1101 did not answer with a plausible VERSION."));
		Serial.println(F("# Nothing below this point will work. Check, in order:"));
		Serial.println(F("#   VCC on 3V3 (never 5V), GND, then MISO/MOSI/SCLK/CSn."));
		Serial.println(F("# See docs/hardware.md and docs/setup-checklist.md."));
		Serial.println(F("# ---------------------------------------------------------"));
	} else {
		Serial.println(F("# ready -- press a remote button, or type ? for commands"));
	}
}

void loop() {
	pollSerial();

	uint32_t now = millis();
	if (now - rssiSampledAt >= RSSI_SAMPLE_MS) {
		rssiSampledAt = now;
		float r = radio.rssiDbm();
		if (r > rssiPeak || now - rssiPeakAt > RSSI_PEAK_HOLD_MS) {
			rssiPeak = r;
			rssiPeakAt = now;
		}
	}

	const PulseFrame *f = sniffer.poll();
	if (f) {
		lastFrame.seq = f->seq;
		lastFrame.startMillis = f->startMillis;
		lastFrame.count = f->count;
		lastFrame.firstLevel = f->firstLevel;
		lastFrame.truncated = f->truncated;
		memcpy(lastFrame.durations, f->durations, sizeof(uint32_t) * f->count);
		haveLastFrame = true;

		if (frameOutput) {
			printFrame(f, rssiPeak);
			summariseFrame(f, rssiPeak);
		}
	}
}
