#include "plugin.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

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

#define ASSERT_TRUE(x) do {                                               \
	if (!(x)) {                                                           \
		std::fprintf(stderr, "  FAIL: %s:%d: %s\n",                      \
			__FILE__, __LINE__, #x);                                      \
		assert(false);                                                    \
	}                                                                     \
} while (0)

// ─── Mock output event queue ──────────────────────────────────────────────────

struct MidiMsg {
	uint8_t status;
	uint8_t data1;
	uint8_t data2;
};

static std::vector<MidiMsg> g_captured;

static bool mock_try_push(const clap_output_events_t *, const clap_event_header_t *hdr) {
	if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID && hdr->type == CLAP_EVENT_MIDI) {
		const auto *me = reinterpret_cast<const clap_event_midi_t *>(hdr);
		g_captured.push_back({ me->data[0], me->data[1], me->data[2] });
	}
	return true;
}

static const clap_output_events_t g_mock_out = {
	nullptr,        // ctx
	mock_try_push,
};

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Stub host — we need a minimal clap_host_t to construct PitchBendPlugin.
static const void *stub_get_extension(const clap_host_t *, const char *) { return nullptr; }
static void stub_request_restart(const clap_host_t *) {}
static void stub_request_process(const clap_host_t *) {}
static void stub_request_callback(const clap_host_t *) {}

static const clap_host_t g_stub_host = {
	CLAP_VERSION_INIT,
	nullptr,
	"test-host",
	"test",
	"",
	"0.0.0",
	stub_get_extension,
	stub_request_restart,
	stub_request_process,
	stub_request_callback,
};

// Create a PitchBendPlugin configured for testing.
static std::unique_ptr<PitchBendPlugin> make_plugin(GlideMode mode = GlideMode::LEGATO,
                                                     double glide_ms = 100.0,
                                                     double bend_range = 12.0) {
	auto p = std::make_unique<PitchBendPlugin>(&g_stub_host);
	p->glide.glide_mode    = mode;
	p->glide.glide_time_ms = glide_ms;
	p->glide.bend_range    = bend_range;
	p->glide.activate(44100.0);
	p->active = true;
	return p;
}

// Build a CLAP note-on event.
static clap_event_note_t make_note_on(int key, int channel = 0, double vel = 0.8) {
	clap_event_note_t ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.header.size     = sizeof(ev);
	ev.header.time     = 0;
	ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	ev.header.type     = CLAP_EVENT_NOTE_ON;
	ev.header.flags    = 0;
	ev.port_index      = 0;
	ev.channel         = static_cast<int16_t>(channel);
	ev.key             = static_cast<int16_t>(key);
	ev.note_id         = -1;
	ev.velocity        = vel;
	return ev;
}

// Build a CLAP note-off event.
static clap_event_note_t make_note_off(int key, int channel = 0) {
	clap_event_note_t ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.header.size     = sizeof(ev);
	ev.header.time     = 0;
	ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	ev.header.type     = CLAP_EVENT_NOTE_OFF;
	ev.header.flags    = 0;
	ev.port_index      = 0;
	ev.channel         = static_cast<int16_t>(channel);
	ev.key             = static_cast<int16_t>(key);
	ev.note_id         = -1;
	ev.velocity        = 0.0;
	return ev;
}

// Build a raw MIDI note-on event.
static clap_event_midi_t make_midi_note_on(uint8_t key, uint8_t vel, uint8_t channel = 0) {
	clap_event_midi_t ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.header.size     = sizeof(ev);
	ev.header.time     = 0;
	ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	ev.header.type     = CLAP_EVENT_MIDI;
	ev.header.flags    = 0;
	ev.port_index      = 0;
	ev.data[0]         = static_cast<uint8_t>(0x90u | (channel & 0x0Fu));
	ev.data[1]         = key;
	ev.data[2]         = vel;
	return ev;
}

// Build a raw MIDI note-off event.
static clap_event_midi_t make_midi_note_off(uint8_t key, uint8_t channel = 0) {
	clap_event_midi_t ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.header.size     = sizeof(ev);
	ev.header.time     = 0;
	ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	ev.header.type     = CLAP_EVENT_MIDI;
	ev.header.flags    = 0;
	ev.port_index      = 0;
	ev.data[0]         = static_cast<uint8_t>(0x80u | (channel & 0x0Fu));
	ev.data[1]         = key;
	ev.data[2]         = 0;
	return ev;
}

static void send(PitchBendPlugin &p, const clap_event_header_t *hdr) {
	handle_event(&p, hdr, &g_mock_out);
}

// Count captured messages matching a status nibble (0x90 = note-on, 0x80 = note-off).
static int count_status(uint8_t status_hi) {
	int n = 0;
	for (auto &m : g_captured) {
		if ((m.status & 0xF0u) == status_hi) n++;
	}
	return n;
}

// Find the last captured message with the given status nibble.
static MidiMsg last_with_status(uint8_t status_hi) {
	for (int i = static_cast<int>(g_captured.size()) - 1; i >= 0; i--) {
		if ((g_captured[i].status & 0xF0u) == status_hi)
			return g_captured[i];
	}
	return { 0, 0, 0 };
}

// ─── Tests: CLAP note events ─────────────────────────────────────────────────

TEST(clap_single_note_emits_note_on) {
	g_captured.clear();
	auto p = make_plugin();
	auto ev = make_note_on(60);
	send(*p, &ev.header);

	ASSERT_EQ(count_status(0x90), 1);
	ASSERT_EQ(g_captured[0].data1, 60);
}

TEST(clap_legato_second_note_suppresses_note_on) {
	g_captured.clear();
	auto p = make_plugin(GlideMode::LEGATO, 100.0);

	auto n1 = make_note_on(60);
	send(*p, &n1.header);
	ASSERT_EQ(count_status(0x90), 1); // first note emits

	auto n2 = make_note_on(72);
	send(*p, &n2.header);
	ASSERT_EQ(count_status(0x90), 1); // second note suppressed
	ASSERT_EQ(p->glide.state, GlideState::GLIDING);
}

TEST(clap_always_legato_suppresses_note_on) {
	g_captured.clear();
	auto p = make_plugin(GlideMode::ALWAYS, 100.0);

	auto n1 = make_note_on(60);
	send(*p, &n1.header);
	ASSERT_EQ(count_status(0x90), 1);

	auto n2 = make_note_on(72);
	send(*p, &n2.header);
	ASSERT_EQ(count_status(0x90), 1); // suppressed — glide handles it
}

TEST(clap_non_legato_emits_both_notes) {
	// LEGATO mode, but notes don't overlap → two note-ons.
	g_captured.clear();
	auto p = make_plugin(GlideMode::LEGATO, 100.0);

	auto n1 = make_note_on(60);
	send(*p, &n1.header);

	auto off1 = make_note_off(60);
	send(*p, &off1.header);

	auto n2 = make_note_on(72);
	send(*p, &n2.header);

	ASSERT_EQ(count_status(0x90), 2);
}

TEST(clap_note_off_only_when_all_released) {
	g_captured.clear();
	auto p = make_plugin(GlideMode::LEGATO, 100.0);

	auto n1 = make_note_on(60);
	send(*p, &n1.header);
	auto n2 = make_note_on(72);
	send(*p, &n2.header);

	// Release second note — first note still held → no MIDI note-off.
	auto off2 = make_note_off(72);
	send(*p, &off2.header);
	ASSERT_EQ(count_status(0x80), 0);

	// Release first note — all released → MIDI note-off emitted.
	auto off1 = make_note_off(60);
	send(*p, &off1.header);
	ASSERT_EQ(count_status(0x80), 1);
}

TEST(clap_note_off_releases_sounding_key) {
	g_captured.clear();
	auto p = make_plugin(GlideMode::LEGATO, 100.0);

	auto n1 = make_note_on(60);
	send(*p, &n1.header);
	auto n2 = make_note_on(72);
	send(*p, &n2.header);

	// Release both.
	auto off2 = make_note_off(72);
	send(*p, &off2.header);
	auto off1 = make_note_off(60);
	send(*p, &off1.header);

	// The note-off should be for key 60 (the sounding key), not 72.
	MidiMsg off_msg = last_with_status(0x80);
	ASSERT_EQ(off_msg.data1, 60);
}

TEST(clap_sounding_key_updates_after_full_release) {
	g_captured.clear();
	auto p = make_plugin(GlideMode::LEGATO, 100.0);

	// Play note 60 → 72 legato, then release all.
	auto n1 = make_note_on(60);
	send(*p, &n1.header);
	auto n2 = make_note_on(72);
	send(*p, &n2.header);
	auto off2 = make_note_off(72);
	send(*p, &off2.header);
	auto off1 = make_note_off(60);
	send(*p, &off1.header);

	ASSERT_EQ(p->sounding_key, -1);

	// Now play a new note — should get a fresh note-on.
	auto n3 = make_note_on(64);
	send(*p, &n3.header);
	ASSERT_EQ(count_status(0x90), 2); // original 60 + new 64
	ASSERT_EQ(p->sounding_key, 64);
}

TEST(clap_three_note_legato_chain) {
	// 60 → 72 → 84 legato: only one note-on (60), two note-ons suppressed.
	g_captured.clear();
	auto p = make_plugin(GlideMode::LEGATO, 100.0);

	auto n1 = make_note_on(60);
	send(*p, &n1.header);
	auto n2 = make_note_on(72);
	send(*p, &n2.header);
	auto n3 = make_note_on(84);
	send(*p, &n3.header);

	ASSERT_EQ(count_status(0x90), 1);
	ASSERT_EQ(p->sounding_key, 60);
}

TEST(clap_zero_glide_time_emits_all_notes) {
	// With glide_time_ms = 0, there's no glide → all notes emit note-on.
	g_captured.clear();
	auto p = make_plugin(GlideMode::ALWAYS, 0.0);

	auto n1 = make_note_on(60);
	send(*p, &n1.header);
	auto n2 = make_note_on(72);
	send(*p, &n2.header);

	ASSERT_EQ(count_status(0x90), 2);
}

// ─── Tests: Raw MIDI events ──────────────────────────────────────────────────

TEST(midi_single_note_passes_through) {
	g_captured.clear();
	auto p = make_plugin();

	auto ev = make_midi_note_on(60, 100);
	send(*p, &ev.header);

	ASSERT_EQ(count_status(0x90), 1);
	ASSERT_EQ(g_captured[0].data1, 60);
}

TEST(midi_legato_second_note_suppressed) {
	g_captured.clear();
	auto p = make_plugin(GlideMode::LEGATO, 100.0);

	auto n1 = make_midi_note_on(60, 100);
	send(*p, &n1.header);
	auto n2 = make_midi_note_on(72, 100);
	send(*p, &n2.header);

	ASSERT_EQ(count_status(0x90), 1);
}

TEST(midi_note_off_releases_sounding_key) {
	g_captured.clear();
	auto p = make_plugin(GlideMode::LEGATO, 100.0);

	auto n1 = make_midi_note_on(60, 100);
	send(*p, &n1.header);
	auto n2 = make_midi_note_on(72, 100);
	send(*p, &n2.header);

	auto off2 = make_midi_note_off(72);
	send(*p, &off2.header);
	ASSERT_EQ(count_status(0x80), 0); // still holding 60

	auto off1 = make_midi_note_off(60);
	send(*p, &off1.header);
	ASSERT_EQ(count_status(0x80), 1);

	MidiMsg off_msg = last_with_status(0x80);
	ASSERT_EQ(off_msg.data1, 60); // sounding key, not 72
}

TEST(midi_non_legato_passes_both_through) {
	g_captured.clear();
	auto p = make_plugin(GlideMode::LEGATO, 100.0);

	auto n1 = make_midi_note_on(60, 100);
	send(*p, &n1.header);
	auto off1 = make_midi_note_off(60);
	send(*p, &off1.header);

	auto n2 = make_midi_note_on(72, 100);
	send(*p, &n2.header);

	ASSERT_EQ(count_status(0x90), 2);
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
	std::printf("Running %d legato-routing tests...\n", g_test_count);
	for (int i = 0; i < g_test_count; i++) {
		std::printf("  [%2d/%d] %s... ", i + 1, g_test_count, g_tests[i].name);
		g_tests[i].fn();
		g_tests_passed++;
		std::printf("OK\n");
	}
	std::printf("\n%d/%d tests passed.\n", g_tests_passed, g_test_count);
	return (g_tests_passed == g_test_count) ? 0 : 1;
}
