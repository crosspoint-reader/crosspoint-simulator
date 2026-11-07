#include "SimLog.h"

#include <deque>
#include <mutex>

#include "Arduino.h"

namespace sim_log {
namespace {

// A firmware log line is at most 256 bytes, so this is about 1 MB at most.
constexpr size_t kMaxLines = 4096;

struct Line {
  unsigned long atMs;
  std::string text;
};

struct State {
  std::mutex mutex;
  std::deque<Line> lines;
  std::string partial;
};

State& state() {
  // Firmware global constructors can log before this translation unit initializes.
  static State instance;
  return instance;
}

}  // namespace

void write(const char* data, size_t size) {
  auto& log = state();
  const std::lock_guard<std::mutex> lock(log.mutex);
  for (size_t i = 0; i < size; ++i) {
    if (data[i] != '\n') {
      log.partial.push_back(data[i]);
      continue;
    }
    if (!log.partial.empty() && log.partial.back() == '\r') log.partial.pop_back();
    log.lines.push_back({millis(), std::move(log.partial)});
    log.partial.clear();
    if (log.lines.size() > kMaxLines) log.lines.pop_front();
  }
}

bool findSince(unsigned long sinceMs, const std::regex& pattern, std::string* line) {
  auto& log = state();
  const std::lock_guard<std::mutex> lock(log.mutex);
  for (const Line& entry : log.lines) {
    if (entry.atMs < sinceMs || !std::regex_search(entry.text, pattern)) continue;
    if (line) *line = entry.text;
    return true;
  }
  return false;
}

}  // namespace sim_log
