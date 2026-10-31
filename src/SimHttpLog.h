#pragma once

#include <map>
#include <string>

// Records each HTTP request the firmware makes when CROSSPOINT_SIM_HTTP_LOG is
// set (a file path, or "-" for stderr). See README.md.
namespace sim_http_log {

struct Entry {
  const char *method;
  const std::string &url;
  const std::map<std::string, std::string> &headers;
  const std::string &basicAuth;
  const char *body;
  int status;
  size_t responseBytes;
  int curlExitCode;
  const char *source; // "network", "mock", or "file"
  unsigned long elapsedMs;
};

void record(const Entry &entry);

} // namespace sim_http_log
