#include "SimHttpRoutes.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sim_http_routes {
namespace {

std::string lowercase(std::string value) {
  for (char &c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

// '*' matches any run of characters; everything else matches itself, so the
// '?' of a query string needs no escaping.
bool globMatch(const char *pattern, const char *text) {
  const char *star = nullptr;
  const char *resume = nullptr;
  while (*text) {
    if (*pattern == '*') {
      star = pattern++;
      resume = text;
    } else if (*pattern == *text) {
      ++pattern;
      ++text;
    } else if (star) {
      pattern = star + 1;
      text = ++resume;
    } else {
      return false;
    }
  }
  while (*pattern == '*')
    ++pattern;
  return *pattern == '\0';
}

bool readText(const std::string &path, std::string &out) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  std::ostringstream text;
  text << file.rdbuf();
  out = text.str();
  return true;
}

bool headersMatch(JsonObjectConst wanted,
                  const std::map<std::string, std::string> &requestHeaders) {
  for (JsonPairConst header : wanted) {
    const std::string name = lowercase(header.key().c_str());
    const char *pattern = header.value().as<const char *>();
    bool found = false;
    for (const auto &sent : requestHeaders) {
      if (lowercase(sent.first) == name && pattern &&
          globMatch(pattern, sent.second.c_str())) {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

bool routeMatches(JsonObjectConst route, const std::string &url,
                  const std::string &method,
                  const std::map<std::string, std::string> &requestHeaders) {
  const char *routeMethod = route["method"] | "*";
  if (std::string(routeMethod) != "*" && lowercase(routeMethod) != method)
    return false;
  const char *routeUrl = route["url"] | "";
  return *routeUrl && globMatch(routeUrl, url.c_str()) &&
         headersMatch(route["request_headers"].as<JsonObjectConst>(),
                      requestHeaders);
}

} // namespace

Result respond(const std::string &url, const char *method,
               const std::map<std::string, std::string> &requestHeaders,
               int &status, std::map<std::string, std::string> &headers,
               std::string &body) {
  const char *path = std::getenv("CROSSPOINT_SIM_HTTP_ROUTES");
  if (!path || !*path)
    return Result::NotHandled;

  // A broken routes file refuses every request instead of letting the test
  // reach the real service.
  std::string text;
  if (!readText(path, text)) {
    std::fprintf(stderr, "[SIM] Cannot read HTTP routes %s\n", path);
    return Result::Refused;
  }
  JsonDocument doc;
  if (const DeserializationError error = deserializeJson(doc, text)) {
    std::fprintf(stderr, "[SIM] Cannot parse HTTP routes %s: %s\n", path,
                 error.c_str());
    return Result::Refused;
  }

  const std::string verb = lowercase(method ? method : "GET");
  JsonArrayConst routes = doc.is<JsonArrayConst>()
                              ? doc.as<JsonArrayConst>()
                              : doc["routes"].as<JsonArrayConst>();
  for (JsonObjectConst route : routes) {
    if (!routeMatches(route, url, verb, requestHeaders))
      continue;

    status = route["status"] | 200;
    headers.clear();
    for (JsonPairConst header : route["headers"].as<JsonObjectConst>())
      headers[lowercase(header.key().c_str())] =
          header.value().as<std::string>();
    body.clear();
    if (const char *file = route["body_file"]) {
      std::string bodyPath = file;
      const std::string routesPath = path;
      const size_t slash = routesPath.find_last_of('/');
      if (!bodyPath.empty() && bodyPath[0] != '/' && slash != std::string::npos)
        bodyPath = routesPath.substr(0, slash + 1) + bodyPath;
      if (!readText(bodyPath, body)) {
        std::fprintf(stderr, "[SIM] Cannot read route body %s\n",
                     bodyPath.c_str());
        return Result::Refused;
      }
    } else if (route["body"].is<const char *>()) {
      body = route["body"].as<const char *>();
    } else if (!route["body"].isNull()) {
      serializeJson(route["body"], body);
    }
    return Result::Answered;
  }

  if (std::string(doc["unmatched"] | "network") != "fail")
    return Result::NotHandled;
  // The query string is left out: it can carry credentials.
  std::fprintf(stderr, "[SIM] No HTTP route for %s %s\n",
               method ? method : "GET", url.substr(0, url.find('?')).c_str());
  return Result::Refused;
}

} // namespace sim_http_routes
