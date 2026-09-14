#include "harness.h"
#include "thread-pool.h"
#include "thread-role.h"

#include <atomic>
#include <numeric>
#include <set>
#include <vector>

using nch::ThreadPool;

TEST(every_task_runs_exactly_once) {
	ThreadPool pool;
	pool.start(4);

	constexpr uint32_t tasks = 1000;
	std::vector<std::atomic<int>> counts(tasks);
	for (auto &count : counts)
		count.store(0);

	pool.run(tasks, [&counts](uint32_t index) { counts[index].fetch_add(1); });

	bool exactlyOnce = true;
	for (const auto &count : counts)
		exactlyOnce = exactlyOnce && count.load() == 1;
	CHECK(exactlyOnce);
}

TEST(run_returns_only_when_the_work_is_finished) {
	ThreadPool pool;
	pool.start(4);

	std::atomic<uint32_t> done{0};
	pool.run(200, [&done](uint32_t) { done.fetch_add(1); });
	// No waiting here: if run() returned early this would be short.
	CHECK_EQ(done.load(), 200u);
}

TEST(the_pool_can_be_used_again_and_again) {
	ThreadPool pool;
	pool.start(3);
	for (int round = 0; round < 50; ++round) {
		std::atomic<uint32_t> done{0};
		pool.run(37, [&done](uint32_t) { done.fetch_add(1); });
		CHECK_EQ(done.load(), 37u);
	}
}

TEST(workers_run_as_the_audio_thread) {
	ThreadPool pool;
	pool.start(4);
	// CLAP runs exec() on the audio thread, so a plug-in asking the host which
	// thread it is on must be told the truth -- including for the share the
	// calling thread does, which in a real host is already inside process().
	nch::ScopedThreadRole callerIsAudio(nch::ThreadRole::Audio);
	std::atomic<uint32_t> onAudioThread{0};
	pool.run(200, [&onAudioThread](uint32_t) {
		if (nch::currentThreadRole() == nch::ThreadRole::Audio)
			onAudioThread.fetch_add(1);
	});
	CHECK_EQ(onAudioThread.load(), 200u);
}

TEST(a_pool_with_no_workers_still_does_the_work) {
	ThreadPool pool;
	pool.start(1);
	CHECK_EQ(pool.workerCount(), 0u);

	std::atomic<uint32_t> done{0};
	pool.run(10, [&done](uint32_t) { done.fetch_add(1); });
	CHECK_EQ(done.load(), 10u);
}

TEST(no_tasks_is_not_a_hang) {
	ThreadPool pool;
	pool.start(4);
	pool.run(0, [](uint32_t) { CHECK(false); });
	CHECK(true);
}

TEST(stopping_and_starting_again_works) {
	ThreadPool pool;
	pool.start(4);
	pool.stop();
	CHECK_EQ(pool.workerCount(), 0u);

	pool.start(4);
	std::atomic<uint32_t> done{0};
	pool.run(64, [&done](uint32_t) { done.fetch_add(1); });
	CHECK_EQ(done.load(), 64u);
}
