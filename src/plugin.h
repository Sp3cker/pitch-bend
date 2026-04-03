#pragma once

#include <clap/clap.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>
#include <clap/ext/latency.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/gui.h>

#include <atomic>
#include <cstdint>

#include "glide_engine.h"
#include "event_buffer.h"
#include "params.h"

struct PluginGui; // defined in gui.mm

// ─── PitchBendPlugin ──────────────────────────────────────────────────────────
//
// All state for one plugin instance.  The host holds a heap-allocated
// clap_plugin_t whose plugin_data field points to this struct.
//
// Threading:
//   - Audio thread:  activate / deactivate / process / flush (params)
//   - Main thread:   init / destroy / GUI / on_main_thread
//   - params[] are std::atomic<double> for lock-free cross-thread reads.
//
struct PitchBendPlugin {
	const clap_host_t *host;

	// Cached host extension pointers (set in plugin_init, read-only after).
	const clap_host_params_t  *host_params  = nullptr;
	const clap_host_latency_t *host_latency = nullptr;

	// ── Parameters ────────────────────────────────────────────────────────────
	// Indexed by static_cast<uint32_t>(ParamId::*).
	// Written by: audio thread (from CLAP_EVENT_PARAM_VALUE), main thread (GUI).
	// Read by:    audio thread (process/flush), main thread (get_value, GUI).
	std::atomic<double> params[PARAM_COUNT];

	// Bitmask of parameters dirtied by the GUI; cleared in params_flush.
	std::atomic<uint32_t> dirty_params { 0 };

	// ── Audio-thread state ────────────────────────────────────────────────────
	GlideEngine glide;
	EventBuffer ev_buf;
	uint64_t    block_start_sample = 0;
	double      sample_rate        = 44100.0;
	bool        active             = false;

	// Pitch bend emission (audio thread only).
	int      last_bend_value       = 8192;
	uint32_t bend_decimate_counter = 0;

	// Set on audio thread; cleared on main thread via on_main_thread.
	std::atomic<bool> notify_latency { false };

	// ── GUI (main thread only) ────────────────────────────────────────────────
	PluginGui *gui = nullptr;

	// ─────────────────────────────────────────────────────────────────────────

	explicit PitchBendPlugin(const clap_host_t *host);

	// Called from the GUI when the user changes a parameter.
	// Clamps value, stores it atomically, marks dirty, and requests a flush.
	void param_gesture_end(ParamId id, double value);

	static PitchBendPlugin *from_clap(const clap_plugin_t *p) {
		return static_cast<PitchBendPlugin *>(p->plugin_data);
	}
};
