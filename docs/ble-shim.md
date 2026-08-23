# The BLE peripheral shim

The simulator fakes a BLE peripheral. Firmware BLE code runs unchanged; only
the radio is replaced. A client speaks a line protocol over a TCP socket and
plays the part of the central (the phone).

**Shim** here means a header-compatible fake: it declares the same C++ API as
the real NimBLE library and implements it differently. No NimBLE source is
compiled.

Status: **built**. Both halves exist and run. Every claim below is marked
`[contract]` (what the shim must do) or `[verified]` (observed running, with
the command that showed it). The transport half is `[verified]`: see
"Transport specifics" below. The GATT half is `[verified]` too: see "GATT model
specifics" below.

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
- **A false return from `indicate()`, of any kind.** Real NimBLE's `sendValue`
  has no connection check, no CCCD check and no property check
  (`NimBLECharacteristic.cpp:272-328`), and `ble_gatts_indicate_custom` refuses
  only on out-of-memory or, under `BLE_GATT_CACHING`, an unaware peer
  (`ble_gattc.c:4888-4936`). Neither of those is modelled here, so **the shim's
  `indicate()` never returns false except on its own two guards** (stack down,
  null characteristic), and neither of those is reachable from the firmware. So
  the firmware's park-and-flush path for transfer status, and its
  retry-on-false loop, are both unreachable in the simulator. Whatever those
  paths do, they are untested here.
- **Throughput, of anything.** A `write` op returns when TCP took the bytes.
  There is no ATT write response, no connection interval and no radio in
  between, so a transfer rate measured here is a measurement of loopback.
  Numbers, so nobody quotes one: a 52 KB file pushed through this shim clocked
  **3377 KB/s** and, in a second run, **4196 KB/s**; the same laptop tool
  measured **2.6 KB/s** over a real radio at MTU 256. Three orders of
  magnitude. `[verified]` by running both.
- **MTU negotiation, and the ATT bearer's own size check.** The client declares
  the MTU with the `connect` or `mtu` op; nothing negotiates and nothing
  refuses an oversized write. A 485-byte frame was accepted on a link the
  firmware believed had a 15-byte payload budget, and the firmware processed it
  whole. That is deliberate on the client side -- BlueZ turns an over-MTU write
  into a long write, so a client cannot honestly reject one locally -- but the
  consequence is that firmware which never compares an arriving frame against
  its own payload arithmetic looks correct here and is bounded only by the real
  bearer. `[verified]` by running.
- **A link that dies without notice.** `disconnect` is a polite op the client
  sends. A supervision timeout has no equivalent here, so a run exercises the
  device-side disconnect cleanup and never the radio-loss path. `[verified]` --
  a transfer dropped mid-stream reached the firmware's `onDisconnect` hook and
  its cleanup ran; the loss path did not.
- **A hole in the delivered bytes from a withheld confirm.** The `indicate`
  event goes out when the payload enters the pending slot, and `clobber` only
  when a later payload takes that slot (captured at `src/SimBleGatt.cpp:382`, both
  lines emitted at `:358-368`). So a
  client is handed the clobbered payload and *then* told it was dropped. On
  hardware a clobbered indication is genuinely gone -- 18 back-to-back calls,
  the peer saw the first and the last. Withholding one confirm therefore cannot
  reproduce a missing byte range here; it reproduces the stall and the
  `clobber` event only. A gap can still be produced the other way, by making
  the firmware itself stop sending: a confirm withheld on the first chunk of a
  multi-chunk reply aborts the rest of that reply at the source, and those
  bytes are really lost. `[verified]` -- both cases run.
- **Storage failures behind a transfer.** The simulated card is a host
  directory. `HalStorage::mkdir` returns true on `EEXIST`
  (`src/HalStorage.cpp:326`) and `HalStorage::open` opens a directory *as a
  directory* and reports it open (`src/HalStorage.cpp:306-309`), so a firmware
  "could not create the directory" branch is unreachable here and a plain file
  standing where a directory component belongs surfaces as an open failure
  instead. A full card needs a size-limited loop filesystem; a shared host
  filesystem cannot produce one. `[verified]` by reading, and by a transfer that
  answered ready on a directory.

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

**`auto_confirm` is not connection state, and it is not reset when a client
leaves.** It is set at `src/SimBleGatt.cpp:716` and cleared only by
`init()` (`src/SimBleGatt.cpp:82`); `clearConnectionLocked`
(`src/SimBleGatt.cpp:786`) resets the MTU at `:789` and does not touch it.
A reconnecting client therefore inherits the previous client's setting. Real
hardware has no such notion, so this is a test-isolation trap rather than a
fidelity gap -- but a test that reconnects must set `auto_confirm` explicitly
instead of trusting the documented default. It cost one confused pass: a
control run with confirms nominally on hit 3000 ms timeouts, because an earlier
run had turned them off on a simulator process that was still alive.
`[verified]` -- reproduced, then traced to those lines.

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

**`props` is an integer, and that is part of the contract.** The `gatt` event
carries each characteristic's NimBLE property bitmask as a JSON number, not a
list and not a string: `{"uuid":"...0002","props":8}`
(`src/SimBleGatt.cpp:264`, `line += std::to_string(characteristic->m_properties)`).
Observed values and their meaning, so a client can decode without guessing:
`2` read, `8` write, `16` notify, `32` indicate, and `56` (`0x38`) the
write|notify|indicate combination the command characteristic carries
(`src/NimBLECharacteristic.h:32-37`). Those are NimBLE's own
`BLE_GATT_CHR_F_*` values, read off the pinned host header at
`nimble/host/include/host/ble_gatt.h:130-145` -- check them there rather than
trusting this table. **`1` is broadcast, not read**, and it briefly appeared
here as read: the shim had the wrong constant, no firmware characteristic sets
READ so nothing observable rode on it, and no test could have caught it. It
would have surfaced the first time a characteristic became readable and a
client decoded the bitmask back into names. This is written down because it was the
one field whose type nobody pinned: a laptop client built against the same
prose assumed a list, called `list()` on the number, and every tool using it
died inside `connect()` with `TypeError: 'int' object is not iterable` -- before
a single byte of traffic. The two halves had each picked something reasonable
and had never been connected to each other. `[verified]` by reading the socket
raw.

### Rules the shim enforces, because the real stack does

- `write` or `subscribe` with no central connected: `error`, no callback.
  `[verified]` for `write` -- "write with no connection fires no callback" and
  "write with no connection emits error" (`src/SimBleGatt.cpp:628`).
  `subscribe` takes the same branch (`src/SimBleGatt.cpp:648`) and is
  `[contract]`.
- `indicate()` with nobody subscribed, or with nothing connected at all:
  **returns true**, because real NimBLE does. Read `sendValue`
  (`NimBLECharacteristic.cpp:272-328`): `rc` starts at 0, the peer loop is the
  only thing that can move it, and there is no CCCD or property test anywhere.
  With no peers the loop body never runs and it returns true; with an
  unsubscribed peer it transmits anyway. The firmware says the same thing in
  its own words -- "indicate() succeeds into an empty room"
  (`BlePositionServer.cpp:79-81`). `[verified]` -- "indicate with nobody
  subscribed returns true", "indicate with nothing connected returns true"
  (`src/SimBleGatt.cpp:348`).
- A subscription belongs to a connection. On `disconnect` the shim clears
  subscription state itself, because NimBLE fires no unsubscribe callback.
  `[verified]` -- "disconnect fires no unsubscribe callback, same as NimBLE"
  and "the subscription is gone after a disconnect: no confirm on the new
  link", paired with "a fresh subscribe restores the confirm, so the clear was
  real" (`src/SimBleGatt.cpp:798`).
- Advertising stops on connect and is not restarted by the shim. `[verified]`
  -- "advertising goes down on connect" (`src/SimBleGatt.cpp:604`). Only the
  firmware's own `onDisconnect` brings it back.
- Two more the real stack makes, added while building it: a `write` to a
  characteristic without the WRITE property is an `error` (`[verified]`,
  "write to an indicate-only characteristic emits error"), and a client op
  arriving while the stack is down is an `error` with no callback.

### State replay on attach `[verified]`

**A client receives three events on connect without sending anything:** `stack`
`up`, the `gatt` table if one is built, and the current `advertising` state, in
that order.

**This is not something a phone would see.** Same status as `clobber`: it exists
because the simulator is not a radio. A real central learns the peripheral's
state by scanning; a TCP client has no scan, and every event it missed is gone.
And it missed all of them: `stack up` is emitted by `NimBLEDevice::init()`,
which is also the call that starts the listener, so at that instant no client
can possibly be attached and the line is dropped
(`src/SimBleLink.cpp:445-446`). The event was never racy. It was structurally
undeliverable. The `gatt` table and the first `advertising` line have the same
problem whenever the firmware builds them before a client turns up, which is
the normal case.

Mechanism, for a client author who sees it in a packet trace: the accept path
synthesizes an `attach` op into the sink (`src/SimBleLink.cpp:244-267`), the
mirror of the synthetic `disconnect` a lost socket already produces, and the
GATT model answers it by emitting current state
(`src/SimBleGatt.cpp:272-286`). A client never sends `attach` and gets an
`error` if it invents one that the model does not recognise. The replay fires
for **every** client, including the second one to attach after the first left.

What is **not** replayed: a live connection. A client that was not there for
the `connect` op is not the central that made it, so it is told nothing about
it. A client that attaches before the firmware ever calls
`NimBLEDevice::init()` receives nothing at all -- there is no host thread to
answer, so it must wait for `stack`/`up` to arrive the normal way.

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
(`tests/sim_ble_link_selftest.py:76-80`).

### Loopback only

The listener binds `INADDR_LOOPBACK` (`src/SimBleLink.cpp:356`), never
`INADDR_ANY`. This is a hazard decision, not a style one: the process on the
other end of this socket runs firmware command handling, so a LAN-reachable
port would hand the device model to anything on the network. The gate connects
to the host's own routable address and requires a refusal.

Backlog is 4 (`src/SimBleLink.cpp:359`). A second client has to complete its
connect before it can be told to go away.

### stop() wakes a blocked reader with a self-pipe

The reader thread never blocks in `accept()` or `recv()`. It sits in `poll()`
over three fds: the listener, the connected client, and the read end of a
self-pipe (`src/SimBleLink.cpp:270-320`). `accept()` and `recv()` run only on
a fd `poll()` already reported readable, and the `recv()` uses `MSG_DONTWAIT`
(`src/SimBleLink.cpp:306`).

`stop()` sets a stop flag, writes one byte into the self-pipe
(`src/SimBleLink.cpp:89-95`), shuts a connected client down, then joins
(`src/SimBleLink.cpp:388-398`). Measured: 0 to 2 ms with a client connected.

Why the self-pipe and not the alternatives:

- `shutdown()` on a **listening** socket is not portable. On Linux it does not
  reliably wake an `accept()`.
- Closing a fd another thread is polling is a use after free waiting to
  happen: the number can be handed to a new socket between the close and the
  wake.
- A poll timeout loop would work but would either burn wakeups or add latency
  to `stop()`. The pipe costs two fds and is exact.

`stop()` is safe when the link was never started and safe twice in a row
(`src/SimBleLink.cpp:380-386`). `start(0)` returns false and leaves the
feature off.

### The reader owns the client fd

Only the reader thread closes the client socket. `emit()` writes under the
mutex and, on a write failure, calls `shutdown()` rather than `close()`
(`src/SimBleLink.cpp:447-450`). That makes the reader's `poll()` return and
keeps the teardown in one place.

The client socket carries `SO_SNDTIMEO` of 5 s (`src/SimBleLink.cpp:47`,
`src/SimBleLink.cpp:236-240`). A client that stops reading cannot hang a
firmware thread inside `emit()` forever: the send fails, the link is dropped,
the simulator carries on.

### emit() is one line, whole, under one mutex

`emit()` frames and writes the whole line while holding the state mutex
(`src/SimBleLink.cpp:430-451`), so two threads cannot interleave halves of two
lines. Verified: two threads emitting 300 lines each, 600 lines received, every
one intact, each thread's lines in its own order, the two threads interleaved
at line granularity.

An embedded `\n` or `\r` in the caller's JSON would inject a second frame, so
`emit()` replaces both with a space (`src/SimBleLink.cpp:434-437`). Callers do
not have to be careful.

### A lost socket synthesizes a disconnect

A client that drops its socket is a link that went away. The reader delivers
`op="disconnect"`, `a=0x13` to the sink so the GATT model does not keep
believing a central is connected (`src/SimBleLink.cpp:137-158`). The teardown
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

## GATT model specifics `[verified]`

The API the firmware compiles against lives in `src/NimBLEDevice.h`,
`src/NimBLECharacteristic.h`, `src/NimBLEConnInfo.h`, `src/NimBLEAttValue.h`
and `src/host/ble_gap.h`. Every method in them is a forwarder
(`src/NimBLEDevice.cpp`). The model, the host thread, the single indication
slot and the outgoing JSON are all in `src/SimBleGatt.h` and
`src/SimBleGatt.cpp`. No NimBLE source is compiled and no JSON library is
linked: seven event shapes are hand rolled (`src/SimBleGatt.cpp:11-59`).

Everything in this section was demonstrated by

```
g++ -std=c++17 -Wall -Wextra -pthread -Isrc -o /tmp/sim_ble_gatt_selftest \
    src/NimBLEDevice.cpp src/SimBleGatt.cpp src/SimBleProtocol.cpp \
    tests/sim_ble_gatt_stub.cpp tests/sim_ble_gatt_selftest.cpp
/tmp/sim_ble_gatt_selftest                 # 42 checks, 0 failures
```

`tests/sim_ble_gatt_selftest.cpp` builds the same table the firmware builds, in
the same order, then drives it. `tests/sim_ble_gatt_stub.cpp` is a `SimBleLink`
that opens no socket: it hands decoded ops to the sink and captures the emitted
lines, so the GATT half is provable without the transport, and the assertions
are deterministic. `SimBleProtocol.cpp` is linked for `portFromEnv()` alone.
Rebuild with `-fsanitize=thread` and run under `setarch $(uname -m) -R` for the
race check.

A second binary drives the GATT model over the **real** transport, so the one
behaviour a stub cannot show -- the state replay on attach -- is proven on a
socket:

```
g++ -std=c++17 -Wall -Wextra -pthread -Isrc \
    -o /tmp/sim_ble_gatt_attach_selftest src/NimBLEDevice.cpp \
    src/SimBleGatt.cpp src/SimBleLink.cpp src/SimBleProtocol.cpp \
    tests/sim_ble_gatt_attach_selftest.cpp
/tmp/sim_ble_gatt_attach_selftest          # 9 checks, 0 failures
```

It builds the table with nobody connected, then connects to itself and reads
what arrives. It also proves the two halves link together.

**Both test binaries live in `tests/`, and that is enforcement, not tidiness.**
The library has no `srcFilter`, so anything under `src/` is compiled into the
library archive next to `simulator_main.o` -- and a test file that defines
`main()` can win the link. It did: the simulator built fine, never ran, printed
one line from a global constructor and aborted in a destructor of code it had
never entered. A header comment saying "do not link this" is not enforcement. A
directory that is not compiled cannot be forgotten. Both binaries also end with
`fflush(stdout); _exit()` rather than returning, so a verdict is never lost to
someone else's teardown.

### The firmware translation unit compiles against it

The whole firmware builds against this shim with no NimBLE symbol missing. That
is what proves the surface complete rather than plausible.

The same thing on one translation unit, for a quick check while editing the
headers:

```
g++ -std=c++17 -fsyntax-only -DFREEINK_CAP_BLE_PERIPHERAL=1 -Isrc \
    -I<firmware>/lib/BlePositionServer/include -I<firmware>/lib/Logging \
    -include Arduino.h -include freertos/FreeRTOS.h \
    -include freertos/task.h -include freertos/semphr.h \
    <firmware>/lib/BlePositionServer/src/BlePositionServer.cpp
```

The `-include` flags stand in for what the firmware's own build has already
pulled in by the time it reaches this file. Anything they miss surfaces as a
FreeRTOS name, not a NimBLE one, so it does not touch what this run is for.

Two NimBLE calls the seed contract did not list turned up building it, and both
are real:

- **`NimBLEServer::start()`** (`BlePositionServer.cpp:322`). The firmware calls
  it, not the deprecated `NimBLEService::start()`, and builds the GATT table
  with it. It is what emits the `gatt` event (`src/SimBleGatt.cpp:249-270`).
  `NimBLEService::start()` exists anyway and returns true.
- **`NimBLEServer::setCallbacks()` takes a second argument**
  (`BlePositionServer.cpp:283`, `deleteCallbacks=false`). The shim ignores it
  and never owns the pointer; the firmware registers a static object, so
  nothing leaks either way (`src/NimBLEDevice.cpp:37`).

Nothing else was missing.

**Neither was findable by grep, and that is the lesson.** The API list this
shim was built against was produced by grepping the firmware file for
`NimBLE[A-Za-z]*::[a-zA-Z_]*` and `->[a-zA-Z_]*(`. That output is not silent
about these two calls -- it contains `->start(` and `->setCallbacks(` -- it is
*ambiguous* about them, in exactly the two ways that matter. `->start(` does
not say which class the pointer had, and the only class-qualified `start` the
grep found, `NimBLEService::start`, came from a **comment** saying that call is
deprecated and not used. `->setCallbacks(` does not say how many arguments were
passed. A grep over call syntax cannot answer a question about types or arity,
so it cannot detect a stale API contract; only a compiler can. Freeze the list
if you like, but verify it by building, not by re-running the grep that wrote
it.

**The whole surface has since been driven by real firmware, not only compiled
against.** Four independent sessions in one day pushed a position feed, a
line-oriented command channel with multi-line replies at MTU 23 and MTU 517, a
52 KB file transfer with twenty-odd refusal cases, and a device-initiated
fetch loop through this shim, and every finding they produced was in the
firmware rather than here. That is the strongest statement available about the
surface being complete: a plausible-looking fake fails on the second
unusual thing a real caller does.

### Decisions worth naming

- **The indication slot is per connection, not per characteristic**
  (`src/SimBleGatt.cpp:382`). That is what the firmware assumes: its transfer
  status channel parks a line precisely because the command channel can be
  holding the one slot (`BlePositionServer.cpp:958-966`).
- **A clobbered burst yields one confirm, not one per call.** The shim's
  auto-confirm carries the sequence number of the payload it was created for,
  and a confirm for a payload that has since been clobbered is dropped
  silently (`src/SimBleGatt.cpp:667`). Two `indicate()` calls with one confirm
  between them is the hardware behaviour: 18 calls, two payloads seen.
- **A client `confirm` op confirms whatever is pending**, regardless of which
  payload the client thought it was confirming. It errors when nothing is
  pending, or when its `uuid` names a different characteristic than the
  pending one.
- **`advertising` is also emitted with `up:false` when a central connects**
  (`src/SimBleGatt.cpp:604`). The event table above lists start/stop; this is
  the third case, and without it a client's view of advertising would be wrong
  for the whole connection.
- **`start()` while already advertising is a no-op success and emits nothing**
  (`src/SimBleGatt.cpp:434`). Real NimBLE behaves that way
  (`NimBLEAdvertising.cpp:194-197`), which is why the firmware's slow-interval
  switch calls `stop()` first. A shim that emitted a fresh event here would
  hide that.
- **0/0 interval bounds are reported as the fast pair.** The firmware sets
  0/0 to mean "let the host pick" (`BlePositionServer.cpp:225-226`), and the
  host picks `BLE_GAP_ADV_FAST_INTERVAL1`. The `advertising` event reports what
  the radio would use, not the sentinel (`src/SimBleGatt.cpp:815`).
- **`deinit(clearAll)` deletes the table when `clearAll` is true**, same as the
  real API, which is why the firmware nulls its own characteristic pointers
  first (`BlePositionServer.cpp:388-393`). With `false` the objects survive.
- **`deinit()` called from the host thread is refused with an `error`**
  (`src/SimBleGatt.cpp:112`). It would be a self-join. Real NimBLE deinit from
  the host task is equally broken; the firmware never does it, and a clear line
  beats an abort.
- **The advertising name has no default in the shim.** The firmware passes it
  in, and the shim never invents one, so no device name is baked in here.

### `waitIdle()` is the one addition that is not NimBLE

`SimBleGatt::waitIdle()` blocks until the host thread's queue is empty and it
is not mid-dispatch, including events not yet due
(`src/SimBleGatt.cpp:560`). It exists so a test can assert that a callback did
**not** fire without racing the host thread. The firmware never calls it, and a
client cannot reach it.

## Threading model

- `NimBLEDevice::init()` starts **two** threads: a socket reader and a **host
  thread**. `deinit()` joins both. `[verified]` -- the host thread is started
  in `init()` (`src/SimBleGatt.cpp:86`) and joined in `deinit()`
  (`src/SimBleGatt.cpp:129`); the reader is `SimBleLink`'s.
- The reader parses lines and pushes events onto the host thread's queue. It
  never calls firmware code. `[verified]` by reading
  `SimBleGatt::onReaderEvent` (`src/SimBleGatt.cpp:460`): it maps an op name to
  a queue entry and returns. Even a state-only op (`rssi`, `auto_confirm`) is
  queued rather than applied there, so ordering is whatever the client sent.
- The host thread dispatches every firmware callback. It is the simulator's
  stand-in for the NimBLE host task. `[verified]` -- "callback ran on a
  different thread than the caller" compares `std::this_thread::get_id()`
  inside the callback against the thread that fed the event.
- `indicate()` is called from the activity thread. It fills a single pending
  slot, emits `indicate` (plus `clobber` if it overwrote one), and returns. The
  confirm arrives later as an event and is dispatched on the host thread.
  `[verified]` -- "indicate returned before any confirm arrived" and "the
  delayed confirm arrived later".
- The `portENTER_CRITICAL` shim is a real `std::mutex`
  (`src/freertos/FreeRTOS.h:27-29`), so existing critical sections keep working
  across these threads. `[verified]` by reading that file.
- One mutex guards the whole GATT model, including the fields inside the
  `NimBLE*` wrapper objects, and it is **dropped before every firmware
  callback** (`src/SimBleGatt.cpp:548`). It has to be: the firmware's
  `onDisconnect` calls `advertising->start()`, which comes straight back in.
  `[verified]` -- the self-test runs clean under ThreadSanitizer
  (`-fsanitize=thread`, under `setarch -R` because TSan needs ASLR off on this
  kernel).

`SimBleLink.h` is the frozen seam between the transport and the GATT model, and
its header comment restates this split.

## Fidelity: four things that must be right, or the simulator lies

A shim that gets these wrong hides exactly the bugs a real device already
showed. All four are `[verified]`, each by the named self-test check.

1. **Callbacks run on the host thread**, never inline on the caller's thread.
   Inline dispatch makes a whole class of deadlock impossible to reproduce.
   `[verified]` -- "callback ran on a different thread than the caller".
2. **Indication confirm is out of band, and withholdable.** `indicate()`
   returns true when the single pending slot accepted the payload, not when the
   peer got it. The confirm arrives later through `onStatus`. A shim that
   confirms synchronously never executes the firmware's timeout path.
   `[verified]` twice: "indicate returned before any confirm arrived" (with
   `auto_confirm` on and a 150 ms delay, `onStatus` had not fired when
   `indicate()` returned) and "a withheld confirm never fires onStatus" (with
   `auto_confirm` off, it never fires at all). Even the shim's own auto-confirm
   goes through the host thread's queue with a delay
   (`src/SimBleGatt.cpp:421`), so it cannot short-circuit.
3. **A second `indicate()` before a confirm clobbers the first.** Real and
   measured on hardware: back-to-back calls all returned true, the peer saw the
   first and the last. The shim must reproduce the clobber, not queue politely.
   The `clobber` event is how that stays visible. `[verified]` -- "a second
   indicate before a confirm still returns true", "the second indicate emitted
   clobber" and "clobber names the dropped payload (line-A)".
4. **The client sets the MTU.** MTU drives the firmware's payload arithmetic
   and chunk counts, so a wrong default tests different arithmetic than a
   device runs. 23 is the pessimistic default; 517 is the fast path.
   `[verified]` -- "MTU defaults to 23 when the client says nothing".

### How the first of those got broken: the shim copied a comment, not a library

Worth reading before adding to this shim, because nothing in the process caught
it.

The shim's `indicate()` used to return **false** when nobody was subscribed,
when nothing was connected, and when the characteristic lacked NOTIFY/INDICATE,
under a comment reading "refusals the real stack makes". Real NimBLE makes none
of those three checks. The value came from a firmware comment that is itself
wrong -- `BlePositionServer.cpp:294-296` claims "a busy/still-pending
`indicate()` actually returns false for the retry loop to catch" -- rather than
from `sendValue`, which was on disk the whole time.

The cost, measured against the real firmware with a connected-but-unsubscribed
peer:

```
false: [ERR] reply indicate failed after 40 attempts: INFO pos=0     (~1008 ms)
true:  [ERR] reply unconfirmed after 3000 ms, 11 bytes dropped       (3000 ms)
```

Different duration, different branch, different log line, and different
persistent state: only the second path sets `lastConfirmTimedOut_`
(`BlePositionServer.cpp:639`), which is the flag that suppresses `FETCH_CANCEL`
downstream. A developer reproducing the unsubscribed case in the simulator was
exercising code the device never reaches.

**No test could have caught it.** Two self-test checks asserted the inversion as
correct, so the suite agreed with the bug. A test written from the same belief
as the code confirms the belief, not the behaviour. The only thing that would
have caught it is reading the library being faked -- which is the rule this repo
already has for the pinned SDK, applied to a vendored library instead.

A fifth one is not a "must be right" but a "must be remembered", because it
falsifies results in both directions:

5. **The shim is orders of magnitude faster than a radio, and that changes
   which races are reachable.** Two consequences seen in one session. A race
   the firmware's own comment prices as theoretical -- two file arrivals
   collapsing into one because the activity loop polls a single-slot handoff --
   fired on the first attempt, because a whole file transfer finished in
   milliseconds where a radio needs seconds. That is evidence the race exists
   and that its price is higher than the comment says; it is **not** evidence
   it can happen on hardware. In the other direction, the shim's own 10 ms
   auto-confirm delay is longer than a whole transfer, so two indications that
   a radio would separate by seconds land inside one confirm window and produce
   a `clobber` a device would never see. A `clobber` in a fast run is a
   simulator artefact until a slow run reproduces it. `[verified]` -- both
   observed, the second chased with verbose logging and dismissed.

## Fault injection

The point of a shim over hardware. The last four needed firmware attached and
were `[contract]` until a firmware with a file-transfer receiver was driven
through them.

- Withhold a confirm (`auto_confirm` false, then never send `confirm`).
  `[verified]` -- "a withheld confirm never fires onStatus".
- **Withhold a confirm and leave it withheld, to time a firmware's own retry
  budget.** `[verified]` -- a firmware waiting 3000 ms per indication took
  3000.x ms per line, 23 times over 69 s, on two independent clocks (the
  client's monotonic clock and the firmware's own millisecond log). This is the
  path a synchronous confirm can never execute.
- **Withhold the confirm for one chunk in the middle of a multi-chunk reply**,
  rather than for a whole line. `[verified]`, and it is the sharper tool: the
  firmware abandoned the rest of that reply, so the peer held an unterminated
  prefix and the next reply block was appended straight onto it. One corrupt
  line, no error, and the reply's own terminator still arrived.
- Send a malformed frame (trailing bytes, bad path, oversized length).
  `[verified]` -- twenty malformed inputs in the transport gate, each answered
  with `error` and no crash.
- Drop the link mid-transfer (`disconnect` while a transfer is running).
  `[verified]` -- the firmware's disconnect hook ran, its partial file was
  deleted and its transfer state was cleared, shown by a later transfer being
  accepted rather than refused as busy. Note what this is *not*: a polite
  `disconnect` op, never a radio dropout.
- Send a transfer `begin` without subscribing to the status characteristic.
  `[verified]` -- refused, and the refusal was parked and delivered the moment
  the client subscribed, so a client can receive a verdict for a frame it has
  already given up on.
- **Write a field value the wire format forbids.** `[verified]` -- a client can
  put any byte string on a characteristic, including one whose length or field
  domain the format does not allow, which is how a coordinate outside the
  planet reached a renderer. Nothing between the client and the firmware
  validates anything.
- **Corrupt the card underneath a transfer while it streams.** The simulated SD
  is a host directory, so a `.part` file can be damaged from outside while
  every byte on the wire stays correct. `[verified]` -- it separates "the
  firmware checksummed the bytes it received" from "the firmware checksummed
  the file it wrote", and here it was the latter.
