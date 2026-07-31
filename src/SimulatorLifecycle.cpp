#include "SimulatorLifecycle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

constexpr const char *kWakeReasonEnv = "CROSSPOINT_SIM_WAKE_REASON";
constexpr const char *kInputScriptEnv = "CROSSPOINT_SIM_INPUT_SCRIPT";
constexpr const char *kInputScriptAfterWakeEnv =
    "CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE";
constexpr const char *kScreenshotsEnv = "CROSSPOINT_SIM_SCREENSHOTS";
constexpr const char *kScreenshotsAfterWakeEnv =
    "CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE";
char **gArgv = nullptr;

#if CROSSPOINT_SIM_REBOOT_IN_PROCESS
std::jmp_buf gRebootJump;
bool gRebootJumpArmed = false;
#endif

void promoteAfterWakeValue(const char *target, const char *afterWake) {
  const char *value = std::getenv(afterWake);
  if (value) {
    setenv(target, value, 1);
  } else {
    unsetenv(target);
  }
  unsetenv(afterWake);
}

} // namespace

namespace SimulatorLifecycle {

void initProcessArgs(char **argv) { gArgv = argv; }

WakeReason consumeWakeReason() {
  const char *value = std::getenv(kWakeReasonEnv);
  if (!value) {
    return WakeReason::None;
  }

  unsetenv(kWakeReasonEnv);
  if (std::strcmp(value, "power") == 0) {
    return WakeReason::PowerButton;
  }
  return WakeReason::None;
}

#if CROSSPOINT_SIM_REBOOT_IN_PROCESS
std::jmp_buf &rebootJumpBuffer() { return gRebootJump; }
void armRebootJump() { gRebootJumpArmed = true; }
#endif

[[noreturn]] void rebootAsPowerWake() {
  setenv(kWakeReasonEnv, "power", 1);
  // A deep-sleep wake is a fresh process. Do not replay the pre-sleep script,
  // which would otherwise put every relaunched process back to sleep forever.
  // Tests can provide an explicit post-wake schedule when they need to capture
  // or terminate the relaunched instance.
  promoteAfterWakeValue(kInputScriptEnv, kInputScriptAfterWakeEnv);
  promoteAfterWakeValue(kScreenshotsEnv, kScreenshotsAfterWakeEnv);

#if CROSSPOINT_SIM_REBOOT_IN_PROCESS
  // Preferred on iOS: re-enter setup() rather than exec a new process, which
  // the sandbox forbids. See the header for what this costs.
  if (gRebootJumpArmed) {
    std::longjmp(gRebootJump, 1);
  }
  std::fputs("SimulatorLifecycle: reboot jump not armed\n", stderr);
#endif

  if (!gArgv || !gArgv[0]) {
    std::fputs("SimulatorLifecycle: missing argv for reboot\n", stderr);
    _exit(1);
  }
  execvp(gArgv[0], gArgv);

  std::perror("execvp");
  _exit(1);
}

} // namespace SimulatorLifecycle
