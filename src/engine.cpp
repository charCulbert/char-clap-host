#include "engine.h"

#include "session.h"
#include "thread-role.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nch {
namespace {

clap_beattime toBeatTime(double beats) {
	return static_cast<clap_beattime>(beats * CLAP_BEATTIME_FACTOR);
}

clap_sectime toSecTime(double seconds) {
	return static_cast<clap_sectime>(seconds * CLAP_SECTIME_FACTOR);
}

} // namespace

Engine::Engine(Session &session) : session_(session) {}

bool Engine::start(std::string &error) {
	if (!session_.isLoaded()) {
		error = "no plug-in loaded";
		return false;
	}
	if (!session_.isActive() &&
	    !session_.activate(session_.sampleRate(), 1, session_.blockSize(), error))
		return false;
	buffers_.build(session_, session_.blockSize());
	if (!running_) {
		ScopedThreadRole role(ThreadRole::Audio);
		if (!session_.plugin()->start_processing(session_.plugin())) {
			error = "start_processing failed";
			return false;
		}
		running_ = true;
		session_.setProcessing(true);
	}
	return true;
}

void Engine::stop() {
	if (!running_)
		return;
	if (session_.isLoaded()) {
		ScopedThreadRole role(ThreadRole::Audio);
		session_.plugin()->stop_processing(session_.plugin());
	}
	running_ = false;
	session_.setProcessing(false);
}

void Engine::resetPlayhead() {
	playhead_ = 0;
	inputPosition_ = 0;
	transport_.songBeats = 0.0;
	transport_.songSeconds = 0.0;
}

void Engine::scheduleAfter(const clap_event_header_t *event, uint64_t delayFrames) {
	scheduleAt(event, playhead_ + delayFrames);
}

void Engine::scheduleAt(const clap_event_header_t *event, uint64_t frame) {
	if (event == nullptr || event->size < sizeof(clap_event_header_t))
		return;
	ScheduledEvent scheduled;
	scheduled.frame = frame;
	scheduled.bytes.resize(event->size);
	std::memcpy(scheduled.bytes.data(), event, event->size);
	std::lock_guard<std::mutex> lock(scheduleMutex_);
	schedule_.push_back(std::move(scheduled));
	std::stable_sort(schedule_.begin(), schedule_.end(),
	                 [](const ScheduledEvent &a, const ScheduledEvent &b) { return a.frame < b.frame; });
}

void Engine::clearSchedule() {
	std::lock_guard<std::mutex> lock(scheduleMutex_);
	schedule_.clear();
}

size_t Engine::scheduledCount() const {
	std::lock_guard<std::mutex> lock(scheduleMutex_);
	return schedule_.size();
}

void Engine::setInput(AudioData input) {
	input_ = std::move(input);
	inputPosition_ = 0;
}

void Engine::clearInput() {
	input_ = {};
	inputPosition_ = 0;
}

namespace {

clap_event_note_t makeNote(uint16_t type, int16_t port, int16_t channel, int16_t key, double velocity, int32_t noteId) {
	clap_event_note_t event{};
	event.header.size = sizeof(event);
	event.header.time = 0;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = type;
	event.header.flags = 0;
	event.port_index = port;
	event.channel = channel;
	event.key = key;
	event.note_id = noteId;
	event.velocity = velocity;
	return event;
}

bool sameNote(const clap_event_note_t &a, const clap_event_note_t &b) {
	return a.port_index == b.port_index && a.channel == b.channel && a.key == b.key;
}

} // namespace

void Engine::noteOn(int16_t port, int16_t channel, int16_t key, double velocity, int32_t noteId, uint64_t delayFrames) {
	const clap_event_note_t event = makeNote(CLAP_EVENT_NOTE_ON, port, channel, key, velocity, noteId);
	scheduleAfter(&event.header, delayFrames);
	for (auto &active : activeNotes_)
		if (sameNote(active, event))
			return;
	activeNotes_.push_back(event);
}

void Engine::noteOff(int16_t port, int16_t channel, int16_t key, double velocity, int32_t noteId, uint64_t delayFrames) {
	const clap_event_note_t event = makeNote(CLAP_EVENT_NOTE_OFF, port, channel, key, velocity, noteId);
	scheduleAfter(&event.header, delayFrames);
	for (auto it = activeNotes_.begin(); it != activeNotes_.end(); ++it) {
		if (sameNote(*it, event)) {
			activeNotes_.erase(it);
			return;
		}
	}
}

void Engine::allNotesOff(uint64_t delayFrames) {
	const std::vector<clap_event_note_t> sounding = activeNotes_;
	for (const auto &note : sounding)
		noteOff(note.port_index, note.channel, note.key, 0.0, note.note_id, delayFrames);
}

std::vector<clap_event_note_t> Engine::activeNotes() const {
	return activeNotes_;
}

void Engine::collectBlockEvents(uint32_t frames) {
	inEvents_.clear();
	std::lock_guard<std::mutex> lock(scheduleMutex_);
	const uint64_t blockEnd = playhead_ + frames;
	size_t consumed = 0;
	for (const auto &scheduled : schedule_) {
		if (scheduled.frame >= blockEnd)
			break;
		// An event whose time has already passed lands on the first frame
		// rather than being dropped.
		const uint64_t at = scheduled.frame < playhead_ ? playhead_ : scheduled.frame;
		auto *header = reinterpret_cast<clap_event_header_t *>(const_cast<uint8_t *>(scheduled.bytes.data()));
		header->time = static_cast<uint32_t>(at - playhead_);
		inEvents_.push(header);
		++consumed;
	}
	if (consumed != 0)
		schedule_.erase(schedule_.begin(), schedule_.begin() + static_cast<long>(consumed));
	inEvents_.sortByTime();
}

void Engine::buildTransportEvent(uint32_t frames) {
	(void)frames;
	transportEvent_ = {};
	transportEvent_.header.size = sizeof(transportEvent_);
	transportEvent_.header.time = 0;
	transportEvent_.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	transportEvent_.header.type = CLAP_EVENT_TRANSPORT;
	transportEvent_.header.flags = 0;

	uint32_t flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
	                 CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
	if (transport_.playing)
		flags |= CLAP_TRANSPORT_IS_PLAYING;
	if (transport_.recording)
		flags |= CLAP_TRANSPORT_IS_RECORDING;
	if (transport_.loopActive)
		flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
	transportEvent_.flags = flags;

	transportEvent_.tempo = transport_.tempo;
	transportEvent_.tempo_inc = 0.0;
	transportEvent_.tsig_num = transport_.timeSigNumerator;
	transportEvent_.tsig_denom = transport_.timeSigDenominator;
	transportEvent_.song_pos_beats = toBeatTime(transport_.songBeats);
	transportEvent_.song_pos_seconds = toSecTime(transport_.songSeconds);
	transportEvent_.bar_start = toBeatTime(0.0);
	transportEvent_.bar_number = static_cast<int32_t>(transport_.songBeats /
	                                                  std::max(1.0, static_cast<double>(transport_.timeSigNumerator)));
	transportEvent_.loop_start_beats = toBeatTime(transport_.loopStartBeats);
	transportEvent_.loop_end_beats = toBeatTime(transport_.loopEndBeats);
	transportEvent_.loop_start_seconds = toSecTime(transport_.loopStartBeats * 60.0 / transport_.tempo);
	transportEvent_.loop_end_seconds = toSecTime(transport_.loopEndBeats * 60.0 / transport_.tempo);
}

void Engine::advanceTransport(uint32_t frames) {
	if (!transport_.playing)
		return;
	const double seconds = static_cast<double>(frames) / session_.sampleRate();
	transport_.songSeconds += seconds;
	transport_.songBeats += seconds * transport_.tempo / 60.0;
	if (transport_.loopActive && transport_.loopEndBeats > transport_.loopStartBeats &&
	    transport_.songBeats >= transport_.loopEndBeats) {
		const double span = transport_.loopEndBeats - transport_.loopStartBeats;
		transport_.songBeats = transport_.loopStartBeats + std::fmod(transport_.songBeats - transport_.loopStartBeats, span);
		transport_.songSeconds = transport_.songBeats * 60.0 / transport_.tempo;
	}
}

int32_t Engine::processBlock(uint32_t frames, AudioData *output) {
	if (!running_ || !session_.isLoaded())
		return CLAP_PROCESS_ERROR;

	buffers_.silence(frames);
	if (!input_.channels.empty()) {
		buffers_.fillMainInput(input_, inputPosition_, frames);
		inputPosition_ += frames;
	}
	collectBlockEvents(frames);
	outEvents_.clear();
	buildTransportEvent(frames);

	clap_process_t process{};
	process.steady_time = static_cast<int64_t>(playhead_);
	process.frames_count = frames;
	process.transport = transport_.send ? &transportEvent_ : nullptr;
	process.audio_inputs = buffers_.inputs();
	process.audio_outputs = buffers_.outputs();
	process.audio_inputs_count = buffers_.inputPortCount();
	process.audio_outputs_count = buffers_.outputPortCount();
	process.in_events = inEvents_.input();
	process.out_events = outEvents_.output();

	int32_t status = CLAP_PROCESS_ERROR;
	{
		ScopedThreadRole role(ThreadRole::Audio);
		status = session_.plugin()->process(session_.plugin(), &process);
	}
	if (status == CLAP_PROCESS_ERROR)
		session_.validator().error("clap_plugin.process", "returned CLAP_PROCESS_ERROR");

	if (output != nullptr)
		buffers_.appendMainOutput(*output, frames);
	playhead_ += frames;
	advanceTransport(frames);
	return status;
}

int32_t Engine::processInterleaved(const float *input, uint32_t inputChannels, float *output,
                                   uint32_t outputChannels, uint32_t frames) {
	if (!running_ || !session_.isLoaded()) {
		if (output != nullptr)
			std::memset(output, 0, static_cast<size_t>(frames) * outputChannels * sizeof(float));
		return CLAP_PROCESS_ERROR;
	}
	buffers_.silence(frames);
	if (input != nullptr)
		buffers_.writeMainInput(input, frames, inputChannels);
	else if (!input_.channels.empty()) {
		buffers_.fillMainInput(input_, inputPosition_, frames);
		inputPosition_ += frames;
	}
	collectBlockEvents(frames);
	outEvents_.clear();
	buildTransportEvent(frames);

	clap_process_t process{};
	process.steady_time = static_cast<int64_t>(playhead_);
	process.frames_count = frames;
	process.transport = transport_.send ? &transportEvent_ : nullptr;
	process.audio_inputs = buffers_.inputs();
	process.audio_outputs = buffers_.outputs();
	process.audio_inputs_count = buffers_.inputPortCount();
	process.audio_outputs_count = buffers_.outputPortCount();
	process.in_events = inEvents_.input();
	process.out_events = outEvents_.output();

	const int32_t status = session_.plugin()->process(session_.plugin(), &process);
	buffers_.readMainOutput(output, frames, outputChannels);
	playhead_ += frames;
	advanceTransport(frames);
	return status;
}

bool Engine::render(uint64_t frames, AudioData &out, std::string &error) {
	if (!start(error))
		return false;
	out.sampleRate = session_.sampleRate();
	const uint32_t blockSize = session_.blockSize();
	uint64_t remaining = frames;
	while (remaining > 0) {
		const uint32_t block = static_cast<uint32_t>(std::min<uint64_t>(blockSize, remaining));
		processBlock(block, &out);
		remaining -= block;
	}
	return true;
}

} // namespace nch
