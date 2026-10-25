#include <ResumableFetch.h>

#include <cassert>
#include <string>

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string base = argv[1];
  assert(freeink::fetchOrigin("HTTPS://user:pass@Example.COM/book") == "https://example.com:443");
  assert(freeink::fetchOrigin("http://[::1]:8080/book") == "http://[::1]:8080");
  assert(freeink::fetchOrigin("bad").empty());
  std::string resolved = base + "/dir/feed?old=1";
  assert(freeink::SecureHttpClient::resolveUrl(resolved, "book", resolved));
  assert(resolved == base + "/dir/book");
  assert(freeink::SecureHttpClient::resolveUrl(base, "//other.test/book", resolved));
  assert(resolved == "http://other.test/book");

  freeink::SecureHttpClient http;
  assert(http.begin(base + "/data"));
  std::string body;
  assert(http.GET([&](const uint8_t *data, size_t len) {
    assert(http.getStatus() == 200);
    assert(http.hasContentLength() && http.getContentLength() == 4);
    assert(http.getHeader("X-TEST") == "value");
    body.append(reinterpret_cast<const char *>(data), len);
    return true;
  }) == 200);
  assert(body == "book" && http.responseComplete());
  assert(!http.getHeaders().empty());
  assert(http.begin(base + "/redirect"));
  assert(http.GET() == 302);
  assert(http.getHeader("Location") == "/data");
  assert(http.begin(base + "/empty"));
  assert(http.getStatus() == 0 && http.getHeader("X-Test").empty());
  assert(http.GET() == 200);
  assert(http.hasContentLength() && http.getContentLength() == 0);
  assert(http.responseComplete());
  assert(http.begin(base + "/unknown"));
  assert(http.GET() == 200 && !http.hasContentLength());
  assert(http.begin(base + "/truncated"));
  assert(http.GET() == 200 && !http.responseComplete());
  assert(http.begin(base + "/data"));
  assert(http.GET([](const uint8_t *, size_t) { return false; }) == 200);
  assert(!http.responseComplete());

  freeink::FetchSink sink;
  body.clear();
  sink.write = [&](const uint8_t *data, size_t len) {
    body.append(reinterpret_cast<const char *>(data), len);
    return true;
  };
  int hops = 0;
  auto result = freeink::fetchResumable(base + "/cross-origin", {},
      [&](freeink::SecureHttpClient &client, bool sameOrigin) {
        assert(sameOrigin == (hops++ == 0));
        if (sameOrigin) client.addHeader("Authorization", "Bearer test");
      }, sink);
  assert(hops == 2 && result.complete && body == "book");
  result = freeink::fetchResumable(base + "/loop", {}, nullptr, sink);
  assert(!result.complete && result.status == 302);
  result = freeink::fetchResumable(base + "/data", {}, nullptr, sink, [] { return true; });
  assert(result.aborted && !result.complete);

  NetworkClient client;
  HTTPClient legacy;
  legacy.begin(client, (base + "/redirect").c_str());
  assert(legacy.GET() == 200 && legacy.getString().s == "book");

  // Timeouts limit each wait for data, not the whole transfer.
  legacy.setTimeout(1000);
  legacy.begin(client, (base + "/slow-start").c_str());
  assert(legacy.GET() == HTTPC_ERROR_READ_TIMEOUT);
  http.setTimeout(4000);
  assert(http.begin(base + "/slow-start"));
  assert(http.GET() == 200 && http.responseComplete());
  http.setTimeout(2000);
  assert(http.begin(base + "/trickle"));
  assert(http.GET() == 200 && http.responseComplete());
  assert(http.getString().s == "slowly");
}
