# ESPRebootManager

ESPRebootManager is a centralized reboot coordinator for ESP32 projects. Any module can request a reboot, registered guard callbacks vote whether reboot is safe, and one unified evaluation callback reports the final decision.

## Features
- Unified guard callback API: `onRebootRequest(...)`.
- Unified decision event API: `onEvaluation(...)`.
- Async request handling on a dedicated FreeRTOS task.
- Single active request policy with deterministic `Busy` responses.
- Polling support for missed events via `isRebootRequested()`, `rebootStatus()`, and `lastEvaluation()`.
- Bounded reason/detail buffers for deterministic memory usage.

## Installation
- PlatformIO: add `https://github.com/ESPToolKit/esp-reboot-manager.git` to `lib_deps`.
- Arduino IDE: install as ZIP from this repository.

## Single Include
```cpp
#include <ESPRebootManager.h>
#include <cstdio>
```

## Quick Start
```cpp
#include <ESPRebootManager.h>

ESPRebootManager rebootManager;

void setup() {
    ESPRebootManagerConfig config;
    config.taskName = "reboot-manager";
    config.taskStackSizeBytes = 6 * 1024;
    config.taskPriority = 1;
    config.taskCoreId = tskNO_AFFINITY;
    config.callbackTimeoutMs = 1000;
    rebootManager.init(config);

    rebootManager.onRebootRequest([](const RebootRequestContext& ctx) {
        RebootVote vote;
        if (ctx.reason[0] == '\0') {
            vote.allow = false;
            std::snprintf(vote.detail, sizeof(vote.detail), "empty reason is not allowed");
        }
        return vote;
    });

    rebootManager.onEvaluation([](const RebootEvaluation& evaluation) {
        if (evaluation.accepted) {
            Serial.printf("reboot accepted: id=%lu reason=%s\n",
                          static_cast<unsigned long>(evaluation.requestId),
                          evaluation.reason);
        } else {
            Serial.printf("reboot rejected: code=%u blocker=%s detail=%s\n",
                          static_cast<unsigned>(evaluation.code),
                          evaluation.blockerName,
                          evaluation.detail);
        }
    });

    (void)rebootManager.requestReboot("firmware-update", 1500);
}
```

## API Summary
- `bool init(const ESPRebootManagerConfig& config = {})`
- `void deinit()`
- `bool isInitialized() const`
- `RebootCallbackId onRebootRequest(GuardCallback cb)`
- `RebootCallbackId onEvaluation(EvaluationCallback cb)`
- `bool offRebootRequest(RebootCallbackId id)`
- `bool offEvaluation(RebootCallbackId id)`
- `RebootSubmitResult requestReboot(const char* reason, uint32_t delayMs = 0)`
- `bool isRebootRequested() const`
- `RebootRequestStatus rebootStatus() const`
- `RebootEvaluation lastEvaluation() const`

## Status Flow
`Idle -> Requested -> Evaluating -> Delaying -> Rebooting -> Idle`

If any guard rejects (or times out), the flow is:
`Idle -> Requested -> Evaluating -> Idle`

## License
MIT - see [LICENSE.md](LICENSE.md).
