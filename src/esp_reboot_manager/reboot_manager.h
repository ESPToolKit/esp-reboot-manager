#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
}

using RebootCallbackId = uint32_t;

enum class RebootRequestStatus : uint8_t {
    Idle = 0,
    Requested = 1,
    Evaluating = 2,
    Delaying = 3,
    Rebooting = 4,
};

enum class RebootSubmitStatus : uint8_t {
    Queued = 0,
    Busy = 1,
    InvalidArgument = 2,
    NotInitialized = 3,
    InternalError = 4,
};

enum class RebootDecisionCode : uint8_t {
    Accepted = 0,
    Blocked = 1,
    CallbackTimeout = 2,
    InvalidArgument = 3,
    InternalError = 4,
};

struct RebootRequestContext {
    uint32_t requestId = 0;
    char reason[64] = {};
    uint32_t delayMs = 0;
    uint32_t requestedAtMs = 0;
};

struct RebootVote {
    bool allow = true;
    char detail[96] = {};
};

struct RebootEvaluation {
    uint32_t requestId = 0;
    bool accepted = false;
    RebootDecisionCode code = RebootDecisionCode::InternalError;
    char reason[64] = {};
    uint32_t delayMs = 0;
    char blockerName[32] = {};
    char detail[96] = {};
    uint32_t evaluatedAtMs = 0;
};

struct RebootSubmitResult {
    RebootSubmitStatus status = RebootSubmitStatus::InternalError;
    uint32_t requestId = 0;
};

struct ESPRebootManagerConfig {
    const char* taskName = "reboot-manager";
    uint32_t taskStackSizeBytes = 6 * 1024;
    UBaseType_t taskPriority = 1;
    BaseType_t taskCoreId = tskNO_AFFINITY;
    uint32_t callbackTimeoutMs = 1000;
    std::function<void()> rebootExecutor;
};

class ESPRebootManager {
   public:
    using GuardCallback = std::function<RebootVote(const RebootRequestContext&)>;
    using EvaluationCallback = std::function<void(const RebootEvaluation&)>;

    ESPRebootManager() = default;
    ~ESPRebootManager();

    bool init(const ESPRebootManagerConfig& config = {});
    void deinit();
    bool isInitialized() const;

    RebootCallbackId onRebootRequest(GuardCallback cb);
    RebootCallbackId onEvaluation(EvaluationCallback cb);
    bool offRebootRequest(RebootCallbackId id);
    bool offEvaluation(RebootCallbackId id);

    RebootSubmitResult requestReboot(const char* reason, uint32_t delayMs = 0);

    bool isRebootRequested() const;
    RebootRequestStatus rebootStatus() const;
    RebootEvaluation lastEvaluation() const;

   private:
    struct GuardEntry {
        RebootCallbackId id = 0;
        GuardCallback callback;
        bool active = false;
    };

    struct EvaluationEntry {
        RebootCallbackId id = 0;
        EvaluationCallback callback;
        bool active = false;
    };

    struct PendingRequest {
        bool pending = false;
        RebootRequestContext request;
    };

    static void taskEntry(void* arg);
    void taskLoop();
    void processRequest(const RebootRequestContext& request);
    void emitEvaluation(const RebootEvaluation& evaluation);

    static void copyText(const char* source, char* destination, size_t destinationSize);
    static void compactGuards(std::vector<GuardEntry>& entries);
    static void compactEvaluations(std::vector<EvaluationEntry>& entries);
    static void formatBlockerName(RebootCallbackId id, char* destination, size_t destinationSize);

    std::vector<GuardEntry> guardSnapshot();
    std::vector<EvaluationEntry> evaluationSnapshot();

    uint32_t nowMs() const;
    void setStatus(RebootRequestStatus status);

    ESPRebootManagerConfig config_{};

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<RebootRequestStatus> status_{RebootRequestStatus::Idle};
    std::atomic<uint32_t> nextCallbackId_{1};
    std::atomic<uint32_t> nextRequestId_{1};

    std::atomic<TaskHandle_t> taskHandle_{nullptr};
    SemaphoreHandle_t signalSemaphore_{nullptr};
    StaticSemaphore_t signalSemaphoreBuffer_{};

    mutable std::mutex mutex_;
    std::vector<GuardEntry> guards_{};
    std::vector<EvaluationEntry> evaluations_{};
    PendingRequest pendingRequest_{};
    RebootEvaluation lastEvaluation_{};
};
