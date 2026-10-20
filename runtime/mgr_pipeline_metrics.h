#pragma once

// Experiment telemetry only. CPU wall times here are not GPU execution times.
// A successful queue-present request is not proof that a frame reached display.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace mgr_pipeline {
inline long long Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline FILE* File() {
  static FILE* file = [] {
    const char* path = std::getenv("MGR_PIPELINE_CSV");
    FILE* f = path ? std::fopen(path, "w") : nullptr;
    if (f) {
      std::fprintf(f, "steady_ns,focus,ui,nonblocking,width,height,outcome,attempt_ns,reuse_ns,acquire_ns,submit_ns,present_ns,input_calls,input_ns,submit_lock_ns,present_lock_ns\n");
      std::setvbuf(f, nullptr, _IOLBF, 0);
    }
    return f;
  }();
  return file;
}
inline std::atomic<int> focus{-1};
inline std::atomic<unsigned long long> input_calls{0}, input_ns{0};
inline std::mutex output_mutex;

struct InputScope {
  long long start = File() ? Now() : 0;
  ~InputScope() {
    if (start) {
      input_ns.fetch_add(Now() - start, std::memory_order_relaxed);
      input_calls.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

struct PaintScope {
  long long start = File() ? Now() : 0;
  long long reuse = 0, acquire = 0, submit = 0, present = 0;
  long long submit_lock = 0, present_lock = 0;
  int ui, nonblocking, width, height;
  const char* outcome = "early_return";
  ~PaintScope() {
    if (!start) return;
    const auto end = Now();
    std::lock_guard lock(output_mutex);
    std::fprintf(File(), "%lld,%d,%d,%d,%d,%d,%s,%lld,%lld,%lld,%lld,%lld,%llu,%llu,%lld,%lld\n",
        end, focus.load(std::memory_order_relaxed), ui, nonblocking, width, height,
        outcome, end-start, reuse, acquire, submit, present,
        input_calls.exchange(0, std::memory_order_relaxed),
        input_ns.exchange(0, std::memory_order_relaxed), submit_lock, present_lock);
  }
};
}  // namespace mgr_pipeline
