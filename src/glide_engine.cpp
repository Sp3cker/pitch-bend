#include "glide_engine.h"

void GlideEngine::activate(double sr) {
	sample_rate = sr;
	update_glide_samples();
	reset();
}

void GlideEngine::reset() {
	state           = GlideState::IDLE;
	current_pitch   = 60.0;
	src_pitch       = 60.0;
	dst_pitch       = 60.0;
	elapsed_samples = 0.0;
	note_count      = 0;
	last_note       = -1;
	active_channel  = 0;
	std::memset(held_notes, 0, sizeof(held_notes));
}

void GlideEngine::update_glide_samples() {
	glide_samples = glide_time_ms * sample_rate / 1000.0;
}

void GlideEngine::note_on(int key, int channel) {
	if (key < 0 || key > 127) return;

	channel = normalize_channel(channel);
	held_notes[channel][key]++;
	note_count++;
	active_channel = channel;

	bool do_glide = (glide_time_ms > 0.0) &&
		(glide_mode == GlideMode::ALWAYS ||
		 (glide_mode == GlideMode::LEGATO &&
		  (state == GlideState::HELD || state == GlideState::GLIDING)));

	if (do_glide) {
		// Snap src to current interpolated position so mid-glide re-targeting
		// never produces a pitch discontinuity.
		src_pitch       = current_pitch;
		dst_pitch       = static_cast<double>(key);
		elapsed_samples = 0.0;
		state           = GlideState::GLIDING;
	} else {
		// Instant jump — no glide (first note, or legato disabled).
		src_pitch     = static_cast<double>(key);
		dst_pitch     = static_cast<double>(key);
		current_pitch = static_cast<double>(key);
		elapsed_samples = 0.0;
		state         = GlideState::HELD;
	}

	last_note = key;
}

bool GlideEngine::note_off(int key, int channel, int *resolved_channel) {
	if (key < 0 || key > 127) return false;

	const int matched_channel = find_held_channel(key, channel);
	if (matched_channel < 0) return false;

	held_notes[matched_channel][key]--;
	note_count--;
	if (resolved_channel) {
		*resolved_channel = matched_channel;
	}
	if (note_count <= 0) {
		note_count = 0;
		state      = GlideState::IDLE;
		// current_pitch is intentionally not reset: pitch bend stays where it
		// is during the synth's release tail, which sounds natural.
	}
	return true;
}

void GlideEngine::pre_target_glide(int key, int channel) {
	// Called when we see an upcoming note-on in the lookahead buffer while
	// processing a note-off.  Start gliding toward the new note immediately
	// so the transition is smooth even if there is a tiny gap between the
	// note-off and note-on events.
	if (glide_time_ms <= 0.0) return;
	if (glide_mode == GlideMode::LEGATO &&
	    state != GlideState::HELD &&
	    state != GlideState::GLIDING) {
		return;
	}

	active_channel   = normalize_channel(channel);
	src_pitch       = current_pitch;
	dst_pitch       = static_cast<double>(key);
	elapsed_samples = 0.0;
	state           = GlideState::GLIDING;
}

int GlideEngine::advance(bool *settled_out) {
	*settled_out = false;

	if (state == GlideState::GLIDING) {
		elapsed_samples += 1.0;

		double t = (glide_samples > 0.0)
			? clamp01(elapsed_samples / glide_samples)
			: 1.0;

		double shaped = apply_curve(t, curve);
		current_pitch = src_pitch + (dst_pitch - src_pitch) * shaped;

		if (t >= 1.0) {
			current_pitch = dst_pitch;
			state         = GlideState::HELD;
			*settled_out  = true;
		}
	}

	// Pitch bend offset is relative to dst_pitch (the target note that the
	// synth has received).  A negative delta means we are still below it.
	double delta = current_pitch - dst_pitch;
	double norm  = delta / bend_range;
	if (norm < -1.0) norm = -1.0;
	if (norm >  1.0) norm =  1.0;

	int bend = static_cast<int>(norm * 8191.0) + 8192;
	if (bend < 0)     bend = 0;
	if (bend > 16383) bend = 16383;
	return bend;
}

double GlideEngine::apply_curve(double t, CurveShape shape) {
	switch (shape) {
	case CurveShape::LINEAR:
		return t;
	case CurveShape::EXPONENTIAL:
		// Ease-in: slow start, faster finish.
		return t * t;
	case CurveShape::SIGMOID: {
		// Logistic function; maps [0,1] → (~0, ~1) via smooth S.
		double x = 10.0 * (t - 0.5);
		return 1.0 / (1.0 + std::exp(-x));
	}
	}
	return t;
}

int GlideEngine::normalize_channel(int channel) const {
	return (channel >= 0 && channel < 16) ? channel : 0;
}

int GlideEngine::find_held_channel(int key, int preferred_channel) const {
	if (preferred_channel >= 0 && preferred_channel < 16 &&
	    held_notes[preferred_channel][key] > 0) {
		return preferred_channel;
	}

	for (int ch = 0; ch < 16; ch++) {
		if (held_notes[ch][key] > 0) {
			return ch;
		}
	}

	return -1;
}
