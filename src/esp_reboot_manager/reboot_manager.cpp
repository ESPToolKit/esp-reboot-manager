#include "esp_reboot_manager/reboot_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#if __has_include(<Arduino.h>)
#include <Arduino.h>
#endif

namespace {

constexpr uint32_t kDeinitJoinTimeoutMs = 3000;
constexpr uint32_t kMinimumDeferTimeoutMs = 1;

void defaultRebootExecutor() {
#if __has_include(<Arduino.h>)
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
	ESP.restart();
#endif
#endif
}

uint32_t stackSizeWords(uint32_t stackSizeBytes) {
	if (stackSizeBytes == 0) {
		return 0;
	}
	const uint32_t wordSize = static_cast<uint32_t>(sizeof(StackType_t));
	return (stackSizeBytes + wordSize - 1U) / wordSize;
}

} // namespace

ESPRebootManager::~ESPRebootManager() {
	deinit();
}

bool ESPRebootManager::init(const ESPRebootManagerConfig &config) {
	if (initialized_.load(std::memory_order_acquire)) {
		deinit();
	}

	ESPRebootManagerConfig effective = config;
	if (effective.taskName == nullptr || effective.taskName[0] == '\0') {
		effective.taskName = "reboot-manager";
	}
	if (effective.taskStackSizeBytes == 0) {
		effective.taskStackSizeBytes = 6U * 1024U;
	}
	if (effective.callbackTimeoutMs == 0) {
		effective.callbackTimeoutMs = 1000;
	}
	if (!effective.rebootExecutor) {
		effective.rebootExecutor = defaultRebootExecutor;
	}

	signalSemaphore_ = xSemaphoreCreateBinaryStatic(&signalSemaphoreBuffer_);
	if (signalSemaphore_ == nullptr) {
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		config_ = std::move(effective);
		guards_.clear();
		evaluations_.clear();
		pendingRequest_ = PendingRequest{};
		lastEvaluation_ = RebootEvaluation{};
	}

	setStatus(RebootRequestStatus::Idle);
	running_.store(true, std::memory_order_release);

	const uint32_t stackWords = stackSizeWords(config_.taskStackSizeBytes);
	TaskHandle_t createdTaskHandle = nullptr;
	BaseType_t created = xTaskCreatePinnedToCore(
	    &ESPRebootManager::taskEntry,
	    config_.taskName,
	    stackWords,
	    this,
	    config_.taskPriority,
	    &createdTaskHandle,
	    config_.taskCoreId
	);

	if (created != pdPASS) {
		running_.store(false, std::memory_order_release);
		vSemaphoreDelete(signalSemaphore_);
		signalSemaphore_ = nullptr;
		taskHandle_.store(nullptr, std::memory_order_release);
		return false;
	}

	taskHandle_.store(createdTaskHandle, std::memory_order_release);
	initialized_.store(true, std::memory_order_release);
	return true;
}

void ESPRebootManager::deinit() {
	running_.store(false, std::memory_order_release);

	if (signalSemaphore_ != nullptr) {
		(void)xSemaphoreGive(signalSemaphore_);
	}

	const uint32_t startMs = nowMs();
	while (taskHandle_.load(std::memory_order_acquire) != nullptr &&
	       (nowMs() - startMs) < kDeinitJoinTimeoutMs) {
		vTaskDelay(pdMS_TO_TICKS(1));
	}

	TaskHandle_t taskHandle = taskHandle_.load(std::memory_order_acquire);
	if (taskHandle != nullptr) {
		vTaskDelete(taskHandle);
		taskHandle_.store(nullptr, std::memory_order_release);
	}

	if (signalSemaphore_ != nullptr) {
		vSemaphoreDelete(signalSemaphore_);
		signalSemaphore_ = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		guards_.clear();
		evaluations_.clear();
		pendingRequest_ = PendingRequest{};
		lastEvaluation_ = RebootEvaluation{};
		config_ = ESPRebootManagerConfig{};
	}

	setStatus(RebootRequestStatus::Idle);
	initialized_.store(false, std::memory_order_release);
}

bool ESPRebootManager::isInitialized() const {
	return initialized_.load(std::memory_order_acquire);
}

RebootCallbackId ESPRebootManager::onRebootRequest(GuardCallback cb) {
	if (!cb) {
		return 0;
	}

	const RebootCallbackId id = nextCallbackId_.fetch_add(1, std::memory_order_acq_rel);

	std::lock_guard<std::mutex> lock(mutex_);
	GuardEntry entry{};
	entry.id = id;
	entry.callback = std::move(cb);
	entry.active = true;
	guards_.push_back(std::move(entry));
	return id;
}

RebootCallbackId ESPRebootManager::onEvaluation(EvaluationCallback cb) {
	if (!cb) {
		return 0;
	}

	const RebootCallbackId id = nextCallbackId_.fetch_add(1, std::memory_order_acq_rel);

	std::lock_guard<std::mutex> lock(mutex_);
	EvaluationEntry entry{};
	entry.id = id;
	entry.callback = std::move(cb);
	entry.active = true;
	evaluations_.push_back(std::move(entry));
	return id;
}

bool ESPRebootManager::offRebootRequest(RebootCallbackId id) {
	if (id == 0) {
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	bool removed = false;
	for (GuardEntry &entry : guards_) {
		if (entry.id == id && entry.active) {
			entry.active = false;
			removed = true;
			break;
		}
	}
	if (removed) {
		compactGuards(guards_);
	}
	return removed;
}

bool ESPRebootManager::offEvaluation(RebootCallbackId id) {
	if (id == 0) {
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	bool removed = false;
	for (EvaluationEntry &entry : evaluations_) {
		if (entry.id == id && entry.active) {
			entry.active = false;
			removed = true;
			break;
		}
	}
	if (removed) {
		compactEvaluations(evaluations_);
	}
	return removed;
}

RebootSubmitResult ESPRebootManager::requestReboot(const char *reason, uint32_t delayMs) {
	RebootSubmitResult result{};

	if (!isInitialized()) {
		result.status = RebootSubmitStatus::NotInitialized;
		return result;
	}

	if (reason == nullptr || reason[0] == '\0') {
		result.status = RebootSubmitStatus::InvalidArgument;
		return result;
	}

	if (std::strlen(reason) >= sizeof(RebootRequestContext::reason)) {
		result.status = RebootSubmitStatus::InvalidArgument;
		return result;
	}

	RebootRequestContext context{};
	context.requestId = nextRequestId_.fetch_add(1, std::memory_order_acq_rel);
	copyText(reason, context.reason, sizeof(context.reason));
	context.delayMs = delayMs;
	context.requestedAtMs = nowMs();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (status_.load(std::memory_order_acquire) != RebootRequestStatus::Idle ||
		    pendingRequest_.pending) {
			result.status = RebootSubmitStatus::Busy;
			return result;
		}

		pendingRequest_.pending = true;
		pendingRequest_.request = context;
		setStatus(RebootRequestStatus::Requested);
	}

	if (signalSemaphore_ == nullptr || xSemaphoreGive(signalSemaphore_) != pdTRUE) {
		std::lock_guard<std::mutex> lock(mutex_);
		pendingRequest_.pending = false;
		setStatus(RebootRequestStatus::Idle);
		result.status = RebootSubmitStatus::InternalError;
		return result;
	}

	result.status = RebootSubmitStatus::Queued;
	result.requestId = context.requestId;
	return result;
}

bool ESPRebootManager::isRebootRequested() const {
	return rebootStatus() != RebootRequestStatus::Idle;
}

RebootRequestStatus ESPRebootManager::rebootStatus() const {
	return status_.load(std::memory_order_acquire);
}

RebootEvaluation ESPRebootManager::lastEvaluation() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return lastEvaluation_;
}

void ESPRebootManager::taskEntry(void *arg) {
	ESPRebootManager *manager = static_cast<ESPRebootManager *>(arg);
	if (manager != nullptr) {
		manager->taskLoop();
	}
}

void ESPRebootManager::taskLoop() {
	while (running_.load(std::memory_order_acquire)) {
		if (signalSemaphore_ == nullptr) {
			break;
		}

		if (xSemaphoreTake(signalSemaphore_, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		if (!running_.load(std::memory_order_acquire)) {
			break;
		}

		RebootRequestContext request{};
		bool hasRequest = false;

		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (pendingRequest_.pending) {
				request = pendingRequest_.request;
				pendingRequest_.pending = false;
				hasRequest = true;
			}
		}

		if (!hasRequest) {
			continue;
		}

		processRequest(request);
	}

	taskHandle_.store(nullptr, std::memory_order_release);
}

void ESPRebootManager::processRequest(const RebootRequestContext &request) {
	while (running_.load(std::memory_order_acquire)) {
		setStatus(RebootRequestStatus::Evaluating);

		RebootEvaluation evaluation{};
		evaluation.requestId = request.requestId;
		copyText(request.reason, evaluation.reason, sizeof(evaluation.reason));
		evaluation.delayMs = request.delayMs;

		const std::vector<GuardEntry> guards = guardSnapshot();
		bool deferred = false;

		for (const GuardEntry &guard : guards) {
			if (!guard.callback) {
				continue;
			}

			const uint32_t startedMs = nowMs();
			RebootVote vote = guard.callback(request);
			const uint32_t elapsedMs = nowMs() - startedMs;

			if (config_.callbackTimeoutMs > 0 && elapsedMs > config_.callbackTimeoutMs) {
				evaluation.accepted = false;
				evaluation.code = RebootDecisionCode::CallbackTimeout;
				formatBlockerName(guard.id, evaluation.blockerName, sizeof(evaluation.blockerName));
				if (vote.detail[0] != '\0') {
					copyText(vote.detail, evaluation.detail, sizeof(evaluation.detail));
				} else {
					copyText(
					    "guard callback exceeded timeout",
					    evaluation.detail,
					    sizeof(evaluation.detail)
					);
				}
				evaluation.evaluatedAtMs = nowMs();
				emitEvaluation(evaluation);
				setStatus(RebootRequestStatus::Idle);
				return;
			}

			if (vote.defer) {
				const uint32_t deferTimeoutMs =
				    vote.deferTimeoutMs > 0 ? vote.deferTimeoutMs : kMinimumDeferTimeoutMs;
				evaluation.accepted = false;
				evaluation.code = RebootDecisionCode::Deferred;
				evaluation.deferTimeoutMs = deferTimeoutMs;
				formatBlockerName(guard.id, evaluation.blockerName, sizeof(evaluation.blockerName));
				if (vote.detail[0] != '\0') {
					copyText(vote.detail, evaluation.detail, sizeof(evaluation.detail));
				} else {
					copyText(
					    "deferred by guard callback",
					    evaluation.detail,
					    sizeof(evaluation.detail)
					);
				}
				evaluation.evaluatedAtMs = nowMs();
				emitEvaluation(evaluation);

				setStatus(RebootRequestStatus::Deferred);
				vTaskDelay(pdMS_TO_TICKS(deferTimeoutMs));
				if (!running_.load(std::memory_order_acquire)) {
					setStatus(RebootRequestStatus::Idle);
					return;
				}

				deferred = true;
				break;
			}

			if (!vote.allow) {
				evaluation.accepted = false;
				evaluation.code = RebootDecisionCode::Blocked;
				formatBlockerName(guard.id, evaluation.blockerName, sizeof(evaluation.blockerName));
				if (vote.detail[0] != '\0') {
					copyText(vote.detail, evaluation.detail, sizeof(evaluation.detail));
				} else {
					copyText(
					    "blocked by guard callback",
					    evaluation.detail,
					    sizeof(evaluation.detail)
					);
				}
				evaluation.evaluatedAtMs = nowMs();
				emitEvaluation(evaluation);
				setStatus(RebootRequestStatus::Idle);
				return;
			}
		}

		if (deferred) {
			continue;
		}

		evaluation.accepted = true;
		evaluation.code = RebootDecisionCode::Accepted;
		evaluation.evaluatedAtMs = nowMs();
		emitEvaluation(evaluation);

		setStatus(RebootRequestStatus::Delaying);
		if (request.delayMs > 0) {
			vTaskDelay(pdMS_TO_TICKS(request.delayMs));
		}

		if (!running_.load(std::memory_order_acquire)) {
			setStatus(RebootRequestStatus::Idle);
			return;
		}

		setStatus(RebootRequestStatus::Rebooting);
		std::function<void()> rebootExecutor;

		{
			std::lock_guard<std::mutex> lock(mutex_);
			rebootExecutor = config_.rebootExecutor;
		}

		if (rebootExecutor) {
			rebootExecutor();
		}

		setStatus(RebootRequestStatus::Idle);
		return;
	}

	setStatus(RebootRequestStatus::Idle);
}

void ESPRebootManager::emitEvaluation(const RebootEvaluation &evaluation) {
	std::vector<EvaluationEntry> callbacks;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		lastEvaluation_ = evaluation;
		callbacks = evaluationSnapshot();
	}

	for (const EvaluationEntry &callback : callbacks) {
		if (callback.callback) {
			callback.callback(evaluation);
		}
	}
}

void ESPRebootManager::copyText(const char *source, char *destination, size_t destinationSize) {
	if (destination == nullptr || destinationSize == 0) {
		return;
	}

	if (source == nullptr) {
		destination[0] = '\0';
		return;
	}

	std::snprintf(destination, destinationSize, "%s", source);
}

void ESPRebootManager::compactGuards(std::vector<GuardEntry> &entries) {
	entries.erase(
	    std::remove_if(
	        entries.begin(),
	        entries.end(),
	        [](const GuardEntry &entry) { return !entry.active; }
	    ),
	    entries.end()
	);
}

void ESPRebootManager::compactEvaluations(std::vector<EvaluationEntry> &entries) {
	entries.erase(
	    std::remove_if(
	        entries.begin(),
	        entries.end(),
	        [](const EvaluationEntry &entry) { return !entry.active; }
	    ),
	    entries.end()
	);
}

void ESPRebootManager::formatBlockerName(
    RebootCallbackId id, char *destination, size_t destinationSize
) {
	if (destination == nullptr || destinationSize == 0) {
		return;
	}

	std::snprintf(destination, destinationSize, "guard:%lu", static_cast<unsigned long>(id));
}

std::vector<ESPRebootManager::GuardEntry> ESPRebootManager::guardSnapshot() {
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<GuardEntry> snapshot;
	snapshot.reserve(guards_.size());
	for (const GuardEntry &guard : guards_) {
		if (guard.active && guard.callback) {
			snapshot.push_back(guard);
		}
	}
	return snapshot;
}

std::vector<ESPRebootManager::EvaluationEntry> ESPRebootManager::evaluationSnapshot() {
	std::vector<EvaluationEntry> snapshot;
	snapshot.reserve(evaluations_.size());
	for (const EvaluationEntry &entry : evaluations_) {
		if (entry.active && entry.callback) {
			snapshot.push_back(entry);
		}
	}
	return snapshot;
}

uint32_t ESPRebootManager::nowMs() const {
	return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void ESPRebootManager::setStatus(RebootRequestStatus status) {
	status_.store(status, std::memory_order_release);
}
