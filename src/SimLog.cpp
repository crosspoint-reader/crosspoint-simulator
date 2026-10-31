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

std::mutex mutex;
std::deque<Line> lines;
std::string partial;

} // namespace

void write(const char *data, size_t size) {
  const std::lock_guard<std::mutex> lock(mutex);
  for (size_t i = 0; i < size; ++i) {
    if (data[i] != '\n') {
      partial.push_back(data[i]);
      continue;
    }
    if (!partial.empty() && partial.back() == '\r')
      partial.pop_back();
    lines.push_back({millis(), std::move(partial)});
    partial.clear();
    if (lines.size() > kMaxLines)
      lines.pop_front();
  }
}

bool findSince(unsigned long sinceMs, const std::regex &pattern,
               std::string *line) {
  const std::lock_guard<std::mutex> lock(mutex);
  for (const Line &entry : lines) {
    if (entry.atMs < sinceMs || !std::regex_search(entry.text, pattern))
      continue;
    if (line)
      *line = entry.text;
    return true;
  }
  return false;
}

} // namespace sim_log
