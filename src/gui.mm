//  gui.mm — Pugl Metal backend + Dear ImGui for Pitch Bend Glide
//
//  Uses puglMetalBackend() (pugl_mac_metal.m) which creates a PuglMetalView
//  that correctly calls [wrapper setReshaped] in viewDidMoveToSuperview,
//  ensuring PUGL_CONFIGURE fires before the first PUGL_EXPOSE.
//
//  Rendering lifecycle:
//    puglUpdate() → PUGL_UPDATE → puglObscureView() → viewWillDraw →
//    dispatchExpose → [backend enter] → PUGL_EXPOSE → render_frame →
//    PUGL_EXPOSE returns → [backend leave] (commit + present)
//
//  Compiled as Objective-C++ with ARC disabled (matches pugl_mac_metal.m).

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>

#include "pugl_mac_metal.h"

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_pugl.h"

#include "gui.h"
#include "plugin.h"
#include "params.h"

#include <pugl/pugl.h>

#include <cmath>

static constexpr uintptr_t INTERNAL_TIMER_ID = 1;

// ─── Forward declarations ─────────────────────────────────────────────────────
static void render_frame(PluginGui *gui);
static void draw_plugin_ui(PitchBendPlugin *plugin, float w, float h);

// ─── Pugl event handler ───────────────────────────────────────────────────────

static PuglStatus on_event(PuglView *view, const PuglEvent *event) {
	PluginGui *gui = static_cast<PluginGui *>(puglGetHandle(view));
	if (!gui) return PUGL_SUCCESS;

	ImGui::SetCurrentContext(gui->imgui_ctx);

	switch (event->type) {

	case PUGL_REALIZE:
		// Graphics context is now live — initialise ImGui Metal renderer.
		if (!gui->render_inited) {
			PuglMetalContext *ctx =
				static_cast<PuglMetalContext *>(puglGetContext(view));
			ImGui_ImplMetal_Init(ctx->device);
			gui->render_inited = true;
		}
		break;

	case PUGL_UNREALIZE:
		if (gui->render_inited) {
			ImGui_ImplMetal_Shutdown();
			gui->render_inited = false;
		}
		break;

	case PUGL_CONFIGURE:
		gui->width  = event->configure.width;
		gui->height = event->configure.height;
		break;

	case PUGL_UPDATE:
		// Called once per puglUpdate(); schedule a redraw so we render continuously.
		puglObscureView(view);
		break;

	case PUGL_EXPOSE:
		if (gui->render_inited)
			render_frame(gui);
		break;

	case PUGL_TIMER:
		if (event->timer.id == INTERNAL_TIMER_ID) {
			// Pugl internal timer: mark view dirty and pump events so the
			// drawing machinery runs this run-loop pass.
			puglObscureView(view);
			puglUpdate(static_cast<PuglWorld *>(gui->pugl_world), 0.0);
		}
		break;

	case PUGL_BUTTON_PRESS:
		puglGrabFocus(view);
		ImGui_ImplPugl_ProcessEvent(event);
		break;

	default:
		ImGui_ImplPugl_ProcessEvent(event);
		break;
	}

	return PUGL_SUCCESS;
}

// ─── PluginGui::create ────────────────────────────────────────────────────────

PluginGui *PluginGui::create(PitchBendPlugin *plugin_ptr) {
	auto *gui          = new PluginGui();
	gui->plugin        = plugin_ptr;
	gui->width          = 360;
	gui->height         = 430;
	gui->visible        = false;
	gui->realized       = false;
	gui->render_inited  = false;
	gui->internal_timer = false;
	gui->imgui_ctx      = nullptr;
	gui->pugl_world     = nullptr;
	gui->pugl_view      = nullptr;

	PuglWorld *world = puglNewWorld(PUGL_MODULE, 0);
	if (!world) { delete gui; return nullptr; }
	gui->pugl_world = world;

	PuglView *view = puglNewView(world);
	if (!view) { puglFreeWorld(world); delete gui; return nullptr; }
	gui->pugl_view = view;

	puglSetHandle(view, gui);
	puglSetEventFunc(view, on_event);
	puglSetBackend(view, puglMetalBackend());

	puglSetSizeHint(view, PUGL_DEFAULT_SIZE,
		static_cast<PuglSpan>(gui->width),
		static_cast<PuglSpan>(gui->height));
	puglSetSizeHint(view, PUGL_MIN_SIZE,
		static_cast<PuglSpan>(gui->width),
		static_cast<PuglSpan>(gui->height));

	// ImGui context — created before realize so the pointer is valid.
	IMGUI_CHECKVERSION();
	gui->imgui_ctx = ImGui::CreateContext();
	ImGui::SetCurrentContext(gui->imgui_ctx);

	ImGuiIO &io    = ImGui::GetIO();
	io.IniFilename = nullptr;

	ImGui::StyleColorsDark();
	ImGuiStyle &s      = ImGui::GetStyle();
	s.WindowPadding    = { 14.0f, 14.0f };
	s.FramePadding     = {  6.0f,  4.0f };
	s.ItemSpacing      = {  8.0f,  8.0f };
	s.WindowRounding   = 6.0f;
	s.FrameRounding    = 4.0f;
	s.GrabRounding     = 4.0f;
	s.WindowBorderSize = 0.0f;
	s.Colors[ImGuiCol_WindowBg] = { 0.12f, 0.12f, 0.12f, 1.0f };

	ImGui_ImplPugl_Init(view);

	return gui;
}

// ─── PluginGui::set_parent ───────────────────────────────────────────────────

bool PluginGui::set_parent(void *native_parent) {
	if (realized) return false;
	PuglView *view = static_cast<PuglView *>(pugl_view);

	puglSetParent(view, reinterpret_cast<PuglNativeView>(native_parent));

	if (puglRealize(view) != PUGL_SUCCESS) return false;

	realized   = true;
	return true;
}

// ─── PluginGui::destroy ──────────────────────────────────────────────────────

void PluginGui::destroy() {
	stop_internal_timer();

	PuglView  *view  = static_cast<PuglView *>(pugl_view);
	PuglWorld *world = static_cast<PuglWorld *>(pugl_world);

	if (imgui_ctx) {
		ImGui::SetCurrentContext(imgui_ctx);

		// Shut down backends explicitly before DestroyContext().
		// PUGL_UNREALIZE is not guaranteed to fire during puglFreeView(),
		// so we cannot rely on the event handler for Metal teardown.
		if (render_inited) {
			ImGui_ImplMetal_Shutdown();
			render_inited = false;
		}
		ImGui_ImplPugl_Shutdown();
	}

	// Free the view now; PUGL_UNREALIZE may fire here, but render_inited is
	// already false so the handler will skip ImGui_ImplMetal_Shutdown().
	if (view)  { puglFreeView(view);   pugl_view  = nullptr; }
	if (world) { puglFreeWorld(world); pugl_world = nullptr; }

	if (imgui_ctx) {
		ImGui::DestroyContext(imgui_ctx);
		imgui_ctx = nullptr;
	}

	delete this;
}

// ─── PluginGui::show / hide ───────────────────────────────────────────────────

void PluginGui::show() {
	if (!realized) return;
	puglShow(static_cast<PuglView *>(pugl_view), PUGL_SHOW_PASSIVE);
	visible = true;
}

void PluginGui::hide() {
	if (!realized) return;
	puglHide(static_cast<PuglView *>(pugl_view));
	visible = false;
}

// ─── PluginGui::render ────────────────────────────────────────────────────────

void PluginGui::get_logical_size(uint32_t *w, uint32_t *h) const {
	// PUGL_CONFIGURE reports backing pixels; CLAP hosts expect logical pixels.
	double scale = pugl_view ? puglGetScaleFactor(static_cast<const PuglView *>(pugl_view)) : 1.0;
	if (scale < 1.0) scale = 1.0;
	*w = static_cast<uint32_t>(width  / scale);
	*h = static_cast<uint32_t>(height / scale);
}

void PluginGui::set_logical_size(uint32_t w, uint32_t h) {
	if (!pugl_view) return;
	PuglView *view = static_cast<PuglView *>(pugl_view);
	double scale = puglGetScaleFactor(view);
	if (scale < 1.0) scale = 1.0;
	puglSetSizeHint(view, PUGL_CURRENT_SIZE,
		static_cast<PuglSpan>(w * scale),
		static_cast<PuglSpan>(h * scale));
}

void PluginGui::start_internal_timer() {
	if (!realized || internal_timer) return;
	puglStartTimer(static_cast<PuglView *>(pugl_view), INTERNAL_TIMER_ID, 1.0 / 60.0);
	internal_timer = true;
}

void PluginGui::stop_internal_timer() {
	if (!internal_timer || !pugl_view) return;
	puglStopTimer(static_cast<PuglView *>(pugl_view), INTERNAL_TIMER_ID);
	internal_timer = false;
}

// ─── render_frame — called from PUGL_EXPOSE ──────────────────────────────────

static void render_frame(PluginGui *gui) {
	ImGui::SetCurrentContext(gui->imgui_ctx);

	PuglMetalContext *ctx =
		static_cast<PuglMetalContext *>(
			puglGetContext(static_cast<PuglView *>(gui->pugl_view)));
	if (!ctx) return;

	ImGui_ImplMetal_NewFrame(ctx->renderPassDescriptor);
	ImGui_ImplPugl_NewFrame();
	ImGui::NewFrame();

	draw_plugin_ui(gui->plugin,
		static_cast<float>(gui->width),
		static_cast<float>(gui->height));

	ImGui::Render();
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(),
		ctx->commandBuffer, ctx->renderEncoder);
	// Backend leave() commits and presents — no manual command buffer work here.
}

// ─── ImGui UI ─────────────────────────────────────────────────────────────────

static void draw_curve_preview(CurveShape shape, float box_w, float box_h) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 origin  = ImGui::GetCursorScreenPos();

	dl->AddRectFilled(
		origin, { origin.x + box_w, origin.y + box_h },
		IM_COL32(28, 28, 28, 255), 4.0f);
	dl->AddRect(
		origin, { origin.x + box_w, origin.y + box_h },
		IM_COL32(70, 70, 70, 255), 4.0f);

	dl->AddLine({ origin.x,         origin.y + box_h * 0.5f },
	            { origin.x + box_w, origin.y + box_h * 0.5f },
	            IM_COL32(50, 50, 50, 255));
	dl->AddLine({ origin.x + box_w * 0.5f, origin.y },
	            { origin.x + box_w * 0.5f, origin.y + box_h },
	            IM_COL32(50, 50, 50, 255));

	static constexpr int STEPS = 80;
	ImVec2 pts[STEPS + 1];
	constexpr float PAD = 4.0f;

	for (int i = 0; i <= STEPS; i++) {
		float t = static_cast<float>(i) / static_cast<float>(STEPS);
		float y;
		switch (shape) {
		case CurveShape::LINEAR:      y = t; break;
		case CurveShape::EXPONENTIAL: y = t * t; break;
		case CurveShape::SIGMOID: {
			float x = 10.0f * (t - 0.5f);
			y = 1.0f / (1.0f + std::exp(-x));
			break;
		}
		default: y = t;
		}
		pts[i] = {
			origin.x + PAD + t * (box_w - 2.0f * PAD),
			origin.y + (box_h - PAD) - y * (box_h - 2.0f * PAD)
		};
	}
	dl->AddPolyline(pts, STEPS + 1, IM_COL32(80, 210, 120, 255), ImDrawFlags_None, 2.0f);

	ImGui::Dummy({ box_w, box_h });
}

static void param_slider(
	PitchBendPlugin *plugin,
	ParamId id,
	const char *label,
	const char *imgui_id,
	float min_v, float max_v,
	const char *fmt,
	float label_col_w,
	float slider_w)
{
	float val = static_cast<float>(
		plugin->params[static_cast<uint32_t>(id)].load(std::memory_order_relaxed));

	ImGui::Text("%s", label);
	ImGui::SameLine(label_col_w);
	ImGui::SetNextItemWidth(slider_w);
	if (ImGui::SliderFloat(imgui_id, &val, min_v, max_v, fmt)) {
		plugin->param_gesture_end(id, static_cast<double>(val));
	}
}

static void draw_plugin_ui(PitchBendPlugin *plugin, float view_w, float view_h) {
	ImGui::SetNextWindowPos({ 0.0f, 0.0f });
	ImGui::SetNextWindowSize({ view_w, view_h });

	constexpr ImGuiWindowFlags WIN_FLAGS =
		ImGuiWindowFlags_NoTitleBar  |
		ImGuiWindowFlags_NoResize    |
		ImGuiWindowFlags_NoMove      |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoCollapse  |
		ImGuiWindowFlags_NoBringToFrontOnFocus;

	ImGui::Begin("##root", nullptr, WIN_FLAGS);

	// ── Title ──────────────────────────────────────────────────────────────
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.55f, 1.0f));
	ImGui::SetWindowFontScale(1.15f);
	ImGui::Text("Pitch Bend Glide");
	ImGui::SetWindowFontScale(1.0f);
	ImGui::PopStyleColor();
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	constexpr float LABEL_W  = 110.0f;
	constexpr float SLIDER_W = 185.0f;

	// ── Glide Time ─────────────────────────────────────────────────────────
	param_slider(plugin,
		ParamId::GLIDE_TIME_MS, "Glide Time", "##glide_time",
		0.0f, 2000.0f, "%.0f ms", LABEL_W, SLIDER_W);

	ImGui::Spacing();

	// ── Glide Mode ─────────────────────────────────────────────────────────
	{
		int mode = static_cast<int>(
			plugin->params[static_cast<uint32_t>(ParamId::GLIDE_MODE)]
				.load(std::memory_order_relaxed) + 0.5);

		ImGui::Text("Mode");
		ImGui::SameLine(LABEL_W);
		bool changed = false;
		if (ImGui::RadioButton("Legato", &mode, 1)) changed = true;
		ImGui::SameLine();
		if (ImGui::RadioButton("Always", &mode, 0)) changed = true;
		if (changed) {
			plugin->param_gesture_end(ParamId::GLIDE_MODE, static_cast<double>(mode));
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// ── Curve ──────────────────────────────────────────────────────────────
	{
		int curve = static_cast<int>(
			plugin->params[static_cast<uint32_t>(ParamId::CURVE)]
				.load(std::memory_order_relaxed) + 0.5);

		ImGui::Text("Curve");
		ImGui::SameLine(LABEL_W);
		bool changed = false;
		if (ImGui::RadioButton("Linear",  &curve, 0)) changed = true;
		ImGui::SameLine();
		if (ImGui::RadioButton("Expo",    &curve, 1)) changed = true;
		ImGui::SameLine();
		if (ImGui::RadioButton("Sigmoid", &curve, 2)) changed = true;
		if (changed) {
			plugin->param_gesture_end(ParamId::CURVE, static_cast<double>(curve));
		}

		ImGui::Spacing();
		draw_curve_preview(static_cast<CurveShape>(curve), view_w - 28.0f, 88.0f);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// ── Lookahead ──────────────────────────────────────────────────────────
	{
		float val = static_cast<float>(
			plugin->params[static_cast<uint32_t>(ParamId::LOOKAHEAD_MS)]
				.load(std::memory_order_relaxed));

		ImGui::Text("Lookahead");
		ImGui::SameLine(LABEL_W);
		ImGui::SetNextItemWidth(SLIDER_W - 60.0f);
		if (ImGui::SliderFloat("##lookahead", &val, 0.0f, 20.0f, "%.1f ms")) {
			plugin->param_gesture_end(ParamId::LOOKAHEAD_MS, static_cast<double>(val));
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(latency)");
	}

	ImGui::Spacing();

	// ── Pitch Bend Range ───────────────────────────────────────────────────
	param_slider(plugin,
		ParamId::PITCH_BEND_RANGE, "Bend Range", "##bend_range",
		1.0f, 48.0f, "%.0f st", LABEL_W, SLIDER_W - 60.0f);

	ImGui::End();
}
