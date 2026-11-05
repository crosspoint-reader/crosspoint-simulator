#include "SimHttpLog.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace sim_http_log {
namespace {

std::mutex mutex;
FILE *output = nullptr;
bool opened = false;

std::string lowercase(std::string value) {
  for (char &c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

// Credentials stay out of the log; the length still shows a value was sent.
bool isSecretName(const std::string &name) {
  const std::string lower = lowercase(name);
  for (const char *word : {"auth", "token", "key", "secret", "password",
                           "cookie", "session", "signature"}) {
    if (lower.find(word) != std::string::npos)
      return true;
  }
  return false;
}

std::string redact(const std::string &value) {
  return "<" + std::to_string(value.size()) + " chars>";
}

// "Bearer abc" -> "Bearer <3 chars>", so a wrong scheme is still visible.
std::string redactHeader(const std::string &value) {
  const size_t space = value.find(' ');
  if (space == std::string::npos || space == 0)
    return redact(value);
  return value.substr(0, space + 1) + redact(value.substr(space + 1));
}

std::string redactQuery(const std::string &url) {
  const size_t query = url.find('?');
  if (query == std::string::npos)
    return url;
  const size_t fragment = url.find('#', query);
  const size_t end = fragment == std::string::npos ? url.size() : fragment;
  std::string out = url.substr(0, query + 1);
  size_t start = query + 1;
  while (start <= end) {
    size_t next = url.find('&', start);
    if (next == std::string::npos || next > end)
      next = end;
    const std::string param = url.substr(start, next - start);
    const size_t equals = param.find('=');
    if (equals != std::string::npos && isSecretName(param.substr(0, equals)))
      out += param.substr(0, equals + 1) + redact(param.substr(equals + 1));
    else
      out += param;
    if (next == end)
      break;
    out += '&';
    start = next + 1;
  }
  return out + url.substr(end);
}

std::string jsonString(const std::string &value) {
  std::string out = "\"";
  for (const char c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char escaped[8];
        std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
        out += escaped;
      } else {
        out += c;
      }
    }
  }
  return out + "\"";
}

FILE *openOutput() {
  if (opened)
    return output;
  opened = true;
  const char *path = std::getenv("CROSSPOINT_SIM_HTTP_LOG");
  if (!path || !*path)
    return nullptr;
  if (std::strcmp(path, "-") == 0) {
    output = stderr;
  } else {
    // Append: a sleep/wake relaunch is a new process writing the same log.
    output = std::fopen(path, "a");
    if (!output)
      std::fprintf(stderr, "[SIM] Cannot open HTTP log %s\n", path);
  }
  return output;
}

} // namespace

void record(const Entry &entry) {
  const std::lock_guard<std::mutex> lock(mutex);
  FILE *file = openOutput();
  if (!file)
    return;

  std::string line = "{\"method\":" +
                     jsonString(entry.method ? entry.method : "GET") +
                     ",\"url\":" + jsonString(redactQuery(entry.url)) +
                     ",\"headers\":{";
  bool first = true;
  for (const auto &header : entry.headers) {
    if (!first)
      line += ',';
    first = false;
    line += jsonString(header.first) + ':' +
            jsonString(isSecretName(header.first) ? redactHeader(header.second)
                                                  : header.second);
  }
  line += '}';
  if (!entry.basicAuth.empty()) {
    const size_t colon = entry.basicAuth.find(':');
    line += ",\"basic_auth\":" +
            jsonString(entry.basicAuth.substr(0, colon) + ":" +
                       redact(colon == std::string::npos
                                  ? std::string()
                                  : entry.basicAuth.substr(colon + 1)));
  }
  line += ",\"request_bytes\":" +
          std::to_string(entry.body ? std::strlen(entry.body) : 0) +
          ",\"status\":" + std::to_string(entry.status) +
          ",\"response_bytes\":" + std::to_string(entry.responseBytes);
  if (entry.curlExitCode != 0)
    line += ",\"curl_exit\":" + std::to_string(entry.curlExitCode);
  line += ",\"source\":" + jsonString(entry.source) +
          ",\"ms\":" + std::to_string(entry.elapsedMs) + "}\n";

  if (file == stderr)
    line = "[SIM] HTTP " + line;
  std::fputs(line.c_str(), file);
  std::fflush(file);
}

} // namespace sim_http_log
