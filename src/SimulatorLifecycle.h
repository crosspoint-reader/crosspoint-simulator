#pragma once

namespace SimulatorLifecycle {

enum class WakeReason { None, PowerButton };

void initProcessArgs(char** argv);
WakeReason consumeWakeReason();
[[noreturn]] void rebootAsPowerWake();
// Process exit status; a failed input script sets it to non-zero.
void setExitCode(int code);
int exitCode();

}  // namespace SimulatorLifecycle
