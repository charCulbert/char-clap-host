#include "param-motion.h"

#include <algorithm>
#include <cmath>

namespace nch {
namespace {

constexpr double kPi = 3.14159265358979323846;

// One cycle of each shape at phase 0..1, from -1 to 1, starting at 0 and
// rising, so every LFO begins where the parameter already is.
double wave(ParamMotion::Shape shape, double phase) {
	switch (shape) {
	case ParamMotion::Shape::Sine:
		return std::sin(2.0 * kPi * phase);
	case ParamMotion::Shape::Triangle:
		return phase < 0.25 ? 4.0 * phase : phase < 0.75 ? 2.0 - 4.0 * phase : 4.0 * phase - 4.0;
	case ParamMotion::Shape::Square:
		return phase < 0.5 ? 1.0 : -1.0;
	case ParamMotion::Shape::Saw:
		return phase < 0.5 ? 2.0 * phase : 2.0 * phase - 2.0;
	case ParamMotion::Shape::Ramp:
		break;
	}
	return 0.0;
}

} // namespace

double ParamMotion::valueAt(uint64_t frame) const {
	const double elapsed = static_cast<double>(frame - std::min(frame, startFrame));
	if (shape == Shape::Ramp) {
		const double span = static_cast<double>(endFrame - startFrame);
		const double t = span > 0.0 ? std::min(1.0, elapsed / span) : 1.0;
		return from + (to - from) * t;
	}
	double cycles = elapsed * cyclesPerFrame;
	cycles -= std::floor(cycles);
	return centre + depth * wave(shape, cycles);
}

void ParamMotion::emit(uint64_t blockStart, uint32_t frames, EventList &out) {
	if (finished)
		return;
	const uint64_t blockEnd = blockStart + frames;
	auto send = [&](uint64_t frame, double value) {
		if (!modulation) {
			value = std::clamp(value, minValue, maxValue);
			if (stepped)
				value = std::clamp(std::round(value), std::ceil(minValue), std::floor(maxValue));
		}
		const auto offset = static_cast<uint32_t>(frame - blockStart);
		if (modulation) {
			auto event = makeEvent<clap_event_param_mod_t>(CLAP_EVENT_PARAM_MOD, offset);
			event.param_id = paramId;
			event.cookie = cookie;
			event.note_id = noteId;
			event.port_index = port;
			event.channel = channel;
			event.key = key;
			event.amount = value;
			out.push(event);
		} else {
			auto event = makeEvent<clap_event_param_value_t>(CLAP_EVENT_PARAM_VALUE, offset);
			event.param_id = paramId;
			event.cookie = cookie;
			event.note_id = noteId;
			event.port_index = port;
			event.channel = channel;
			event.key = key;
			event.value = value;
			out.push(event);
		}
	};

	// Nothing before the start; a motion whose start has already been played
	// starts with this block.
	nextFrame = std::max({nextFrame, startFrame, std::min(blockStart, endFrame)});
	while (nextFrame < blockEnd && nextFrame < endFrame) {
		send(nextFrame, valueAt(nextFrame));
		nextFrame += stepFrames;
	}
	// Where it leaves the parameter: the ramp's target, or back to the centre
	// with no offset left over. A motion stopped at a frame already played
	// closes at the start of this block.
	const uint64_t closing = std::max(endFrame, blockStart);
	if (closing < blockEnd) {
		send(closing, shape == Shape::Ramp ? to : centre);
		finished = true;
	}
}

void ParamMotion::stopAt(uint64_t frame) {
	endFrame = std::max(frame, startFrame);
	if (shape == Shape::Ramp) {
		// A ramp stopped early stays where it got to.
		to = valueAt(endFrame);
	}
}

bool lfoShapeNamed(const std::string &name, ParamMotion::Shape &shape) {
	if (name == "sine")
		shape = ParamMotion::Shape::Sine;
	else if (name == "triangle")
		shape = ParamMotion::Shape::Triangle;
	else if (name == "square")
		shape = ParamMotion::Shape::Square;
	else if (name == "saw")
		shape = ParamMotion::Shape::Saw;
	else
		return false;
	return true;
}

const char *shapeName(ParamMotion::Shape shape) {
	switch (shape) {
	case ParamMotion::Shape::Ramp:
		return "ramp";
	case ParamMotion::Shape::Sine:
		return "sine";
	case ParamMotion::Shape::Triangle:
		return "triangle";
	case ParamMotion::Shape::Square:
		return "square";
	case ParamMotion::Shape::Saw:
		return "saw";
	}
	return "";
}

} // namespace nch
