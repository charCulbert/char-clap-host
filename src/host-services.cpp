#include "host-services.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <thread>
#include <filesystem>

namespace nch {
namespace {

std::string hostTemporaryDirectory(const char *suffix) {
	std::error_code code;
	std::filesystem::path base = std::filesystem::temp_directory_path(code);
	if (code)
		base = ".";
	return (base / (std::string("nativeClapHost-") + suffix)).string();
}

} // namespace

HostServices::HostServices() {
	std::strncpy(trackInfo_.name, "Track 1", sizeof(trackInfo_.name) - 1);
	trackInfo_.flags = CLAP_TRACK_INFO_HAS_TRACK_NAME | CLAP_TRACK_INFO_HAS_TRACK_COLOR |
	                   CLAP_TRACK_INFO_HAS_AUDIO_CHANNEL;
	trackInfo_.color = {0x7A, 0x9E, 0xC8, 0xFF};
	trackInfo_.audio_channel_count = 2;
	trackInfo_.audio_port_type = CLAP_PORT_STEREO;
}

HostServices::~HostServices() {
	releaseResourceDirectory(false);
}

bool HostServices::trackInfo(clap_track_info_t &out) const {
	if (!trackInfoAvailable_)
		return false;
	out = trackInfo_;
	return true;
}

Value HostServices::trackInfoReport() const {
	Object out;
	out["available"] = Value(trackInfoAvailable_);
	out["name"] = Value(trackInfo_.name);
	out["channels"] = Value(trackInfo_.audio_channel_count);
	out["portType"] = Value(trackInfo_.audio_port_type != nullptr ? trackInfo_.audio_port_type : "");
	out["isReturnTrack"] = Value((trackInfo_.flags & CLAP_TRACK_INFO_IS_FOR_RETURN_TRACK) != 0);
	out["isBus"] = Value((trackInfo_.flags & CLAP_TRACK_INFO_IS_FOR_BUS) != 0);
	out["isMaster"] = Value((trackInfo_.flags & CLAP_TRACK_INFO_IS_FOR_MASTER) != 0);
	return Value(std::move(out));
}

bool HostServices::requestResourceDirectory(bool shared) {
	std::string &target = shared ? sharedResourceDirectory_ : privateResourceDirectory_;
	if (!target.empty())
		return true;
	const std::string path = hostTemporaryDirectory(shared ? "shared-resources" : "private-resources");
	std::error_code code;
	std::filesystem::create_directories(path, code);
	if (code)
		return false;
	target = path;
	return true;
}

void HostServices::releaseResourceDirectory(bool shared) {
	if (shared) {
		// A shared directory outlives the plug-in that asked for it.
		sharedResourceDirectory_.clear();
		return;
	}
	if (privateResourceDirectory_.empty())
		return;
	std::error_code code;
	std::filesystem::remove_all(privateResourceDirectory_, code);
	privateResourceDirectory_.clear();
}

std::string HostServices::resourceDirectoryPath(bool shared) const {
	return shared ? sharedResourceDirectory_ : privateResourceDirectory_;
}

bool HostServices::reserveScratch(uint32_t sizeBytes, uint32_t maxConcurrencyHint) {
	// One slot per concurrent accessor, so a plug-in that fans out across the
	// thread pool never sees two threads sharing a block.
	const uint32_t slots = maxConcurrencyHint == 0 ? 1 : maxConcurrencyHint;
	const uint64_t total = static_cast<uint64_t>(sizeBytes) * slots;
	if (total > (1ull << 31))
		return false;
	scratch_.assign(static_cast<size_t>(total), 0);
	scratchSize_ = sizeBytes;
	scratchSlots_ = slots;
	return true;
}

void *HostServices::accessScratch() {
	if (scratch_.empty())
		return nullptr;
	// Slot selection by thread, so concurrent access stays disjoint.
	static thread_local uint32_t slot = 0;
	static uint32_t nextSlot = 0;
	if (slot == 0)
		slot = 1 + (nextSlot++ % (scratchSlots_ == 0 ? 1 : scratchSlots_));
	const uint32_t index = (slot - 1) % (scratchSlots_ == 0 ? 1 : scratchSlots_);
	return scratch_.data() + static_cast<size_t>(index) * scratchSize_;
}

void HostServices::beginChange() {
	changeOpen_ = true;
}

void HostServices::cancelChange() {
	changeOpen_ = false;
}

void HostServices::changeMade(const char *name, const void *delta, size_t deltaSize, bool deltaCanUndo) {
	changeOpen_ = false;
	UndoStep step;
	step.name = name != nullptr ? name : "";
	step.deltaCanUndo = deltaCanUndo;
	if (delta != nullptr && deltaSize != 0) {
		const auto *bytes = static_cast<const uint8_t *>(delta);
		step.delta.assign(bytes, bytes + deltaSize);
	}
	undoSteps_.push_back(std::move(step));
}

Value HostServices::undoReport() const {
	Array steps;
	for (const auto &step : undoSteps_) {
		Object row;
		row["name"] = Value(step.name);
		row["deltaBytes"] = Value(static_cast<uint64_t>(step.delta.size()));
		row["canUndo"] = Value(step.deltaCanUndo);
		steps.push_back(Value(std::move(row)));
	}
	Object out;
	out["steps"] = Value(std::move(steps));
	out["changeOpen"] = Value(changeOpen_);
	out["undoRequests"] = Value(undoRequests_);
	out["redoRequests"] = Value(redoRequests_);
	out["wantsContext"] = Value(wantsUndoContext_);
	return Value(std::move(out));
}

void HostServices::setThreadPoolMode(ThreadPoolMode mode) {
	threadPoolMode_ = mode;
	if (mode == ThreadPoolMode::Parallel)
		threadPool_.start(std::max(2u, std::thread::hardware_concurrency()));
	else
		threadPool_.stop();
}

bool HostServices::registerFd(int fd, uint32_t flags) {
	if (fd < 0)
		return false;
	fds_[fd] = flags;
	return true;
}

bool HostServices::modifyFd(int fd, uint32_t flags) {
	const auto it = fds_.find(fd);
	if (it == fds_.end())
		return false;
	it->second = flags;
	return true;
}

bool HostServices::unregisterFd(int fd) {
	return fds_.erase(fd) != 0;
}

void HostServices::recordCall(const std::string &where) {
	++calls_[where];
}

uint64_t HostServices::callCount(const std::string &where) const {
	const auto it = calls_.find(where);
	return it == calls_.end() ? 0 : it->second;
}

Value HostServices::callReport() const {
	Array rows;
	for (const auto &entry : calls_) {
		Object row;
		row["call"] = Value(entry.first);
		row["count"] = Value(entry.second);
		rows.push_back(Value(std::move(row)));
	}
	Object out;
	out["calls"] = Value(std::move(rows));
	return Value(std::move(out));
}

void HostServices::setBackgroundProgress(double progress, const std::string &message) {
	backgroundProgress_ = progress;
	backgroundMessage_ = message;
}

void HostServices::noteLoadedPreset(uint32_t locationKind, const char *location, const char *loadKey) {
	loadedPresetKind_ = locationKind;
	loadedPresetLocation_ = location != nullptr ? location : "";
	loadedPresetLoadKey_ = loadKey != nullptr ? loadKey : "";
	loadedPresetSeen_ = true;
}

Value HostServices::loadedPresetReport() const {
	Object out;
	out["seen"] = Value(loadedPresetSeen_);
	out["locationKind"] = Value(loadedPresetKind_ == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? "plugin" : "file");
	out["location"] = Value(loadedPresetLocation_);
	out["loadKey"] = Value(loadedPresetLoadKey_);
	return Value(std::move(out));
}

} // namespace nch
