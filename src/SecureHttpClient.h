#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "HTTPClient.h"
#include "NetworkClient.h"
#include "WString.h"

namespace freeink {

class SecureHttpClient {
 public:
  SecureHttpClient() {
    http_.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    setTimeout(15000);  // SDK default
  }
  using DataCallback = std::function<bool(const uint8_t*, size_t)>;
  using AbortCallback = std::function<bool()>;

  void setCACert(const char*) {}
  void setInsecure() {}
  void setUserAgent(const std::string& agent) { http_.addHeader("User-Agent", agent.c_str()); }
  void setBasicAuth(const std::string& user, const std::string& pass) {
    http_.setAuthorization(user.c_str(), pass.c_str());
  }

  bool begin(const String& url) {
    http_.begin(client_, url.c_str());
    callbackStopped_ = false;
    aborted_ = false;
    return !url.isEmpty();
  }

  bool begin(const std::string& url) { return begin(String(url)); }
  bool begin(const char* url) { return begin(String(url)); }

  void end() { http_.end(); }

  void addHeader(const char* name, const String& value) { http_.addHeader(name, value); }

  void addHeader(const char* name, const std::string& value) { http_.addHeader(name, value.c_str()); }

  void addHeader(const char* name, const char* value) { http_.addHeader(name, value); }
  void addHeader(const std::string& name, const std::string& value) { http_.addHeader(name.c_str(), value.c_str()); }

  // Like the SDK, one value limits both connecting and each wait for data.
  void setTimeout(uint32_t ms) {
    http_.setConnectTimeout(static_cast<int32_t>(ms));
    http_.setTimeout(static_cast<uint16_t>(ms > UINT16_MAX ? UINT16_MAX : ms));
  }
  void setReuse(bool reuse) { http_.setReuse(reuse); }

  int GET() { return http_.GET(); }
  int POST(const String& payload) { return http_.POST(payload.c_str()); }
  int sendRequest(const char* method, const String& payload) {
    if (method && std::string(method) == "PUT") {
      return http_.PUT(payload);
    }
    if (method && std::string(method) == "POST") {
      return http_.POST(payload.c_str());
    }
    return http_.GET();
  }
  int sendRequest(const char* method, const std::string& payload) { return sendRequest(method, String(payload)); }

  int sendRequest(const char* method, const uint8_t* payload, size_t len, const DataCallback& onData,
                  const AbortCallback& shouldAbort = nullptr) {
    aborted_ = shouldAbort && shouldAbort();
    if (aborted_) return -1;
    if (len && !payload) return -1;
    const std::string requestBody = len ? std::string(reinterpret_cast<const char*>(payload), len) : std::string();
    const int status = sendRequest(method, requestBody);
    if (status < 0 || !http_.responseComplete()) return status;
    const String body = http_.getString();
    if (!body.s.empty() && onData && !onData(reinterpret_cast<const uint8_t*>(body.s.data()), body.s.size())) {
      callbackStopped_ = true;
    }
    aborted_ = shouldAbort && shouldAbort();
    return status;
  }

  int GET(const DataCallback& onData, const AbortCallback& shouldAbort = nullptr) {
    return sendRequest("GET", nullptr, 0, onData, shouldAbort);
  }

  String getString() { return http_.getString(); }
  int getSize() { return http_.getSize(); }
  int getStatus() const { return http_.getStatus(); }
  bool hasContentLength() const { return http_.hasContentLength(); }
  size_t getContentLength() const { return http_.getContentLength(); }
  std::string getHeader(const std::string& name) const { return http_.getHeader(name); }
  bool responseComplete() const { return http_.responseComplete() && !callbackStopped_ && !aborted_; }
  bool aborted() const { return aborted_; }
  std::vector<std::pair<std::string, std::string>> getHeaders() const {
    const auto& headers = http_.getHeaders();
    return {headers.begin(), headers.end()};
  }

  static bool resolveUrl(const std::string& baseUrl, const std::string& location, std::string& resolved) {
    if (location.find("://") != std::string::npos) {
      resolved = location;
      return true;
    }
    const size_t schemeEnd = baseUrl.find("://");
    if (schemeEnd == std::string::npos) return false;
    const size_t pathStart = baseUrl.find_first_of("/?#", schemeEnd + 3);
    const std::string origin = baseUrl.substr(0, pathStart);
    const std::string scheme = baseUrl.substr(0, schemeEnd);
    if (location.rfind("//", 0) == 0) {
      resolved = scheme + ":" + location;
    } else if (!location.empty() && location.front() == '/') {
      resolved = origin + location;
    } else {
      std::string path = pathStart == std::string::npos ? "/" : baseUrl.substr(pathStart);
      path.erase(path.find_first_of("?#") == std::string::npos ? path.size() : path.find_first_of("?#"));
      const size_t slash = path.rfind('/');
      resolved = origin + (slash == std::string::npos ? "/" : path.substr(0, slash + 1)) + location;
    }
    return true;
  }

  static bool tls13Available() { return true; }

 private:
  NetworkClientSecure client_;
  HTTPClient http_;
  bool callbackStopped_ = false;
  bool aborted_ = false;
};

}  // namespace freeink
