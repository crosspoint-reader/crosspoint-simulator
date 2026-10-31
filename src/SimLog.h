#pragma once

#include <cstddef>
#include <regex>
#include <string>

// Keeps recent firmware serial output, one entry per line, so input scripts
// can wait for a log line (see WAIT in README.md).
namespace sim_log {

// Called with everything the firmware writes to Serial.
void write(const char *data, size_t size);

// Finds the first line logged at or after `sinceMs` (millis()) that matches.
bool findSince(unsigned long sinceMs, const std::regex &pattern,
               std::string *line);

} // namespace sim_log
