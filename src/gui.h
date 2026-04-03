#pragma once

#include <cstdint>

struct ImGuiContext;    // opaque ImGui type
struct PitchBendPlugin; // forward declaration

// ─── PluginGui ────────────────────────────────────────────────────────────────
//
// C++ wrapper around a Pugl-managed NSView with a CAMetalLayer attached.
// Pugl handles windowing and input events; Metal handles rendering.
// imgui_impl_pugl provides the ImGui platform backend (input/cursor/time).
// imgui_impl_metal provides the ImGui rendering backend.
//
// All methods must be called on the main thread.
//
struct PluginGui {
	void           *pugl_world;     // PuglWorld*
	void           *pugl_view;     // PuglView*
	ImGuiContext   *imgui_ctx;
	PitchBendPlugin *plugin;
	uint32_t         width;        // backing pixels (from PUGL_CONFIGURE)
	uint32_t         height;       // backing pixels (from PUGL_CONFIGURE)
	bool             visible;
	bool             realized;
	bool             render_inited;  // true after ImGui_ImplMetal_Init in PUGL_REALIZE
	bool             internal_timer; // true when Pugl timer is running

	// Allocate a PluginGui: creates the PuglWorld and configures the view.
	// The view is NOT yet realised — call set_parent() first.
	static PluginGui *create(PitchBendPlugin *plugin);

	// Set the host's parent native view, realise the Pugl view, attach Metal,
	// and initialise ImGui backends. Must be called once before show().
	bool set_parent(void *native_parent);

	// Destroy view, world, Metal resources, ImGui context, and free this object.
	void destroy();

	void show();
	void hide();

	// Return the GUI size in logical pixels (points), dividing out HiDPI scale.
	// Use these values when reporting size to the CLAP host.
	void get_logical_size(uint32_t *w, uint32_t *h) const;

	// Accept a size in logical pixels from the host and apply it to the view.
	void set_logical_size(uint32_t w, uint32_t h);

	// Start/stop Pugl's internal NSTimer (~60 Hz).
	// Used when the host does not provide CLAP_EXT_TIMER_SUPPORT (e.g. Bitwig).
	void start_internal_timer();
	void stop_internal_timer();
};
