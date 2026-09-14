#include "thread-pool.h"

#include "thread-role.h"

namespace nch {

ThreadPool::~ThreadPool() {
	stop();
}

void ThreadPool::start(uint32_t workerCount) {
	stop();
	if (workerCount < 2)
		return; // one worker is the calling thread doing the work itself
	stopping_ = false;
	// One fewer than asked for: the caller works alongside them.
	for (uint32_t i = 0; i + 1 < workerCount; ++i)
		workers_.emplace_back([this] { workerLoop(); });
}

void ThreadPool::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
	}
	wakeWorkers_.notify_all();
	for (auto &worker : workers_)
		if (worker.joinable())
			worker.join();
	workers_.clear();
}

void ThreadPool::workerLoop() {
	// A worker runs the plug-in's exec(), which CLAP says happens on the audio
	// thread, so it says so.
	ScopedThreadRole role(ThreadRole::Audio);
	uint64_t seen = 0;
	while (true) {
		const std::function<void(uint32_t)> *task = nullptr;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wakeWorkers_.wait(lock, [this, seen] { return stopping_ || generation_ != seen; });
			if (stopping_)
				return;
			seen = generation_;
			task = task_;
		}
		if (task == nullptr)
			continue;

		for (uint32_t index = nextTask_.fetch_add(1, std::memory_order_relaxed); index < taskCount_;
		     index = nextTask_.fetch_add(1, std::memory_order_relaxed))
			(*task)(index);

		if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
			workFinished_.notify_one();
	}
}

void ThreadPool::run(uint32_t taskCount, const std::function<void(uint32_t)> &task) {
	if (taskCount == 0)
		return;
	if (workers_.empty()) {
		for (uint32_t index = 0; index < taskCount; ++index)
			task(index);
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		task_ = &task;
		taskCount_ = taskCount;
		nextTask_.store(0, std::memory_order_relaxed);
		remaining_.store(static_cast<uint32_t>(workers_.size()), std::memory_order_release);
		++generation_;
	}
	wakeWorkers_.notify_all();

	// The calling thread is a worker too, which is what keeps a two-task
	// request from waiting on a thread wake-up.
	for (uint32_t index = nextTask_.fetch_add(1, std::memory_order_relaxed); index < taskCount;
	     index = nextTask_.fetch_add(1, std::memory_order_relaxed))
		task(index);

	std::unique_lock<std::mutex> lock(mutex_);
	workFinished_.wait(lock, [this] { return remaining_.load(std::memory_order_acquire) == 0; });
	task_ = nullptr;
}

} // namespace nch
