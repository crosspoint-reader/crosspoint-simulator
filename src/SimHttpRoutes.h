#pragma once

#include <map>
#include <string>

// Canned HTTP answers from the JSON file in CROSSPOINT_SIM_HTTP_ROUTES. The
// file is read on every request, so a test can change it while the simulator
// runs. See README.md for the format.
namespace sim_http_routes {

enum class Result {
  NotHandled, // no routes file, or no match and "unmatched" is "network"
  Answered,   // status, headers, and body are set
  Refused,    // no match and "unmatched" is "fail", or a bad routes file
};

Result respond(const std::string &url, const char *method,
               const std::map<std::string, std::string> &requestHeaders,
               int &status, std::map<std::string, std::string> &headers,
               std::string &body);

} // namespace sim_http_routes
