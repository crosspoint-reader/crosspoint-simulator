#pragma once

// SimBleProtocol -- the wire codec of the simulator's fake BLE peripheral.
//
// One JSON object per line, UTF-8, newline delimited. Client lines carry an
// "op" key; simulator lines carry an "ev" key. docs/ble-shim.md is the
// authoritative field table.
//
// The parse is hand rolled on purpose. The simulator's dependency list is
// small and this code is bound for upstream, so no JSON library is added for
// ten flat object shapes.
//
// Nothing here touches a socket or a thread: SimBleLink.cpp owns those. This
// is pure functions over bytes, so the codec can be tested without a link.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "SimBleLink.h"

namespace crosspoint_simulator::ble {

// Longest single wire line accepted, in bytes, newline excluded. A longer
// line is dropped whole and answered with one `error` event. 64 KiB holds a
// 32 KiB hex payload, which is far more than any firmware GATT write, and it
// bounds what a hostile or wedged client can make the reader buffer.
constexpr size_t kMaxLineBytes = 65536;

// Longest accepted UUID string. The firmware uses the 36-char form; short
// 16-bit and 32-bit forms are accepted too. The cap only stops nonsense.
constexpr size_t kMaxUuidChars = 64;

// Most keys read from one object. A longer object is a malformed line.
constexpr size_t kMaxObjectKeys = 32;

// Deepest nested value skipped while scanning. Client ops are flat, so
// anything deeper is either a mistake or an attempt to blow the stack.
constexpr int kMaxNestDepth = 8;

// Lowercase-hex string to bytes. Rejects odd length and any non-hex
// character. Uppercase input is accepted; output elsewhere is lowercase.
bool decodeHex(const std::string &hex, std::vector<uint8_t> &out);

// Bytes to lowercase hex. Empty input gives an empty string.
std::string encodeHex(const uint8_t *data, size_t len);
std::string encodeHex(const std::vector<uint8_t> &data);

// JSON string body for `raw`, quotes excluded. Escapes the two mandatory
// characters and every control character.
std::string escapeJson(const std::string &raw);

// A complete `error` event line, newline excluded.
std::string errorLine(const std::string &msg);

// Parses one line (newline already stripped) into `ev`.
//
// Returns true when the line was a well formed client op with usable fields;
// `ev` is then fully populated, defaults applied. Returns false on anything
// else and puts a short reason in `err`; the caller answers with an `error`
// event and drops the line. Never reads outside [line, line + len).
bool parseLine(const char *line, size_t len, SimBleEvent &ev, std::string &err);

// Port from CROSSPOINT_SIM_BLE_PORT. Returns 0 when the variable is absent,
// empty, zero or unparseable, which means the feature stays off.
uint16_t portFromEnv();

} // namespace crosspoint_simulator::ble
