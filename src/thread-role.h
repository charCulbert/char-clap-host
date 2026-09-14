// Which CLAP thread the calling code is on.
//
// clap.thread-check answers from this, and the validator uses it to catch
// calls a plug-in makes from the wrong thread.
#pragma once

namespace nch {

enum class ThreadRole { Unknown, Main, Audio };

// The role of the calling thread.
ThreadRole currentThreadRole();

// Marks the calling thread for the lifetime of the scope and restores the
// previous role on exit, so nested scopes (an audio callback inside a test
// running on the main thread) behave.
class ScopedThreadRole {
public:
	explicit ScopedThreadRole(ThreadRole role);
	~ScopedThreadRole();
	ScopedThreadRole(const ScopedThreadRole &) = delete;
	ScopedThreadRole &operator=(const ScopedThreadRole &) = delete;

private:
	ThreadRole previous_;
};

const char *threadRoleName(ThreadRole role);

} // namespace nch
