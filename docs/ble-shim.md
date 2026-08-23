# The BLE peripheral shim

The simulator fakes a BLE peripheral. Firmware BLE code runs unchanged; only
the radio is replaced. A client speaks a line protocol over a TCP socket and
plays the part of the central (the phone).

**Shim** here means a header-compatible fake: it declares the same C++ API as
the real NimBLE library and implements it differently. No NimBLE source is
compiled.

Status: **seed**. This file was created before the implementation, so it states
the frozen contract, not measured behaviour. Every claim below is marked
`[contract]` (what the shim must do) or `[verified]` (observed running, with
the command that showed it). The transport half is `[verified]`: see
"Transport specifics" below. The GATT half is still `[contract]`.

## What it cannot answer

Write this down first, because it is the part that gets forgotten.

- **Heap.** The real NimBLE host and BT controller are the biggest single RAM
  consumer on a device. The simulator's `esp_get_free_heap_size()` returns a
  flat 1000000 (`src/esp_system.h:4`), so a feature that fits here can still
  fail to fit on hardware. `[verified]` by reading `src/esp_system.h`.
- **The radio.** Range, RSSI (faked, see the `rssi` op), interference, and
  whatever coexistence rules the target platform has.
- **The peer's real GATT stack.** A python client agreeing with the firmware
  proves the firmware self-consistent, not interoperable with a phone's stack.

## Turning it on

```
CROSSPOINT_SIM_BLE_PORT=8765     # absent or 0 = feature off
```

Off by default. A simulator run with no BLE client behaves exactly as it did
before the shim existed. The `CROSSPOINT_SIM_*` prefix matches the existing
simulator env vars (`CROSSPOINT_SIM_INPUT_SCRIPT`, `CROSSPOINT_SIM_SCREENSHOTS`).

## Wire protocol `[contract]`

One TCP listener on loopback. The simulator is the server. Newline-delimited
JSON, one object per line, UTF-8. One client at a time; a second connecting
client is refused with an `error` line.

Binary payloads are lowercase hex strings. UUIDs are the full 36-char form the
firmware uses.

### Client to simulator

| op | fields | effect in the shim |
|---|---|---|
| `connect` | `mtu` (default 23), `interval` (units, default 24), `latency`, `timeout` | fires `onConnect`, then `onMTUChange`, then `onConnParamsUpdate`; stops advertising |
| `disconnect` | `reason` (default 0x13) | fires `onDisconnect`; the shim does **not** resume advertising by itself, because NimBLE does not |
| `write` | `uuid`, `hex`, `response` (default true) | sets the characteristic value, fires `onWrite` |
| `subscribe` | `uuid`, `value` (0 off, 1 notify, 2 indicate, 3 both) | fires `onSubscribe` with that `subValue` |
| `confirm` | `uuid` | fires `onStatus` with `BLE_HS_EDONE` for the pending indication |
| `mtu` | `mtu` | fires `onMTUChange` |
| `connparams` | `interval`, `latency`, `timeout` | fires `onConnParamsUpdate`. This is the central's answer to a device request |
| `rssi` | `value` | what `ble_gap_conn_rssi()` returns from now on |
| `auto_confirm` | `enabled` (default **true**), `delay_ms` (default 10) | confirm every indication automatically after the delay. Set false to drive confirms by hand or to test a timeout |

`auto_confirm` defaults on so a simple client does not have to know the confirm
dance exists. A timeout test turns it off.

### Simulator to client

| ev | fields | when |
|---|---|---|
| `stack` | `state`: `up`/`down` | `NimBLEDevice::init` / `deinit` |
| `gatt` | `service`, `chars`: `[{uuid, props}]` | after the firmware builds the table |
| `advertising` | `up`, `interval_min`, `interval_max`, `name`, `service` | every `start()`/`stop()`, including an interval change |
| `indicate` | `uuid`, `hex` | `indicate()` accepted a payload into the pending slot |
| `clobber` | `uuid`, `dropped_hex` | a new `indicate()` overwrote an unconfirmed one. **Not a real BLE event**: it exists to make the clobber observable instead of silent |
| `connparams_request` | `min`, `max`, `latency`, `timeout` | firmware called `updateConnParams` |
| `error` | `msg` | a client op the real stack would refuse |

### Rules the shim enforces, because the real stack does `[contract]`

- `write` or `subscribe` with no central connected: `error`, no callback.
- `indicate()` with nobody subscribed to that characteristic: returns false.
- A subscription belongs to a connection. On `disconnect` the shim clears
  subscription state itself, because NimBLE fires no unsubscribe callback.
- Advertising stops on connect and is not restarted by the shim.

## Transport specifics `[verified]`

The socket, the reader thread and the line framing live in
`src/SimBleLink.cpp`. The codec lives in `src/SimBleProtocol.h` and
`src/SimBleProtocol.cpp`. Neither touches GATT or firmware code.

Everything in this section was demonstrated by

```
python3 tests/sim_ble_link_selftest.py        # 65 checks, all pass
SELFTEST_SANITIZE=1 python3 tests/sim_ble_link_selftest.py       # ASan + UBSan
SELFTEST_SANITIZE=thread python3 tests/sim_ble_link_selftest.py  # TSan
```

The gate builds `tests/sim_ble_link_selftest.cpp` against `SimBleLink.cpp` and
`SimBleProtocol.cpp` and nothing else: no simulator, no firmware, no GATT
model. So the transport is provable before the GATT half exists. The driver
sends every client op twice, once with explicit fields and once with all
fields absent, and the harness prints the decoded `SimBleEvent` for each. All
three runs come back clean. ThreadSanitizer dies on this kernel unless ASLR is
off, so the driver wraps the binary in `setarch -R`
(`tests/sim_ble_link_selftest.py:75-79`).

### Loopback only

The listener binds `INADDR_LOOPBACK` (`src/SimBleLink.cpp:336`), never
`INADDR_ANY`. This is a hazard decision, not a style one: the process on the
other end of this socket runs firmware command handling, so a LAN-reachable
port would hand the device model to anything on the network. The gate connects
to the host's own routable address and requires a refusal.

Backlog is 4 (`src/SimBleLink.cpp:339`). A second client has to complete its
connect before it can be told to go away.

### stop() wakes a blocked reader with a self-pipe

The reader thread never blocks in `accept()` or `recv()`. It sits in `poll()`
over three fds: the listener, the connected client, and the read end of a
self-pipe (`src/SimBleLink.cpp:250-299`). `accept()` and `recv()` run only on
a fd `poll()` already reported readable, and the `recv()` uses `MSG_DONTWAIT`
(`src/SimBleLink.cpp:286`).

`stop()` sets a stop flag, writes one byte into the self-pipe
(`src/SimBleLink.cpp:89-95`), shuts a connected client down, then joins
(`src/SimBleLink.cpp:368-378`). Measured: 0 to 2 ms with a client connected.

Why the self-pipe and not the alternatives:

- `shutdown()` on a **listening** socket is not portable. On Linux it does not
  reliably wake an `accept()`.
- Closing a fd another thread is polling is a use after free waiting to
  happen: the number can be handed to a new socket between the close and the
  wake.
- A poll timeout loop would work but would either burn wakeups or add latency
  to `stop()`. The pipe costs two fds and is exact.

`stop()` is safe when the link was never started and safe twice in a row
(`src/SimBleLink.cpp:360-366`). `start(0)` returns false and leaves the
feature off.

### The reader owns the client fd

Only the reader thread closes the client socket. `emit()` writes under the
mutex and, on a write failure, calls `shutdown()` rather than `close()`
(`src/SimBleLink.cpp:427-431`). That makes the reader's `poll()` return and
keeps the teardown in one place.

The client socket carries `SO_SNDTIMEO` of 5 s (`src/SimBleLink.cpp:47`,
`src/SimBleLink.cpp:236-241`). A client that stops reading cannot hang a
firmware thread inside `emit()` forever: the send fails, the link is dropped,
the simulator carries on.

### emit() is one line, whole, under one mutex

`emit()` frames and writes the whole line while holding the state mutex
(`src/SimBleLink.cpp:410-433`), so two threads cannot interleave halves of two
lines. Verified: two threads emitting 300 lines each, 600 lines received, every
one intact, each thread's lines in its own order, the two threads interleaved
at line granularity.

An embedded `\n` or `\r` in the caller's JSON would inject a second frame, so
`emit()` replaces both with a space (`src/SimBleLink.cpp:414-417`). Callers do
not have to be careful.

### A lost socket synthesizes a disconnect

A client that drops its socket is a link that went away. The reader delivers
`op="disconnect"`, `a=0x13` to the sink so the GATT model does not keep
believing a central is connected (`src/SimBleLink.cpp:131-159`). The teardown
inside `stop()` does **not** synthesize one: `stop()` is not a link event and
the model is being destroyed anyway.

### Line framing and the 65536 byte cap

Bytes are buffered, split on `\n`, and a trailing `\r` is stripped, so
`\r\n` works (`src/SimBleLink.cpp:186-214`). A blank line is neither an op
nor an error. A line split across `recv` boundaries is reassembled. Several
lines in one `recv` are all handled.

One line is capped at **65536 bytes**, newline excluded
(`src/SimBleProtocol.h:29`). The cap is inclusive: a line of exactly 65536
bytes is accepted. Past it the buffer is thrown away, one `error` event goes
out, and every byte up to the next newline is discarded. Framing then
recovers: the next line parses normally. The cap is what bounds how much a
hostile or wedged client can make the reader buffer.

Three smaller caps stop nonsense earlier: 32 keys per object
(`src/SimBleProtocol.h:36`), 64 characters per UUID
(`src/SimBleProtocol.h:33`), and 8 levels of nesting when skipping a value
that a client op should not have had (`src/SimBleProtocol.h:40`).

### A malformed line answers with `error` and is dropped

The parse is hand rolled: no JSON library is added for ten flat shapes. On any
failure the line produces one `error` event with a short reason and no sink
call (`src/SimBleLink.cpp:161-181`, `src/SimBleProtocol.cpp:584`). The link
stays up and the next line parses normally.

Twenty malformed inputs are in the gate and each one answers with `error`, not
a crash: not JSON at all, a truncated object, an unterminated string, no `op`
field, `op` not a string, an unknown op, a wrong field type, a value out of
range, `1e300`, odd-length hex, non-hex hex, a missing `uuid`, a raw NUL inside
a string, an escaped `\u0000`, twenty levels of nesting, trailing bytes after
the object, `{}`, a bare `[`, a bare `}`, and forty keys.

Two decisions worth naming:

- **A raw control byte inside a string is rejected**
  (`src/SimBleProtocol.cpp:185-189`). That is strict JSON, and it is what keeps
  a binary blob arriving on the socket from being parsed as half an op.
- **A NUL is rejected even when escaped** (`src/SimBleProtocol.cpp:128-132`),
  so every parsed string stays usable as a C string by the consumer.

A wrong type is an error, never a silent fall back to the default: a client
sending `"mtu": "517"` has a bug worth seeing.

### Defaults and accepted ranges

The op table above names the fields. These are the values the parser applies
when a field is absent or `null`, and the ranges it accepts
(`src/SimBleProtocol.cpp:615-667`, ranges at `src/SimBleProtocol.cpp:477-480`).

| field | default | accepted range |
|---|---|---|
| `mtu` | 23 | 23 to 517 |
| `interval` | 24 | 6 to 3200 |
| `latency` | 0 | 0 to 499 |
| `timeout` | 400 | 10 to 3200 |
| `reason` (disconnect) | 0x13 | 0 to 255 |
| `hex` (write) | empty | even run of hex digits, either case |
| `response` (write) | true | `true`/`false`, or 0/1 |
| `value` (subscribe) | 1 (notify) | 0 to 3 |
| `value` (rssi) | -60 | -128 to 127 |
| `enabled` (auto_confirm) | true | `true`/`false`, or 0/1 |
| `delay_ms` (auto_confirm) | 10 | 0 to 60000 |

Out of range is an `error`, not a clamp, because the real stack refuses these
too. `rssi` is stored as the low byte of the `int8_t`
(`src/SimBleProtocol.cpp:657`), which is what the header's `a=value (cast to
int8_t by the consumer)` mapping expects: -60 arrives as `a=196`.

Hex output is always lowercase; hex input accepts either case.

### The second client is refused, then closed

While a client is connected, the next one that connects is accepted, sent one
`error` line, and closed (`src/SimBleLink.cpp:224-234`). The first client is
untouched and keeps working. When the first client goes away the slot frees and
the next connect is served.

### The port

`crosspoint_simulator::ble::portFromEnv()` reads
`CROSSPOINT_SIM_BLE_PORT` and returns 0 when the variable is absent, empty,
zero or unparseable (`src/SimBleProtocol.cpp:674-685`). `start(0)` returns
false, so a bad value is the same as the feature being off, never a crash and
never a default port nobody asked for.

## Threading model `[contract]`

- `NimBLEDevice::init()` starts **two** threads: a socket reader and a **host
  thread**. `deinit()` joins both.
- The reader parses lines and pushes events onto the host thread's queue. It
  never calls firmware code.
- The host thread dispatches every firmware callback. It is the simulator's
  stand-in for the NimBLE host task.
- `indicate()` is called from the activity thread. It fills a single pending
  slot, emits `indicate` (plus `clobber` if it overwrote one), and returns. The
  confirm arrives later as an event and is dispatched on the host thread.
- The `portENTER_CRITICAL` shim is a real `std::mutex`
  (`src/freertos/FreeRTOS.h:27-29`), so existing critical sections keep working
  across these threads. `[verified]` by reading that file.

`SimBleLink.h` is the frozen seam between the transport and the GATT model, and
its header comment restates this split.

## Fidelity: four things that must be right, or the simulator lies

A shim that gets these wrong hides exactly the bugs a real device already
showed. All four are `[contract]` until a run demonstrates them.

1. **Callbacks run on the host thread**, never inline on the caller's thread.
   Inline dispatch makes a whole class of deadlock impossible to reproduce.
2. **Indication confirm is out of band, and withholdable.** `indicate()`
   returns true when the single pending slot accepted the payload, not when the
   peer got it. The confirm arrives later through `onStatus`. A shim that
   confirms synchronously never executes the firmware's timeout path.
3. **A second `indicate()` before a confirm clobbers the first.** Real and
   measured on hardware: back-to-back calls all returned true, the peer saw the
   first and the last. The shim must reproduce the clobber, not queue politely.
   The `clobber` event is how that stays visible.
4. **The client sets the MTU.** MTU drives the firmware's payload arithmetic
   and chunk counts, so a wrong default tests different arithmetic than a
   device runs. 23 is the pessimistic default; 517 is the fast path.

## Fault injection

The point of a shim over hardware. All `[contract]`:

- Withhold a confirm (`auto_confirm` false, then never send `confirm`).
- Drop the link mid-transfer (`disconnect` while a transfer is running).
- Send a malformed frame (trailing bytes, bad path, oversized length).
- Send a transfer `begin` without subscribing to the status characteristic.
