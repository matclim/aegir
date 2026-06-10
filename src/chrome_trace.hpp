// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// chrome_trace.hpp — minimal Chrome Trace Event emitter.
//
// Output is the JSON Array Format documented at
// https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU,
// which both chrome://tracing and ui.perfetto.dev load directly. A small
// RAII helper writes one "complete" event (ph="X") per scope. Emission is
// gated at compile time by AEGIR_ENABLE_TRACE; with the flag off, every
// macro expands to a no-op and the binary has zero tracing overhead.
//
// Destination: the path in $AEGIR_TRACE_FILE. If unset, tracing is silently
// disabled even when AEGIR_ENABLE_TRACE is on.

#pragma once

#ifdef AEGIR_ENABLE_TRACE

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace aegir::trace {

inline std::FILE* file_handle() {
  static std::FILE* f = []() -> std::FILE* {
    char const* path = std::getenv("AEGIR_TRACE_FILE");
    if (!path || !*path) return nullptr;
    std::FILE* fp = std::fopen(path, "w");
    if (!fp) return nullptr;
    std::fprintf(fp, "[\n");
    std::atexit([] {
      // Re-fetch the cached handle (atexit runs after main).
      static std::FILE* cached = file_handle();
      if (cached) {
        std::fprintf(cached, "\n]\n");
        std::fflush(cached);
        std::fclose(cached);
      }
    });
    return fp;
  }();
  return f;
}

inline std::mutex& write_mutex() {
  static std::mutex m;
  return m;
}

inline std::atomic<bool>& first_event() {
  static std::atomic<bool> v{true};
  return v;
}

inline long long now_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch())
      .count();
}

inline long long thread_id() {
  static thread_local long long id = static_cast<long long>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()) &
      0x7fffffffffffffffLL);
  return id;
}

inline void write_complete(char const* cat, char const* name, long long ts,
                           long long dur) {
  auto* f = file_handle();
  if (!f) return;
  std::lock_guard lock(write_mutex());
  std::fprintf(f,
               "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"X\","
               "\"ts\":%lld,\"dur\":%lld,\"pid\":1,\"tid\":%lld}",
               first_event().exchange(false) ? "" : ",\n", name, cat, ts, dur,
               thread_id());
}

inline void write_counter(char const* cat, char const* name, long long value) {
  auto* f = file_handle();
  if (!f) return;
  std::lock_guard lock(write_mutex());
  std::fprintf(f,
               "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"C\","
               "\"ts\":%lld,\"pid\":1,\"tid\":%lld,\"args\":{\"value\":%lld}}",
               first_event().exchange(false) ? "" : ",\n", name, cat, now_us(),
               thread_id(), value);
}

class ScopedEvent {
 public:
  ScopedEvent(char const* cat, char const* name)
      : cat_{cat}, name_{name}, start_{now_us()} {}
  ~ScopedEvent() {
    auto end = now_us();
    write_complete(cat_, name_, start_, end - start_);
  }

 private:
  char const* cat_;
  char const* name_;
  long long start_;
};

}  // namespace aegir::trace

#define AEGIR_TRACE_CONCAT_(a, b) a##b
#define AEGIR_TRACE_CONCAT(a, b) AEGIR_TRACE_CONCAT_(a, b)
#define AEGIR_TRACE_EVENT(cat, name)                                        \
  ::aegir::trace::ScopedEvent AEGIR_TRACE_CONCAT(_aegir_trace_, __LINE__) { \
    cat, name                                                               \
  }
#define AEGIR_TRACE_COUNTER(cat, name, value) \
  ::aegir::trace::write_counter(cat, name, static_cast<long long>(value))

#else  // !AEGIR_ENABLE_TRACE

#define AEGIR_TRACE_EVENT(cat, name) ((void)0)
#define AEGIR_TRACE_COUNTER(cat, name, value) ((void)0)

#endif
