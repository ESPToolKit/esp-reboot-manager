#include <ESPRebootManager.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "test_support.h"

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expectTrue(bool condition, const std::string& message) {
    if( !condition ){
        fail(message);
    }
}

void expectFalse(bool condition, const std::string& message) {
    if( condition ){
        fail(message);
    }
}

template <typename T>
void expectEqual(const T& actual, const T& expected, const std::string& message) {
    if( !(actual == expected) ){
        fail(message);
    }
}

bool waitUntil(const std::function<bool()>& predicate, uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while( std::chrono::steady_clock::now() < deadline ){
        if( predicate() ){
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

void testInitDeinitLifecycle() {
    test_support::resetRuntime();

    ESPRebootManager manager;
    expectFalse(manager.isInitialized(), "manager should start uninitialized");

    manager.deinit();
    expectFalse(manager.isInitialized(), "deinit before init should be safe");

    expectTrue(manager.init(), "init should succeed");
    expectTrue(manager.isInitialized(), "manager should be initialized");

    manager.deinit();
    expectFalse(manager.isInitialized(), "manager should deinitialize cleanly");

    manager.deinit();
    expectFalse(manager.isInitialized(), "deinit should be idempotent");

    expectTrue(test_support::createdTaskCount() >= static_cast<size_t>(1), "init should create worker task");
}

void testCallbackRegistrationAndUnregister() {
    ESPRebootManager manager;
    expectTrue(manager.init(), "init should succeed");

    RebootCallbackId guardId = manager.onRebootRequest([](const RebootRequestContext&) {
        return RebootVote{};
    });
    RebootCallbackId evalId = manager.onEvaluation([](const RebootEvaluation&) {});

    expectTrue(guardId > 0, "guard callback registration should return valid id");
    expectTrue(evalId > 0, "evaluation callback registration should return valid id");

    expectTrue(manager.offRebootRequest(guardId), "guard callback should be removable");
    expectFalse(manager.offRebootRequest(guardId), "removed guard callback should not be removable twice");

    expectTrue(manager.offEvaluation(evalId), "evaluation callback should be removable");
    expectFalse(manager.offEvaluation(evalId), "removed evaluation callback should not be removable twice");

    manager.deinit();
}

void testRequestValidationAndBusyHandling() {
    ESPRebootManager manager;
    ESPRebootManagerConfig cfg{};
    cfg.rebootExecutor = []() {};

    expectTrue(manager.init(cfg), "init should succeed");

    RebootSubmitResult nullReason = manager.requestReboot(nullptr, 0);
    expectEqual(nullReason.status, RebootSubmitStatus::InvalidArgument, "null reason should be invalid");

    RebootSubmitResult emptyReason = manager.requestReboot("", 0);
    expectEqual(emptyReason.status, RebootSubmitStatus::InvalidArgument, "empty reason should be invalid");

    const char* tooLongReason = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890++++";
    RebootSubmitResult longReason = manager.requestReboot(tooLongReason, 0);
    expectEqual(longReason.status, RebootSubmitStatus::InvalidArgument, "long reason should be invalid");

    manager.onRebootRequest([](const RebootRequestContext&) {
        vTaskDelay(pdMS_TO_TICKS(30));
        return RebootVote{};
    });

    RebootSubmitResult first = manager.requestReboot("normal", 60);
    expectEqual(first.status, RebootSubmitStatus::Queued, "first request should queue");

    RebootSubmitResult second = manager.requestReboot("parallel", 0);
    expectEqual(second.status, RebootSubmitStatus::Busy, "second request should return busy while first is active");

    expectTrue(waitUntil([&manager]() { return manager.rebootStatus() == RebootRequestStatus::Idle; }, 500),
               "request should eventually return to idle");

    manager.deinit();
}

void testAcceptedFlowAndRebootExecution() {
    ESPRebootManager manager;

    std::atomic<int> evaluationCount{0};
    std::atomic<int> rebootCount{0};
    RebootEvaluation captured{};

    ESPRebootManagerConfig cfg{};
    cfg.rebootExecutor = [&rebootCount]() {
        rebootCount.fetch_add(1, std::memory_order_relaxed);
    };

    expectTrue(manager.init(cfg), "init should succeed");

    manager.onRebootRequest([](const RebootRequestContext&) {
        vTaskDelay(pdMS_TO_TICKS(5));
        return RebootVote{};
    });
    manager.onRebootRequest([](const RebootRequestContext&) {
        return RebootVote{};
    });

    manager.onEvaluation([&evaluationCount, &captured](const RebootEvaluation& evaluation) {
        captured = evaluation;
        evaluationCount.fetch_add(1, std::memory_order_relaxed);
    });

    RebootSubmitResult result = manager.requestReboot("accepted", 20);
    expectEqual(result.status, RebootSubmitStatus::Queued, "accepted test request should queue");

    expectTrue(waitUntil([&evaluationCount]() { return evaluationCount.load(std::memory_order_relaxed) == 1; }, 500),
               "evaluation callback should fire once for accepted request");

    expectTrue(captured.accepted, "accepted flow should report accepted evaluation");
    expectEqual(captured.code, RebootDecisionCode::Accepted, "accepted flow should use Accepted code");

    expectTrue(waitUntil([&rebootCount]() { return rebootCount.load(std::memory_order_relaxed) == 1; }, 500),
               "accepted flow should invoke reboot executor once");

    expectTrue(waitUntil([&manager]() { return manager.rebootStatus() == RebootRequestStatus::Idle; }, 500),
               "accepted flow should return to idle after executor returns");
    expectFalse(manager.isRebootRequested(), "isRebootRequested should be false in idle state");

    manager.deinit();
}

void testBlockedFlow() {
    ESPRebootManager manager;

    std::atomic<int> rebootCount{0};
    RebootEvaluation captured{};

    ESPRebootManagerConfig cfg{};
    cfg.rebootExecutor = [&rebootCount]() {
        rebootCount.fetch_add(1, std::memory_order_relaxed);
    };

    expectTrue(manager.init(cfg), "init should succeed");

    manager.onRebootRequest([](const RebootRequestContext&) {
        return RebootVote{};
    });

    manager.onRebootRequest([](const RebootRequestContext&) {
        RebootVote vote{};
        vote.allow = false;
        std::snprintf(vote.detail, sizeof(vote.detail), "module2 not ready");
        return vote;
    });

    std::atomic<int> evaluationCount{0};
    manager.onEvaluation([&captured, &evaluationCount](const RebootEvaluation& evaluation) {
        captured = evaluation;
        evaluationCount.fetch_add(1, std::memory_order_relaxed);
    });

    RebootSubmitResult result = manager.requestReboot("blocked", 20);
    expectEqual(result.status, RebootSubmitStatus::Queued, "blocked test request should queue");

    expectTrue(waitUntil([&evaluationCount]() { return evaluationCount.load(std::memory_order_relaxed) == 1; }, 500),
               "blocked request should emit exactly one evaluation");

    expectFalse(captured.accepted, "blocked flow should report rejected evaluation");
    expectEqual(captured.code, RebootDecisionCode::Blocked, "blocked flow should report blocked decision code");
    expectTrue(std::strstr(captured.detail, "module2") != nullptr, "blocked detail should include blocker reason");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    expectEqual(rebootCount.load(std::memory_order_relaxed), 0, "blocked flow must not invoke reboot executor");

    expectTrue(waitUntil([&manager]() { return manager.rebootStatus() == RebootRequestStatus::Idle; }, 200),
               "blocked flow should return to idle");

    manager.deinit();
}

void testCallbackTimeoutFlow() {
    ESPRebootManager manager;

    std::atomic<int> rebootCount{0};
    RebootEvaluation captured{};

    ESPRebootManagerConfig cfg{};
    cfg.callbackTimeoutMs = 10;
    cfg.rebootExecutor = [&rebootCount]() {
        rebootCount.fetch_add(1, std::memory_order_relaxed);
    };

    expectTrue(manager.init(cfg), "init should succeed");

    manager.onRebootRequest([](const RebootRequestContext&) {
        vTaskDelay(pdMS_TO_TICKS(25));
        return RebootVote{};
    });

    std::atomic<int> evaluationCount{0};
    manager.onEvaluation([&captured, &evaluationCount](const RebootEvaluation& evaluation) {
        captured = evaluation;
        evaluationCount.fetch_add(1, std::memory_order_relaxed);
    });

    RebootSubmitResult result = manager.requestReboot("timeout", 0);
    expectEqual(result.status, RebootSubmitStatus::Queued, "timeout test request should queue");

    expectTrue(waitUntil([&evaluationCount]() { return evaluationCount.load(std::memory_order_relaxed) == 1; }, 500),
               "timeout request should emit one evaluation");

    expectFalse(captured.accepted, "timeout flow should reject the reboot");
    expectEqual(captured.code, RebootDecisionCode::CallbackTimeout, "timeout flow should report callback timeout");
    expectEqual(rebootCount.load(std::memory_order_relaxed), 0, "timeout flow must not reboot");

    manager.deinit();
}

void testPollingAndLastEvaluation() {
    ESPRebootManager manager;
    expectTrue(manager.init(), "init should succeed");

    manager.onRebootRequest([](const RebootRequestContext&) {
        RebootVote vote{};
        vote.allow = false;
        std::snprintf(vote.detail, sizeof(vote.detail), "storage flush in progress");
        return vote;
    });

    RebootSubmitResult result = manager.requestReboot("poll-check", 0);
    expectEqual(result.status, RebootSubmitStatus::Queued, "poll-check request should queue");

    expectTrue(manager.isRebootRequested(), "manager should report requested state immediately after queue");

    expectTrue(waitUntil([&manager]() { return manager.rebootStatus() == RebootRequestStatus::Idle; }, 500),
               "rejected request should return to idle");

    RebootEvaluation last = manager.lastEvaluation();
    expectFalse(last.accepted, "last evaluation should report rejection");
    expectEqual(last.code, RebootDecisionCode::Blocked, "last evaluation should preserve blocked code");
    expectTrue(std::strstr(last.detail, "storage") != nullptr, "last evaluation should preserve blocker detail");

    manager.deinit();
}

}  // namespace

int main() {
    try {
        testInitDeinitLifecycle();
        testCallbackRegistrationAndUnregister();
        testRequestValidationAndBusyHandling();
        testAcceptedFlowAndRebootExecution();
        testBlockedFlow();
        testCallbackTimeoutFlow();
        testPollingAndLastEvaluation();
    } catch( const std::exception& exception ){
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }

    std::cout << "All ESPRebootManager tests passed\n";
    return 0;
}
