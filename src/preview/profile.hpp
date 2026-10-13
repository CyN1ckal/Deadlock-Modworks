#pragma once

// Tracy zones for the model tools GUI. A normal build leaves the macros empty.
// The CLion profile preset defines TRACY_ENABLE. Recording then starts when the
// window opens and stays idle until a Tracy server connects.
// Closing the window does not wait for a capture. Zone names are string literals.

#ifdef TRACY_ENABLE
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#endif

#include <tracy/Tracy.hpp>

namespace skeleton_repair {

// Starts the profiler for one GUI process and stops it when the window is gone.
// A normal build constructs nothing. Tests never construct this either.
struct TracySession {
#ifdef TRACY_ENABLE
    TracySession() { tracy::StartupProfiler(); }
    ~TracySession() { tracy::ShutdownProfiler(); }
#else
    TracySession() = default;
    ~TracySession() = default;
#endif
    TracySession(const TracySession&) = delete;
    TracySession& operator=(const TracySession&) = delete;
};

}  // namespace skeleton_repair
