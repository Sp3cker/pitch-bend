#include "plugin.h"
#ifndef PITCH_BEND_NO_GUI
#include "gui.h"
#endif

#include <clap/clap.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>
#include <clap/ext/latency.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/gui.h>


#include <cstdio>
#include <cstring>

// How often (in samples) we emit a pitch-bend MIDI event during a glide.
// At 44100 Hz this is ~0.7 ms — fine enough for imperceptible stairstepping.
static constexpr uint32_t BEND_DECIMATE_PERIOD = 32;

// ─── Plugin descriptor ────────────────────────────────────────────────────────

static const char *const PLUGIN_FEATURES[] = {
	CLAP_PLUGIN_FEATURE_NOTE_EFFECT,
	CLAP_PLUGIN_FEATURE_UTILITY,
	nullptr
};

static const clap_plugin_descriptor_t PLUGIN_DESC = {
	CLAP_VERSION_INIT,
	"com.pitchbend.glide",
	"Pitch Bend Glide",
	"Pitch Bend",
	"",
	"",
	"",
	"0.1.0",
	"Glissando / note glide via MIDI pitch bend",
	PLUGIN_FEATURES
};

// ─── PitchBendPlugin constructor ──────────────────────────────────────────────

PitchBendPlugin::PitchBendPlugin(const clap_host_t *h) : host(h) {
	for (uint32_t i = 0; i < PARAM_COUNT; i++) {
		params[i].store(PARAM_INFO[i].default_value, std::memory_order_relaxed);
	}
}

void PitchBendPlugin::param_gesture_end(ParamId id, double value) {
	uint32_t idx = static_cast<uint32_t>(id);
	if (idx >= PARAM_COUNT) return;

	const auto &pi = PARAM_INFO[idx];
	if (value < pi.min_value) value = pi.min_value;
	if (value > pi.max_value) value = pi.max_value;

	params[idx].store(value, std::memory_order_relaxed);
	dirty_params.fetch_or(1u << idx, std::memory_order_release);

	if (host_params) {
		host_params->request_flush(host);
	}
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Emit a MIDI pitch-bend message (0xEn LSB MSB) into the output event queue.
static void emit_pitch_bend(
	const clap_output_events_t *out,
	uint32_t frame,
	int channel,
	int bend_14bit)
{
	clap_event_midi_t ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.header.size     = sizeof(ev);
	ev.header.time     = frame;
	ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	ev.header.type     = CLAP_EVENT_MIDI;
	ev.header.flags    = 0;
	ev.port_index      = 0;
	ev.data[0]         = static_cast<uint8_t>(0xE0u | (static_cast<unsigned>(channel) & 0x0Fu));
	ev.data[1]         = static_cast<uint8_t>(static_cast<unsigned>(bend_14bit) & 0x7Fu);        // LSB
	ev.data[2]         = static_cast<uint8_t>((static_cast<unsigned>(bend_14bit) >> 7u) & 0x7Fu); // MSB
	out->try_push(out, &ev.header);
}

static int normalize_midi_channel(int channel) {
	return (channel >= 0 && channel < 16) ? channel : 0;
}

static uint8_t clamp_midi_data(int value) {
	if (value < 0) return 0;
	if (value > 127) return 127;
	return static_cast<uint8_t>(value);
}

static uint8_t velocity_to_midi(double velocity) {
	int value = static_cast<int>(velocity * 127.0 + 0.5);
	if (value <= 0) value = 1;
	if (value > 127) value = 127;
	return static_cast<uint8_t>(value);
}

static void emit_midi_message(
	const clap_output_events_t *out,
	uint32_t frame,
	uint8_t status,
	uint8_t data1,
	uint8_t data2)
{
	clap_event_midi_t ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.header.size     = sizeof(ev);
	ev.header.time     = frame;
	ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	ev.header.type     = CLAP_EVENT_MIDI;
	ev.header.flags    = 0;
	ev.port_index      = 0;
	ev.data[0]         = status;
	ev.data[1]         = data1;
	ev.data[2]         = data2;
	out->try_push(out, &ev.header);
}

// Read atomics and push updated values into the glide engine.
// Called at the top of every process() block.
static void sync_params_to_engine(PitchBendPlugin *self) {
	double glide_time = self->params[static_cast<uint32_t>(ParamId::GLIDE_TIME_MS)]
		.load(std::memory_order_relaxed);
	double glide_mode = self->params[static_cast<uint32_t>(ParamId::GLIDE_MODE)]
		.load(std::memory_order_relaxed);
	double curve      = self->params[static_cast<uint32_t>(ParamId::CURVE)]
		.load(std::memory_order_relaxed);
	double lookahead  = self->params[static_cast<uint32_t>(ParamId::LOOKAHEAD_MS)]
		.load(std::memory_order_relaxed);
	double bend_range = self->params[static_cast<uint32_t>(ParamId::PITCH_BEND_RANGE)]
		.load(std::memory_order_relaxed);

	self->glide.glide_time_ms = glide_time;
	self->glide.glide_mode    = static_cast<GlideMode>(static_cast<int>(glide_mode + 0.5));
	self->glide.curve         = static_cast<CurveShape>(static_cast<int>(curve + 0.5));
	self->glide.bend_range    = bend_range;
	self->glide.update_glide_samples();

	uint32_t new_delay = static_cast<uint32_t>(lookahead * self->sample_rate / 1000.0);
	if (new_delay != self->ev_buf.delay_samples()) {
		self->ev_buf.set_delay(new_delay);
		// Notify host of the latency change via on_main_thread.
		self->notify_latency.store(true, std::memory_order_release);
		self->host->request_callback(self->host);
	}
}

// Clamp value to param range and store atomically.
static void apply_param_value(PitchBendPlugin *self, clap_id id, double value) {
	if (id >= PARAM_COUNT) return;
	const auto &pi = PARAM_INFO[id];
	if (value < pi.min_value) value = pi.min_value;
	if (value > pi.max_value) value = pi.max_value;
	self->params[id].store(value, std::memory_order_relaxed);
}

// Process one ready event and pass it through to out.
void handle_event(
	PitchBendPlugin *self,
	const clap_event_header_t *hdr,
	const clap_output_events_t *out)
{
	if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) {
		out->try_push(out, hdr);
		return;
	}

	switch (hdr->type) {
	case CLAP_EVENT_NOTE_ON: {
		const auto *ne = reinterpret_cast<const clap_event_note_t *>(hdr);
		const int channel = normalize_midi_channel(static_cast<int>(ne->channel));
		const int key     = static_cast<int>(ne->key);

		// Capture state before note_on mutates it.
		GlideState prev_state = self->glide.state;
		self->glide.note_on(key, channel);

		// If the engine started gliding (legato transition), suppress the
		// MIDI note-on — pitch bend alone handles the transition on the
		// already-sounding note.
		if (self->glide.state == GlideState::GLIDING &&
		    (prev_state == GlideState::HELD || prev_state == GlideState::GLIDING)) {
			// Legato: retarget bend only, no new note-on.
			break;
		}

		// First note or non-glide: emit the MIDI note-on and remember it.
		self->sounding_key     = key;
		self->sounding_channel = channel;
		emit_midi_message(
			out,
			hdr->time,
			static_cast<uint8_t>(0x90u | static_cast<unsigned>(channel)),
			clamp_midi_data(key),
			velocity_to_midi(ne->velocity));
		break;
	}

	case CLAP_EVENT_NOTE_OFF: {
		const auto *ne = reinterpret_cast<const clap_event_note_t *>(hdr);
		// Before releasing the note, peek ahead for an incoming note-on.
		// If found, pre-target the glide so pitch starts moving immediately
		// even if there is a small gap between note-off and note-on.
		peeked_note_t peeked = self->ev_buf.peek_next_note_on();
		if (peeked.found) {
			self->glide.pre_target_glide(peeked.key, peeked.channel);
		}
		int resolved_channel = 0;
		if (self->glide.note_off(
			static_cast<int>(ne->key),
			static_cast<int>(ne->channel),
			&resolved_channel)) {
			// Only emit MIDI note-off when all notes are released, and
			// release the sounding note (the one the synth actually knows
			// about), not the finger that just lifted.
			if (self->glide.note_count <= 0 && self->sounding_key >= 0) {
				emit_midi_message(
					out,
					hdr->time,
					static_cast<uint8_t>(0x80u | static_cast<unsigned>(self->sounding_channel)),
					clamp_midi_data(self->sounding_key),
					0);
				self->sounding_key     = -1;
				self->sounding_channel = -1;
			}
		}
		break;
	}

	case CLAP_EVENT_NOTE_CHOKE: {
		const auto *ne = reinterpret_cast<const clap_event_note_t *>(hdr);
		int resolved_channel = 0;
		if (self->glide.note_off(
			static_cast<int>(ne->key),
			static_cast<int>(ne->channel),
			&resolved_channel)) {
			if (self->glide.note_count <= 0 && self->sounding_key >= 0) {
				emit_midi_message(
					out,
					hdr->time,
					static_cast<uint8_t>(0x80u | static_cast<unsigned>(self->sounding_channel)),
					clamp_midi_data(self->sounding_key),
					0);
				self->sounding_key     = -1;
				self->sounding_channel = -1;
			}
		}
		break;
	}

	case CLAP_EVENT_MIDI: {
		const auto *me = reinterpret_cast<const clap_event_midi_t *>(hdr);
		uint8_t status  = me->data[0] & 0xF0u;
		uint8_t channel = me->data[0] & 0x0Fu;
		uint8_t key     = me->data[1];
		uint8_t vel     = me->data[2];

		if (status == 0x90u && vel > 0) {
			GlideState prev_state = self->glide.state;
			self->glide.note_on(static_cast<int>(key), static_cast<int>(channel));

			if (self->glide.state == GlideState::GLIDING &&
			    (prev_state == GlideState::HELD || prev_state == GlideState::GLIDING)) {
				// Legato: bend-only, suppress the note-on pass-through.
				break;
			}

			self->sounding_key     = static_cast<int>(key);
			self->sounding_channel = static_cast<int>(channel);
			out->try_push(out, hdr);
		} else if (status == 0x80u || (status == 0x90u && vel == 0)) {
			peeked_note_t peeked = self->ev_buf.peek_next_note_on();
			if (peeked.found) {
				self->glide.pre_target_glide(peeked.key, peeked.channel);
			}
			self->glide.note_off(static_cast<int>(key), static_cast<int>(channel));

			if (self->glide.note_count <= 0 && self->sounding_key >= 0) {
				// Emit note-off for the sounding key.
				emit_midi_message(
					out,
					hdr->time,
					static_cast<uint8_t>(0x80u | static_cast<unsigned>(self->sounding_channel)),
					clamp_midi_data(self->sounding_key),
					0);
				self->sounding_key     = -1;
				self->sounding_channel = -1;
			}
		} else {
			// Other MIDI messages (CC, aftertouch, etc.) pass through.
			out->try_push(out, hdr);
		}
		break;
	}

	case CLAP_EVENT_PARAM_VALUE: {
		// param value events that arrive inside the audio stream (automation).
		const auto *pe = reinterpret_cast<const clap_event_param_value_t *>(hdr);
		apply_param_value(self, pe->param_id, pe->value);
		break;
	}

	default:
		out->try_push(out, hdr);
		break;
	}
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

static bool plugin_init(const clap_plugin_t *p) {
	auto *self = PitchBendPlugin::from_clap(p);

	self->host_params = static_cast<const clap_host_params_t *>(
		self->host->get_extension(self->host, CLAP_EXT_PARAMS));
	self->host_latency = static_cast<const clap_host_latency_t *>(
		self->host->get_extension(self->host, CLAP_EXT_LATENCY));

	return true;
}

static void plugin_destroy(const clap_plugin_t *p) {
	auto *self = PitchBendPlugin::from_clap(p);
	delete self;
	delete p;
}

static bool plugin_activate(
	const clap_plugin_t *p,
	double sample_rate,
	uint32_t /*min_frames*/,
	uint32_t /*max_frames*/)
{
	auto *self        = PitchBendPlugin::from_clap(p);
	self->sample_rate = sample_rate;
	self->active      = true;
	self->block_start_sample  = 0;
	self->last_bend_value     = 8192;
	self->bend_decimate_counter = 0;
	self->sounding_key        = -1;
	self->sounding_channel    = -1;
	self->was_playing         = false;

	double lookahead_ms = self->params[static_cast<uint32_t>(ParamId::LOOKAHEAD_MS)]
		.load(std::memory_order_relaxed);
	uint32_t delay_samples = static_cast<uint32_t>(lookahead_ms * sample_rate / 1000.0);
	self->ev_buf.reset(delay_samples);

	self->glide.activate(sample_rate);
	return true;
}

static void plugin_deactivate(const clap_plugin_t *p) {
	auto *self   = PitchBendPlugin::from_clap(p);
	self->active = false;
}

static bool plugin_start_processing(const clap_plugin_t *) { return true; }
static void plugin_stop_processing(const clap_plugin_t *)  {}

static void plugin_reset(const clap_plugin_t *p) {
	auto *self = PitchBendPlugin::from_clap(p);
	self->glide.reset();
	double lookahead_ms = self->params[static_cast<uint32_t>(ParamId::LOOKAHEAD_MS)]
		.load(std::memory_order_relaxed);
	uint32_t delay = static_cast<uint32_t>(lookahead_ms * self->sample_rate / 1000.0);
	self->ev_buf.reset(delay);
	self->block_start_sample  = 0;
	self->last_bend_value     = 8192;
	self->bend_decimate_counter = 0;
	self->sounding_key        = -1;
	self->sounding_channel    = -1;
	self->was_playing         = false;
}

// ─── Process ──────────────────────────────────────────────────────────────────

static clap_process_status plugin_process(
	const clap_plugin_t *p,
	const clap_process_t *proc)
{
	auto *self = PitchBendPlugin::from_clap(p);
	if (!self->active) return CLAP_PROCESS_SLEEP;

	const uint32_t N = proc->frames_count;

	// ── Transport-aware glide reset ──────────────────────────────────────────
	// When playback starts (or restarts after a stop/pause), reset the glide
	// engine so the first note doesn't glide from a stale pitch.
	{
		bool is_playing = false;
		if (proc->transport) {
			is_playing = (proc->transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
		}
		if (is_playing && !self->was_playing) {
			self->glide.reset();
			self->last_bend_value       = 8192;
			self->bend_decimate_counter = 0;
			self->sounding_key          = -1;
			self->sounding_channel      = -1;
		}
		self->was_playing = is_playing;
	}

	// Sync parameter values into the glide engine.
	sync_params_to_engine(self);

	// ── Enqueue all incoming events ───────────────────────────────────────────
	// CLAP_EVENT_PARAM_VALUE is processed immediately (no delay needed).
	// All other events (notes, MIDI) go into the lookahead delay buffer.
	{
		uint32_t n = proc->in_events->size(proc->in_events);
		for (uint32_t i = 0; i < n; i++) {
			const clap_event_header_t *ev = proc->in_events->get(proc->in_events, i);
			if (ev->space_id == CLAP_CORE_EVENT_SPACE_ID &&
			    ev->type == CLAP_EVENT_PARAM_VALUE) {
				const auto *pe = reinterpret_cast<const clap_event_param_value_t *>(ev);
				apply_param_value(self, pe->param_id, pe->value);
			} else {
				uint64_t abs = self->block_start_sample + static_cast<uint64_t>(ev->time);
				self->ev_buf.push(ev, abs);
			}
		}
	}

	// ── Process ready (aged) events ───────────────────────────────────────────
	// These have been held in the buffer for at least delay_samples, giving
	// the lookahead window time to accumulate subsequent note events.
	{
		auto ready = self->ev_buf.pop_ready(self->block_start_sample);
		for (auto &entry : ready) {
			const auto *hdr = reinterpret_cast<const clap_event_header_t *>(entry.bytes.data());
			handle_event(self, hdr, proc->out_events);
		}
	}

	// ── Pitch bend interpolation loop ─────────────────────────────────────────
	for (uint32_t frame = 0; frame < N; frame++) {
		bool settled = false;
		int  bend    = self->glide.advance(&settled);

		self->bend_decimate_counter++;

		// Always emit on glide completion to guarantee the channel returns
		// to centre pitch (bend = 8192).
		bool do_emit = settled;

		// During an active glide, emit periodically if the value changed.
		if (!do_emit &&
		    self->glide.state == GlideState::GLIDING &&
		    self->bend_decimate_counter % BEND_DECIMATE_PERIOD == 0 &&
		    bend != self->last_bend_value) {
			do_emit = true;
		}

		// If we have just gone IDLE or HELD after a glide, emit 8192 once
		// to return the channel to centre pitch.
		if (!do_emit &&
		    self->glide.state != GlideState::GLIDING &&
		    self->last_bend_value != 8192) {
			bend    = 8192;
			do_emit = true;
		}

		if (do_emit) {
			emit_pitch_bend(proc->out_events, frame, self->glide.active_channel, bend);
			self->last_bend_value = bend;
		}
	}

	self->block_start_sample += N;

	return CLAP_PROCESS_CONTINUE;
}

// ─── on_main_thread ───────────────────────────────────────────────────────────

static void plugin_on_main_thread(const clap_plugin_t *p) {
	auto *self = PitchBendPlugin::from_clap(p);
	// Notify host that reported latency has changed.
	if (self->notify_latency.exchange(false, std::memory_order_acquire)) {
		if (self->host_latency) {
			self->host_latency->changed(self->host);
		}
	}
}

// ─── Extension: PARAMS ───────────────────────────────────────────────────────

static uint32_t params_count(const clap_plugin_t *) {
	return PARAM_COUNT;
}

static bool params_get_info(
	const clap_plugin_t *,
	uint32_t index,
	clap_param_info_t *info)
{
	if (index >= PARAM_COUNT) return false;
	const auto &pi = PARAM_INFO[index];

	std::memset(info, 0, sizeof(*info));
	info->id            = static_cast<clap_id>(pi.id);
	info->flags         = CLAP_PARAM_IS_AUTOMATABLE;
	if (pi.is_stepped)  info->flags |= CLAP_PARAM_IS_STEPPED;
	info->min_value     = pi.min_value;
	info->max_value     = pi.max_value;
	info->default_value = pi.default_value;
	info->cookie        = nullptr;
	std::snprintf(info->name,   sizeof(info->name),   "%s", pi.name);
	std::snprintf(info->module, sizeof(info->module),  "");
	return true;
}

static bool params_get_value(const clap_plugin_t *p, clap_id id, double *value) {
	auto *self = PitchBendPlugin::from_clap(p);
	if (id >= PARAM_COUNT) return false;
	*value = self->params[id].load(std::memory_order_relaxed);
	return true;
}

static bool params_value_to_text(
	const clap_plugin_t *,
	clap_id id,
	double value,
	char *buf,
	uint32_t size)
{
	switch (static_cast<ParamId>(id)) {
	case ParamId::GLIDE_TIME_MS:
		std::snprintf(buf, size, "%.0f ms", value);
		return true;
	case ParamId::GLIDE_MODE:
		std::snprintf(buf, size, "%s", value >= 0.5 ? "Legato" : "Always");
		return true;
	case ParamId::CURVE:
		if      (value < 0.5) std::snprintf(buf, size, "Linear");
		else if (value < 1.5) std::snprintf(buf, size, "Exponential");
		else                  std::snprintf(buf, size, "Sigmoid");
		return true;
	case ParamId::LOOKAHEAD_MS:
		std::snprintf(buf, size, "%.1f ms", value);
		return true;
	case ParamId::PITCH_BEND_RANGE:
		std::snprintf(buf, size, "%.0f st", value);
		return true;
	default:
		return false;
	}
}

static bool params_text_to_value(
	const clap_plugin_t *,
	clap_id /*id*/,
	const char *text,
	double *value)
{
	char *end = nullptr;
	*value    = std::strtod(text, &end);
	return (end != text);
}

static void params_flush(
	const clap_plugin_t *p,
	const clap_input_events_t *in,
	const clap_output_events_t *out)
{
	auto *self = PitchBendPlugin::from_clap(p);

	// Apply incoming param-value events from the host (e.g. automation).
	uint32_t n = in->size(in);
	for (uint32_t i = 0; i < n; i++) {
		const clap_event_header_t *ev = in->get(in, i);
		if (ev->space_id == CLAP_CORE_EVENT_SPACE_ID &&
		    ev->type == CLAP_EVENT_PARAM_VALUE) {
			const auto *pe = reinterpret_cast<const clap_event_param_value_t *>(ev);
			apply_param_value(self, pe->param_id, pe->value);
		}
	}

	// Push any GUI-driven param changes to the host as output events.
	// Each value change must be bracketed by gesture begin/end so the host
	// commits the change (records automation, updates its UI, etc.).
	uint32_t dirty = self->dirty_params.exchange(0, std::memory_order_acquire);
	for (uint32_t i = 0; i < PARAM_COUNT; i++) {
		if (!(dirty & (1u << i))) continue;

		double v = self->params[i].load(std::memory_order_relaxed);

		clap_event_param_gesture_t gesture;
		std::memset(&gesture, 0, sizeof(gesture));
		gesture.header.size     = sizeof(gesture);
		gesture.header.time     = 0;
		gesture.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		gesture.header.flags    = 0;
		gesture.param_id        = static_cast<clap_id>(i);

		gesture.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
		out->try_push(out, &gesture.header);

		clap_event_param_value_t value_ev;
		std::memset(&value_ev, 0, sizeof(value_ev));
		value_ev.header.size     = sizeof(value_ev);
		value_ev.header.time     = 0;
		value_ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		value_ev.header.type     = CLAP_EVENT_PARAM_VALUE;
		value_ev.header.flags    = 0;
		value_ev.param_id        = static_cast<clap_id>(i);
		value_ev.cookie          = nullptr;
		value_ev.note_id         = -1;
		value_ev.port_index      = -1;
		value_ev.channel         = -1;
		value_ev.key             = -1;
		value_ev.value           = v;
		out->try_push(out, &value_ev.header);

		gesture.header.type = CLAP_EVENT_PARAM_GESTURE_END;
		out->try_push(out, &gesture.header);
	}
}

static const clap_plugin_params_t PLUGIN_PARAMS = {
	params_count,
	params_get_info,
	params_get_value,
	params_value_to_text,
	params_text_to_value,
	params_flush,
};

// ─── Extension: STATE ────────────────────────────────────────────────────────

static bool state_save(const clap_plugin_t *p, const clap_ostream_t *stream) {
	auto *self = PitchBendPlugin::from_clap(p);
	double values[PARAM_COUNT];
	for (uint32_t i = 0; i < PARAM_COUNT; i++) {
		values[i] = self->params[i].load(std::memory_order_relaxed);
	}
	int64_t written = stream->write(stream, values, sizeof(values));
	return written == static_cast<int64_t>(sizeof(values));
}

static bool state_load(const clap_plugin_t *p, const clap_istream_t *stream) {
	auto *self = PitchBendPlugin::from_clap(p);
	double values[PARAM_COUNT];
	int64_t read = stream->read(stream, values, sizeof(values));
	if (read != static_cast<int64_t>(sizeof(values))) return false;

	for (uint32_t i = 0; i < PARAM_COUNT; i++) {
		const auto &pi = PARAM_INFO[i];
		double v = values[i];
		if (v < pi.min_value) v = pi.min_value;
		if (v > pi.max_value) v = pi.max_value;
		self->params[i].store(v, std::memory_order_relaxed);
	}
	return true;
}

static const clap_plugin_state_t PLUGIN_STATE = {
	state_save,
	state_load,
};

// ─── Extension: LATENCY ──────────────────────────────────────────────────────

static uint32_t latency_get(const clap_plugin_t *p) {
	auto *self = PitchBendPlugin::from_clap(p);
	double lookahead_ms = self->params[static_cast<uint32_t>(ParamId::LOOKAHEAD_MS)]
		.load(std::memory_order_relaxed);
	return static_cast<uint32_t>(lookahead_ms * self->sample_rate / 1000.0);
}

static const clap_plugin_latency_t PLUGIN_LATENCY = {
	latency_get,
};

// ─── Extension: AUDIO PORTS ──────────────────────────────────────────────────
// This is a MIDI-only effect — declare zero audio ports.

static uint32_t audio_ports_count(const clap_plugin_t *, bool /*is_input*/) {
	return 0;
}

static bool audio_ports_get(
	const clap_plugin_t *,
	uint32_t /*index*/,
	bool /*is_input*/,
	clap_audio_port_info_t * /*info*/)
{
	return false;
}

static const clap_plugin_audio_ports_t PLUGIN_AUDIO_PORTS = {
	audio_ports_count,
	audio_ports_get,
};

// ─── Extension: NOTE PORTS ───────────────────────────────────────────────────

static uint32_t note_ports_count(const clap_plugin_t *, bool /*is_input*/) {
	return 1;
}

static bool note_ports_get(
	const clap_plugin_t *,
	uint32_t index,
	bool is_input,
	clap_note_port_info_t *info)
{
	if (index != 0) return false;
	std::memset(info, 0, sizeof(*info));
	info->id                 = 0;
	info->supported_dialects = is_input
		? (CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI)
		: CLAP_NOTE_DIALECT_MIDI;
	info->preferred_dialect  = CLAP_NOTE_DIALECT_MIDI;
	std::snprintf(info->name, sizeof(info->name), "Notes");
	return true;
}

static const clap_plugin_note_ports_t PLUGIN_NOTE_PORTS = {
	note_ports_count,
	note_ports_get,
};

// ─── Extension: GUI ──────────────────────────────────────────────────────────

#ifndef PITCH_BEND_NO_GUI

static bool gui_is_api_supported(
	const clap_plugin_t *,
	const char *api,
	bool is_floating)
{
	return !is_floating && std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
}

static bool gui_get_preferred_api(
	const clap_plugin_t *,
	const char **api,
	bool *is_floating)
{
	*api        = CLAP_WINDOW_API_COCOA;
	*is_floating = false;
	return true;
}

static bool gui_create(const clap_plugin_t *p, const char *api, bool is_floating) {
	if (is_floating || std::strcmp(api, CLAP_WINDOW_API_COCOA) != 0) return false;
	auto *self = PitchBendPlugin::from_clap(p);
	if (self->gui) return true; // already created
	self->gui = PluginGui::create(self);
	return self->gui != nullptr;
}

static void gui_destroy(const clap_plugin_t *p) {
	auto *self = PitchBendPlugin::from_clap(p);
	if (!self->gui) return;
	self->gui->destroy();
	self->gui = nullptr;
}

static bool gui_set_scale(const clap_plugin_t *, double) { return false; }

static bool gui_get_size(const clap_plugin_t *p, uint32_t *width, uint32_t *height) {
	auto *self = PitchBendPlugin::from_clap(p);
	if (!self->gui) return false;
	self->gui->get_logical_size(width, height);
	return true;
}

static bool gui_can_resize(const clap_plugin_t *) { return false; }

static bool gui_get_resize_hints(const clap_plugin_t *, clap_gui_resize_hints_t *hints) {
	std::memset(hints, 0, sizeof(*hints));
	return false;
}

static bool gui_adjust_size(const clap_plugin_t *, uint32_t *, uint32_t *) {
	return false;
}

static bool gui_set_size(const clap_plugin_t *p, uint32_t width, uint32_t height) {
	auto *self = PitchBendPlugin::from_clap(p);
	if (!self->gui) return false;
	self->gui->set_logical_size(width, height);
	return true;
}

static bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *window) {
	auto *self = PitchBendPlugin::from_clap(p);
	if (!self->gui) return false;
	return self->gui->set_parent(window->cocoa);
}

static bool gui_set_transient(const clap_plugin_t *, const clap_window_t *) {
	return false;
}

static void gui_suggest_title(const clap_plugin_t *, const char *) {}

static bool gui_show(const clap_plugin_t *p) {
	auto *self = PitchBendPlugin::from_clap(p);
	if (!self->gui) return false;
	self->gui->show();
	self->gui->start_internal_timer();
	return true;
}

static bool gui_hide(const clap_plugin_t *p) {
	auto *self = PitchBendPlugin::from_clap(p);
	if (!self->gui) return false;
	self->gui->stop_internal_timer();
	self->gui->hide();
	return true;
}

static const clap_plugin_gui_t PLUGIN_GUI = {
	gui_is_api_supported,
	gui_get_preferred_api,
	gui_create,
	gui_destroy,
	gui_set_scale,
	gui_get_size,
	gui_can_resize,
	gui_get_resize_hints,
	gui_adjust_size,
	gui_set_size,
	gui_set_parent,
	gui_set_transient,
	gui_suggest_title,
	gui_show,
	gui_hide,
};
#endif // PITCH_BEND_NO_GUI

// ─── get_extension ───────────────────────────────────────────────────────────

static const void *plugin_get_extension(const clap_plugin_t *, const char *id) {
	if (std::strcmp(id, CLAP_EXT_PARAMS)      == 0) return &PLUGIN_PARAMS;
	if (std::strcmp(id, CLAP_EXT_STATE)       == 0) return &PLUGIN_STATE;
	if (std::strcmp(id, CLAP_EXT_LATENCY)     == 0) return &PLUGIN_LATENCY;
	if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &PLUGIN_AUDIO_PORTS;
	if (std::strcmp(id, CLAP_EXT_NOTE_PORTS)  == 0) return &PLUGIN_NOTE_PORTS;
#ifndef PITCH_BEND_NO_GUI
	if (std::strcmp(id, CLAP_EXT_GUI)         == 0) return &PLUGIN_GUI;
#endif
	return nullptr;
}

// ─── Plugin factory ───────────────────────────────────────────────────────────

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *) {
	return 1;
}

static const clap_plugin_descriptor_t *factory_get_plugin_descriptor(
	const clap_plugin_factory_t *,
	uint32_t index)
{
	return (index == 0) ? &PLUGIN_DESC : nullptr;
}

static const clap_plugin_t *factory_create_plugin(
	const clap_plugin_factory_t *,
	const clap_host_t *host,
	const char *plugin_id)
{
	if (!clap_version_is_compatible(host->clap_version)) return nullptr;
	if (std::strcmp(plugin_id, PLUGIN_DESC.id) != 0)     return nullptr;

	auto *self   = new PitchBendPlugin(host);
	auto *plugin = new clap_plugin_t;
	std::memset(plugin, 0, sizeof(*plugin));
	plugin->desc             = &PLUGIN_DESC;
	plugin->plugin_data      = self;
	plugin->init             = plugin_init;
	plugin->destroy          = plugin_destroy;
	plugin->activate         = plugin_activate;
	plugin->deactivate       = plugin_deactivate;
	plugin->start_processing = plugin_start_processing;
	plugin->stop_processing  = plugin_stop_processing;
	plugin->reset            = plugin_reset;
	plugin->process          = plugin_process;
	plugin->get_extension    = plugin_get_extension;
	plugin->on_main_thread   = plugin_on_main_thread;
	return plugin;
}

static const clap_plugin_factory_t PLUGIN_FACTORY = {
	factory_get_plugin_count,
	factory_get_plugin_descriptor,
	factory_create_plugin,
};

// ─── Entry point ──────────────────────────────────────────────────────────────

static bool entry_init(const char * /*plugin_path*/) { return true; }
static void entry_deinit(void) {}

static const void *entry_get_factory(const char *factory_id) {
	if (std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &PLUGIN_FACTORY;
	return nullptr;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
	CLAP_VERSION_INIT,
	entry_init,
	entry_deinit,
	entry_get_factory,
};
