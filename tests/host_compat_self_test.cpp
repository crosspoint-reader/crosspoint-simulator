#ifdef NDEBUG
#undef NDEBUG
#endif

#include <Arduino.h>
#include <HalClock.h>
#include <IPAddress.h>
#include <esp_http_client.h>
#include <mbedtls/sha256.h>

#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
struct HeaderCapture {
  std::vector<std::pair<std::string, std::string>> fields;
};

esp_err_t captureHeader(esp_http_client_event_t *event) {
  if (!event || event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data)
    return ESP_FAIL;
  auto *capture = static_cast<HeaderCapture *>(event->user_data);
  capture->fields.emplace_back(event->header_key, event->header_value);
  return ESP_OK;
}
} // namespace

int main() {
  std::array<uint8_t, 32> randomBytes{};
  esp_fill_random(randomBytes.data(), randomBytes.size());
  for (int i = 0; i < 32; ++i) {
    const long value = random(std::numeric_limits<long>::max());
    assert(value >= 0 && value < std::numeric_limits<long>::max());
  }
  assert(random(0) == 0);
  assert(random(7, 7) == 7);

  IPAddress address(192, 168, 1, 2);
  assert(std::string(address.toString().c_str()) == "192.168.1.2");
  assert(address[0] == 192);
  address[3] = 9;
  assert(address[3] == 9);

  HalClock clock;
  clock.begin();
  assert(clock.hasValidTime());
  assert(clock.syncState() == ClockSyncState::Idle);
  clock.update();
  assert(clock.syncState() == ClockSyncState::Succeeded);

  clock.setAutoSyncEnabled(false);
  const std::time_t manualTime = std::time(nullptr) + 3600;
  assert(clock.setUtcTime(manualTime));
  assert(std::llabs(static_cast<long long>(clock.nowUtc() - manualTime)) <= 1);
  assert(clock.requestSync());
  assert(clock.syncState() == ClockSyncState::Succeeded);
  assert(std::llabs(static_cast<long long>(clock.nowUtc() - std::time(nullptr))) <= 1);

  constexpr char rawHeaders[] =
      "HTTP/1.1 302 Found\r\n"
      "Set-Cookie: wr_rt=redirect; Path=/\r\n"
      "Connection: close\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Location: https://example.invalid/final\r\n\r\n"
      "HTTP/1.1 200 OK\r\n"
      "Set-Cookie:\twr_skey=final; HttpOnly\r\n"
      "Connection: keep-alive\r\n\r\n";
  sim_http_fetch::Response response;
  sim_http_fetch::parseHeaders(rawHeaders, response);

  HeaderCapture capture;
  esp_http_client_config_t config{};
  config.url = "https://example.invalid/start";
  config.event_handler = captureHeader;
  config.user_data = &capture;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  assert(client);
  sim_http_client_detail::emitHeaders(client, response);
  assert(capture.fields.size() == 6);
  assert(capture.fields[0].first == "Set-Cookie");
  assert(capture.fields[0].second == "wr_rt=redirect; Path=/");
  assert(capture.fields[4].first == "Set-Cookie");
  assert(capture.fields[4].second == "wr_skey=final; HttpOnly");
  assert(!esp_http_client_is_chunked_response(client));
  assert(esp_http_client_is_persistent_connection(client));

  constexpr char finalCloseHeaders[] =
      "HTTP/1.1 200 OK\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n\r\n";
  sim_http_fetch::parseHeaders(finalCloseHeaders, response);
  sim_http_client_detail::emitHeaders(client, response);
  assert(esp_http_client_is_chunked_response(client));
  assert(!esp_http_client_is_persistent_connection(client));

  esp_http_client_handle_t reused = client;
  assert(esp_http_client_set_url(client, "https://example.invalid/chapter/2") == ESP_OK);
  assert(esp_http_client_set_method(client, HTTP_METHOD_POST) == ESP_OK);
  assert(esp_http_client_set_timeout_ms(client, 1234) == ESP_OK);
  assert(esp_http_client_set_header(client, "Cookie", "chapter=1") == ESP_OK);
  assert(esp_http_client_set_header(client, "cookie", "chapter=2") == ESP_OK);
  assert(client->headers.size() == 1);
  assert(esp_http_client_open(client, 6) == ESP_OK);
  assert(esp_http_client_write(client, "abcdef", 6) == 6);
  assert(client == reused);
  assert(client->requestBody == "abcdef");

  client->responseBody = "0123456789abcdef";
  client->bodyOffset = 0;
  client->performed = true;
  client->complete = true;
  std::string body;
  std::array<char, 3> chunk{};
  while (true) {
    const int read = esp_http_client_read(client, chunk.data(), chunk.size());
    assert(read >= 0);
    if (read == 0)
      break;
    body.append(chunk.data(), static_cast<size_t>(read));
  }
  assert(body == "0123456789abcdef");
  assert(esp_http_client_is_complete_data_received(client));

  client->statusCode = 200;
  client->contentLength = 5;
  client->responseBody = "stale";
  client->bodyOffset = 2;
  client->complete = true;
  client->persistent = true;
  client->chunked = true;
  assert(esp_http_client_set_url(client, "") == ESP_OK);
  assert(esp_http_client_perform(client) == ESP_FAIL);
  assert(esp_http_client_get_status_code(client) == 0);
  assert(esp_http_client_get_content_length(client) == -1);
  assert(client->responseBody.empty());
  assert(client->bodyOffset == 0);
  assert(!esp_http_client_is_complete_data_received(client));
  assert(!esp_http_client_is_persistent_connection(client));
  assert(!esp_http_client_is_chunked_response(client));
  assert(esp_http_client_cleanup(client) == ESP_OK);

  mbedtls_sha256_context sha{};
  unsigned char digest[32]{};
  assert(mbedtls_sha256_starts(&sha, 0) == 0);
  assert(mbedtls_sha256_update(
             &sha, reinterpret_cast<const unsigned char *>("abc"), 3) == 0);
  assert(mbedtls_sha256_finish(&sha, digest) == 0);
  constexpr unsigned char expectedPrefix[] = {0xba, 0x78, 0x16, 0xbf};
  assert(std::memcmp(digest, expectedPrefix, sizeof(expectedPrefix)) == 0);
}
