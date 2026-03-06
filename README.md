# ESPRebootManager

ESPRebootManager is a centralized reboot coordinator for ESP32 projects. Any module can request a reboot, registered guard callbacks vote whether reboot is safe, and registered evaluation callbacks report the final decision.

## CI / Release / License
[![CI](https://github.com/ESPToolKit/esp-reboot-manager/actions/workflows/ci.yml/badge.svg)](https://github.com/ESPToolKit/esp-reboot-manager/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ESPToolKit/esp-reboot-manager?sort=semver)](https://github.com/ESPToolKit/esp-reboot-manager/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Features
- Multi-guard callback API: `onRebootRequest(...)`.
- Multi-evaluation event API: `onEvaluation(...)`.
- Deferred guard votes: any guard can defer and force evaluation restart from guard #1.
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

## Examples
- [`basic_reboot_request`](examples/basic_reboot_request/basic_reboot_request.ino): minimal accepted request flow with guard + evaluation callbacks.
- [`busy_and_retry`](examples/busy_and_retry/busy_and_retry.ino): demonstrates `Busy` submit status and retrying once the manager returns to `Idle`.
- [`guard_blocking`](examples/guard_blocking/guard_blocking.ino): shows a guard rejecting reboot until a maintenance window opens.
- [`callback_timeout`](examples/callback_timeout/callback_timeout.ino): demonstrates `CallbackTimeout` behavior when a guard runs longer than `callbackTimeoutMs`.

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
Accepted path:
`Idle -> Requested -> Evaluating -> Delaying -> Rebooting -> Idle`

Deferred path:
`Idle -> Requested -> Evaluating -> Deferred -> Evaluating -> ...`

If any guard rejects (or times out), the flow is:
`Idle -> Requested -> Evaluating -> Idle`

## Guard Vote Semantics
- `allow=true`: guard accepts current pass.
- `allow=false`: request is blocked immediately and reboot does not proceed.
- `defer=true`: request is deferred immediately, remaining guards are skipped for the current pass, and evaluation restarts from the first guard after `deferTimeoutMs` (minimum 1ms when omitted/0).

## License
MIT - see [LICENSE.md](LICENSE.md).
