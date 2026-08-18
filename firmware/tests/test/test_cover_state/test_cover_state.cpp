// Unit tests for CoverState_t. Host only; see ../platformio.ini for why.
#include <unity.h>

#include "cover_state.h"

// The queue hands back codes with a not-before time, so draining it means asking
// repeatedly as the clock advances. Returns how many codes came out, and what.
static uint16_t drain(CoverState_t &cover, uint32_t fromMs, uint32_t toMs,
                      uint32_t *codes, uint32_t *at, uint16_t max) {
	uint16_t n = 0;
	for (uint32_t t = fromMs; t <= toMs; t += 10) {
		cover.tick(t);
		uint32_t code = 0;
		while (n < max && cover.nextTx(t, &code)) {
			codes[n] = code;
			at[n] = t;
			n++;
		}
	}
	return n;
}

// The bug that reached the roof. Believing the roof was already fully open made
// travelMsFor(100, 100) return 0, so the mandatory stop was scheduled 500 ms after
// the open instead of after a full travel. The roof moved for half a second and
// stopped, which presented as the pergola not responding at all.
static void test_open_gets_full_travel_even_when_believed_open(void) {
	CoverState_t cover;
	cover.begin(100);  // a stale belief: says open, roof is really closed

	uint32_t codes[8] = {0}, at[8] = {0};
	cover.commandOpen(1000);
	const uint16_t n = drain(cover, 1000, 20000, codes, at, 8);

	TEST_ASSERT_EQUAL_UINT16(2, n);
	TEST_ASSERT_EQUAL_HEX32(PERGOLA_CODE_OPEN, codes[0]);
	TEST_ASSERT_EQUAL_HEX32(PERGOLA_CODE_STOP, codes[1]);
	// The whole point: the gap must cover a real full open, not a computed zero.
	TEST_ASSERT_GREATER_OR_EQUAL_UINT32(PERGOLA_TRAVEL_OPEN_MS, at[1] - at[0]);
}

static void test_close_gets_full_travel_even_when_believed_closed(void) {
	CoverState_t cover;
	cover.begin(0);  // believes closed; the roof may be anywhere

	uint32_t codes[8] = {0}, at[8] = {0};
	cover.commandClose(1000);
	const uint16_t n = drain(cover, 1000, 20000, codes, at, 8);

	TEST_ASSERT_EQUAL_UINT16(2, n);
	TEST_ASSERT_EQUAL_HEX32(PERGOLA_CODE_CLOSE, codes[0]);
	TEST_ASSERT_EQUAL_HEX32(PERGOLA_CODE_STOP, codes[1]);
	TEST_ASSERT_GREATER_OR_EQUAL_UINT32(PERGOLA_TRAVEL_CLOSE_MS, at[1] - at[0]);
}

// The rule the whole project is built around.
static void test_every_open_is_followed_by_a_stop(void) {
	CoverState_t cover;
	cover.begin(0);
	uint32_t codes[8] = {0}, at[8] = {0};
	cover.commandOpen(500);
	const uint16_t n = drain(cover, 500, 20000, codes, at, 8);
	TEST_ASSERT_EQUAL_UINT16(2, n);
	TEST_ASSERT_EQUAL_HEX32(PERGOLA_CODE_OPEN, codes[0]);
	TEST_ASSERT_EQUAL_HEX32(PERGOLA_CODE_STOP, codes[1]);
}

// movePending() is what the daemon mirrors into NVS, so it has to be true for the
// whole window in which a stop is outstanding and false once it is not.
static void test_move_pending_spans_the_whole_open(void) {
	CoverState_t cover;
	cover.begin(0);
	TEST_ASSERT_FALSE(cover.movePending());
	cover.commandOpen(1000);
	TEST_ASSERT_TRUE(cover.movePending());
	cover.tick(1000 + PERGOLA_TRAVEL_OPEN_MS / 2);
	TEST_ASSERT_TRUE(cover.movePending());  // still owed half way through
	cover.tick(1000 + PERGOLA_TRAVEL_OPEN_MS + PERGOLA_AUTOSTOP_MARGIN_MS + 50);
	TEST_ASSERT_FALSE(cover.movePending());
}

// A partial move should still interpolate rather than always running full travel.
static void test_partial_move_uses_interpolated_travel(void) {
	CoverState_t cover;
	cover.begin(0);
	uint32_t codes[8] = {0}, at[8] = {0};
	cover.commandSetPosition(1000, 50);
	const uint16_t n = drain(cover, 1000, 20000, codes, at, 8);
	TEST_ASSERT_EQUAL_UINT16(2, n);
	const uint32_t gap = at[1] - at[0];
	TEST_ASSERT_LESS_THAN_UINT32(PERGOLA_TRAVEL_OPEN_MS, gap);
	TEST_ASSERT_GREATER_THAN_UINT32(PERGOLA_TRAVEL_OPEN_MS / 4, gap);
}

// The light bar trick: a close with no auto-stop, so it cannot switch itself off.
static void test_light_on_schedules_no_stop(void) {
	CoverState_t cover;
	cover.begin(0);
	cover.commandLightOn(1000);
	TEST_ASSERT_FALSE(cover.movePending());
	uint32_t codes[8] = {0}, at[8] = {0};
	const uint16_t n = drain(cover, 1000, 20000, codes, at, 8);
	TEST_ASSERT_EQUAL_UINT16(1, n);
	TEST_ASSERT_EQUAL_HEX32(PERGOLA_CODE_CLOSE, codes[0]);
	TEST_ASSERT_TRUE(cover.lightOn());
}

int main(int, char **) {
	UNITY_BEGIN();
	RUN_TEST(test_open_gets_full_travel_even_when_believed_open);
	RUN_TEST(test_close_gets_full_travel_even_when_believed_closed);
	RUN_TEST(test_every_open_is_followed_by_a_stop);
	RUN_TEST(test_move_pending_spans_the_whole_open);
	RUN_TEST(test_partial_move_uses_interpolated_travel);
	RUN_TEST(test_light_on_schedules_no_stop);
	return UNITY_END();
}
