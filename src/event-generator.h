// Generating the event streams a plug-in has to survive.
//
// A plug-in that only ever sees tidy input is barely tested. These generators
// produce the streams that find bugs: notes in every dialect the port accepts,
// wildcards in the addressing tuple, note-offs for notes that were never on,
// parameter changes several times per block, and values at and beyond the ends
// of their own ranges.
//
// Everything is driven from a seeded Random, so a failure reproduces exactly.
#pragma once

#include "event-list.h"
#include "note-encoding.h"
#include "random.h"

#include <clap/clap.h>

#include <vector>

namespace nch {

// A note as the generator remembers it, so it never ends one that is not
// playing unless it is asked to.
struct ActiveNote {
	int16_t port = 0;
	int16_t channel = 0;
	int16_t key = 0;
	int32_t noteId = -1;
};

class NoteGenerator {
public:
	NoteGenerator(Random &random, NoteEncoding encoding) : random_(random), encoding_(encoding) {}

	// Deliberately break the rules: note-offs for notes that were never on,
	// the same note started twice, expressions for notes that have ended.
	void setInconsistent(bool inconsistent) { inconsistent_ = inconsistent; }
	// Replace each of port, channel, key and note id with -1 at this
	// probability, independently. A wildcard matches every voice in that part
	// of the tuple, and plug-ins routinely mishandle it.
	void setWildcardChance(double chance) { wildcardChance_ = chance; }
	// Whether two note-ons for the same key may overlap, which is only legal
	// when clap.voice-info says so.
	void setAllowOverlap(bool allow) { allowOverlap_ = allow; }

	// Appends one block's worth of events, spread across `frames`.
	void fillBlock(EventList &events, uint32_t frames, uint32_t count);

	const std::vector<ActiveNote> &sounding() const { return sounding_; }
	// Ends every note still playing, so a test can leave the plug-in quiet.
	void releaseAll(EventList &events);
	void reset() { sounding_.clear(); }

private:
	void emitNoteOn(EventList &events, uint32_t time);
	void emitNoteOff(EventList &events, uint32_t time);
	void emitExpression(EventList &events, uint32_t time);
	void emitMidi(EventList &events, uint32_t time);
	int16_t wildcardOr(int16_t value);

	Random &random_;
	NoteEncoding encoding_;
	std::vector<ActiveNote> sounding_;
	bool inconsistent_ = false;
	bool allowOverlap_ = false;
	double wildcardChance_ = 0.0;
	int32_t nextNoteId_ = 1;
};

// How a parameter value is chosen.
enum class ParamValueStyle {
	Anywhere,  // uniform across the range
	Bounds,    // exactly the minimum or the maximum
	Beyond,    // outside the range, which a plug-in must clamp rather than trust
};

class ParamFuzzer {
public:
	ParamFuzzer(Random &random, std::vector<clap_param_info_t> params) : random_(random), params_(std::move(params)) {}

	void setStyle(ParamValueStyle style) { style_ = style; }
	// Send a null cookie instead of the one the plug-in gave out. A plug-in
	// that dereferences it without checking crashes here and nowhere else.
	void setNullCookies(bool nullCookies) { nullCookies_ = nullCookies; }

	// One value event per parameter, all at the same frame.
	void fillBlock(EventList &events, uint32_t time);
	// A full sweep of every parameter every `interval` samples, carrying the
	// cursor across block boundaries so the stream is continuous.
	void fillSampleAccurate(EventList &events, uint32_t frames, uint32_t interval, uint32_t &cursor);
	// Modulation rather than automation, optionally addressed at a voice.
	void fillModulation(EventList &events, uint32_t time, const std::vector<ActiveNote> &voices);

	double chooseValue(const clap_param_info_t &info);
	const std::vector<clap_param_info_t> &params() const { return params_; }

private:
	Random &random_;
	std::vector<clap_param_info_t> params_;
	ParamValueStyle style_ = ParamValueStyle::Anywhere;
	bool nullCookies_ = false;
};

} // namespace nch
