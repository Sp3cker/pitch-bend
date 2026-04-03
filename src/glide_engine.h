#pragma once

#include <cmath>
#include <cstring>
#include <cstdint>

#include "params.h"

// ─── GlideEngine ──────────────────────────────────────────────────────────────
//
// Monophonic pitch-glide state machine.  Operates entirely in semitone space
// so that interpolation is perceptually even regardless of interval size.
//
// Threading: all methods are called exclusively from the audio thread.
//
enum class GlideState {
	IDLE,    // No notes held; pitch is frozen at current_pitch.
	HELD,    // One or more notes held; pitch is stable at dst_pitch.
	GLIDING, // Interpolating src_pitch → dst_pitch over glide_samples.
};

struct GlideEngine {
	GlideState state          = GlideState::IDLE;

	double src_pitch          = 60.0; // semitones; start of current glide
	double dst_pitch          = 60.0; // semitones; target of current glide
	double current_pitch      = 60.0; // semitones; live interpolated position

	double elapsed_samples    = 0.0;
	double glide_samples      = 0.0;  // glide_time_ms * sample_rate / 1000

	int    note_count         = 0;
	int    held_notes[16][128] = {};  // reference count per MIDI channel/key
	int    last_note          = -1;
	int    active_channel     = 0;    // MIDI channel of the most recent note-on

	// Parameters — written by sync_params_to_engine on the audio thread.
	GlideMode  glide_mode     = GlideMode::LEGATO;
	CurveShape curve          = CurveShape::LINEAR;
	double     glide_time_ms  = 80.0;
	double     bend_range     = 12.0;
	double     sample_rate    = 44100.0;

	void activate(double sr);
	void reset();
	void update_glide_samples();

	// Called when a note-on event arrives.
	void note_on(int key, int channel);

	// Called when a note-off or choke event arrives.
	// channel = -1 means "first matching channel".
	bool note_off(int key, int channel, int *resolved_channel = nullptr);

	// Pre-target a glide toward an upcoming note that was spotted in the
	// lookahead buffer.  Called just before note_off so the pitch starts
	// moving immediately instead of waiting for the note-on event.
	void pre_target_glide(int key, int channel);

	// Advance state by one sample.
	// Returns a 14-bit pitch bend value: 0..16383, 8192 = centre (no bend).
	// Sets *settled_out = true on the exact frame the glide completes.
	int advance(bool *settled_out);

	private:
		static double apply_curve(double t, CurveShape shape);

		static double clamp01(double v) {
			return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
		}

		int normalize_channel(int channel) const;
		int find_held_channel(int key, int preferred_channel) const;
};
