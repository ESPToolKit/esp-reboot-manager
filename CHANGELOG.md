# Changelog

All notable changes to this project will be documented in this file.

## [0.1.0] - 2026-03-06
### Added
- Initial ESPRebootManager release.
- Unified reboot guard callback API (`onRebootRequest`).
- Unified final evaluation callback API (`onEvaluation`).
- Async `requestReboot(reason, delayMs)` processing on internal FreeRTOS task.
- Polling helpers: `isRebootRequested`, `rebootStatus`, and `lastEvaluation`.
- Cooperative guard callback timeout handling.
