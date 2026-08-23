#include "SimBleProtocol.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace crosspoint_simulator::ble {

namespace {

// ---------------------------------------------------------------------------
// Hex
// ---------------------------------------------------------------------------

int hexDigit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// ---------------------------------------------------------------------------
// A parsed JSON value. Client ops are flat, so only these four types are
// kept; a nested object or array is skipped and remembered as Other so a
// wrong-type field reports as a type error instead of a missing field.
// ---------------------------------------------------------------------------

enum class VType { Str, Num, Bool, Null, Other };

struct Value {
  VType type = VType::Null;
  std::string str;
  double num = 0.0;
  bool boolean = false;
};

struct Member {
  std::string key;
  Value value;
};

class Parser {
public:
  Parser(const char *data, size_t len) : p_(data), end_(data + len) {}

  // Parses one top level object into `out`. Returns false and fills `err` on
  // anything that is not exactly one object followed by whitespace.
  bool parseTopObject(std::vector<Member> &out, std::string &err) {
    skipWs();
    if (!parseObject(out, err))
      return false;
    skipWs();
    if (p_ != end_) {
      err = "trailing bytes after the object";
      return false;
    }
    return true;
  }

private:
  const char *p_;
  const char *end_;

  bool atEnd() const { return p_ >= end_; }
  char peek() const { return *p_; }

  void skipWs() {
    while (!atEnd() && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n'))
      ++p_;
  }

  bool expect(char c, std::string &err) {
    if (atEnd() || *p_ != c) {
      err = std::string("expected '") + c + "'";
      return false;
    }
    ++p_;
    return true;
  }

  bool parseObject(std::vector<Member> &out, std::string &err) {
    if (!expect('{', err))
      return false;
    skipWs();
    if (!atEnd() && peek() == '}') {
      ++p_;
      return true;
    }
    while (true) {
      skipWs();
      Member m;
      if (!parseString(m.key, err))
        return false;
      skipWs();
      if (!expect(':', err))
        return false;
      skipWs();
      if (!parseValue(m.value, 0, err))
        return false;
      if (out.size() >= kMaxObjectKeys) {
        err = "too many keys";
        return false;
      }
      out.push_back(std::move(m));
      skipWs();
      if (atEnd()) {
        err = "unterminated object";
        return false;
      }
      if (peek() == ',') {
        ++p_;
        continue;
      }
      if (peek() == '}') {
        ++p_;
        return true;
      }
      err = "expected ',' or '}'";
      return false;
    }
  }

  // Appends `cp` to `out` as UTF-8. Rejects NUL so every parsed string stays
  // usable as a C string by the consumer.
  bool appendUtf8(uint32_t cp, std::string &out, std::string &err) {
    if (cp == 0) {
      err = "NUL in a string";
      return false;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return true;
  }

  bool parseHex4(uint32_t &out, std::string &err) {
    if (end_ - p_ < 4) {
      err = "truncated \\u escape";
      return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const int d = hexDigit(p_[i]);
      if (d < 0) {
        err = "bad \\u escape";
        return false;
      }
      v = (v << 4) | static_cast<uint32_t>(d);
    }
    p_ += 4;
    out = v;
    return true;
  }

  bool parseString(std::string &out, std::string &err) {
    if (!expect('"', err))
      return false;
    out.clear();
    while (true) {
      if (atEnd()) {
        err = "unterminated string";
        return false;
      }
      const unsigned char c = static_cast<unsigned char>(*p_);
      if (c == '"') {
        ++p_;
        return true;
      }
      if (c < 0x20) {
        // A raw control byte, a raw NUL included, is not valid JSON. Rejecting
        // here is what keeps a binary blob on the socket from being parsed as
        // half an op.
        err = "raw control byte in a string";
        return false;
      }
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        ++p_;
        continue;
      }
      ++p_;
      if (atEnd()) {
        err = "truncated escape";
        return false;
      }
      const char esc = *p_++;
      switch (esc) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '/':
        out.push_back('/');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        uint32_t cp = 0;
        if (!parseHex4(cp, err))
          return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          // High surrogate. A low surrogate must follow or the string is bad.
          if (end_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') {
            err = "lone surrogate";
            return false;
          }
          p_ += 2;
          uint32_t low = 0;
          if (!parseHex4(low, err))
            return false;
          if (low < 0xDC00 || low > 0xDFFF) {
            err = "bad surrogate pair";
            return false;
          }
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          err = "lone surrogate";
          return false;
        }
        if (!appendUtf8(cp, out, err))
          return false;
        break;
      }
      default:
        err = "unknown escape";
        return false;
      }
    }
  }

  bool parseNumber(Value &out, std::string &err) {
    const char *start = p_;
    if (!atEnd() && *p_ == '-')
      ++p_;
    if (atEnd() || *p_ < '0' || *p_ > '9') {
      err = "bad number";
      return false;
    }
    if (*p_ == '0') {
      ++p_;
    } else {
      while (!atEnd() && *p_ >= '0' && *p_ <= '9')
        ++p_;
    }
    if (!atEnd() && *p_ == '.') {
      ++p_;
      if (atEnd() || *p_ < '0' || *p_ > '9') {
        err = "bad fraction";
        return false;
      }
      while (!atEnd() && *p_ >= '0' && *p_ <= '9')
        ++p_;
    }
    if (!atEnd() && (*p_ == 'e' || *p_ == 'E')) {
      ++p_;
      if (!atEnd() && (*p_ == '+' || *p_ == '-'))
        ++p_;
      if (atEnd() || *p_ < '0' || *p_ > '9') {
        err = "bad exponent";
        return false;
      }
      while (!atEnd() && *p_ >= '0' && *p_ <= '9')
        ++p_;
    }
    // strtod needs a terminated buffer, and the line is not terminated.
    const std::string token(start, static_cast<size_t>(p_ - start));
    errno = 0;
    char *stop = nullptr;
    const double v = std::strtod(token.c_str(), &stop);
    if (stop != token.c_str() + token.size()) {
      err = "bad number";
      return false;
    }
    out.type = VType::Num;
    out.num = v;
    return true;
  }

  bool parseLiteral(const char *text, Value &out, VType type, bool boolean,
                    std::string &err) {
    const size_t n = std::strlen(text);
    if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, text, n) != 0) {
      err = "bad literal";
      return false;
    }
    p_ += n;
    out.type = type;
    out.boolean = boolean;
    return true;
  }

  // Consumes a nested object or array without keeping it. Depth bounded.
  bool skipNested(int depth, std::string &err) {
    if (depth >= kMaxNestDepth) {
      err = "nesting too deep";
      return false;
    }
    const char open = *p_;
    const char close = (open == '{') ? '}' : ']';
    ++p_;
    while (true) {
      skipWs();
      if (atEnd()) {
        err = "unterminated nested value";
        return false;
      }
      if (peek() == close) {
        ++p_;
        return true;
      }
      if (peek() == ',' || peek() == ':') {
        ++p_;
        continue;
      }
      Value ignored;
      if (!parseValue(ignored, depth + 1, err))
        return false;
    }
  }

  bool parseValue(Value &out, int depth, std::string &err) {
    if (atEnd()) {
      err = "value expected";
      return false;
    }
    switch (peek()) {
    case '"': {
      out.type = VType::Str;
      return parseString(out.str, err);
    }
    case 't':
      return parseLiteral("true", out, VType::Bool, true, err);
    case 'f':
      return parseLiteral("false", out, VType::Bool, false, err);
    case 'n':
      return parseLiteral("null", out, VType::Null, false, err);
    case '{':
    case '[':
      out.type = VType::Other;
      return skipNested(depth, err);
    default:
      return parseNumber(out, err);
    }
  }
};

// ---------------------------------------------------------------------------
// Field access. Every getter reports a wrong type as an error instead of
// falling back to the default, because a client sending "mtu": "517" has a
// bug worth seeing.
// ---------------------------------------------------------------------------

const Value *find(const std::vector<Member> &members, const char *key) {
  for (const Member &m : members) {
    if (m.key == key)
      return &m.value;
  }
  return nullptr;
}

bool getInt(const std::vector<Member> &members, const char *key, long def,
            long lo, long hi, long &out, std::string &err) {
  const Value *v = find(members, key);
  if (!v || v->type == VType::Null) {
    out = def;
    return true;
  }
  if (v->type != VType::Num) {
    err = std::string(key) + ": expected a number";
    return false;
  }
  // Range first. A cast of 1e300 to long is undefined behaviour, so the
  // bounds check has to happen while the value is still a double.
  const double d = v->num;
  if (!(d >= static_cast<double>(lo) && d <= static_cast<double>(hi))) {
    err = std::string(key) + ": out of range";
    return false;
  }
  const long got = static_cast<long>(d);
  if (static_cast<double>(got) != d) {
    err = std::string(key) + ": expected a whole number";
    return false;
  }
  out = got;
  return true;
}

bool getBool(const std::vector<Member> &members, const char *key, bool def,
             bool &out, std::string &err) {
  const Value *v = find(members, key);
  if (!v || v->type == VType::Null) {
    out = def;
    return true;
  }
  if (v->type == VType::Bool) {
    out = v->boolean;
    return true;
  }
  // 0 and 1 are accepted, because a hand written client sends them.
  if (v->type == VType::Num && (v->num == 0.0 || v->num == 1.0)) {
    out = (v->num != 0.0);
    return true;
  }
  err = std::string(key) + ": expected true or false";
  return false;
}

bool getUuid(const std::vector<Member> &members, std::string &out,
             std::string &err) {
  const Value *v = find(members, "uuid");
  if (!v || v->type == VType::Null) {
    err = "uuid: missing";
    return false;
  }
  if (v->type != VType::Str) {
    err = "uuid: expected a string";
    return false;
  }
  if (v->str.empty() || v->str.size() > kMaxUuidChars) {
    err = "uuid: bad length";
    return false;
  }
  out = v->str;
  return true;
}

bool getHex(const std::vector<Member> &members, std::vector<uint8_t> &out,
            std::string &err) {
  const Value *v = find(members, "hex");
  if (!v || v->type == VType::Null) {
    out.clear();
    return true;
  }
  if (v->type != VType::Str) {
    err = "hex: expected a string";
    return false;
  }
  if (!decodeHex(v->str, out)) {
    err = "hex: not an even run of hex digits";
    return false;
  }
  return true;
}

// Connection parameter ranges are the Bluetooth spec ones. Out of range is an
// error, not a clamp: the real stack refuses these too.
constexpr long kMtuMin = 23, kMtuMax = 517;
constexpr long kIntervalMin = 6, kIntervalMax = 3200;
constexpr long kLatencyMax = 499;
constexpr long kTimeoutMin = 10, kTimeoutMax = 3200;

bool readConnParams(const std::vector<Member> &members, SimBleEvent &ev,
                    std::string &err) {
  long interval = 0, latency = 0, timeout = 0;
  if (!getInt(members, "interval", 24, kIntervalMin, kIntervalMax, interval,
              err))
    return false;
  if (!getInt(members, "latency", 0, 0, kLatencyMax, latency, err))
    return false;
  if (!getInt(members, "timeout", 400, kTimeoutMin, kTimeoutMax, timeout, err))
    return false;
  ev.b = static_cast<uint32_t>(interval);
  ev.c = static_cast<uint32_t>(latency);
  ev.d = static_cast<uint32_t>(timeout);
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public codec
// ---------------------------------------------------------------------------

bool decodeHex(const std::string &hex, std::vector<uint8_t> &out) {
  out.clear();
  if ((hex.size() % 2) != 0)
    return false;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hexDigit(hex[i]);
    const int lo = hexDigit(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      out.clear();
      return false;
    }
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

std::string encodeHex(const uint8_t *data, size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  if (!data || len == 0)
    return out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0x0F]);
  }
  return out;
}

std::string encodeHex(const std::vector<uint8_t> &data) {
  return encodeHex(data.data(), data.size());
}

std::string escapeJson(const std::string &raw) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() + 8);
  for (const char ch : raw) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
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
      if (c < 0x20) {
        out += "\\u00";
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 0x0F]);
      } else {
        out.push_back(ch);
      }
      break;
    }
  }
  return out;
}

std::string errorLine(const std::string &msg) {
  return "{\"ev\":\"error\",\"msg\":\"" + escapeJson(msg) + "\"}";
}

bool parseLine(const char *line, size_t len, SimBleEvent &ev,
               std::string &err) {
  err.clear();
  if (!line) {
    err = "empty line";
    return false;
  }
  if (len > kMaxLineBytes) {
    err = "line too long";
    return false;
  }

  std::vector<Member> members;
  members.reserve(8);
  Parser parser(line, len);
  if (!parser.parseTopObject(members, err))
    return false;

  const Value *opValue = find(members, "op");
  if (!opValue) {
    err = "no op field";
    return false;
  }
  if (opValue->type != VType::Str || opValue->str.empty()) {
    err = "op: expected a non-empty string";
    return false;
  }

  SimBleEvent out;
  out.op = opValue->str;

  if (out.op == "connect") {
    long mtu = 0;
    if (!getInt(members, "mtu", 23, kMtuMin, kMtuMax, mtu, err))
      return false;
    if (!readConnParams(members, out, err))
      return false;
    out.a = static_cast<uint32_t>(mtu);
  } else if (out.op == "disconnect") {
    long reason = 0;
    if (!getInt(members, "reason", 0x13, 0, 0xFF, reason, err))
      return false;
    out.a = static_cast<uint32_t>(reason);
  } else if (out.op == "write") {
    if (!getUuid(members, out.uuid, err))
      return false;
    if (!getHex(members, out.data, err))
      return false;
    if (!getBool(members, "response", true, out.flag, err))
      return false;
  } else if (out.op == "subscribe") {
    if (!getUuid(members, out.uuid, err))
      return false;
    long value = 0;
    if (!getInt(members, "value", 1, 0, 3, value, err))
      return false;
    out.a = static_cast<uint32_t>(value);
  } else if (out.op == "confirm") {
    if (!getUuid(members, out.uuid, err))
      return false;
  } else if (out.op == "mtu") {
    long mtu = 0;
    if (!getInt(members, "mtu", 23, kMtuMin, kMtuMax, mtu, err))
      return false;
    out.a = static_cast<uint32_t>(mtu);
  } else if (out.op == "connparams") {
    if (!readConnParams(members, out, err))
      return false;
  } else if (out.op == "rssi") {
    long value = 0;
    if (!getInt(members, "value", -60, -128, 127, value, err))
      return false;
    // The consumer reads this back as int8_t, so store the low byte.
    out.a = static_cast<uint32_t>(static_cast<uint8_t>(value & 0xFF));
  } else if (out.op == "auto_confirm") {
    if (!getBool(members, "enabled", true, out.flag, err))
      return false;
    long delay = 0;
    if (!getInt(members, "delay_ms", 10, 0, 60000, delay, err))
      return false;
    out.a = static_cast<uint32_t>(delay);
  } else {
    err = "unknown op: " + out.op;
    return false;
  }

  ev = std::move(out);
  return true;
}

uint16_t portFromEnv() {
  const char *configured = std::getenv("CROSSPOINT_SIM_BLE_PORT");
  if (!configured || !*configured)
    return 0;
  errno = 0;
  char *stop = nullptr;
  const long parsed = std::strtol(configured, &stop, 10);
  if (errno != 0 || stop == configured || *stop != '\0' || parsed <= 0 ||
      parsed > 65535)
    return 0;
  return static_cast<uint16_t>(parsed);
}

} // namespace crosspoint_simulator::ble
