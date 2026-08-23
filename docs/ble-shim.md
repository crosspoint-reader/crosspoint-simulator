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
the command that showed it). Nothing is `[verified]` yet.

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
