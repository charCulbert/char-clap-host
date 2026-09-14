// The worker threads clap.thread-pool runs a plug-in's tasks on.
//
// CLAP has the host run these from inside process(), which means the one thing
// the host must not do is create them there: spawning and joining OS threads
// is exactly the kind of work thread-check.h tells a host to keep out of the
// audio thread. The threads are made once, up front, and parked on a condition
// variable until there is something to do.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nch {

class ThreadPool {
public:
	ThreadPool() = default;
	~ThreadPool();
	ThreadPool(const ThreadPool &) = delete;
	ThreadPool &operator=(const ThreadPool &) = delete;

	// Creates the workers. Called from the main thread, never from process().
	// Asking for fewer than two workers leaves the pool empty, and run() then
	// does the work on the calling thread.
	void start(uint32_t workerCount);
	void stop();
	uint32_t workerCount() const { return static_cast<uint32_t>(workers_.size()); }

	// Runs `task` for every index below `taskCount` and returns once they have
	// all finished. The calling thread takes a share of the work rather than
	// waiting idle, so it must already hold whatever thread role the task
	// expects -- in a host that means run() is called from inside process().
	void run(uint32_t taskCount, const std::function<void(uint32_t)> &task);

private:
	void workerLoop();

	std::vector<std::thread> workers_;
	std::mutex mutex_;
	std::condition_variable wakeWorkers_;
	std::condition_variable workFinished_;

	const std::function<void(uint32_t)> *task_ = nullptr;
	uint32_t taskCount_ = 0;
	std::atomic<uint32_t> nextTask_{0};
	std::atomic<uint32_t> remaining_{0};
	uint64_t generation_ = 0;
	bool stopping_ = false;
};

} // namespace nch
