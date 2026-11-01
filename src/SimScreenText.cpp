#include "SimScreenText.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "HardwareSerial.h"

namespace {

struct DrawnText {
  int x;
  int y;
  std::string text;

  bool operator<(const DrawnText &other) const {
    return std::tie(y, x, text) < std::tie(other.y, other.x, other.text);
  }
  bool operator==(const DrawnText &other) const {
    return x == other.x && y == other.y && text == other.text;
  }
};

std::mutex mutex;
std::vector<DrawnText> drawn;
std::string lastLine;

bool enabled() {
  static const bool on = [] {
    const char *value = std::getenv("CROSSPOINT_SIM_SCREEN_TEXT");
    return value && std::strcmp(value, "1") == 0;
  }();
  return on;
}

} // namespace

void simulatorScreenText(int x, int y, const char *text) {
  if (!enabled() || !text || !*text)
    return;
  const std::lock_guard<std::mutex> lock(mutex);
  drawn.push_back({x, y, text});
}

namespace sim_screen_text {

void clear() {
  if (!enabled())
    return;
  const std::lock_guard<std::mutex> lock(mutex);
  drawn.clear();
}

void flush() {
  if (!enabled())
    return;
  std::string line;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    // Reading order; strip rendering draws the same string once per strip.
    std::sort(drawn.begin(), drawn.end());
    drawn.erase(std::unique(drawn.begin(), drawn.end()), drawn.end());
    line = "[SIM] SCREEN:";
    for (size_t i = 0; i < drawn.size(); ++i)
      line += (i == 0 ? " " : " | ") + drawn[i].text;
    // A refresh that shows the same text again (a clock tick, a progress
    // redraw) would only repeat the previous line.
    if (line == lastLine)
      return;
    lastLine = line;
  }
  line += '\n';
  // Through Serial, like a firmware log line.
  Serial.write(reinterpret_cast<const uint8_t *>(line.data()), line.size());
}

} // namespace sim_screen_text
