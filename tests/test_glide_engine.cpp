#include "glide_engine.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

// ─── Minimal test harness ─────────────────────────────────────────────────────

static int g_tests_passed = 0;

#define TEST(name)                                                        \
	static void test_##name();                                            \
	static bool test_reg_##name = (register_test(#name, test_##name), true); \
	static void test_##name()

struct TestEntry {
	const char *name;
	void (*fn)();
};

static TestEntry g_tests[256];
static int       g_test_count = 0;

static void register_test(const char *name, void (*fn)()) {
	g_tests[g_test_count++] = { name, fn };
}

#define ASSERT_EQ(a, b) do {                                              \
	if ((a) != (b)) {                                                     \
		std::fprintf(stderr, "  FAIL: %s:%d: %s != %s (%d vs %d)\n",     \
			__FILE__, __LINE__, #a, #b, (int)(a), (int)(b));              \
		assert(false);                                                    \
	}                                                                     \
} while (0)

#define ASSERT_NEAR(a, b, eps) do {                                       \
	if (std::fabs((double)(a) - (double)(b)) > (eps)) {                   \
		std::fprintf(stderr, "  FAIL: %s:%d: %s ≈ %s (%.6f vs %.6f)\n",  \
			__FILE__, __LINE__, #a, #b, (double)(a), (double)(b));        \
		assert(false);                                                    \
	}                                                                     \
} while (0)

#define ASSERT_TRUE(x) do {                                               \
	if (!(x)) {                                                           \
		std::fprintf(stderr, "  FAIL: %s:%d: %s\n",                      \
			__FILE__, __LINE__, #x);                                      \
		assert(false);                                                    \
	}                                                                     \
} while (0)

// ─── Helpers ──────────────────────────────────────────────────────────────────

static constexpr int BEND_CENTER = 8192;
static constexpr double TEST_SR  = 44100.0;

// Create a default-configured GlideEngine with a known sample rate.
static GlideEngine make_engine(
	GlideMode mode      = GlideMode::LEGATO,
	double glide_ms     = 80.0,
	double bend_range   = 12.0,
	CurveShape curve    = CurveShape::LINEAR)
{
	GlideEngine e;
	e.glide_mode    = mode;
	e.glide_time_ms = glide_ms;
	e.bend_range    = bend_range;
	e.curve         = curve;
	e.activate(TEST_SR);
	return e;
}

// Advance N samples and return the final bend value.
static int advance_n(GlideEngine &e, int n) {
	int bend = BEND_CENTER;
	bool settled;
	for (int i = 0; i < n; i++) {
		bend = e.advance(&settled);
	}
	return bend;
}

// Advance until settled (or max iterations) and return the final bend.
static int advance_until_settled(GlideEngine &e, int max_samples = 500000) {
	bool settled = false;
	int bend = BEND_CENTER;
	for (int i = 0; i < max_samples && !settled; i++) {
		bend = e.advance(&settled);
	}
	return bend;
}

// Simulate what sync_params_to_engine does: map raw CLAP param doubles
// into engine fields.
static void sync_params(GlideEngine &e, double glide_time, double mode,
                         double curve, double bend_range) {
	e.glide_time_ms = glide_time;
	e.glide_mode    = static_cast<GlideMode>(static_cast<int>(mode + 0.5));
	e.curve         = static_cast<CurveShape>(static_cast<int>(curve + 0.5));
	e.bend_range    = bend_range;
	e.update_glide_samples();
}

// ─── Tests ────────────────────────────────────────────────────────────────────

// ---------- Idle / default state ----------

TEST(idle_returns_center_bend) {
	GlideEngine e = make_engine();
	// No notes; advance should return center.
	bool settled;
	int bend = e.advance(&settled);
	ASSERT_EQ(bend, BEND_CENTER);
	ASSERT_TRUE(!settled);
}

// ---------- Single note-on (no glide possible) ----------

TEST(single_note_on_no_glide) {
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0);
	e.note_on(60, 0);
	// ALWAYS mode enters GLIDING even on the first note (src==dst==60),
	// but the pitch bend output should still be dead center because
	// there is no actual pitch difference.
	bool settled;
	int bend = e.advance(&settled);
	ASSERT_EQ(bend, BEND_CENTER);
}

TEST(single_note_legato_no_glide) {
	GlideEngine e = make_engine(GlideMode::LEGATO, 100.0);
	e.note_on(72, 0);
	bool settled;
	int bend = e.advance(&settled);
	ASSERT_EQ(bend, BEND_CENTER);
}

// ---------- Glide direction and range ----------

TEST(glide_always_second_note_starts_below_center) {
	// Note 60 → 72 with bend_range=12.  At t=0 the engine src=60, dst=72,
	// so delta = 60 - 72 = –12 semitones, norm = –1.0 → bend ≈ 0.
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0);
	e.note_on(60, 0);
	advance_until_settled(e);
	e.note_on(72, 0);

	bool settled;
	int bend = e.advance(&settled);
	// First sample: very close to the bottom of the range.
	ASSERT_TRUE(bend < BEND_CENTER);
	ASSERT_TRUE(bend >= 0);
}

TEST(glide_always_second_note_starts_above_center) {
	// Note 72 → 60 with bend_range=12.  delta = 72 - 60 = +12, norm = +1.
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0);
	e.note_on(72, 0);
	// Let first note settle so current_pitch reaches 72.
	advance_until_settled(e);
	ASSERT_NEAR(e.current_pitch, 72.0, 0.001);

	e.note_on(60, 0);

	bool settled;
	int bend = e.advance(&settled);
	ASSERT_TRUE(bend > BEND_CENTER);
	ASSERT_TRUE(bend <= 16383);
}

// ---------- Glide settles to center ----------

TEST(glide_settles_to_center) {
	GlideEngine e = make_engine(GlideMode::ALWAYS, 10.0, 12.0);
	e.note_on(60, 0);
	advance_until_settled(e);
	e.note_on(72, 0);

	int bend = advance_until_settled(e);
	ASSERT_EQ(bend, BEND_CENTER);
}

// ---------- Glide with zero time → instant jump ----------

TEST(zero_glide_time_instant_jump) {
	GlideEngine e = make_engine(GlideMode::ALWAYS, 0.0, 12.0);
	e.note_on(60, 0);
	e.note_on(72, 0);
	// With glide_time_ms == 0, we should instantly be at dst_pitch.
	ASSERT_NEAR(e.current_pitch, 72.0, 0.001);

	bool settled;
	int bend = e.advance(&settled);
	ASSERT_EQ(bend, BEND_CENTER);
}

// ---------- LEGATO mode requires held note ----------

TEST(legato_no_glide_from_idle) {
	GlideEngine e = make_engine(GlideMode::LEGATO, 100.0, 12.0);
	// First note — no prior held note, should jump.
	e.note_on(60, 0);
	ASSERT_EQ(e.state, GlideState::HELD);

	// Release, then press a second note — state went through IDLE,
	// so LEGATO should NOT glide.
	e.note_off(60, 0);
	ASSERT_EQ(e.state, GlideState::IDLE);

	e.note_on(72, 0);
	ASSERT_EQ(e.state, GlideState::HELD); // jumped, not gliding
	ASSERT_NEAR(e.current_pitch, 72.0, 0.001);
}

TEST(legato_glides_when_held) {
	GlideEngine e = make_engine(GlideMode::LEGATO, 100.0, 12.0);
	e.note_on(60, 0);
	ASSERT_EQ(e.state, GlideState::HELD);

	// Second note while first is still held → LEGATO glide.
	e.note_on(72, 0);
	ASSERT_EQ(e.state, GlideState::GLIDING);
}

// ---------- ALWAYS mode glides even from first pair ----------

TEST(always_mode_glides_from_held) {
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0);
	e.note_on(60, 0);
	// Release, then press a second note — ALWAYS should glide.
	e.note_off(60, 0);
	// State is IDLE after release, but ALWAYS mode should still glide
	// — wait, ALWAYS checks state for HELD/GLIDING too? Let me re-read.
	// Actually looking at note_on: ALWAYS mode only checks glide_time > 0.
	// No, re-reading: do_glide = (glide_time > 0) && (mode==ALWAYS || (mode==LEGATO && state==HELD/GLIDING))
	// So ALWAYS always glides as long as glide_time > 0.
	e.note_on(72, 0);
	ASSERT_EQ(e.state, GlideState::GLIDING);
}

// ---------- Bend range scaling ----------

TEST(bend_range_scales_output) {
	// With bend_range=24, a 12-semitone glide should only use half the range.
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 24.0);
	e.note_on(60, 0);
	advance_until_settled(e);
	e.note_on(72, 0);

	// At sample 0: delta = 60 - 72 = -12, norm = -12/24 = -0.5
	// bend = int(-0.5 * 8191) + 8192 ≈ 4097
	bool settled;
	int bend = e.advance(&settled);
	// After 1 sample of glide the pitch will have moved very slightly toward 72,
	// so delta is slightly more than -12.  But it should be close.
	ASSERT_TRUE(bend > 0);
	ASSERT_TRUE(bend < BEND_CENTER);
	// Should be roughly halfway between 0 and center (≈4096).
	ASSERT_TRUE(bend > 3000);
	ASSERT_TRUE(bend < 5000);
}

TEST(bend_clamps_to_range) {
	// Interval bigger than bend_range → clamp to -1..+1.
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 2.0);
	e.note_on(60, 0);
	advance_until_settled(e);
	e.note_on(72, 0); // 12 semitones, but range is only 2

	bool settled;
	int bend = e.advance(&settled);
	// Clamped to min: norm = -1.0 → bend = 1
	ASSERT_TRUE(bend <= 1);
}

// ---------- Curve shapes ----------

TEST(exponential_curve_slower_start) {
	// Exponential (ease-in) should be behind linear at t=0.5.
	int half_samples = static_cast<int>(100.0 * TEST_SR / 1000.0 / 2.0);

	GlideEngine lin = make_engine(GlideMode::ALWAYS, 100.0, 12.0, CurveShape::LINEAR);
	lin.note_on(60, 0);
	advance_until_settled(lin);
	lin.note_on(72, 0);
	int bend_lin = advance_n(lin, half_samples);

	GlideEngine exp = make_engine(GlideMode::ALWAYS, 100.0, 12.0, CurveShape::EXPONENTIAL);
	exp.note_on(60, 0);
	advance_until_settled(exp);
	exp.note_on(72, 0);
	int bend_exp = advance_n(exp, half_samples);

	// Both are gliding upward from 60→72 so bend starts below center and
	// rises.  Exponential should be further from center (lower value) at
	// the halfway point because it starts slower.
	ASSERT_TRUE(bend_exp < bend_lin);
}

TEST(sigmoid_curve_symmetric) {
	// Sigmoid should be near the middle at t=0.5 — close to linear there.
	int half_samples = static_cast<int>(100.0 * TEST_SR / 1000.0 / 2.0);

	GlideEngine lin = make_engine(GlideMode::ALWAYS, 100.0, 12.0, CurveShape::LINEAR);
	lin.note_on(60, 0);
	advance_until_settled(lin);
	lin.note_on(72, 0);
	int bend_lin = advance_n(lin, half_samples);

	GlideEngine sig = make_engine(GlideMode::ALWAYS, 100.0, 12.0, CurveShape::SIGMOID);
	sig.note_on(60, 0);
	advance_until_settled(sig);
	sig.note_on(72, 0);
	int bend_sig = advance_n(sig, half_samples);

	// Sigmoid at t=0.5 passes through ~0.5 (center of S-curve), so it
	// should be fairly close to linear at the midpoint.
	int diff = std::abs(bend_sig - bend_lin);
	ASSERT_TRUE(diff < 500);
}

// ---------- note_off ----------

TEST(note_off_returns_to_idle) {
	GlideEngine e = make_engine();
	e.note_on(60, 0);
	ASSERT_EQ(e.state, GlideState::HELD);
	e.note_off(60, 0);
	ASSERT_EQ(e.state, GlideState::IDLE);
	ASSERT_EQ(e.note_count, 0);
}

TEST(note_off_preserves_pitch_during_release) {
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0);
	e.note_on(60, 0);
	advance_until_settled(e);
	e.note_on(72, 0);
	// Advance partway through glide.
	advance_n(e, 100);
	double pitch_before = e.current_pitch;

	e.note_off(72, 0);
	e.note_off(60, 0);
	// Pitch should NOT be reset — it stays where it was for the release tail.
	ASSERT_NEAR(e.current_pitch, pitch_before, 0.001);
}

TEST(note_off_unknown_key_returns_false) {
	GlideEngine e = make_engine();
	e.note_on(60, 0);
	bool result = e.note_off(61, 0); // not held
	ASSERT_TRUE(!result);
	ASSERT_EQ(e.note_count, 1);
}

// ---------- pre_target_glide ----------

TEST(pre_target_glide_starts_glide) {
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0);
	e.note_on(60, 0);
	e.pre_target_glide(72, 0);
	ASSERT_EQ(e.state, GlideState::GLIDING);
	ASSERT_NEAR(e.dst_pitch, 72.0, 0.001);
}

TEST(pre_target_glide_nop_when_glide_time_zero) {
	GlideEngine e = make_engine(GlideMode::ALWAYS, 0.0, 12.0);
	e.note_on(60, 0);
	e.pre_target_glide(72, 0);
	// Should still be HELD, no glide started.
	ASSERT_EQ(e.state, GlideState::HELD);
}

TEST(pre_target_legato_nop_from_idle) {
	GlideEngine e = make_engine(GlideMode::LEGATO, 100.0, 12.0);
	// No notes held → IDLE.
	e.pre_target_glide(72, 0);
	ASSERT_EQ(e.state, GlideState::IDLE);
}

// ---------- Mid-glide retarget ----------

TEST(mid_glide_retarget_no_pitch_discontinuity) {
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0);
	e.note_on(48, 0);
	advance_until_settled(e);
	e.note_on(60, 0);

	// Advance part-way.
	advance_n(e, 500);
	double pitch_before = e.current_pitch;

	// Retarget to a third note.
	e.note_on(72, 0);
	// src_pitch should snap to current_pitch — no jump.
	ASSERT_NEAR(e.src_pitch, pitch_before, 0.001);
	ASSERT_EQ(e.state, GlideState::GLIDING);
}

// ---------- Multi-channel ----------

TEST(note_off_resolves_correct_channel) {
	GlideEngine e = make_engine();
	e.note_on(60, 0);
	e.note_on(60, 1);
	ASSERT_EQ(e.note_count, 2);

	int resolved = -1;
	e.note_off(60, 1, &resolved);
	ASSERT_EQ(resolved, 1);
	ASSERT_EQ(e.note_count, 1);

	e.note_off(60, 0, &resolved);
	ASSERT_EQ(resolved, 0);
	ASSERT_EQ(e.note_count, 0);
}

TEST(note_off_fallback_channel_search) {
	GlideEngine e = make_engine();
	e.note_on(60, 3);

	// Request channel -1 (wildcard) — should find channel 3.
	int resolved = -1;
	bool ok = e.note_off(60, -1, &resolved);
	ASSERT_TRUE(ok);
	ASSERT_EQ(resolved, 3);
}

// ---------- sync_params (CLAP param → engine mapping) ----------

TEST(sync_params_maps_glide_mode_always) {
	GlideEngine e = make_engine();
	sync_params(e, 200.0, 0.0, 0.0, 12.0);
	ASSERT_EQ(static_cast<int>(e.glide_mode), static_cast<int>(GlideMode::ALWAYS));
	ASSERT_NEAR(e.glide_time_ms, 200.0, 0.001);
}

TEST(sync_params_maps_glide_mode_legato) {
	GlideEngine e = make_engine();
	sync_params(e, 80.0, 1.0, 0.0, 12.0);
	ASSERT_EQ(static_cast<int>(e.glide_mode), static_cast<int>(GlideMode::LEGATO));
}

TEST(sync_params_maps_curve_shapes) {
	GlideEngine e = make_engine();

	sync_params(e, 80.0, 0.0, 0.0, 12.0);
	ASSERT_EQ(static_cast<int>(e.curve), static_cast<int>(CurveShape::LINEAR));

	sync_params(e, 80.0, 0.0, 1.0, 12.0);
	ASSERT_EQ(static_cast<int>(e.curve), static_cast<int>(CurveShape::EXPONENTIAL));

	sync_params(e, 80.0, 0.0, 2.0, 12.0);
	ASSERT_EQ(static_cast<int>(e.curve), static_cast<int>(CurveShape::SIGMOID));
}

TEST(sync_params_updates_glide_samples) {
	GlideEngine e;
	e.activate(TEST_SR);
	sync_params(e, 100.0, 0.0, 0.0, 12.0);
	double expected = 100.0 * TEST_SR / 1000.0;
	ASSERT_NEAR(e.glide_samples, expected, 0.001);
}

TEST(sync_params_bend_range) {
	GlideEngine e = make_engine();
	sync_params(e, 80.0, 0.0, 0.0, 24.0);
	ASSERT_NEAR(e.bend_range, 24.0, 0.001);
}

// ---------- Output range invariants ----------

TEST(bend_output_always_in_14bit_range) {
	// Run a full glide and verify every sample is in [0, 16383].
	GlideEngine e = make_engine(GlideMode::ALWAYS, 50.0, 2.0, CurveShape::LINEAR);
	e.note_on(30, 0);
	advance_until_settled(e);
	e.note_on(90, 0); // huge interval, will clamp

	bool settled = false;
	for (int i = 0; i < 100000 && !settled; i++) {
		int bend = e.advance(&settled);
		ASSERT_TRUE(bend >= 0);
		ASSERT_TRUE(bend <= 16383);
	}
}

TEST(settled_flag_fires_exactly_once) {
	GlideEngine e = make_engine(GlideMode::ALWAYS, 10.0, 12.0);
	e.note_on(60, 0);
	advance_until_settled(e);
	e.note_on(72, 0);

	int settle_count = 0;
	bool settled;
	for (int i = 0; i < 100000; i++) {
		e.advance(&settled);
		if (settled) settle_count++;
	}
	ASSERT_EQ(settle_count, 1);
}

// ---------- Exact bend value for one-octave glide ----------

TEST(exact_bend_at_glide_start_12st) {
	// 60 → 72, bend_range = 12, linear.
	// After first note settles, current_pitch = 60.  Second note: src=60, dst=72.
	// delta = 60 - 72 = -12, norm = -1 → bend ≈ 1.
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0, CurveShape::LINEAR);
	e.note_on(60, 0);
	advance_until_settled(e);
	e.note_on(72, 0);

	bool settled;
	int bend = e.advance(&settled);
	// Should be very close to 1 (bottom of range).
	ASSERT_TRUE(bend >= 0);
	ASSERT_TRUE(bend < 10);
}

TEST(exact_bend_at_glide_start_downward) {
	// 72 → 60, bend_range = 12.
	// After first note settles at 72, second note: src=72, dst=60.
	// delta = 72 - 60 = +12, norm = +1 → bend = 8191 + 8192 = 16383.
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0, CurveShape::LINEAR);
	e.note_on(72, 0);
	advance_until_settled(e);
	e.note_on(60, 0);

	bool settled;
	int bend = e.advance(&settled);
	ASSERT_TRUE(bend > 16370);
	ASSERT_TRUE(bend <= 16383);
}

// ---------- Key edge cases ----------

TEST(out_of_range_key_ignored) {
	GlideEngine e = make_engine();
	e.note_on(-1, 0);
	ASSERT_EQ(e.note_count, 0);
	e.note_on(128, 0);
	ASSERT_EQ(e.note_count, 0);
}

TEST(note_off_out_of_range_returns_false) {
	GlideEngine e = make_engine();
	ASSERT_TRUE(!e.note_off(-1, 0));
	ASSERT_TRUE(!e.note_off(128, 0));
}

// ---------- Reset (simulates transport start) ----------

TEST(reset_clears_glide_state) {
	// After playing some notes and gliding, reset() should put the engine
	// back to its initial state so the next note doesn't glide from a
	// stale pitch.  This is what the plugin calls on transport start.
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0);
	e.note_on(60, 0);
	advance_until_settled(e);
	e.note_on(72, 0);
	advance_n(e, 500); // partway through a glide

	ASSERT_TRUE(e.note_count > 0);
	ASSERT_TRUE(e.current_pitch != 60.0);

	e.reset();

	ASSERT_EQ(e.state, GlideState::IDLE);
	ASSERT_NEAR(e.current_pitch, 60.0, 0.001);
	ASSERT_NEAR(e.src_pitch, 60.0, 0.001);
	ASSERT_NEAR(e.dst_pitch, 60.0, 0.001);
	ASSERT_EQ(e.note_count, 0);
	ASSERT_EQ(e.last_note, -1);
}

TEST(reset_first_note_no_glide) {
	// After reset, the very first note should NOT produce a glide even in
	// ALWAYS mode, because there is no meaningful source pitch.
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0);

	// Play a note, let it settle, then reset (simulates stop → play).
	e.note_on(48, 0);
	advance_until_settled(e);
	e.reset();

	// Now press a new note.  In ALWAYS mode the first note after reset
	// should jump to the key, not glide from the old pitch.
	e.note_on(72, 0);

	// The engine currently enters GLIDING even on a single note with
	// ALWAYS mode, but src==dst==72 so the output bend is centre.
	bool settled;
	int bend = e.advance(&settled);
	ASSERT_EQ(bend, BEND_CENTER);
}

TEST(reset_second_note_after_reset_does_glide) {
	// Verify that glide works normally again after reset — only the
	// first-note-after-reset is suppressed.
	GlideEngine e = make_engine(GlideMode::ALWAYS, 100.0, 12.0);
	e.note_on(60, 0);
	advance_until_settled(e);
	e.reset();

	// First note after reset → no glide.
	e.note_on(60, 0);
	advance_until_settled(e);

	// Second note → should glide from 60 → 72.
	e.note_on(72, 0);
	ASSERT_EQ(e.state, GlideState::GLIDING);

	bool settled;
	int bend = e.advance(&settled);
	ASSERT_TRUE(bend < BEND_CENTER); // gliding upward
}

TEST(reset_clears_held_notes) {
	GlideEngine e = make_engine();
	e.note_on(60, 0);
	e.note_on(64, 1);
	e.note_on(67, 2);
	ASSERT_EQ(e.note_count, 3);

	e.reset();
	ASSERT_EQ(e.note_count, 0);

	// held_notes should be zeroed — note_off for old keys should return false.
	ASSERT_TRUE(!e.note_off(60, 0));
	ASSERT_TRUE(!e.note_off(64, 1));
	ASSERT_TRUE(!e.note_off(67, 2));
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
	std::printf("Running %d glide-engine tests...\n", g_test_count);
	for (int i = 0; i < g_test_count; i++) {
		std::printf("  [%2d/%d] %s... ", i + 1, g_test_count, g_tests[i].name);
		g_tests[i].fn();
		g_tests_passed++;
		std::printf("OK\n");
	}
	std::printf("\n%d/%d tests passed.\n", g_tests_passed, g_test_count);
	return (g_tests_passed == g_test_count) ? 0 : 1;
}
