#include "engine.h"

#include "session.h"
#include "timeline.h"
#include "thread-role.h"

#include <algorithm>
#include <cmath>
#include <algorithm>
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
	if (!session_.isActive() && !session_.activate(session_.sampleRate(), 1, session_.blockSize(), error))
		return false;
	// Buffers are shaped by the port layout, which may only change while the
	// plug-in is inactive, so building them after activation is safe.
	buffers_.build(session_, session_.blockSize());
	if (!session_.instance().startProcessing(error))
		return false;
	running_ = true;
	return true;
}

void Engine::stop() {
	if (!running_)
		return;
	session_.instance().stopProcessing();
	running_ = false;
}

void Engine::resetPlayhead() {
	// "clap_process.steady_time may jump backward" only across reset(), so the
	// plug-in has to be told; otherwise anything deriving time from the deltas
	// sees a negative one.
	if (session_.isActive() && session_.plugin() != nullptr && session_.plugin()->reset != nullptr) {
		ScopedThreadRole role(ThreadRole::Audio);
		session_.plugin()->reset(session_.plugin());
	}
	playhead_ = 0;
	inputPosition_ = 0;
	transport_.songBeats = 0.0;
	transport_.songSeconds = 0.0;
	activeNotes_.clear(); // reset "kills all voices"
}

void Engine::scheduleAfter(const clap_event_header_t *event, uint64_t delayFrames) {
	scheduleAt(event, playhead_ + delayFrames);
}

void Engine::scheduleLive(const clap_event_header_t *event, std::chrono::steady_clock::time_point arrival) {
	using namespace std::chrono;
	const int64_t arrivalNanos = duration_cast<nanoseconds>(arrival.time_since_epoch()).count();
	const int64_t referenceNanos = blockStartNanos_.load(std::memory_order_acquire);
	const uint64_t referenceFrame = blockStartFrame_.load(std::memory_order_acquire);
	scheduleAt(event, frameForArrival(arrivalNanos, referenceNanos, referenceFrame, session_.sampleRate()));
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

clap_event_midi_t makeMidiNote(bool on, int16_t port, int16_t channel, int16_t key, double velocity) {
	clap_event_midi_t event{};
	event.header.size = sizeof(event);
	event.header.time = 0;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = CLAP_EVENT_MIDI;
	event.header.flags = 0;
	event.port_index = static_cast<uint16_t>(port < 0 ? 0 : port);
	event.data[0] = static_cast<uint8_t>((on ? 0x90 : 0x80) | (channel & 0x0F));
	event.data[1] = static_cast<uint8_t>(key & 0x7F);
	event.data[2] = static_cast<uint8_t>(std::lround(std::min(1.0, std::max(0.0, velocity)) * 127.0));
	return event;
}

} // namespace

// A plug-in only receives notes in the dialect its port declares, so the
// choice is made once here and every caller inherits it.
NoteEncoding Engine::noteEncoding(int16_t port) const {
	if (currentThreadRole() != ThreadRole::Main) {
		// clap.note-ports is main-thread only, so a device thread takes the
		// answer the main thread last worked out rather than asking again.
		NoteEncoding encoding;
		encoding.dialect = cachedDialect_.load(std::memory_order_acquire);
		encoding.portIndex = port;
		return encoding;
	}
	return encodingForPort(session_.plugin(),
	                       session_.pluginExtension<clap_plugin_note_ports_t>(CLAP_EXT_NOTE_PORTS), port);
}

void Engine::refreshNoteEncoding() {
	cachedDialect_.store(noteEncoding(0).dialect, std::memory_order_release);
}

NoteTranslation Engine::scheduleMidi(const uint8_t *bytes, uint32_t size, int16_t port, uint32_t flags,
                                     uint64_t delayFrames) {
	NoteTranslation translation = translateMidi(bytes, size, noteEncoding(port), flags);
	size_t offset = 0;
	for (uint32_t i = 0; i < translation.produced; ++i) {
		const auto *header = reinterpret_cast<const clap_event_header_t *>(translation.storage.data() + offset);
		scheduleAfter(header, delayFrames);
		offset += header->size;
	}
	return translation;
}

NoteTranslation Engine::scheduleLiveMidi(const uint8_t *bytes, uint32_t size, int16_t port,
                                         std::chrono::steady_clock::time_point arrival) {
	NoteTranslation translation = translateMidi(bytes, size, noteEncoding(port), CLAP_EVENT_IS_LIVE);
	size_t offset = 0;
	for (uint32_t i = 0; i < translation.produced; ++i) {
		const auto *header = reinterpret_cast<const clap_event_header_t *>(translation.storage.data() + offset);
		scheduleLive(header, arrival);
		offset += header->size;
	}
	return translation;
}

void Engine::noteOn(int16_t port, int16_t channel, int16_t key, double velocity, int32_t noteId, uint64_t delayFrames) {
	const clap_event_note_t event = makeNote(CLAP_EVENT_NOTE_ON, port, channel, key, velocity, noteId);
	if (noteEncoding(port).wantsClapNotes()) {
		scheduleAfter(&event.header, delayFrames);
	} else {
		const clap_event_midi_t midi = makeMidiNote(true, port, channel, key, velocity);
		scheduleAfter(&midi.header, delayFrames);
	}
	for (auto &active : activeNotes_)
		if (sameNote(active, event))
			return;
	activeNotes_.push_back(event);
}

void Engine::noteOff(int16_t port, int16_t channel, int16_t key, double velocity, int32_t noteId, uint64_t delayFrames) {
	const clap_event_note_t event = makeNote(CLAP_EVENT_NOTE_OFF, port, channel, key, velocity, noteId);
	if (noteEncoding(port).wantsClapNotes()) {
		scheduleAfter(&event.header, delayFrames);
	} else {
		const clap_event_midi_t midi = makeMidiNote(false, port, channel, key, velocity);
		scheduleAfter(&midi.header, delayFrames);
	}
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

void Engine::retireNote(int16_t port, int16_t channel, int16_t key) {
	for (auto it = activeNotes_.begin(); it != activeNotes_.end(); ++it) {
		// -1 is a wildcard in note events, so a plug-in may end a whole
		// channel or every voice at once.
		const bool samePort = port < 0 || it->port_index == port;
		const bool sameChannel = channel < 0 || it->channel == channel;
		const bool sameKey = key < 0 || it->key == key;
		if (samePort && sameChannel && sameKey) {
			activeNotes_.erase(it);
			return;
		}
	}
}

void Engine::collectBlockEvents(uint32_t frames) {
	inEvents_.clear();
	std::lock_guard<std::mutex> lock(scheduleMutex_);
	const uint64_t blockEnd = playhead_ + frames;
	size_t consumed = 0;
	for (const auto &scheduled : schedule_) {
		if (scheduled.frame >= blockEnd)
			break;
		auto *header = reinterpret_cast<clap_event_header_t *>(const_cast<uint8_t *>(scheduled.bytes.data()));
		header->time = eventOffsetInBlock(scheduled.frame, playhead_, frames);
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

	const double barBeats = beatsPerBar(transport_.timeSigNumerator, transport_.timeSigDenominator);
	transportEvent_.bar_start = toBeatTime(barStart(transport_.songBeats, barBeats));
	transportEvent_.bar_number = barNumber(transport_.songBeats, barBeats);
	transportEvent_.loop_start_beats = toBeatTime(transport_.loopStartBeats);
	transportEvent_.loop_end_beats = toBeatTime(transport_.loopEndBeats);
	transportEvent_.loop_start_seconds = toSecTime(transport_.loopStartBeats * 60.0 / transport_.tempo);
	transportEvent_.loop_end_seconds = toSecTime(transport_.loopEndBeats * 60.0 / transport_.tempo);
}

void Engine::advanceTransport(uint32_t frames) {
	if (!transport_.playing)
		return;
	const Playhead moved =
	    advancePlayhead({transport_.songBeats, transport_.songSeconds}, frames, session_.sampleRate(),
	                    transport_.tempo, transport_.loopActive, transport_.loopStartBeats,
	                    transport_.loopEndBeats);
	transport_.songBeats = moved.beats;
	transport_.songSeconds = moved.seconds;
}

void Engine::markBlockStart() {
	using namespace std::chrono;
	// Published together so a reader on another thread sees a consistent pair:
	// the frame first, then the clock reading that pins it.
	blockStartFrame_.store(playhead_, std::memory_order_release);
	blockStartNanos_.store(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count(),
	                       std::memory_order_release);
}

int32_t Engine::processBlock(uint32_t frames, AudioData *output) {
	BlockIo io;
	io.collected = output;
	return processBlock(frames, io);
}

int32_t Engine::processBlock(uint32_t frames, const BlockIo &io) {
	if (!running_ || !session_.isLoaded()) {
		if (io.interleavedOutput != nullptr)
			std::memset(io.interleavedOutput,
			            0,
			            static_cast<size_t>(frames) * io.interleavedOutputChannels * sizeof(float));
		return CLAP_PROCESS_ERROR;
	}

	// "the host must guarantee that single plugin instance will not be two
	// audio-threads at the same time." A render typed at the prompt while a
	// device stream is live would be exactly that.
	bool expected = false;
	if (!insideProcess_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		session_.validator().error("clap_plugin.process",
		                           "two threads tried to process the same plug-in at once");
		if (io.interleavedOutput != nullptr)
			std::memset(io.interleavedOutput,
			            0,
			            static_cast<size_t>(frames) * io.interleavedOutputChannels * sizeof(float));
		return CLAP_PROCESS_ERROR;
	}

	markBlockStart();
	// Never more than the plug-in was activated for: it allocated for
	// max_frames_count and nothing more.
	const uint32_t blockFrames = std::min(frames, session_.blockSize());
	if (blockFrames != frames)
		session_.validator().warn("clap_plugin.process",
		                          "a block larger than the activated maximum was clamped");

	buffers_.silence(blockFrames);
	if (io.interleavedInput != nullptr) {
		buffers_.writeMainInput(io.interleavedInput, blockFrames, io.interleavedInputChannels);
	} else if (!input_.channels.empty()) {
		buffers_.fillMainInput(input_, inputPosition_, blockFrames);
		inputPosition_ += blockFrames;
	}
	collectBlockEvents(blockFrames);
	outEvents_.clear();
	buildTransportEvent(blockFrames);

	clap_process_t process{};
	process.steady_time = static_cast<int64_t>(playhead_);
	process.frames_count = blockFrames;
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
	if (status == CLAP_PROCESS_ERROR) {
		// "Processing failed. The output buffer must be discarded." Writing it
		// anyway would put whatever the plug-in left behind into a file or a
		// speaker.
		session_.validator().error("clap_plugin.process", "returned CLAP_PROCESS_ERROR");
		buffers_.silence(blockFrames);
	}

	session_.absorbOutputEvents(outEvents_);
	if (io.collected != nullptr)
		buffers_.appendMainOutput(*io.collected, blockFrames);
	if (io.interleavedOutput != nullptr)
		buffers_.readMainOutput(io.interleavedOutput, blockFrames, io.interleavedOutputChannels);
	playhead_ += blockFrames;
	advanceTransport(blockFrames);

	insideProcess_.store(false, std::memory_order_release);
	return status;
}

int32_t Engine::processInterleaved(const float *input, uint32_t inputChannels, float *output,
                                   uint32_t outputChannels, uint32_t frames) {
	BlockIo io;
	io.interleavedInput = input;
	io.interleavedInputChannels = inputChannels;
	io.interleavedOutput = output;
	io.interleavedOutputChannels = outputChannels;
	return processBlock(frames, io);
}

bool Engine::runSilentBlock(std::string &error) {
	if (!session_.isLoaded()) {
		error = "no plug-in loaded";
		return false;
	}
	if (running_) {
		// Already processing: the next block carries whatever is queued.
		return true;
	}
	buffers_.build(session_, session_.blockSize());
	if (!session_.instance().startProcessing(error))
		return false;
	running_ = true;
	processBlock(session_.blockSize(), nullptr);
	stop();
	return true;
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
