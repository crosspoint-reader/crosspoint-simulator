#include "SimulatorLifecycle.h"
#include "HalGPIO.h"
#include "SimPosixCompat.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#ifdef _WIN32
#include <SDL.h>
#include <string>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr const char *kWakeReasonEnv = "CROSSPOINT_SIM_WAKE_REASON";
constexpr const char *kInputScriptEnv = "CROSSPOINT_SIM_INPUT_SCRIPT";
constexpr const char *kInputScriptAfterWakeEnv =
    "CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE";
constexpr const char *kScreenshotsEnv = "CROSSPOINT_SIM_SCREENSHOTS";
constexpr const char *kScreenshotsAfterWakeEnv =
    "CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE";
char **gArgv = nullptr;
std::atomic<int> gExitCode{0};

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

void setExitCode(int code) { gExitCode.store(code); }

int exitCode() { return gExitCode.load(); }

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

[[noreturn]] void rebootAsPowerWake() {
  if (!gArgv || !gArgv[0]) {
    std::fputs("SimulatorLifecycle: missing argv for reboot\n", stderr);
    _exit(1);
  }

  setenv(kWakeReasonEnv, "power", 1);
  // Retain the physical switch level when the host relaunches after sleep.
  if (gpio.hasToggleSwitch())
    setenv("CROSSPOINT_SIM_SLIDE_SWITCH",
           gpio.isToggleSwitchOn() ? "on" : "off", 1);
  // A deep-sleep wake is a fresh process. Do not replay the pre-sleep script,
  // which would otherwise put every relaunched process back to sleep forever.
  // Tests can provide an explicit post-wake schedule when they need to capture
  // or terminate the relaunched instance.
  promoteAfterWakeValue(kInputScriptEnv, kInputScriptAfterWakeEnv);
  promoteAfterWakeValue(kScreenshotsEnv, kScreenshotsAfterWakeEnv);
#ifdef _WIN32
  // Windows has no exec that keeps the process: _execvp starts a new process
  // and ends this one, so whoever waits on the simulator (a QA script,
  // `timeout`) returns before the woken instance has run and loses its exit
  // status. Close this window, run the woken instance from this process's own
  // command line (its quoting and Unicode intact; the environment set above is
  // inherited), wait, and exit with its status. Each sleep adds one waiting
  // process; a kill-on-close job ends the woken ones if a waiter is killed.
  SDL_Quit();
  std::fflush(nullptr);
  // The job handle stays open on purpose: closing it is what kills the job.
  if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                            sizeof(limits));
    AssignProcessToJobObject(job, GetCurrentProcess());
  }
  // The program by its own path; the command line keeps the arguments.
  wchar_t program[MAX_PATH];
  const DWORD programLength = GetModuleFileNameW(nullptr, program, MAX_PATH);
  std::wstring commandLine = GetCommandLineW();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child{};
  if (programLength > 0 && programLength < MAX_PATH &&
      CreateProcessW(program, commandLine.data(), nullptr, nullptr, TRUE, 0,
                     nullptr, nullptr, &startup, &child)) {
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(child.hProcess, &status);
    _exit(static_cast<int>(status));
  }
  std::fprintf(stderr, "SimulatorLifecycle: CreateProcessW failed (%lu)\n",
               GetLastError());
#else
  execvp(gArgv[0], gArgv);
  std::perror("execvp");
#endif
  _exit(1);
}

} // namespace SimulatorLifecycle
