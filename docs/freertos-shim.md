# The FreeRTOS shim

The simulator replaces the FreeRTOS API with host shims: `src/freertos/FreeRTOS.h`,
`src/freertos/task.h`, `src/freertos/semphr.h`. A task is a `std::thread`
(`src/freertos/task.h:26`), a mutex is a `std::recursive_mutex`
(`src/freertos/semphr.h:21`), a task notification is a condvar
(`src/freertos/FreeRTOS.h:47`).

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

## Gap 3: no `pdMS_TO_TICKS`

Also absent, and not merely unused: the firmware passes every one of its
millisecond timeouts through it. Four call sites, all in
`lib/BlePositionServer/src/BlePositionServer.cpp` -- `:249`
`vTaskDelay(pdMS_TO_TICKS(20))`, `:404` `vTaskDelay(pdMS_TO_TICKS(50))`, `:629`
`xSemaphoreTake(sem, pdMS_TO_TICKS(kConfirmTimeoutMs))` with
`kConfirmTimeoutMs = 3000` (`:612`), and `:653`
`vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs))` with `kRetryDelayMs = 25` (`:608`).
Without the macro that file does not compile against the shim at all, whatever
the semaphore does.

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

**`pdMS_TO_TICKS`** (`src/freertos/FreeRTOS.h:13-23`) lives in `FreeRTOS.h`
because that is where real FreeRTOS reaches it from -- it is defined in
`projdefs.h`, which `FreeRTOS.h` includes. The shim's expression is

```
#define pdMS_TO_TICKS(xTimeInMs) \
  ((uint32_t)((uint32_t)(xTimeInMs) / (uint32_t)portTICK_PERIOD_MS))
```

which is FreeRTOS's own expression rewritten through the one macro this shim
has, not an invented one. In the pinned ESP-IDF (5.5.2.260206, on disk):

- `components/freertos/FreeRTOS-Kernel/include/freertos/projdefs.h:46` defines
  it as `((TickType_t)(xTimeInMs) * configTICK_RATE_HZ) / 1000U`, integer
  division, so it rounds **down** and a sub-tick delay yields 0 ticks.
- `components/freertos/FreeRTOS-Kernel/portable/riscv/include/freertos/portmacro.h:126`
  defines `portTICK_PERIOD_MS` as `1000 / configTICK_RATE_HZ`.

Substituting the second into the first gives `xTimeInMs / portTICK_PERIOD_MS`.
Same rounding, and strictly safer: FreeRTOS multiplies before dividing and can
overflow a 32-bit tick type, a divide cannot. At `portTICK_PERIOD_MS == 1` it is
the identity, so `pdMS_TO_TICKS(n) == n`.

The reduction is exact whenever `configTICK_RATE_HZ` divides 1000 evenly, which
every rate that yields a whole-millisecond `portTICK_PERIOD_MS` does. Checked by
running both expressions against each other over 10 tick rates x 14 millisecond
values: 140 pairs, 0 mismatches. A rate that does not divide 1000 evenly does
differ (at 333 Hz, `pdMS_TO_TICKS(3)` is 0 in FreeRTOS and 1 here), but such a
rate cannot be expressed by `portTICK_PERIOD_MS` in the first place, so the shim
cannot reach that case.

**Nothing else was added.** Sweeping every FreeRTOS symbol the firmware
references against the shim turned up four more absentees, and none of them is a
gap the build has:

- `xSemaphoreCreateRecursiveMutex`, `xSemaphoreTakeRecursive`,
  `xSemaphoreGiveRecursive` -- used only in the firmware's `lib/hal/HalStorage.cpp`,
  and `hal` is in the simulator env's `lib_ignore`. The simulator ships its own
  `src/HalStorage.cpp`, which uses none of them.
- `vPortEnterCritical`, `xTaskPriorityDisinherit` -- appear in firmware comments
  only, never called.

## Tick rate

One tick is one millisecond. Basis: `portTICK_PERIOD_MS` is defined as `1` and
is the only tick-rate definition in the shim (`src/freertos/FreeRTOS.h:11`).
There is no `configTICK_RATE_HZ` here. All three conversions go through
`portTICK_PERIOD_MS` rather than hardcoding 1 (`src/freertos/FreeRTOS.h:22-23`,
`src/freertos/semphr.h:58-59`, `src/freertos/task.h:98-99`), so redefining that
macro moves all three.

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

`pdMS_TO_TICKS` was checked the same way, including the firmware's four call
sites verbatim. All 13 assertions passed, and the two `static_assert`s compiled,
which is what proves the macro is usable in a constant expression like the real
one:

```
pdMS_TO_TICKS(0)                  = 0            want 0            ok
pdMS_TO_TICKS(1)                  = 1            want 1            ok
pdMS_TO_TICKS(25)                 = 25           want 25           ok
pdMS_TO_TICKS(3000)               = 3000         want 3000         ok
pdMS_TO_TICKS(4294967295u)        = 4294967295   want 4294967295   ok   (no overflow)
pdMS_TO_TICKS(10 + 15)            = 25           want 25           ok   (argument parenthesised)
pdMS_TO_TICKS(3000) / 2           = 1500         want 1500         ok   (result parenthesised)
pdMS_TO_TICKS(20)                 = 20           want 20           ok   (BlePositionServer.cpp:249)
pdMS_TO_TICKS(50)                 = 50           want 50           ok   (BlePositionServer.cpp:404)
pdMS_TO_TICKS(kConfirmTimeoutMs)  = 3000         want 3000         ok   (BlePositionServer.cpp:629)
pdMS_TO_TICKS(kRetryDelayMs)      = 25           want 25           ok   (BlePositionServer.cpp:653)
xSemaphoreTake(sem, pdMS_TO_TICKS(3000)) != pdTRUE -> 1 after 3000 ms   ok
vTaskDelay(pdMS_TO_TICKS(25)) slept 25 ms                              ok
```

The last two run the call sites' whole statements, not just the macro, so the
3000 ms confirm timeout that could never expire before now expires in 3000 ms.

Mutex parity was checked the same way: one program exercising only the mutex API
(create, peek free, take, holder, recursive re-take, peek from another thread
while held, give, give again, holder cleared, null handle for all four entry
points) compiled against the old headers and the new ones and printed identical
output.

Note one pre-existing quirk that parity run confirms is unchanged: `xQueuePeek`
on a mutex uses `try_lock`, and a `std::recursive_mutex` grants `try_lock` to
its own holder. So peek from the holding thread reports the mutex as free. Only
another thread sees it as taken.
