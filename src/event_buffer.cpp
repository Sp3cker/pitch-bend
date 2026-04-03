#include "event_buffer.h"
#include <algorithm>

void EventBuffer::set_delay(uint32_t delay_samples) {
	_delay = delay_samples;
}

void EventBuffer::reset(uint32_t delay_samples) {
	_delay = delay_samples;
	_buf.clear();
}

void EventBuffer::push(const clap_event_header_t *ev, uint64_t abs_offset) {
	stored_event_t entry;
	entry.abs_offset = abs_offset;
	entry.bytes.resize(ev->size);
	std::memcpy(entry.bytes.data(), ev, ev->size);

	// Insert in sorted order so pop_ready can walk from the front.
	auto pos = std::upper_bound(
		_buf.begin(), _buf.end(), abs_offset,
		[](uint64_t v, const stored_event_t &e) { return v < e.abs_offset; }
	);
	_buf.insert(pos, std::move(entry));
}

std::vector<stored_event_t> EventBuffer::pop_ready(uint64_t current_block_start) {
	// An event at abs_offset A is ready when:
	//   A + delay <= current_block_start
	//   i.e. A <= current_block_start - delay
	// Guard against uint64_t underflow.
	if (current_block_start < static_cast<uint64_t>(_delay)) {
		return {};
	}
	uint64_t threshold = current_block_start - _delay;

	std::vector<stored_event_t> ready;
	auto it = _buf.begin();
	while (it != _buf.end() && it->abs_offset <= threshold) {
		ready.push_back(std::move(*it));
		++it;
	}
	_buf.erase(_buf.begin(), it);
	return ready;
}

peeked_note_t EventBuffer::peek_next_note_on() const {
	for (const auto &entry : _buf) {
		const auto *hdr = reinterpret_cast<const clap_event_header_t *>(entry.bytes.data());
		if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;

		if (hdr->type == CLAP_EVENT_NOTE_ON) {
			const auto *ne = reinterpret_cast<const clap_event_note_t *>(entry.bytes.data());
			return { true, static_cast<int>(ne->key), static_cast<int>(ne->channel) };
		}
		if (hdr->type == CLAP_EVENT_MIDI) {
			const auto *me = reinterpret_cast<const clap_event_midi_t *>(entry.bytes.data());
			uint8_t status = me->data[0] & 0xF0u;
			uint8_t ch     = me->data[0] & 0x0Fu;
			if (status == 0x90u && me->data[2] > 0) {
				return { true, static_cast<int>(me->data[1]), static_cast<int>(ch) };
			}
		}
	}
	return { false, -1, 0 };
}
