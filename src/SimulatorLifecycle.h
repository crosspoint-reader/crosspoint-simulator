#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
#define CROSSPOINT_SIM_REBOOT_IN_PROCESS 1
#include <csetjmp>
#else
#define CROSSPOINT_SIM_REBOOT_IN_PROCESS 0
#endif

namespace SimulatorLifecycle {

enum class WakeReason { None, PowerButton };

void initProcessArgs(char** argv);
WakeReason consumeWakeReason();
[[noreturn]] void rebootAsPowerWake();

#if CROSSPOINT_SIM_REBOOT_IN_PROCESS
// A deep-sleep wake is a chip reset on real hardware, and on desktop it is a
// process relaunch via execvp(). Neither is available on iOS: the sandbox denies
// process-exec, so the exec falls through to _exit() -- which iOS reports as a
// crash, and which in practice leaves the app frozen on the sleep screen.
//
// So the wake is performed in-process instead: rebootAsPowerWake() longjmps back
// to a setjmp() in main() placed ahead of setup(), and setup() runs again. That
// models the reset closely enough, provided the things setup() touches are
// idempotent -- see HalDisplay::begin() (reuses its window) and xTaskCreate()
// (dedupes by task name, because a real reset leaves exactly one render task).
//
// The jump skips destructors for every frame between main() and the sleep loop,
// exactly as execvp() would. Anything those frames owned is leaked for the life
// of the process. On hardware that memory is wiped by the reset; here it is not,
// so a very long sleep/wake cycle count will grow the heap.
std::jmp_buf& rebootJumpBuffer();

// Arm the jump. Until this is called, rebootAsPowerWake() has nowhere to go and
// falls back to the exec path, so an early wake cannot jump into a frame that
// does not exist yet.
void armRebootJump();
#endif

}  // namespace SimulatorLifecycle
