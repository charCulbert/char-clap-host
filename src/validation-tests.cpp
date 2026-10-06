// The tests themselves.
//
// Ids match clap-validator's where the test is the same, so the two reports can
// be compared line for line. Tests whose id has no counterpart there are ones
// neither reference tool performs -- the modulation family in particular, which
// clap-validator names but does not actually send.
#include "engine.h"
#include "event-generator.h"
#include "process-check.h"
#include "session.h"
#include "state-stream.h"
#include "validation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace nch {
namespace {

std::string quote(const std::string &text) {
	return "\"" + text + "\"";
}

// The parameters with every flag in `mustHave` and none in `mustLack`.
std::vector<clap_param_info_t> parameters(Session &session, uint32_t mustHave = 0, uint32_t mustLack = 0) {
	std::vector<clap_param_info_t> found;
	const auto *params = session.pluginExtension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
	if (params == nullptr || params->count == nullptr || params->get_info == nullptr)
		return found;
	const uint32_t count = params->count(session.plugin());
	for (uint32_t i = 0; i < count; ++i) {
		clap_param_info_t info{};
		if (params->get_info(session.plugin(), i, &info) && (info.flags & mustHave) == mustHave &&
		    (info.flags & mustLack) == 0)
			found.push_back(info);
	}
	return found;
}

bool readValue(Session &session, clap_id id, double &value) {
	const auto *params = session.pluginExtension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
	if (params == nullptr || params->get_value == nullptr)
		return false;
	return params->get_value(session.plugin(), id, &value);
}

// The current value of each parameter that will report one.
std::map<clap_id, double> snapshot(Session &session, const std::vector<clap_param_info_t> &params) {
	std::map<clap_id, double> values;
	for (const auto &info : params) {
		double value = 0.0;
		if (readValue(session, info.id, value))
			values[info.id] = value;
	}
	return values;
}

// Two parameter values are the same if a stepped parameter rounds to the same
// step, or a continuous one agrees to within a ten-thousandth of its range.
bool sameValue(const clap_param_info_t &info, double a, double b) {
	if ((info.flags & CLAP_PARAM_IS_STEPPED) != 0)
		return std::llround(a) == std::llround(b);
	const double range = info.max_value - info.min_value;
	return std::fabs(a - b) <= (range > 0.0 ? range * 1e-4 : 1e-4);
}

// Turns on the per-block checks and enters processing.
bool startChecked(TestContext &context) {
	Session &session = context.session();
	session.engine().setProcessChecking(true);
	std::string error;
	if (!session.engine().start(error)) {
		context.fail("could not start processing: " + error);
		return false;
	}
	return true;
}

// Fails on the first error the host has noticed. Returns false if it did.
bool noViolations(TestContext &context) {
	for (const auto &violation : context.session().validator().violations()) {
		if (violation.severity == Severity::Error) {
			context.fail(violation.where + ": " + violation.message);
			return false;
		}
	}
	return true;
}

// Processes one block and reports the first thing wrong with it. Returns false
// if anything was.
bool processOneChecked(TestContext &context, uint32_t frames,
                       const char *failure = "process() returned CLAP_PROCESS_ERROR") {
	if (context.session().engine().processBlock(frames, nullptr) == CLAP_PROCESS_ERROR) {
		context.fail(failure);
		return false;
	}
	return noViolations(context);
}

// Runs `blocks` blocks, checking each one, and reports the first thing wrong.
void processChecked(TestContext &context, uint32_t blocks, uint32_t frames) {
	if (!startChecked(context))
		return;
	for (uint32_t block = 0; block < blocks && !context.failed(); ++block) {
		if (context.session().engine().processBlock(frames, nullptr) == CLAP_PROCESS_ERROR) {
			context.fail("process() returned CLAP_PROCESS_ERROR");
			return;
		}
	}
	// Anything the host noticed while processing counts too.
	noViolations(context);
}

// Hands a generated block to the plug-in at the times the generator chose.
void scheduleAll(Session &session, const EventList &events) {
	for (uint32_t i = 0; i < events.size(); ++i) {
		const clap_event_header_t *header = events.at(i);
		session.engine().scheduleAfter(header, header->time);
	}
}

// The dialect the plug-in's note input accepts, or nothing if it has none.
bool noteInputEncoding(Session &session, NoteEncoding &encoding) {
	const auto *ports = session.pluginExtension<clap_plugin_note_ports_t>(CLAP_EXT_NOTE_PORTS);
	if (ports == nullptr || ports->count == nullptr || ports->count(session.plugin(), true) == 0)
		return false;
	encoding = session.engine().noteEncoding(0);
	return true;
}

// --- descriptor -----------------------------------------------------------

void descriptorConsistency(TestContext &context) {
	Session &session = context.session();
	const clap_plugin_descriptor_t *fromInstance = session.descriptor();
	if (fromInstance == nullptr) {
		context.fail("the created plug-in has no descriptor");
		return;
	}
	const clap_plugin_descriptor_t *fromFactory = session.bundle().findPlugin(fromInstance->id, 0);
	if (fromFactory == nullptr) {
		context.fail("the factory does not list the plug-in it created");
		return;
	}
	const auto same = [](const char *a, const char *b) {
		return (a == nullptr && b == nullptr) || (a != nullptr && b != nullptr && std::strcmp(a, b) == 0);
	};
	if (!same(fromFactory->id, fromInstance->id) || !same(fromFactory->name, fromInstance->name) ||
	    !same(fromFactory->vendor, fromInstance->vendor) || !same(fromFactory->version, fromInstance->version))
		context.fail("the descriptor from the factory differs from the one on the created plug-in");
}

void featuresCategories(TestContext &context) {
	const clap_plugin_descriptor_t *descriptor = context.session().descriptor();
	static const char *categories[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
	                                   CLAP_PLUGIN_FEATURE_NOTE_EFFECT, CLAP_PLUGIN_FEATURE_NOTE_DETECTOR,
	                                   CLAP_PLUGIN_FEATURE_ANALYZER};
	for (const char *const *feature = descriptor->features; feature != nullptr && *feature != nullptr; ++feature)
		for (const char *category : categories)
			if (std::strcmp(*feature, category) == 0)
				return;
	context.fail("the plug-in declares no category feature; a host cannot tell what it is");
}

void featuresDuplicates(TestContext &context) {
	const clap_plugin_descriptor_t *descriptor = context.session().descriptor();
	std::set<std::string> seen;
	for (const char *const *feature = descriptor->features; feature != nullptr && *feature != nullptr; ++feature)
		if (!seen.insert(*feature).second)
			context.fail("the feature " + quote(*feature) + " is declared more than once");
}

// --- factory --------------------------------------------------------------

void queryNonexistentFactory(TestContext &context) {
	// A plug-in returning its plugin factory for any id at all breaks every
	// factory CLAP adds in future.
	for (int attempt = 0; attempt < 10; ++attempt) {
		const std::string id = "not-a-factory-" + std::to_string(context.random().next());
		if (context.session().bundle().getFactory(id.c_str()) != nullptr) {
			context.fail("the plug-in returned a factory for " + quote(id) +
			             ", so it is answering every id rather than the ones it knows");
			return;
		}
	}
}

void createIdWithTrailingGarbage(TestContext &context) {
	Session &session = context.session();
	const std::string realId = session.descriptor()->id;
	const std::string wrongId = realId + "x1";
	// Loading is how this host creates, so the assertion is that it refuses.
	Session &host = session;
	std::string error;
	if (host.load(context.pluginPath(), wrongId, 0, error)) {
		context.fail("the plug-in created an instance for " + quote(wrongId) +
		             ", an id it does not declare");
	}
	// Put the real plug-in back so unloading behaves.
	host.load(context.pluginPath(), {}, 0, error);
}

// --- parameters -----------------------------------------------------------

void paramInfoConsistency(TestContext &context) {
	const std::vector<clap_param_info_t> params = parameters(context.session());
	if (params.empty()) {
		context.skip("the plug-in has no parameters");
		return;
	}

	std::set<clap_id> ids;
	uint32_t bypassCount = 0;
	for (const auto &info : params) {
		const std::string name = info.name[0] != '\0' ? info.name : "(unnamed)";
		if (info.id == CLAP_INVALID_ID)
			context.fail(name + " uses CLAP_INVALID_ID as its parameter id");
		if (!ids.insert(info.id).second)
			context.fail("parameter id " + std::to_string(info.id) + " is used more than once");
		if (info.min_value > info.max_value)
			context.fail(name + " has a minimum above its maximum");
		if (info.default_value < info.min_value || info.default_value > info.max_value)
			context.fail(name + " has a default outside its own range");
		if ((info.flags & CLAP_PARAM_IS_STEPPED) != 0 &&
		    (info.min_value != std::floor(info.min_value) || info.max_value != std::floor(info.max_value)))
			context.fail(name + " is stepped but its range is not whole numbers");
		if ((info.flags & CLAP_PARAM_IS_BYPASS) != 0) {
			++bypassCount;
			if ((info.flags & CLAP_PARAM_IS_STEPPED) == 0)
				context.fail(name + " is the bypass parameter but is not stepped");
		}
		// A per-voice flag without the flag it qualifies is meaningless.
		const uint32_t perNote = CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID | CLAP_PARAM_IS_AUTOMATABLE_PER_KEY |
		                         CLAP_PARAM_IS_AUTOMATABLE_PER_CHANNEL | CLAP_PARAM_IS_AUTOMATABLE_PER_PORT;
		if ((info.flags & perNote) != 0 && (info.flags & CLAP_PARAM_IS_AUTOMATABLE) == 0)
			context.fail(name + " claims per-voice automation without being automatable at all");
		const uint32_t perNoteMod = CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY |
		                            CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL | CLAP_PARAM_IS_MODULATABLE_PER_PORT;
		if ((info.flags & perNoteMod) != 0 && (info.flags & CLAP_PARAM_IS_MODULATABLE) == 0)
			context.fail(name + " claims per-voice modulation without being modulatable at all");
		if ((info.flags & CLAP_PARAM_IS_READONLY) != 0 &&
		    (info.flags & (CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE)) != 0)
			context.fail(name + " is read-only and yet automatable or modulatable");
	}
	if (bypassCount > 1)
		context.fail("the plug-in declares " + std::to_string(bypassCount) + " bypass parameters; there may be one");
}

void paramDefaultValues(TestContext &context) {
	const std::vector<clap_param_info_t> params = parameters(context.session());
	if (params.empty()) {
		context.skip("the plug-in has no parameters");
		return;
	}
	for (const auto &info : params) {
		double value = 0.0;
		if (!readValue(context.session(), info.id, value)) {
			context.fail(std::string(info.name) + ": get_value failed for a parameter the plug-in listed");
			continue;
		}
		if (!sameValue(info, value, info.default_value))
			context.fail(std::string(info.name) + " starts at " + std::to_string(value) +
			             " but declares a default of " + std::to_string(info.default_value));
	}
}

void paramSetWrongNamespace(TestContext &context) {
	Session &session = context.session();
	const std::vector<clap_param_info_t> params = parameters(session);
	if (params.empty()) {
		context.skip("the plug-in has no parameters");
		return;
	}

	std::map<clap_id, double> before = snapshot(session, params);

	std::string error;
	if (!session.activate(48000.0, 1, 512, error)) {
		context.skip("could not activate: " + error);
		return;
	}
	// A value event in a namespace the host made up. The plug-in must check
	// the space id and ignore it.
	for (const auto &info : params) {
		clap_event_param_value_t event{};
		event.header.size = sizeof(event);
		event.header.space_id = 0xB33F;
		event.header.type = CLAP_EVENT_PARAM_VALUE;
		event.param_id = info.id;
		event.cookie = info.cookie;
		event.note_id = -1;
		event.port_index = -1;
		event.channel = -1;
		event.key = -1;
		event.value = info.min_value + (info.max_value - info.min_value) * 0.75;
		session.engine().scheduleAfter(&event.header, 0);
	}
	processChecked(context, 2, 512);

	for (const auto &info : params) {
		double value = 0.0;
		if (!readValue(session, info.id, value) || before.count(info.id) == 0)
			continue;
		if (!sameValue(info, value, before[info.id]))
			context.fail(std::string(info.name) +
			             " changed in response to an event in an unknown namespace; the plug-in is not checking "
			             "the event's space id");
	}
}

// --- modulation -----------------------------------------------------------
//
// Neither clap-validator nor pluginval sends CLAP_EVENT_PARAM_MOD at all, so
// everything below is untested ground.

void paramModulationDoesNotMoveValue(TestContext &context) {
	Session &session = context.session();
	const std::vector<clap_param_info_t> modulatable = parameters(session, CLAP_PARAM_IS_MODULATABLE);
	if (modulatable.empty()) {
		context.skip("the plug-in has no modulatable parameters");
		return;
	}

	std::string error;
	if (!session.activate(48000.0, 1, 512, error)) {
		context.skip("could not activate: " + error);
		return;
	}

	std::map<clap_id, double> before = snapshot(session, modulatable);
	for (const auto &info : modulatable) {
		// Half the range, which is deliberately enough to be audible.
		const double amount = (info.max_value - info.min_value) * 0.5;
		session.engine().scheduleParamMod(info.id, info.cookie, amount, -1, -1, -1, -1, 0);
	}
	processChecked(context, 4, 512);
	if (context.failed())
		return;

	// The defining property: the value heard is value + modulation, so the
	// value itself must not have moved.
	for (const auto &info : modulatable) {
		double value = 0.0;
		if (!readValue(session, info.id, value) || before.count(info.id) == 0)
			continue;
		if (!sameValue(info, value, before[info.id]))
			context.fail(std::string(info.name) +
			             " changed its reported value in response to modulation; modulation is an offset on top "
			             "of the value, not a change to it");
	}
	context.note("modulated " + std::to_string(modulatable.size()) + " parameters without moving their values");
}

void paramModulationPolyphonic(TestContext &context) {
	Session &session = context.session();
	const uint32_t perVoice = CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY |
	                          CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL | CLAP_PARAM_IS_MODULATABLE_PER_PORT;
	std::vector<clap_param_info_t> poly;
	for (const auto &info : parameters(session, CLAP_PARAM_IS_MODULATABLE))
		if ((info.flags & perVoice) != 0)
			poly.push_back(info);
	if (poly.empty()) {
		context.skip("the plug-in has no per-voice modulatable parameters");
		return;
	}

	std::string error;
	if (!session.activate(48000.0, 1, 512, error)) {
		context.skip("could not activate: " + error);
		return;
	}

	// Three voices, then modulation addressed at one of them, then at a
	// wildcard. A plug-in that mishandles the tuple usually crashes or
	// modulates the wrong voice.
	session.engine().noteOn(0, 0, 60, 0.8, 1, 0);
	session.engine().noteOn(0, 0, 64, 0.8, 2, 0);
	session.engine().noteOn(0, 0, 67, 0.8, 3, 0);
	processChecked(context, 2, 512);
	if (context.failed())
		return;

	for (const auto &info : poly) {
		const double amount = (info.max_value - info.min_value) * 0.25;
		// By note id, by key, by channel, and fully wildcard in turn.
		session.engine().scheduleParamMod(info.id, info.cookie, amount, -1, -1, -1, 2, 0);
		session.engine().scheduleParamMod(info.id, info.cookie, -amount, -1, -1, 64, -1, 128);
		session.engine().scheduleParamMod(info.id, info.cookie, amount, -1, 0, -1, -1, 256);
		session.engine().scheduleParamMod(info.id, info.cookie, 0.0, -1, -1, -1, -1, 384);
	}
	processChecked(context, 4, 512);
	if (!context.failed())
		context.note("accepted per-note-id, per-key, per-channel and wildcard modulation");
}

void paramModulationSurvivesReset(TestContext &context) {
	Session &session = context.session();
	const std::vector<clap_param_info_t> modulatable = parameters(session, CLAP_PARAM_IS_MODULATABLE);
	if (modulatable.empty()) {
		context.skip("the plug-in has no modulatable parameters");
		return;
	}

	std::string error;
	if (!session.activate(48000.0, 1, 512, error)) {
		context.skip("could not activate: " + error);
		return;
	}
	for (const auto &info : modulatable)
		session.engine().scheduleParamMod(info.id, info.cookie, (info.max_value - info.min_value) * 0.5,
		                                  -1, -1, -1, -1, 0);
	processChecked(context, 2, 512);
	if (context.failed())
		return;

	// reset() "clears all buffers, performs a full reset of the processing
	// state and kills all voices", so modulation must not survive it.
	session.engine().resetPlayhead();
	processChecked(context, 2, 512);
	for (const auto &info : modulatable) {
		double value = 0.0;
		if (readValue(session, info.id, value) && !sameValue(info, value, info.default_value) &&
		    (info.flags & CLAP_PARAM_IS_READONLY) == 0) {
			// Only a note, not a failure: a plug-in may legitimately have had
			// its value changed earlier in the test by other means.
			context.note(std::string(info.name) + " reads " + std::to_string(value) + " after reset");
		}
	}
}

// --- processing -----------------------------------------------------------

void processAudioBasic(TestContext &context) {
	Session &session = context.session();
	if (session.pluginExtension<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS) == nullptr) {
		context.skip("the plug-in has no audio ports");
		return;
	}
	std::string error;
	if (!session.activate(48000.0, 1, 512, error)) {
		context.fail("could not activate: " + error);
		return;
	}
	processChecked(context, 5, 512);
}

void processVaryingBlockSizes(TestContext &context) {
	Session &session = context.session();
	// One frame and a couple of primes are where an assumption about block
	// length shows up.
	static const uint32_t sizes[] = {1, 17, 256, 1024, 2027, 4096};
	for (const uint32_t size : sizes) {
		if (context.failed())
			return;
		std::string error;
		session.deactivate();
		if (!session.activate(48000.0, 1, size, error)) {
			context.fail("could not activate at a block size of " + std::to_string(size) + ": " + error);
			return;
		}
		processChecked(context, 3, size);
	}
	context.note("processed at block sizes 1, 17, 256, 1024, 2027 and 4096");
}

void processVaryingSampleRates(TestContext &context) {
	Session &session = context.session();
	// The fractional rates are the point: a plug-in that assumes an integer
	// rate, or one of the usual few, fails here.
	static const double rates[] = {8000.0, 44100.0, 48000.0, 192000.0, 1234.5678, 45678.901};
	for (const double rate : rates) {
		if (context.failed())
			return;
		std::string error;
		session.deactivate();
		if (!session.activate(rate, 1, 256, error)) {
			context.fail("could not activate at " + std::to_string(rate) + " Hz: " + error);
			return;
		}
		processChecked(context, 3, 256);
	}
	context.note("processed at rates from 1234.5678 Hz to 192 kHz");
}

void processRandomBlockSizes(TestContext &context) {
	Session &session = context.session();
	std::string error;
	if (!session.activate(48000.0, 1, 2048, error)) {
		context.fail("could not activate: " + error);
		return;
	}
	if (!session.engine().start(error)) {
		context.fail("could not start processing: " + error);
		return;
	}
	// Mostly random, occasionally a single frame: a plug-in caching state
	// sized to its first block fails on the short ones.
	for (uint32_t block = 0; block < 20 && !context.failed(); ++block) {
		const uint32_t frames = context.random().chance(0.8)
		                            ? static_cast<uint32_t>(context.random().between(2, 2048))
		                            : 1;
		if (session.engine().processBlock(frames, nullptr) == CLAP_PROCESS_ERROR)
			context.fail("process() returned CLAP_PROCESS_ERROR at a block size of " + std::to_string(frames));
	}
}

// --- state ----------------------------------------------------------------

void stateInvalidEmpty(TestContext &context) {
	Session &session = context.session();
	const auto *state = session.pluginExtension<clap_plugin_state_t>(CLAP_EXT_STATE);
	if (state == nullptr || state->load == nullptr) {
		context.skip("the plug-in does not implement clap.state");
		return;
	}
	InputStream empty({});
	if (state->load(session.plugin(), empty.stream()))
		context.warn("the plug-in accepted an empty state as valid, which is likely a bug");
}

void stateInvalidRandom(TestContext &context) {
	Session &session = context.session();
	const auto *state = session.pluginExtension<clap_plugin_state_t>(CLAP_EXT_STATE);
	if (state == nullptr || state->load == nullptr) {
		context.skip("the plug-in does not implement clap.state");
		return;
	}
	// The assertion is that it does not crash; accepting it is merely unwise.
	for (int attempt = 0; attempt < 3; ++attempt) {
		std::vector<uint8_t> noise(64 * 1024);
		for (auto &byte : noise)
			byte = static_cast<uint8_t>(context.random().next());
		InputStream stream(std::move(noise));
		if (state->load(session.plugin(), stream.stream()))
			context.warn("the plug-in accepted 64 KiB of random bytes as valid state");
	}
}

void stateRoundTrip(TestContext &context) {
	Session &session = context.session();
	const auto *state = session.pluginExtension<clap_plugin_state_t>(CLAP_EXT_STATE);
	if (state == nullptr || state->save == nullptr || state->load == nullptr) {
		context.skip("the plug-in does not implement clap.state");
		return;
	}
	const std::vector<clap_param_info_t> params = parameters(session);

	OutputStream saved;
	if (!state->save(session.plugin(), saved.stream())) {
		context.fail("the plug-in refused to save its state");
		return;
	}
	std::map<clap_id, double> before = snapshot(session, params);

	InputStream reloaded(saved.bytes());
	if (!state->load(session.plugin(), reloaded.stream())) {
		context.fail("the plug-in refused to load the state it had just saved");
		return;
	}
	for (const auto &info : params) {
		double value = 0.0;
		if (!readValue(session, info.id, value) || before.count(info.id) == 0)
			continue;
		if (!sameValue(info, value, before[info.id]))
			context.fail(std::string(info.name) + " did not survive a save and load of the plug-in's own state");
	}

	// Saving again must produce the same bytes: a difference means something
	// uninitialised, or an iteration order that is not stable.
	OutputStream again;
	if (state->save(session.plugin(), again.stream()) && again.bytes() != saved.bytes())
		context.warn("saving twice produced different bytes, which usually means uninitialised padding or an "
		             "unstable iteration order");
}

void stateBuffered(TestContext &context) {
	Session &session = context.session();
	const auto *state = session.pluginExtension<clap_plugin_state_t>(CLAP_EXT_STATE);
	if (state == nullptr || state->save == nullptr || state->load == nullptr) {
		context.skip("the plug-in does not implement clap.state");
		return;
	}
	OutputStream saved;
	if (!state->save(session.plugin(), saved.stream())) {
		context.fail("the plug-in refused to save its state");
		return;
	}
	// Seventeen bytes at a time: a plug-in assuming read() fills the buffer it
	// asked for fails here and nowhere else.
	InputStream trickle(saved.bytes(), 17);
	if (!state->load(session.plugin(), trickle.stream()))
		context.fail("the plug-in could not load its own state when the stream returned short reads; read() may "
		             "return fewer bytes than asked for");
}

// --- generated event streams ----------------------------------------------

// Drives `blocks` blocks of generated notes, checking each one.
void runNoteStream(TestContext &context, bool inconsistent, double wildcardChance) {
	Session &session = context.session();
	NoteEncoding encoding;
	if (!noteInputEncoding(session, encoding)) {
		context.skip("the plug-in has no note input");
		return;
	}
	std::string error;
	if (!session.activate(48000.0, 1, 512, error)) {
		context.fail("could not activate: " + error);
		return;
	}

	NoteGenerator generator(context.random(), encoding);
	generator.setInconsistent(inconsistent);
	generator.setWildcardChance(wildcardChance);
	// Overlapping notes are only legal when the plug-in says it can take them.
	const auto *voiceInfo = session.pluginExtension<clap_plugin_voice_info_t>(CLAP_EXT_VOICE_INFO);
	clap_voice_info_t info{};
	if (voiceInfo != nullptr && voiceInfo->get != nullptr && voiceInfo->get(session.plugin(), &info))
		generator.setAllowOverlap((info.flags & CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES) != 0);

	if (!startChecked(context))
		return;
	for (uint32_t block = 0; block < 8 && !context.failed(); ++block) {
		EventList events;
		generator.fillBlock(events, 512, 6);
		scheduleAll(session, events);
		if (!processOneChecked(context, 512))
			return;
	}
	EventList release;
	generator.releaseAll(release, 512);
	scheduleAll(session, release);
	session.engine().processBlock(512, nullptr);
}

void processNoteBasic(TestContext &context) {
	runNoteStream(context, false, 0.0);
}

void processNoteInconsistent(TestContext &context) {
	// Note-offs for notes that were never on, and the same note started twice.
	// A plug-in must survive a host that gets it wrong.
	runNoteStream(context, true, 0.0);
}

void processNoteWildcard(TestContext &context) {
	// Each part of the addressing tuple becomes -1 one time in ten. A wildcard
	// matches every voice in that position.
	runNoteStream(context, false, 0.1);
}

// Drives parameter changes in a given style, checking every block.
void runParamFuzz(TestContext &context, ParamValueStyle style, bool nullCookies, uint32_t interval) {
	Session &session = context.session();
	const std::vector<clap_param_info_t> params = parameters(session, 0, CLAP_PARAM_IS_READONLY);
	if (params.empty()) {
		context.skip("the plug-in has no writable parameters");
		return;
	}

	std::string error;
	if (!session.activate(48000.0, 1, 512, error)) {
		context.fail("could not activate: " + error);
		return;
	}
	ParamFuzzer fuzzer(context.random(), params);
	fuzzer.setStyle(style);
	fuzzer.setNullCookies(nullCookies);

	if (!startChecked(context))
		return;

	uint32_t cursor = 0;
	for (uint32_t block = 0; block < 10 && !context.failed(); ++block) {
		EventList events;
		if (interval == 0)
			fuzzer.fillBlock(events, 0);
		else
			fuzzer.fillSampleAccurate(events, 512, interval, cursor);
		scheduleAll(session, events);
		if (!processOneChecked(context, 512))
			return;
	}
}

void paramFuzzBasic(TestContext &context) {
	runParamFuzz(context, ParamValueStyle::Anywhere, false, 0);
}

void paramFuzzBounds(TestContext &context) {
	runParamFuzz(context, ParamValueStyle::Bounds, false, 0);
}

void paramFuzzBeyond(TestContext &context) {
	// Values outside the range the plug-in declared. It must clamp them rather
	// than trust the host, and neither reference tool sends these.
	runParamFuzz(context, ParamValueStyle::Beyond, false, 0);
}

void paramFuzzSampleAccurate(TestContext &context) {
	// A full sweep of every parameter every ten samples: about fifty sweeps
	// per block.
	runParamFuzz(context, ParamValueStyle::Anywhere, false, 10);
}

void paramFuzzNoCookies(TestContext &context) {
	// The cookie is an optimisation, not a promise. A plug-in that
	// dereferences it without checking crashes here and nowhere else.
	runParamFuzz(context, ParamValueStyle::Anywhere, true, 0);
}

void paramFuzzModulation(TestContext &context) {
	Session &session = context.session();
	const std::vector<clap_param_info_t> modulatable = parameters(session, CLAP_PARAM_IS_MODULATABLE);
	if (modulatable.empty()) {
		context.skip("the plug-in has no modulatable parameters");
		return;
	}

	std::string error;
	if (!session.activate(48000.0, 1, 512, error)) {
		context.fail("could not activate: " + error);
		return;
	}

	NoteEncoding encoding;
	const bool hasNotes = noteInputEncoding(session, encoding);
	NoteGenerator notes(context.random(), encoding);
	ParamFuzzer fuzzer(context.random(), modulatable);

	if (!startChecked(context))
		return;
	for (uint32_t block = 0; block < 10 && !context.failed(); ++block) {
		EventList events;
		if (hasNotes)
			notes.fillBlock(events, 512, 3);
		// Real CLAP_EVENT_PARAM_MOD, addressed at a sounding voice half the
		// time. This is the stream neither reference tool produces.
		fuzzer.fillModulation(events, static_cast<uint32_t>(context.random().below(512)), notes.sounding());
		events.sortByTime();
		scheduleAll(session, events);
		if (!processOneChecked(context, 512))
			return;
	}
	context.note("sent polyphonic modulation alongside a live note stream");
}

void paramSetEvents(TestContext &context) {
	Session &session = context.session();
	const std::vector<clap_param_info_t> params = parameters(session, 0, CLAP_PARAM_IS_READONLY);
	if (params.empty()) {
		context.skip("the plug-in has no writable parameters");
		return;
	}
	const auto *ext = session.pluginExtension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
	if (ext == nullptr || ext->flush == nullptr) {
		context.skip("the plug-in does not implement params.flush");
		return;
	}

	// The same changes sent two ways must land in the same place: through
	// flush while inactive, and through process while active.
	ParamFuzzer fuzzer(context.random(), params);
	EventList wanted;
	fuzzer.fillBlock(wanted, 0);

	EventList out;
	ext->flush(session.plugin(), wanted.input(), out.output());
	std::map<clap_id, double> viaFlush = snapshot(session, params);

	// A second instance, because cookies are per-instance.
	std::string error;
	if (!session.load(context.pluginPath(), {}, 0, error)) {
		context.fail("could not load a second instance: " + error);
		return;
	}
	if (!session.activate(48000.0, 1, 512, error)) {
		context.fail("could not activate: " + error);
		return;
	}
	// The parameters are the new instance's, so the events are rebuilt.
	const std::vector<clap_param_info_t> reloaded = parameters(session, 0, CLAP_PARAM_IS_READONLY);
	for (const auto &info : reloaded) {
		if (viaFlush.count(info.id) == 0)
			continue;
		clap_event_param_value_t event{};
		event.header.size = sizeof(event);
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = CLAP_EVENT_PARAM_VALUE;
		event.param_id = info.id;
		event.cookie = info.cookie;
		event.note_id = -1;
		event.port_index = -1;
		event.channel = -1;
		event.key = -1;
		event.value = viaFlush[info.id];
		session.engine().scheduleAfter(&event.header, 0);
	}
	processChecked(context, 2, 512);
	if (context.failed())
		return;

	for (const auto &info : reloaded) {
		double value = 0.0;
		if (!readValue(session, info.id, value) || viaFlush.count(info.id) == 0)
			continue;
		if (!sameValue(info, value, viaFlush[info.id]))
			context.fail(std::string(info.name) + " landed at " + std::to_string(value) +
			             " through process() but at " + std::to_string(viaFlush[info.id]) +
			             " through flush(); the two routes must agree");
	}
}

// --- transport ------------------------------------------------------------

void transportNull(TestContext &context) {
	Session &session = context.session();
	std::string error;
	if (!session.activate(48000.0, 1, 128, error)) {
		context.fail("could not activate: " + error);
		return;
	}
	// A host without a timeline sends no transport at all, and a plug-in that
	// assumes one is always there falls over here.
	session.engine().transport().send = false;
	NoteEncoding encoding;
	const bool hasNotes = noteInputEncoding(session, encoding);
	NoteGenerator notes(context.random(), encoding);

	if (!startChecked(context))
		return;
	for (uint32_t block = 0; block < 5 && !context.failed(); ++block) {
		EventList events;
		if (hasNotes)
			notes.fillBlock(events, 128, 3);
		scheduleAll(session, events);
		if (session.engine().processBlock(128, nullptr) == CLAP_PROCESS_ERROR)
			context.fail("process() returned CLAP_PROCESS_ERROR with no transport");
	}
	for (const auto &violation : session.validator().violations())
		if (violation.severity == Severity::Error)
			context.fail(violation.where + ": " + violation.message);
}

void transportFuzz(TestContext &context) {
	Session &session = context.session();
	std::string error;
	if (!session.activate(48000.0, 1, 128, error)) {
		context.fail("could not activate: " + error);
		return;
	}
	Transport &transport = session.engine().transport();
	if (!startChecked(context))
		return;

	static const uint32_t optional[] = {CLAP_TRANSPORT_HAS_TEMPO, CLAP_TRANSPORT_HAS_BEATS_TIMELINE,
	                                    CLAP_TRANSPORT_HAS_SECONDS_TIMELINE, CLAP_TRANSPORT_HAS_TIME_SIGNATURE};
	for (uint32_t block = 0; block < 40 && !context.failed(); ++block) {
		// Withhold a different subset each block. The fields behind a
		// withheld flag carry NaN and extremes, so a plug-in that reads
		// without checking produces something the output check catches.
		uint32_t suppressed = 0;
		for (const uint32_t flag : optional)
			if (context.random().chance(0.3))
				suppressed |= flag;
		transport.suppressedFlags = suppressed;
		transport.playing = context.random().chance(0.6);
		transport.recording = context.random().chance(0.2);
		transport.tempo = context.random().between(40.0, 480.0);
		transport.timeSigNumerator = static_cast<uint16_t>(context.random().between(1, 16));
		transport.timeSigDenominator = static_cast<uint16_t>(1u << context.random().below(4));
		// Loops, which clap-validator never sends at all.
		transport.loopActive = context.random().chance(0.4);
		transport.loopStartBeats = context.random().between(0.0, 8.0);
		transport.loopEndBeats = transport.loopStartBeats + context.random().between(0.0, 8.0);
		if (context.random().chance(0.2)) {
			// A seek, including backwards, which a loop or a locate does.
			transport.songBeats = context.random().between(0.0, 240.0);
			transport.songSeconds = transport.songBeats * 60.0 / transport.tempo;
		}

		if (!processOneChecked(context, 128,
		                       "process() returned CLAP_PROCESS_ERROR while the transport was changing"))
			return;
	}
	transport.suppressedFlags = 0;
	context.note("varied tempo, time signature, loop points and which flags were set");
}

// --- interface ------------------------------------------------------------

void guiOpenClose(TestContext &context) {
	Session &session = context.session();
	const auto *gui = session.pluginExtension<clap_plugin_gui_t>(CLAP_EXT_GUI);
	if (gui == nullptr) {
		context.skip("the plug-in has no interface");
		return;
	}
	// Twice, because the second time is the one that finds a plug-in whose
	// teardown does not undo its setup.
	for (int attempt = 0; attempt < 2; ++attempt) {
		std::string error;
		if (!session.gui().open({}, false, error)) {
			context.skip("could not open the interface: " + error);
			return;
		}
		session.gui().close();
	}
	context.note("opened and closed twice");
}

void guiResize(TestContext &context) {
	Session &session = context.session();
	const auto *gui = session.pluginExtension<clap_plugin_gui_t>(CLAP_EXT_GUI);
	if (gui == nullptr || gui->get_size == nullptr) {
		context.skip("the plug-in has no interface");
		return;
	}
	std::string error;
	if (!session.gui().open({}, false, error)) {
		context.skip("could not open the interface: " + error);
		return;
	}

	uint32_t width = 0;
	uint32_t height = 0;
	if (!gui->get_size(session.plugin(), &width, &height)) {
		context.fail("the plug-in could not report the size of the interface it just created");
		session.gui().close();
		return;
	}
	if (width == 0 || height == 0)
		context.fail("the plug-in reports an interface with no area");

	if (gui->can_resize != nullptr && gui->can_resize(session.plugin())) {
		// adjust_size must settle: a size it returns, fed back in, must come
		// back unchanged, or a host dragging a window never converges.
		uint32_t wanted = width + 137;
		uint32_t wantedHeight = height + 71;
		if (gui->adjust_size != nullptr) {
			gui->adjust_size(session.plugin(), &wanted, &wantedHeight);
			uint32_t again = wanted;
			uint32_t againHeight = wantedHeight;
			gui->adjust_size(session.plugin(), &again, &againHeight);
			if (again != wanted || againHeight != wantedHeight)
				context.fail("adjust_size does not settle: a size it returned came back different the second "
				             "time, so a host resizing the window would never converge");
		}
		if (gui->set_size != nullptr && !gui->set_size(session.plugin(), wanted, wantedHeight))
			context.fail("the plug-in refused a size its own adjust_size produced");
	} else {
		context.note("the interface is a fixed size");
	}
	session.gui().close();
}

} // namespace

const std::vector<TestCase> &allTests() {
	static const std::vector<TestCase> cases = {
	    {"descriptor-consistency", "The factory and the created plug-in describe themselves the same way.",
	     descriptorConsistency},
	    {"features-categories", "The plug-in declares what kind of plug-in it is.", featuresCategories},
	    {"features-duplicates", "No feature is declared twice.", featuresDuplicates},
	    {"query-nonexistent-factory", "Asking for a factory that does not exist returns nothing.",
	     queryNonexistentFactory},
	    {"create-id-with-trailing-garbage", "An id the plug-in does not declare creates nothing.",
	     createIdWithTrailingGarbage},
	    {"param-info-consistency", "Parameter ids, ranges and flags agree with the specification.",
	     paramInfoConsistency},
	    {"param-default-values", "Every parameter starts at the default it declares.", paramDefaultValues},
	    {"param-set-wrong-namespace", "Events in an unknown namespace are ignored.", paramSetWrongNamespace},
	    {"param-modulation-value-unchanged",
	     "Modulation offsets the value heard without changing the value itself.",
	     paramModulationDoesNotMoveValue},
	    {"param-modulation-polyphonic", "Modulation addressed at one voice, one key, one channel, and all of them.",
	     paramModulationPolyphonic},
	    {"param-modulation-reset", "Modulation does not survive a reset.", paramModulationSurvivesReset},
	    {"process-audio-basic", "Five ordinary blocks produce usable audio.", processAudioBasic},
	    {"process-varying-block-sizes", "Block sizes from one frame to four thousand.", processVaryingBlockSizes},
	    {"process-varying-sample-rates", "Sample rates including fractional ones.", processVaryingSampleRates},
	    {"process-random-block-sizes", "Block sizes that change every block.", processRandomBlockSizes},
	    {"state-invalid-empty", "An empty state is not mistaken for a valid one.", stateInvalidEmpty},
	    {"state-invalid-random", "Random bytes do not crash the plug-in.", stateInvalidRandom},
	    {"state-round-trip", "State saved and loaded returns the same parameters.", stateRoundTrip},
	    {"state-buffered", "State loads from a stream that returns short reads.", stateBuffered},
	    {"process-note-basic", "A consistent stream of notes and expressions.", processNoteBasic},
	    {"process-note-inconsistent", "Notes ended that never started, and started twice.",
	     processNoteInconsistent},
	    {"process-note-wildcard", "Notes addressed with wildcards in the tuple.", processNoteWildcard},
	    {"param-fuzz-basic", "Every parameter changed every block.", paramFuzzBasic},
	    {"param-fuzz-bounds", "Every parameter at exactly its minimum or maximum.", paramFuzzBounds},
	    {"param-fuzz-beyond", "Values outside the range the plug-in declared.", paramFuzzBeyond},
	    {"param-fuzz-sample-accurate", "Parameters swept many times within a block.", paramFuzzSampleAccurate},
	    {"param-fuzz-no-cookies", "Parameter events carrying no cookie.", paramFuzzNoCookies},
	    {"param-fuzz-modulation", "Polyphonic modulation alongside a live note stream.", paramFuzzModulation},
	    {"param-set-events", "flush() and process() put a parameter in the same place.", paramSetEvents},
	    {"transport-null", "Processing with no transport at all.", transportNull},
	    {"transport-fuzz", "Tempo, metre, loops and withheld flags changing every block.", transportFuzz},
	    {"gui-open-close", "The interface opens and closes twice.", guiOpenClose},
	    {"gui-resize", "The interface reports a size, and adjust_size settles.", guiResize},
	};
	return cases;
}

} // namespace nch
