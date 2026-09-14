#include "thread-role.h"

namespace nch {
namespace {

thread_local ThreadRole tRole = ThreadRole::Unknown;

} // namespace

ThreadRole currentThreadRole() {
	return tRole;
}

ScopedThreadRole::ScopedThreadRole(ThreadRole role) : previous_(tRole) {
	tRole = role;
}

ScopedThreadRole::~ScopedThreadRole() {
	tRole = previous_;
}

const char *threadRoleName(ThreadRole role) {
	switch (role) {
	case ThreadRole::Main: return "main";
	case ThreadRole::Audio: return "audio";
	default: return "unknown";
	}
}

} // namespace nch
