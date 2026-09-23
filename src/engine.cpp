#include "engine.h"

#include "session.h"
#include "timeline.h"
#include "thread-role.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <thread>

namespace nch {
namespace {

clap_beattime toBeatTime(double beats) {
	return static_cast<clap_beattime>(beats * CLAP_BEATTIME_FACTOR);
}

clap_sectime toSecTime(double seconds) {
	return static_cast<clap_sectime>(seconds * CLAP_SECTIME_FACTOR);
}

// Writes the device output from a source of `sources` channels, read through
// `sample(channel, frame)`. A mono source feeds every output; an output the
// source has no channel for is silence.
template <typename Sample>
void writeOutput(const Engine::BlockIo &io, uint32_t frames, uint32_t sources, Sample sample) {
	if (io.interleavedOutput == nullptr)
		return;
	const uint32_t outputs = io.interleavedOutputChannels;
	for (uint32_t frame = 0; frame < frames; ++frame) {
		for (uint32_t channel = 0; channel < outputs; ++channel) {
			const uint32_t source = sources == 1 ? 0 : channel;
			io.interleavedOutput[frame * outputs + channel] = source < sources ? sample(source, frame) : 0.0f;
		}
	}
}

// With nothing processing, the device output is its input.
void passThrough(const Engine::BlockIo &io, uint32_t frames, const float *input) {
	const uint32_t channels = input != nullptr ? io.interleavedInputChannels : 0;
	writeOutput(io, frames, channels,
	            [&](uint32_t channel, uint32_t frame) { return input[frame * channels + channel]; });
}

} // namespace

Engine::Engine(Session &session) : session_(session) {}

bool Engine::start(std::string &error) {
	// Room for a block's events, so filling the list on the audio thread does
	// not allocate.
	inEvents_.reserve(1024, 64 * 1024);
	outEvents_.reserve(1024, 64 * 1024);
	if (!session_.isLoaded()) {
		error = "no plug-in loaded";
		return false;
	}
	// Already processing: rebuilding the buffers now would pull them out from
	// under a block on the device thread.
	if (isRunning())
		return true;
	if (!session_.isActive() && !session_.activate(session_.sampleRate(), 1, session_.blockSize(), error))
		return false;
	// Buffers are shaped by the port layout, which may only change while the
	// plug-in is inactive, so building them after activation is safe.
	buffers_.build(session_.instance(), session_.blockSize());
	if (!session_.instance().startProcessing(error))
		return false;
	sleeping_.store(false, std::memory_order_release);
	tailRemaining_ = 0;
	bypassMix_ = isBypassed() ? 1.0f : 0.0f;
	running_.store(true, std::memory_order_release);
	return true;
}

bool Engine::blockHasInput(uint32_t frames) const {
	// "until the next event or variation in audio input": either wakes it.
	return !inEvents_.empty() || !buffers_.inputsQuiet(frames);
}

void Engine::applyProcessStatus(int32_t status, uint32_t frames, bool hadInput) {
	lastStatus_.store(status, std::memory_order_relaxed);
	bool sleep = false;
	switch (status) {
	case CLAP_PROCESS_SLEEP:
		// "no more processing is required, until the next event or variation
		// in audio input."
		sleep = !hadInput;
		break;
	case CLAP_PROCESS_CONTINUE_IF_NOT_QUIET:
		// "keep processing if the output is not quiet."
		sleep = !hadInput && buffers_.outputsQuiet(frames);
		break;
	case CLAP_PROCESS_TAIL: {
		// "Rely upon the plugin's tail to determine if the plugin should
		// continue to process." Input restarts the tail; silence runs it down.
		if (hadInput) {
			tailRemaining_ = 0;
			break;
		}
		if (tailRemaining_ == 0) {
			const auto *tail = session_.pluginExtension<clap_plugin_tail_t>(CLAP_EXT_TAIL);
			const uint32_t declared = tail != nullptr && tail->get != nullptr ? tail->get(session_.plugin()) : 0;
			// "Any value greater or equal to INT32_MAX implies infinite tail."
			// UINT64_MAX stands in for never.
			tailRemaining_ = declared >= static_cast<uint32_t>(INT32_MAX) ? UINT64_MAX : std::max<uint64_t>(declared, 1);
		}
		if (tailRemaining_ != UINT64_MAX) {
			tailRemaining_ = frames >= tailRemaining_ ? 0 : tailRemaining_ - frames;
			sleep = tailRemaining_ == 0;
		}
		break;
	}
	default:
		tailRemaining_ = 0;
		break;
	}
	if (!sleep)
		return;
	// A sleeping plug-in is one the host has stopped processing: the same
	// [audio-thread] pair that brackets every run of blocks, so what the
	// plug-in sees is a stop, and later a start, not a gap.
	session_.instance().stopProcessing();
	sleeping_.store(true, std::memory_order_release);
}

void Engine::stop() {
	if (!isRunning())
		return;
	// No new block starts once this is false; one already inside process()
	// is waited for, so stop_processing never overlaps it.
	running_.store(false, std::memory_order_release);
	if (acquireAudioExclusion()) {
		session_.instance().stopProcessing();
		releaseAudioExclusion();
	} else {
		session_.validator().error("clap_plugin.process", "a block did not finish in time for stop_processing");
		session_.instance().stopProcessing();
	}
}

bool Engine::acquireAudioExclusion() {
	// Said before trying, so a device block that loses the race knows it lost
	// to the host between blocks and not to a second audio thread.
	hostExclusive_.store(true, std::memory_order_release);
	// A block is short; a few hundred milliseconds is far longer than any
	// legitimate one and short enough that a stuck plug-in still fails fast.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
	for (;;) {
		bool expected = false;
		if (insideProcess_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
			return true;
		if (std::chrono::steady_clock::now() >= deadline) {
			hostExclusive_.store(false, std::memory_order_release);
			return false;
		}
		std::this_thread::yield();
	}
}

void Engine::releaseAudioExclusion() {
	insideProcess_.store(false, std::memory_order_release);
	hostExclusive_.store(false, std::memory_order_release);
}

bool Engine::runAsAudioThread(const std::function<void()> &work) {
	if (!acquireAudioExclusion())
		return false;
	{
		ScopedThreadRole role(ThreadRole::Audio);
		work();
	}
	releaseAudioExclusion();
	return true;
}

void Engine::resetPlayhead() {
	// "clap_process.steady_time may jump backward" only across reset(), so the
	// plug-in has to be told; otherwise anything deriving time from the deltas
	// sees a negative one. reset is [audio-thread], and not concurrent with
	// process(), so it waits its turn with the device callback.
	// The input file rewinds with it, under the same guard as its reader.
	const clap_plugin_t *plugin = session_.isActive() ? session_.plugin() : nullptr;
	const bool reset = runAsAudioThread([this, plugin] {
		if (plugin != nullptr)
			plugin->reset(plugin);
		inputPosition_ = 0;
		inputPositionShown_.store(0, std::memory_order_relaxed);
	});
	if (!reset && plugin != nullptr)
		session_.validator().error("clap_plugin.reset", "a block did not finish in time; reset was skipped");
	playhead_ = 0;
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
	// Built before the lock is taken, so the copy and the allocation happen
	// off the critical section the audio thread wants.
	ScheduledEvent scheduled;
	scheduled.frame = frame;
	scheduled.bytes.resize(event->size);
	std::memcpy(scheduled.bytes.data(), event, event->size);

	wakeRequested_.store(true, std::memory_order_release);
	std::lock_guard<std::mutex> lock(scheduleMutex_);
	// Inserted in place rather than appended and re-sorted: the list is
	// already ordered, so this is a walk rather than a sort, and it holds the
	// lock for a fraction as long.
	const auto position = std::upper_bound(
	    schedule_.begin(), schedule_.end(), frame,
	    [](uint64_t at, const ScheduledEvent &existing) { return at < existing.frame; });
	schedule_.insert(position, std::move(scheduled));
}

size_t Engine::scheduledCount() const {
	std::lock_guard<std::mutex> lock(scheduleMutex_);
	return schedule_.size();
}

bool Engine::setInput(AudioData input, bool loop, std::string path) {
	// Swapped between blocks, never under one; the old file is freed here on
	// the calling thread rather than on the audio thread.
	if (input.frameCount() == 0) {
		input = {};
		path.clear();
	}
	const uint64_t frames = input.frameCount();
	const double sampleRate = input.sampleRate;
	if (!acquireAudioExclusion())
		return false;
	std::swap(input_, input);
	inputPosition_ = 0;
	inputPositionShown_.store(0, std::memory_order_relaxed);
	inputLoop_.store(loop, std::memory_order_release);
	inputPlaying_.store(frames != 0, std::memory_order_release);
	releaseAudioExclusion();
	inputPath_ = std::move(path);
	inputFrames_ = frames;
	inputSampleRate_ = sampleRate;
	return true;
}

bool Engine::clearInput() {
	return setInput({}, false, {});
}

bool Engine::seekInput(uint64_t frame) {
	if (!acquireAudioExclusion())
		return false;
	inputPosition_ = std::min<uint64_t>(frame, input_.frameCount());
	inputPositionShown_.store(inputPosition_, std::memory_order_relaxed);
	releaseAudioExclusion();
	return true;
}

void Engine::advanceInput(uint32_t frames) {
	if (input_.channels.empty() || !inputPlaying_.load(std::memory_order_acquire))
		return;
	const uint64_t length = input_.frameCount();
	inputPosition_ += frames;
	if (inputLoop_.load(std::memory_order_acquire)) {
		inputPosition_ %= length;
	} else if (inputPosition_ >= length) {
		inputPosition_ = 0;
		inputPlaying_.store(false, std::memory_order_release);
	}
	inputPositionShown_.store(inputPosition_, std::memory_order_relaxed);
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

void Engine::scheduleParamMod(clap_id paramId, void *cookie, double amount, int16_t port, int16_t channel,
                              int16_t key, int32_t noteId, uint64_t delayFrames) {
	clap_event_param_mod_t event{};
	event.header.size = sizeof(event);
	event.header.time = 0;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = CLAP_EVENT_PARAM_MOD;
	event.header.flags = 0;
	event.param_id = paramId;
	event.cookie = cookie;
	event.note_id = noteId;
	event.port_index = port;
	event.channel = channel;
	event.key = key;
	event.amount = amount;
	scheduleAfter(&event.header, delayFrames);
}

void Engine::noteOn(int16_t port, int16_t channel, int16_t key, double velocity, int32_t noteId, uint64_t delayFrames) {
	// "A note-on event with a '-1' for port, channel or key is invalid": the
	// host will not commit one, whatever it was asked.
	if (port < 0 || channel < 0 || key < 0)
		return;
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
	// Taking this lock is the one place the audio thread could be made to
	// wait for the main thread, so it tries rather than blocks.
	std::unique_lock<std::mutex> lock(scheduleMutex_, std::try_to_lock);
	if (!lock.owns_lock()) {
		missedCollections_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
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

void Engine::buildTransportEvent() {
	transportEvent_ = {};
	transportEvent_.header.size = sizeof(transportEvent_);
	transportEvent_.header.time = 0;
	transportEvent_.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	transportEvent_.header.type = CLAP_EVENT_TRANSPORT;
	transportEvent_.header.flags = 0;

	uint32_t flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
	                 CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
	flags &= ~transport_.suppressedFlags;
	if (transport_.playing)
		flags |= CLAP_TRANSPORT_IS_PLAYING;
	if (transport_.recording)
		flags |= CLAP_TRANSPORT_IS_RECORDING;
	if (transport_.loopActive)
		flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
	transportEvent_.flags = flags;

	// A field whose flag is not set carries a value no plug-in could use, so
	// one that reads it without checking shows up in the output immediately
	// rather than months later.
	const bool hasTempo = (flags & CLAP_TRANSPORT_HAS_TEMPO) != 0;
	const bool hasBeats = (flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0;
	const bool hasSeconds = (flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) != 0;
	const bool hasTimeSig = (flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) != 0;

	transportEvent_.tempo = hasTempo ? transport_.tempo : std::nan("");
	transportEvent_.tempo_inc = hasTempo ? 0.0 : std::nan("");
	transportEvent_.tsig_num = hasTimeSig ? transport_.timeSigNumerator : UINT16_MAX;
	transportEvent_.tsig_denom = hasTimeSig ? transport_.timeSigDenominator : 0;
	transportEvent_.song_pos_beats = hasBeats ? toBeatTime(transport_.songBeats) : INT64_MIN;
	transportEvent_.song_pos_seconds = hasSeconds ? toSecTime(transport_.songSeconds) : INT64_MIN;

	if (hasBeats) {
		const double barBeats = beatsPerBar(transport_.timeSigNumerator, transport_.timeSigDenominator);
		transportEvent_.bar_start = toBeatTime(barStart(transport_.songBeats, barBeats));
		transportEvent_.bar_number = barNumber(transport_.songBeats, barBeats);
	} else {
		transportEvent_.bar_start = INT64_MIN;
		transportEvent_.bar_number = INT32_MIN;
	}
	if (transport_.loopActive) {
		transportEvent_.loop_start_beats = toBeatTime(transport_.loopStartBeats);
		transportEvent_.loop_end_beats = toBeatTime(transport_.loopEndBeats);
		transportEvent_.loop_start_seconds = toSecTime(transport_.loopStartBeats * 60.0 / transport_.tempo);
		transportEvent_.loop_end_seconds = toSecTime(transport_.loopEndBeats * 60.0 / transport_.tempo);
	} else {
		// No loop means no loop points, rather than stale ones.
		transportEvent_.loop_start_beats = INT64_MAX;
		transportEvent_.loop_end_beats = INT64_MIN;
		transportEvent_.loop_start_seconds = INT64_MAX;
		transportEvent_.loop_end_seconds = INT64_MIN;
	}
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
	// A muted input is no input, whichever the source.
	const bool inputMuted = inputMuted_.load(std::memory_order_acquire);
	const float *deviceInput = inputMuted ? nullptr : io.interleavedInput;

	// "the host must guarantee that single plugin instance will not be two
	// audio-threads at the same time." A render typed at the prompt while a
	// device stream is live would be exactly that.
	bool expected = false;
	if (!insideProcess_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		// The host holds the guard between blocks to stop, reset or swap the
		// input file; a device block arriving then is early, not a second
		// audio thread. Either way it is silence: which input it should hear
		// is exactly what may be changing.
		if (isRunning() && !hostExclusive_.load(std::memory_order_acquire))
			session_.validator().error("clap_plugin.process",
			                           "two threads tried to process the same plug-in at once");
		passThrough(io, frames, nullptr);
		return CLAP_PROCESS_ERROR;
	}
	// A loaded file stands in for the device whether or not it is playing;
	// paused or muted, the input is silence.
	const bool fromFile = !input_.channels.empty();
	const bool fileAudible = fromFile && !inputMuted && inputPlaying_.load(std::memory_order_acquire);
	const bool loop = inputLoop_.load(std::memory_order_acquire);

	// Nothing processing: the host is a wire from its input to its output,
	// not a mute. Checked under the guard, so a stop() that lands in between
	// cannot let the plug-in be called.
	if (!isRunning() || !session_.isLoaded()) {
		if (fileAudible)
			writeOutput(io, frames, input_.channelCount(), [this, loop](uint32_t channel, uint32_t frame) {
				const uint64_t length = input_.frameCount();
				const uint64_t index = loop ? (inputPosition_ + frame) % length : inputPosition_ + frame;
				return index < length ? input_.channels[channel][index] : 0.0f;
			});
		else
			passThrough(io, frames, fromFile ? nullptr : deviceInput);
		advanceInput(frames);
		insideProcess_.store(false, std::memory_order_release);
		return CLAP_PROCESS_ERROR;
	}

	markBlockStart();
	// Never more than the plug-in was activated for: it allocated for
	// max_frames_count and nothing more.
	const uint32_t blockFrames = std::min(frames, session_.blockSize());
	if (blockFrames != frames)
		session_.validator().warn("clap_plugin.process",
		                          "a block larger than the activated maximum was clamped");

	// A muted file keeps moving, the way a tape would; a paused one does not.
	buffers_.silence(blockFrames);
	if (fileAudible)
		buffers_.fillMainInput(input_, inputPosition_, blockFrames, loop);
	else if (!fromFile && deviceInput != nullptr)
		buffers_.writeMainInput(deviceInput, blockFrames, io.interleavedInputChannels);
	advanceInput(blockFrames);
	collectBlockEvents(blockFrames);
	outEvents_.clear();
	buildTransportEvent();

	const bool hadInput = blockHasInput(blockFrames);
	if (sleeping_.load(std::memory_order_acquire)) {
		if (hadInput || wakeRequested_.exchange(false, std::memory_order_acq_rel)) {
			std::string error;
			if (!session_.instance().startProcessing(error)) {
				session_.validator().error("clap_plugin.start_processing", "refused when waking: " + error);
				insideProcess_.store(false, std::memory_order_release);
				return CLAP_PROCESS_ERROR;
			}
			sleeping_.store(false, std::memory_order_release);
			tailRemaining_ = 0;
		} else {
			// Asleep and nothing to hear: the block is silence and the plug-in
			// is not called, which is the whole point of the status it gave.
			sleptBlocks_.fetch_add(1, std::memory_order_relaxed);
			if (io.interleavedOutput != nullptr)
				std::memset(io.interleavedOutput, 0,
				            static_cast<size_t>(frames) * io.interleavedOutputChannels * sizeof(float));
			if (io.collected != nullptr)
				buffers_.appendMainOutput(*io.collected, blockFrames);
			playhead_ += blockFrames;
			advanceTransport(blockFrames);
			insideProcess_.store(false, std::memory_order_release);
			return CLAP_PROCESS_SLEEP;
		}
	}
	wakeRequested_.store(false, std::memory_order_relaxed);

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

	if (checkBlocks_)
		check_.before(buffers_, blockFrames);

	int32_t status = CLAP_PROCESS_ERROR;
	{
		ScopedThreadRole role(ThreadRole::Audio);
		status = session_.plugin()->process(session_.plugin(), &process);
	}

	if (checkBlocks_) {
		// Reported through the validator, so a problem found here reads
		// alongside everything else the host noticed.
		for (const auto &problem : check_.after(buffers_, blockFrames, outEvents_))
			session_.validator().error("clap_plugin.process", problem);
	}
	if (status == CLAP_PROCESS_ERROR) {
		// "Processing failed. The output buffer must be discarded." Writing it
		// anyway would put whatever the plug-in left behind into a file or a
		// speaker.
		session_.validator().error("clap_plugin.process", "returned CLAP_PROCESS_ERROR");
		buffers_.silence(blockFrames);
	}

	session_.absorbOutputEvents(outEvents_, playhead_);
	if (status != CLAP_PROCESS_ERROR)
		applyProcessStatus(status, blockFrames, hadInput);
	const float bypassTarget = bypassed_.load(std::memory_order_acquire) ? 1.0f : 0.0f;
	if (bypassMix_ != 0.0f || bypassTarget != 0.0f) {
		buffers_.mixMainInputIntoOutput(blockFrames, bypassMix_, bypassTarget);
		bypassMix_ = bypassTarget;
	}
	if (io.collected != nullptr)
		buffers_.appendMainOutput(*io.collected, blockFrames);
	if (io.interleavedOutput != nullptr) {
		buffers_.readMainOutput(io.interleavedOutput, blockFrames, io.interleavedOutputChannels);
		// The device asked for more than the plug-in was activated for; the
		// rest of its buffer is silence rather than whatever was there.
		if (blockFrames < frames)
			std::memset(io.interleavedOutput + static_cast<size_t>(blockFrames) * io.interleavedOutputChannels, 0,
			            static_cast<size_t>(frames - blockFrames) * io.interleavedOutputChannels * sizeof(float));
	}
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
	if (isRunning()) {
		// Already processing: the next block carries whatever is queued.
		return true;
	}
	buffers_.build(session_.instance(), session_.blockSize());
	if (!session_.instance().startProcessing(error))
		return false;
	running_.store(true, std::memory_order_release);
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
