#pragma once

#include <clap/events.h>
#include <cstdint>
#include <cstring>
#include <vector>

// A heap copy of one CLAP event, tagged with an absolute sample offset.
struct stored_event_t {
	uint64_t             abs_offset; // absolute sample position from plugin start
	std::vector<uint8_t> bytes;      // raw copy of clap_event_header_t + body
};

// Key/channel pair returned by peek_next_note_on.
struct peeked_note_t {
	bool found;
	int  key;
	int  channel;
};

// ─── EventBuffer ──────────────────────────────────────────────────────────────
//
// Ring buffer that introduces a configurable delay (in samples) to incoming
// CLAP events.  Events are not made available for processing until they have
// aged by delay_samples.  This gives the glide engine a lookahead window:
// when a note-off is being processed the next note-on is already visible in
// the buffer.
//
// The declared plugin latency must equal delay_samples so the host compensates
// the audio output timestamp.
//
struct EventBuffer {
	// (Re)configure the delay.  Existing buffered events are kept; the new
	// delay is applied on the next pop_ready call.
	void set_delay(uint32_t delay_samples);

	// Fully reset: clear all buffered events and set delay.
	void reset(uint32_t delay_samples);

	// Copy ev and enqueue at abs_offset = block_start + event.time.
	void push(const clap_event_header_t *ev, uint64_t abs_offset);

	// Remove and return all events whose abs_offset + delay <= current_block_start.
	// The returned list is sorted by abs_offset.
	std::vector<stored_event_t> pop_ready(uint64_t current_block_start);

	// Look at — but do not remove — the first NOTE_ON (either CLAP or MIDI)
	// that is still buffered (not yet ready).  Returns {false, -1, 0} if none.
	peeked_note_t peek_next_note_on() const;

	uint32_t delay_samples() const { return _delay; }

private:
	uint32_t                    _delay = 0;
	std::vector<stored_event_t> _buf;  // kept sorted by abs_offset
};
