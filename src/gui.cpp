//  gui.cpp — Pugl + OpenGL3 + Dear ImGui rendering for Pitch Bend Glide

#define GL_SILENCE_DEPRECATION 1

#include "gui.h"
#include "plugin.h"
#include "params.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_pugl.h"

#include <pugl/pugl.h>
#include <pugl/gl.h>

#include <cmath>
#include <cstring>

static constexpr uint32_t GUI_WIDTH  = 360;
static constexpr uint32_t GUI_HEIGHT = 430;

// ─── Forward declaration of the ImGui layout function ────────────────────────
static void draw_plugin_ui(PitchBendPlugin *plugin);

// ─── Pugl event handler ───────────────────────────────────────────────────────

static PuglStatus on_event(PuglView *view, const PuglEvent *event) {
	PluginGui *gui = static_cast<PluginGui *>(puglGetHandle(view));
	if (!gui) return PUGL_SUCCESS;

	// Keep ImGui context current for every callback that touches ImGui state.
	if (gui->imgui_ctx) {
		ImGui::SetCurrentContext(gui->imgui_ctx);
	}

	switch (event->type) {
	case PUGL_REALIZE:
		// The GL context is now live. Initialise ImGui and its GL backend.
		ImGui_ImplOpenGL3_Init("#version 150");
		break;

	case PUGL_UNREALIZE:
		ImGui_ImplOpenGL3_Shutdown();
		break;

	case PUGL_CONFIGURE:
		glViewport(
			0, 0,
			static_cast<GLsizei>(event->configure.width),
			static_cast<GLsizei>(event->configure.height));
		break;

	case PUGL_EXPOSE:
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplPugl_NewFrame();
		ImGui::NewFrame();

		draw_plugin_ui(gui->plugin);

		ImGui::Render();
		glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		break;

	default:
		ImGui_ImplPugl_ProcessEvent(event);
		break;
	}

	return PUGL_SUCCESS;
}

// ─── PluginGui implementation ─────────────────────────────────────────────────

PluginGui *PluginGui::create(PitchBendPlugin *plugin_ptr) {
	auto *gui       = new PluginGui();
	gui->plugin     = plugin_ptr;
	gui->width      = GUI_WIDTH;
	gui->height     = GUI_HEIGHT;
	gui->visible    = false;
	gui->realized   = false;
	gui->imgui_ctx  = nullptr;

	// Create a Pugl world for this plugin instance.
	PuglWorld *world = puglNewWorld(PUGL_PROGRAM, 0);
	if (!world) {
		delete gui;
		return nullptr;
	}
	gui->world_opaque = reinterpret_cast<PuglViewImpl *>(world);

	// Create and configure the view — do NOT realize it yet.
	PuglView *view = puglNewView(world);
	if (!view) {
		puglFreeWorld(world);
		delete gui;
		return nullptr;
	}
	gui->view_opaque = reinterpret_cast<PuglViewImpl *>(view);

	puglSetHandle(view, gui);
	puglSetEventFunc(view, on_event);
	puglSetBackend(view, puglGlBackend());

	// Request a core-profile OpenGL 3.2 context (GLSL 150).
	puglSetViewHint(view, PUGL_CONTEXT_API,           PUGL_OPENGL_API);
	puglSetViewHint(view, PUGL_CONTEXT_VERSION_MAJOR, 3);
	puglSetViewHint(view, PUGL_CONTEXT_VERSION_MINOR, 2);
	puglSetViewHint(view, PUGL_CONTEXT_PROFILE,       PUGL_OPENGL_CORE_PROFILE);

	puglSetSizeHint(view, PUGL_DEFAULT_SIZE,
		static_cast<PuglSpan>(GUI_WIDTH),
		static_cast<PuglSpan>(GUI_HEIGHT));
	puglSetSizeHint(view, PUGL_MIN_SIZE,
		static_cast<PuglSpan>(GUI_WIDTH),
		static_cast<PuglSpan>(GUI_HEIGHT));

	// Create the ImGui context now (before realize) so the pointer is valid.
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

	// Init Pugl platform backend (clipboard, cursor, display-size tracking).
	ImGui_ImplPugl_Init(view);

	return gui;
}

void PluginGui::destroy() {
	PuglView  *view  = reinterpret_cast<PuglView *>(view_opaque);
	PuglWorld *world = reinterpret_cast<PuglWorld *>(world_opaque);

	if (view) {
		if (realized) {
			puglHide(view);
		}
		puglFreeView(view);
	}

	// Shutdown ImGui backends; GL backend was shut down in PUGL_UNREALIZE.
	if (imgui_ctx) {
		ImGui::SetCurrentContext(imgui_ctx);
		ImGui_ImplPugl_Shutdown();
		ImGui::DestroyContext(imgui_ctx);
	}

	if (world) {
		puglFreeWorld(world);
	}

	delete this;
}

bool PluginGui::set_parent(void *native_parent) {
	PuglView *view = reinterpret_cast<PuglView *>(view_opaque);
	if (!view || realized) return false;

	// native_parent is an NSView* on macOS; cast to uintptr_t for Pugl.
	puglSetParent(view, reinterpret_cast<PuglNativeView>(native_parent));

	PuglStatus status = puglRealize(view);
	if (status != PUGL_SUCCESS) return false;

	realized = true;
	return true;
}

void PluginGui::show() {
	if (!realized) return;
	PuglView *view = reinterpret_cast<PuglView *>(view_opaque);
	puglShow(view, PUGL_SHOW_RAISE);
	visible = true;
}

void PluginGui::hide() {
	if (!realized) return;
	PuglView *view = reinterpret_cast<PuglView *>(view_opaque);
	puglHide(view);
	visible = false;
}

void PluginGui::render() {
	if (!realized || !visible) return;
	PuglWorld *world = reinterpret_cast<PuglWorld *>(world_opaque);
	// 0.0 timeout = non-blocking; just flush pending events then return.
	puglUpdate(world, 0.0);
}

// ─── ImGui UI ─────────────────────────────────────────────────────────────────

static void draw_curve_preview(CurveShape shape, float box_w, float box_h) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 origin  = ImGui::GetCursorScreenPos();

	dl->AddRectFilled(
		origin,
		{ origin.x + box_w, origin.y + box_h },
		IM_COL32(28, 28, 28, 255), 4.0f);
	dl->AddRect(
		origin,
		{ origin.x + box_w, origin.y + box_h },
		IM_COL32(70, 70, 70, 255), 4.0f);

	// Faint axis lines.
	dl->AddLine({ origin.x,            origin.y + box_h * 0.5f },
	            { origin.x + box_w,    origin.y + box_h * 0.5f },
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
		// Flip y: screen y increases downward.
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

static void draw_plugin_ui(PitchBendPlugin *plugin) {
	PuglArea size = puglGetSizeHint(
		reinterpret_cast<PuglView *>(
			reinterpret_cast<PluginGui *>(
				// Recover the PluginGui* from plugin->gui (set in plugin.h).
				plugin->gui
			)->view_opaque
		),
		PUGL_CURRENT_SIZE
	);
	float view_w = static_cast<float>(size.width);
	float view_h = static_cast<float>(size.height);

	ImGui::SetNextWindowPos({ 0.0f, 0.0f });
	ImGui::SetNextWindowSize({ view_w, view_h });

	constexpr ImGuiWindowFlags WIN_FLAGS =
		ImGuiWindowFlags_NoTitleBar  |
		ImGuiWindowFlags_NoResize    |
		ImGuiWindowFlags_NoMove      |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoCollapse;

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
	constexpr float SLIDER_W = 180.0f;

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
