#pragma once

#include <algorithm>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <strings.h>

#include "SimHttpFetch.h"
#include "esp_err.h"

enum http_event {
  HTTP_EVENT_ERROR = 0,
  HTTP_EVENT_ON_CONNECTED,
  HTTP_EVENT_HEADERS_SENT,
  HTTP_EVENT_ON_HEADER,
  HTTP_EVENT_ON_DATA,
  HTTP_EVENT_ON_FINISH,
  HTTP_EVENT_DISCONNECTED,
  HTTP_EVENT_REDIRECT,
};

enum esp_http_client_method_t {
  HTTP_METHOD_GET,
  HTTP_METHOD_POST,
  HTTP_METHOD_PUT,
  HTTP_METHOD_DELETE,
  HTTP_METHOD_HEAD,
};

enum esp_http_client_auth_type_t {
  HTTP_AUTH_TYPE_NONE = 0,
  HTTP_AUTH_TYPE_BASIC,
  HTTP_AUTH_TYPE_DIGEST,
};

struct SimEspHttpClient;
typedef SimEspHttpClient *esp_http_client_handle_t;

struct esp_http_client_event_t {
  http_event event_id;
  esp_http_client_handle_t client;
  void *data;
  int data_len;
  void *user_data;
  const char *header_key;
  const char *header_value;
};

typedef esp_err_t (*http_event_handler_cb)(esp_http_client_event_t *evt);
typedef http_event_handler_cb esp_http_client_event_cb_t;

struct esp_http_client_config_t {
  const char *url = nullptr;
  http_event_handler_cb event_handler = nullptr;
  int timeout_ms = 0;
  int buffer_size = 0;
  int buffer_size_tx = 0;
  void *user_data = nullptr;
  esp_http_client_method_t method = HTTP_METHOD_GET;
  bool skip_cert_common_name_check = false;
  esp_err_t (*crt_bundle_attach)(void *conf) = nullptr;
  const char *cert_pem = nullptr;
  int cert_len = 0;
  bool keep_alive_enable = false;
  const char *username = nullptr;
  const char *password = nullptr;
  esp_http_client_auth_type_t auth_type = HTTP_AUTH_TYPE_NONE;
};

struct SimEspHttpClient {
  esp_http_client_config_t config{};
  std::string url;
  std::map<std::string, std::string> headers;
  std::string requestBody;
  int statusCode = 0;
  int contentLength = -1;
  std::string responseBody;
  size_t bodyOffset = 0;
  bool opened = false;
  bool performed = false;
  bool complete = false;
  bool persistent = true;
  bool chunked = false;
};

namespace sim_http_client_detail {
inline const char *methodName(esp_http_client_method_t method) {
  switch (method) {
  case HTTP_METHOD_POST:
    return "POST";
  case HTTP_METHOD_PUT:
    return "PUT";
  case HTTP_METHOD_DELETE:
    return "DELETE";
  case HTTP_METHOD_HEAD:
    return "HEAD";
  case HTTP_METHOD_GET:
  default:
    return "GET";
  }
}

inline bool hasToken(const std::string &value, const char *token) {
  size_t start = 0;
  while (start < value.size()) {
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t' || value[start] == ','))
      ++start;
    size_t end = value.find(',', start);
    if (end == std::string::npos)
      end = value.size();
    size_t trimmedEnd = end;
    while (trimmedEnd > start &&
           (value[trimmedEnd - 1] == ' ' || value[trimmedEnd - 1] == '\t'))
      --trimmedEnd;
    if (trimmedEnd - start == std::strlen(token) &&
        strncasecmp(value.data() + start, token, trimmedEnd - start) == 0)
      return true;
    start = end + 1;
  }
  return false;
}

inline void emitHeaders(esp_http_client_handle_t handle,
                        const sim_http_fetch::Response &response) {
  handle->persistent = true;
  handle->chunked = false;
  for (size_t i = 0; i < response.headers.size(); ++i) {
    const auto &header = response.headers[i];
    if (i >= response.finalHeadersBegin) {
      if (strcasecmp(header.first.c_str(), "Connection") == 0 &&
          hasToken(header.second, "close")) {
        handle->persistent = false;
      } else if (strcasecmp(header.first.c_str(), "Transfer-Encoding") == 0 &&
                 hasToken(header.second, "chunked")) {
        handle->chunked = true;
      }
    }

    if (!handle->config.event_handler)
      continue;
    esp_http_client_event_t event{};
    event.event_id = HTTP_EVENT_ON_HEADER;
    event.client = handle;
    event.user_data = handle->config.user_data;
    event.header_key = header.first.c_str();
    event.header_value = header.second.c_str();
    handle->config.event_handler(&event);
  }
}

inline bool transfer(esp_http_client_handle_t handle, bool emitBody) {
  if (!handle)
    return false;

  handle->statusCode = 0;
  handle->contentLength = -1;
  handle->responseBody.clear();
  handle->bodyOffset = 0;
  handle->performed = true;
  handle->complete = false;
  handle->persistent = false;
  handle->chunked = false;
  if (handle->url.empty())
    return false;

  std::string basicAuth;
  if (handle->config.username &&
      handle->config.auth_type != HTTP_AUTH_TYPE_NONE) {
    basicAuth = handle->config.username;
    basicAuth += ':';
    if (handle->config.password)
      basicAuth += handle->config.password;
  }

  sim_http_fetch::Response response;
  if (!sim_http_fetch::fetch(
          handle->url, methodName(handle->config.method), handle->headers,
          basicAuth,
          handle->requestBody.empty() ? nullptr : handle->requestBody.c_str(),
          response, handle->config.timeout_ms)) {
    return false;
  }

  handle->statusCode = response.statusCode;
  handle->responseBody = std::move(response.body);
  handle->contentLength = static_cast<int>(handle->responseBody.size());
  handle->bodyOffset = 0;
  handle->opened = true;
  handle->complete = true;
  emitHeaders(handle, response);

  if (emitBody && handle->config.event_handler &&
      !handle->responseBody.empty()) {
    esp_http_client_event_t event{};
    event.event_id = HTTP_EVENT_ON_DATA;
    event.client = handle;
    event.data = handle->responseBody.data();
    event.data_len = static_cast<int>(handle->responseBody.size());
    event.user_data = handle->config.user_data;
    handle->config.event_handler(&event);
    handle->bodyOffset = handle->responseBody.size();
  }
  return true;
}
} // namespace sim_http_client_detail

inline esp_err_t esp_http_client_set_header(esp_http_client_handle_t handle,
                                            const char *name,
                                            const char *value) {
  if (!handle || !name)
    return ESP_FAIL;
  for (auto it = handle->headers.begin(); it != handle->headers.end(); ++it) {
    if (strcasecmp(it->first.c_str(), name) == 0) {
      if (it->first == name) {
        it->second = value ? value : "";
      } else {
        handle->headers.erase(it);
        handle->headers[name] = value ? value : "";
      }
      return ESP_OK;
    }
  }
  handle->headers[name] = value ? value : "";
  return ESP_OK;
}

inline esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t handle,
                                                const char *data, int len) {
  if (!handle || len < 0 || (!data && len > 0))
    return ESP_FAIL;
  handle->requestBody.assign(data ? data : "", static_cast<size_t>(len));
  return ESP_OK;
}

inline esp_err_t esp_http_client_set_url(esp_http_client_handle_t handle,
                                         const char *url) {
  if (!handle || !url)
    return ESP_FAIL;
  handle->url = url;
  return ESP_OK;
}

inline esp_err_t
esp_http_client_set_method(esp_http_client_handle_t handle,
                           esp_http_client_method_t method) {
  if (!handle)
    return ESP_FAIL;
  handle->config.method = method;
  return ESP_OK;
}

inline esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t handle,
                                                int timeoutMs) {
  if (!handle)
    return ESP_FAIL;
  handle->config.timeout_ms = timeoutMs;
  return ESP_OK;
}

inline esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t handle,
                                               void *userData) {
  if (!handle)
    return ESP_FAIL;
  handle->config.user_data = userData;
  return ESP_OK;
}

inline bool
esp_http_client_is_chunked_response(esp_http_client_handle_t handle) {
  return handle && handle->chunked;
}

inline int esp_http_client_get_content_length(esp_http_client_handle_t handle) {
  return handle ? handle->contentLength : -1;
}

inline esp_err_t esp_http_client_get_chunk_length(esp_http_client_handle_t,
                                                  int *len) {
  if (len)
    *len = 0;
  return ESP_OK;
}

inline int esp_http_client_get_status_code(esp_http_client_handle_t handle) {
  return handle ? handle->statusCode : 0;
}

inline esp_http_client_handle_t
esp_http_client_init(const esp_http_client_config_t *config) {
  if (!config || !config->url)
    return nullptr;
  auto *handle = new (std::nothrow) SimEspHttpClient();
  if (!handle)
    return nullptr;
  handle->config = *config;
  handle->url = config->url;
  return handle;
}

inline esp_err_t esp_http_client_perform(esp_http_client_handle_t handle) {
  return sim_http_client_detail::transfer(handle, true) ? ESP_OK : ESP_FAIL;
}

inline esp_err_t esp_http_client_open(esp_http_client_handle_t handle,
                                      int /*write_len*/) {
  if (!handle || handle->url.empty())
    return ESP_FAIL;
  handle->requestBody.clear();
  handle->responseBody.clear();
  handle->bodyOffset = 0;
  handle->statusCode = 0;
  handle->contentLength = -1;
  handle->opened = true;
  handle->performed = false;
  handle->complete = false;
  handle->persistent = true;
  handle->chunked = false;
  return ESP_OK;
}

inline int esp_http_client_write(esp_http_client_handle_t handle,
                                 const char *data, int len) {
  if (!handle || len < 0 || (!data && len > 0))
    return -1;
  handle->requestBody.append(data ? data : "", static_cast<size_t>(len));
  return len;
}

inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t handle) {
  if (!handle || !handle->opened)
    return -1;
  if (!handle->performed &&
      !sim_http_client_detail::transfer(handle, false))
    return -1;
  return static_cast<int64_t>(handle->responseBody.size());
}

inline esp_err_t
esp_http_client_set_redirection(esp_http_client_handle_t /*handle*/) {
  // curl -L already follows redirects.
  return ESP_OK;
}

inline esp_err_t esp_http_client_close(esp_http_client_handle_t handle) {
  if (!handle)
    return ESP_FAIL;
  handle->requestBody.clear();
  handle->responseBody.clear();
  handle->bodyOffset = 0;
  handle->opened = false;
  handle->performed = false;
  handle->complete = false;
  return ESP_OK;
}

inline int esp_http_client_read(esp_http_client_handle_t handle, char *buf,
                                int len) {
  if (!handle || !handle->opened || !handle->performed || !buf || len <= 0)
    return -1;
  const size_t remaining = handle->responseBody.size() - handle->bodyOffset;
  const size_t toRead = std::min(static_cast<size_t>(len), remaining);
  if (toRead == 0)
    return 0;
  std::memcpy(buf, handle->responseBody.data() + handle->bodyOffset, toRead);
  handle->bodyOffset += toRead;
  return static_cast<int>(toRead);
}

inline bool
esp_http_client_is_complete_data_received(esp_http_client_handle_t handle) {
  return handle && handle->performed && handle->complete &&
         handle->bodyOffset >= handle->responseBody.size();
}

inline bool
esp_http_client_is_persistent_connection(esp_http_client_handle_t handle) {
  return handle && handle->persistent;
}

inline esp_err_t esp_http_client_get_and_clear_last_tls_error(
    esp_http_client_handle_t, int *esp_tls_code, int *esp_tls_flags) {
  if (esp_tls_code)
    *esp_tls_code = 0;
  if (esp_tls_flags)
    *esp_tls_flags = 0;
  return ESP_OK;
}

inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t handle) {
  if (!handle)
    return ESP_FAIL;
  delete handle;
  return ESP_OK;
}

extern "C" {
inline esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }
}
