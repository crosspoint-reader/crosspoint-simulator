# The FreeRTOS shim

The simulator replaces the FreeRTOS API with host shims: `src/freertos/FreeRTOS.h`,
`src/freertos/task.h`, `src/freertos/semphr.h`. A task is a `std::thread`
(`src/freertos/task.h:26`), a mutex is a `std::recursive_mutex`
(`src/freertos/semphr.h:21`), a task notification is a condvar
(`src/freertos/FreeRTOS.h:34`).

Two primitives were missing rather than simplified. This doc says what they
were, what replaced them, and what the change cost the existing timing.

## Gap 1: no binary semaphore, and no timeout

Read off the code, before this change:

- `xSemaphoreCreateBinary()` did not exist. Only `xSemaphoreCreateMutex()` did.
- `xSemaphoreTake` ignored `ticksToWait` entirely (the parameter was unnamed and
  unused). It always blocked until it got the mutex, then returned `true`.

So firmware that creates a confirm semaphore and waits on it with a deadline
could not time out. The concrete case in the firmware this simulator builds:
`lib/BlePositionServer/src/BlePositionServer.cpp:261` creates it with
`xSemaphoreCreateBinary()`, `:621` polls it with `ticksToWait == 0` to drain a
stale give, `:629` waits 3000 ms and treats anything but `pdTRUE` as a dropped
reply, and `:75` gives it from the BLE callback thread. Under the old shim
`:261` did not compile, `:621` would have taken the mutex, and `:629` could
never have returned `pdFALSE`.

## Gap 2: `vTaskDelay` was a no-op

`inline void vTaskDelay(int) {}` -- an empty body, so every delay took zero
time. A firmware retry loop of 40 iterations x 25 ms ran instantly, and every
`yield every N rows` helper yielded nothing.

## What was added

**One handle type, two objects, tag dispatch.** `SemaphoreHandle_t` is now
`SimSemaphoreBase *` (`src/freertos/semphr.h:40`). `SimSemaphoreBase` carries a
`SimSemaphoreKind` tag; `SimMutex` and `SimBinarySemaphore` derive from it. The
mutex was not retyped and its internals were not touched: `SimMutex` still holds
the same `std::recursive_mutex`, `holder` and `holdCount`
(`src/freertos/semphr.h:21-30`), because `xSemaphoreGetMutexHolder` and
`xQueuePeek` read them.

`xSemaphoreTake`, `xSemaphoreGive` and `xQueuePeek` are shared entry points, so
each one branches on the tag and leaves the mutex arm exactly as it was.

**The binary semaphore** (`src/freertos/semphr.h:33-38`) is a `std::mutex` plus
a `std::condition_variable` plus one `bool available`, created empty:

- `xSemaphoreTake(sem, portMAX_DELAY)` waits forever.
- `xSemaphoreTake(sem, n)` waits at most `n` ticks and returns `false`
  (`pdFALSE`) if the token never arrived.
- `xSemaphoreTake(sem, 0)` tests once and returns. `wait_for` with a zero
  duration evaluates the predicate and gives up, which is the non-blocking poll
  the drain-a-stale-give call needs.
- A successful take clears the token, so the next take blocks again.
- `xSemaphoreGive` takes the semaphore's own lock, sets the token, drops the
  lock, then notifies. It requires nothing of the calling thread, so a BLE
  callback thread can give a semaphore the activity thread is waiting on.
- `xSemaphoreGive` on an already-full semaphore returns `false`, which is what
  FreeRTOS does (`errQUEUE_FULL`).
- `xQueuePeek` on a binary semaphore reports whether a token is waiting.
  `xSemaphoreGetMutexHolder` on one returns `nullptr`.

`xSemaphoreCreateCounting` was **not** added. Nothing in the build asks for it.

**Return type kept as `bool`.** Real FreeRTOS returns `BaseType_t`. The shim
already returned `bool` and every call site either ignores the result or
compares it against `pdTRUE` / `pdFALSE`, where `false == pdFALSE == 0` and
`true == pdTRUE == 1`. Leaving the signature alone keeps the mutex arm's diff to
zero.

**No `vSemaphoreDelete`.** There was none before and there is none now, so
nothing ever deletes through the base pointer and the base needs no virtual
destructor (`src/freertos/semphr.h:44-46`).

**`vTaskDelay` now sleeps** (`src/freertos/task.h:92`). A positive tick count
becomes a `std::this_thread::sleep_for`. Zero or negative yields instead of
sleeping, which is what FreeRTOS does with a zero delay.

## Tick rate

One tick is one millisecond. Basis: `portTICK_PERIOD_MS` is defined as `1` and
is the only tick-rate definition in the shim (`src/freertos/FreeRTOS.h:10`).
There is no `configTICK_RATE_HZ` and no `pdMS_TO_TICKS` here. Both new
conversions multiply by `portTICK_PERIOD_MS` rather than hardcoding 1
(`src/freertos/semphr.h:58-59`, `src/freertos/task.h:98-99`), so redefining
that macro moves both.

This matches the firmware's own target config, which sets
`CONFIG_FREERTOS_HZ=1000`, so a tick is 1 ms on device too.

Measured on this host: `vTaskDelay(1)` costs 1.057 ms per call over 1000 calls,
and `vTaskDelay(0)` costs 0.00066 ms. So the sleep overshoots its nominal
duration by about 6 percent. Verified by running.

## Regression: what making `vTaskDelay` real cost

A no-op `vTaskDelay` is load-bearing until proven otherwise, so the same gate
ran before and after.

Gate: build the firmware's `simulator` env, then run a scripted session that
boots, enters the map on a persisted fix, and screenshots it.

```
pio run -e simulator
CROSSPOINT_SIM_INPUT_SCRIPT='1200:ENTER;12000:QUIT' \
CROSSPOINT_SIM_SCREENSHOTS='6000:./qa-artifacts/map.bmp' \
  <build-dir>/program
```

| | before | after |
|---|---|---|
| build | SUCCESS | SUCCESS |
| unique compiler warnings | 4 | 4, same 4 |
| map render | 4 tiles, 2874 ways, 18 places, 22 ms | 4 tiles, 2874 ways, 18 places, 20 ms |
| screenshot | 480x800, 2 grey levels, 11.88 percent dark | byte-identical to before |
| process wall clock | 12.12 s | 12.12 s |

Verified by running, 2026-08-23. The screenshot draws tiles, ways, place labels,
scale bar and compass in both runs, and the two BMPs compare byte for byte
equal. No gate behind an env var was needed; the real sleep is unconditional.

**What the gate does not cover.** No `vTaskDelay` call site is on the boot ->
home -> map path, so the gate proves the change is harmless there, not
everywhere. The remaining call sites in the firmware, and their cost at
1.057 ms per tick, read off the code:

- `lib/Xtc/Xtc.cpp:20` -- one tick every 8 thumbnail rows. A 480-row thumbnail
  pays about 63 ms.
- `lib/PngToBmpConverter/PngToBmpConverter.cpp:80` -- one tick every 8 decoded
  rows, same shape.
- `src/activities/reader/TxtReaderActivity.cpp:162` -- one tick per 20 indexed
  pages. A 2000-page book pays about 106 ms.
- `lib/JpegToBmpConverter/JpegToBmpConverter.cpp:176` -- one tick every 4 file
  IO operations. The IO count is not visible from the call site, so this is the
  one site whose cost is **open**; a JPEG-heavy screen is what would show it.
- `src/activities/map/MapTransferReceiver.cpp:159` -- a poll loop with its own
  millisecond deadline. It used to spin a core flat out; now it sleeps. Strictly
  better.
- `src/activities/reader/KOReaderSyncActivity.cpp:56` -- 100 ticks, so about
  106 ms where it used to be 0.

## Standalone checks

The gate cannot reach the binary semaphore: the firmware gates that code behind
a BLE capability flag the simulator build does not set, so the file compiles to
stubs. It was verified with a host program compiled straight against these
headers instead. Verified by running, 2026-08-23:

- an empty binary semaphore, `ticksToWait == 0`, returns `pdFALSE` in under
  20 ms
- `ticksToWait == 300` returns `pdFALSE` after 300 ms
- a give from a second thread wakes a blocked 3000-tick take after 150 ms
- the token is consumed: the next zero-tick take fails again
- a second give on a full semaphore returns `pdFALSE`
- `vTaskDelay(200)` sleeps 200 ms, `vTaskDelay(0)` returns immediately

Mutex parity was checked the same way: one program exercising only the mutex API
(create, peek free, take, holder, recursive re-take, peek from another thread
while held, give, give again, holder cleared, null handle for all four entry
points) compiled against the old headers and the new ones and printed identical
output.

Note one pre-existing quirk that parity run confirms is unchanged: `xQueuePeek`
on a mutex uses `try_lock`, and a `std::recursive_mutex` grants `try_lock` to
its own holder. So peek from the holding thread reports the mutex as free. Only
another thread sees it as taken.
