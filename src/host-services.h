// State behind the host-side extensions that need to remember something:
// the track a plug-in believes it sits on, a resource directory, scratch
// memory, an undo history, registered file descriptors, and a tally of every
// other callback so tests can prove one happened.
#pragma once

#include "json.h"

#include <clap/clap.h>

#include <map>
#include <string>
#include <vector>

namespace nch {

struct UndoStep {
	std::string name;
	std::vector<uint8_t> delta;
	bool deltaCanUndo = false;
};

// How clap.thread-pool requests are served.
enum class ThreadPoolMode { Sequential, Parallel, Reject };

class HostServices {
public:
	HostServices();
	~HostServices();

	// --- clap.track-info --------------------------------------------------
	bool trackInfo(clap_track_info_t &out) const;
	void setTrackInfo(const clap_track_info_t &info) { trackInfo_ = info; }
	void setTrackInfoAvailable(bool available) { trackInfoAvailable_ = available; }
	bool trackInfoAvailable() const { return trackInfoAvailable_; }
	Value trackInfoReport() const;

	// --- clap.resource-directory ------------------------------------------
	// Creates the directory on first request and returns its path. A shared
	// directory survives release; a private one is emptied.
	bool requestResourceDirectory(bool shared);
	void releaseResourceDirectory(bool shared);
	std::string resourceDirectoryPath(bool shared) const;

	// --- clap.scratch-memory ----------------------------------------------
	bool reserveScratch(uint32_t sizeBytes, uint32_t maxConcurrencyHint);
	void releaseScratch();
	void *accessScratch();
	uint32_t scratchSize() const { return scratchSize_; }

	// --- clap.undo ---------------------------------------------------------
	void beginChange();
	void cancelChange();
	void changeMade(const char *name, const void *delta, size_t deltaSize, bool deltaCanUndo);
	bool undoRequested() const { return undoRequests_ != 0; }
	void requestUndo() { ++undoRequests_; }
	void requestRedo() { ++redoRequests_; }
	void setWantsUndoContext(bool wants) { wantsUndoContext_ = wants; }
	Value undoReport() const;

	// --- clap.thread-pool --------------------------------------------------
	ThreadPoolMode threadPoolMode() const { return threadPoolMode_; }
	void setThreadPoolMode(ThreadPoolMode mode) { threadPoolMode_ = mode; }

	// --- clap.posix-fd-support ---------------------------------------------
	bool registerFd(int fd, uint32_t flags);
	bool modifyFd(int fd, uint32_t flags);
	bool unregisterFd(int fd);
	const std::map<int, uint32_t> &registeredFds() const { return fds_; }

	// --- everything else ---------------------------------------------------
	// Records that a host callback happened, so `callbacks` can show which
	// extensions a plug-in actually exercised.
	void recordCall(const std::string &where);
	uint64_t callCount(const std::string &where) const;
	Value callReport() const;

	// Things a plug-in told the host that are worth showing back.
	void setHoveredParam(clap_id paramId) { hoveredParam_ = paramId; }
	clap_id hoveredParam() const { return hoveredParam_; }
	void setBackgroundProgress(double progress, const std::string &message);
	double backgroundProgress() const { return backgroundProgress_; }
	const std::string &backgroundMessage() const { return backgroundMessage_; }
	void setCancelBackground(bool cancel) { cancelBackground_ = cancel; }
	bool cancelBackground() const { return cancelBackground_; }
	void setSuggestedRemotePage(clap_id page) { suggestedRemotePage_ = page; }
	clap_id suggestedRemotePage() const { return suggestedRemotePage_; }
	void noteLoadedPreset(uint32_t locationKind, const char *location, const char *loadKey);
	Value loadedPresetReport() const;
	void setMiniCurveDynamic(bool dynamic) { miniCurveDynamic_ = dynamic; }
	bool miniCurveDynamic() const { return miniCurveDynamic_; }

private:
	clap_track_info_t trackInfo_{};
	bool trackInfoAvailable_ = true;

	std::string sharedResourceDirectory_;
	std::string privateResourceDirectory_;

	std::vector<uint8_t> scratch_;
	uint32_t scratchSize_ = 0;
	uint32_t scratchSlots_ = 0;

	bool changeOpen_ = false;
	std::vector<UndoStep> undoSteps_;
	uint64_t undoRequests_ = 0;
	uint64_t redoRequests_ = 0;
	bool wantsUndoContext_ = false;

	ThreadPoolMode threadPoolMode_ = ThreadPoolMode::Sequential;
	std::map<int, uint32_t> fds_;
	std::map<std::string, uint64_t> calls_;

	clap_id hoveredParam_ = CLAP_INVALID_ID;
	double backgroundProgress_ = 0.0;
	std::string backgroundMessage_;
	bool cancelBackground_ = false;
	clap_id suggestedRemotePage_ = CLAP_INVALID_ID;
	bool miniCurveDynamic_ = false;
	std::string loadedPresetLocation_;
	std::string loadedPresetLoadKey_;
	uint32_t loadedPresetKind_ = 0;
	bool loadedPresetSeen_ = false;
};

} // namespace nch
