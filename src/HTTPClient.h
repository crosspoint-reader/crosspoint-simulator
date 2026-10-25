#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include "SimHttpFetch.h"
#include "Stream.h"
#include "WString.h"

class NetworkClient;

#define HTTPC_ERROR_CONNECTION_REFUSED (-1)
#define HTTPC_ERROR_SEND_HEADER_FAILED (-2)
#define HTTPC_ERROR_SEND_PAYLOAD_FAILED (-3)
#define HTTPC_ERROR_NOT_CONNECTED (-4)
#define HTTPC_ERROR_CONNECTION_LOST (-5)
#define HTTPC_ERROR_NO_STREAM (-6)
#define HTTPC_ERROR_NO_HTTP_SERVER (-7)
#define HTTPC_ERROR_TOO_LESS_RAM (-8)
#define HTTPC_ERROR_ENCODING (-9)
#define HTTPC_ERROR_STREAM_WRITE (-10)
#define HTTPC_ERROR_READ_TIMEOUT (-11)

enum { HTTPC_DISABLE_FOLLOW_REDIRECTS, HTTPC_STRICT_FOLLOW_REDIRECTS, HTTP_CODE_OK = 200 };

inline int simCurlExitCodeToHttpError(int curlExitCode) {
  switch (curlExitCode) {
    case 6:
      return HTTPC_ERROR_NO_HTTP_SERVER;
    case 7:
      return HTTPC_ERROR_CONNECTION_REFUSED;
    case 28:
      return HTTPC_ERROR_READ_TIMEOUT;
    case 52:
    case 55:
    case 56:
      return HTTPC_ERROR_CONNECTION_LOST;
    default:
      return HTTPC_ERROR_CONNECTION_REFUSED;
  }
}

class HTTPClient {
 public:
  HTTPClient() {}
  ~HTTPClient() {}

  void begin(NetworkClient& client, const char* url) {
    (void)client;
    url_ = url ? url : "";
    responseBody_.s.clear();
    responseStream_.reset();
    statusCode_ = 0;
    responseComplete_ = false;
    responseHeaders_.clear();
  }
  void setFollowRedirects(int mode) { followRedirects_ = mode != HTTPC_DISABLE_FOLLOW_REDIRECTS; }
  void setReuse(bool reuse) { (void)reuse; }
  void setConnectTimeout(int32_t timeout) {
    if (timeout > 0) timeouts_.connectMs = static_cast<unsigned long>(timeout);
  }
  void setTimeout(uint16_t timeout) { timeouts_.readMs = timeout; }
  void addHeader(const char* name, const String& value) {
    if (name) headers_[name] = value.c_str();
  }
  void addHeader(const char* name, const char* value) {
    if (name) headers_[name] = value ? value : "";
  }
  void setAuthorization(const char* user, const char* pass) {
    if (user && pass) basicAuth_ = std::string(user) + ":" + pass;
  }

  int GET() { return perform("GET", nullptr); }
  int POST() { return perform("POST", ""); }
  int POST(const char* body) { return perform("POST", body ? body : ""); }
  int PUT(const char* body) { return perform("PUT", body ? body : ""); }
  int PUT(const String& body) { return perform("PUT", body.c_str()); }

  String getString() { return responseBody_; }
  int getSize() { return static_cast<int>(responseBody_.length()); }
  bool responseComplete() const { return responseComplete_; }
  int getStatus() const { return statusCode_; }
  bool hasContentLength() const { return responseHeaders_.count("content-length") != 0; }
  size_t getContentLength() const { return std::strtoull(getHeader("content-length").c_str(), nullptr, 10); }
  std::string getHeader(std::string name) const {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto it = responseHeaders_.find(name);
    return it == responseHeaders_.end() ? "" : it->second;
  }
  const std::map<std::string, std::string>& getHeaders() const { return responseHeaders_; }
  Stream* getStreamPtr() {
    if (!responseStream_) responseStream_ = std::make_unique<ResponseBodyStream>(responseBody_);
    return responseStream_.get();
  }
  bool connected() { return responseStream_ ? responseStream_->available() > 0 : statusCode_ > 0; }
  int writeToStream(Stream* stream) {
    if (!stream) return 0;
    return static_cast<int>(
        stream->write(reinterpret_cast<const uint8_t*>(responseBody_.c_str()), responseBody_.length()));
  }

  void end() {
    url_.clear();
    headers_.clear();
    basicAuth_.clear();
    responseBody_ = "";
    responseStream_.reset();
    statusCode_ = 0;
    responseComplete_ = false;
    responseHeaders_.clear();
  }

  static String errorToString(int error) {
    switch (error) {
      case HTTPC_ERROR_CONNECTION_REFUSED:
        return String("connection refused");
      case HTTPC_ERROR_SEND_HEADER_FAILED:
        return String("send header failed");
      case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
        return String("send payload failed");
      case HTTPC_ERROR_NOT_CONNECTED:
        return String("not connected");
      case HTTPC_ERROR_CONNECTION_LOST:
        return String("connection lost");
      case HTTPC_ERROR_NO_STREAM:
        return String("no stream");
      case HTTPC_ERROR_NO_HTTP_SERVER:
        return String("no HTTP server");
      case HTTPC_ERROR_TOO_LESS_RAM:
        return String("too less ram");
      case HTTPC_ERROR_ENCODING:
        return String("Transfer-Encoding not supported");
      case HTTPC_ERROR_STREAM_WRITE:
        return String("Stream write error");
      case HTTPC_ERROR_READ_TIMEOUT:
        return String("read Timeout");
      default:
        return String(std::to_string(error));
    }
  }

 private:
  class ResponseBodyStream final : public Stream {
   public:
    explicit ResponseBodyStream(const String& body) : body_(body.s) {}

    int available() override { return position_ < body_.size() ? static_cast<int>(body_.size() - position_) : 0; }
    int read() override {
      if (position_ >= body_.size()) return -1;
      return static_cast<uint8_t>(body_[position_++]);
    }
    int peek() override {
      if (position_ >= body_.size()) return -1;
      return static_cast<uint8_t>(body_[position_]);
    }
    size_t write(uint8_t value) override {
      (void)value;
      return 0;
    }

   private:
    std::string body_;
    size_t position_ = 0;
  };

  std::string url_;
  std::map<std::string, std::string> headers_;
  std::string basicAuth_;
  String responseBody_;
  std::unique_ptr<ResponseBodyStream> responseStream_;
  int statusCode_ = 0;
  bool responseComplete_ = false;
  bool followRedirects_ = true;
  sim_http_fetch::Timeouts timeouts_;
  std::map<std::string, std::string> responseHeaders_;

  int perform(const char* method, const char* body) {
    if (url_.empty()) return 0;

    sim_http_fetch::Response response;
    if (!sim_http_fetch::fetch(url_, method, headers_, basicAuth_, body, response, followRedirects_, timeouts_)) {
      responseBody_ = "";
      responseStream_.reset();
      statusCode_ = 0;
      responseComplete_ = false;
      responseHeaders_.clear();
      return simCurlExitCodeToHttpError(response.curlExitCode);
    }

    responseBody_ = response.body;
    responseHeaders_ = std::move(response.headers);
    responseStream_.reset();
    statusCode_ = response.statusCode;
    responseComplete_ = response.curlExitCode == 0;
    return statusCode_;
  }
};
