#include "Arduino.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "test_support.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

namespace {

struct FakeSemaphore {
	std::mutex mutex;
	std::condition_variable condition;
	bool available = false;
	bool deleted = false;
};

struct FakeTask {
	TaskFunction_t entry = nullptr;
	void *arg = nullptr;
	std::thread worker;
};

std::atomic<size_t> g_createdTasks{0};
std::atomic<size_t> g_deletedTasks{0};

const auto g_startTime = std::chrono::steady_clock::now();
thread_local TaskHandle_t g_currentTaskHandle = nullptr;

} // namespace

extern "C" unsigned long millis(void) {
	const auto elapsed = std::chrono::steady_clock::now() - g_startTime;
	return static_cast<unsigned long>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
	);
}

extern "C" SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t * /*buffer*/) {
	FakeSemaphore *semaphore = new (std::nothrow) FakeSemaphore{};
	return reinterpret_cast<SemaphoreHandle_t>(semaphore);
}

extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks) {
	if (handle == nullptr) {
		return pdFALSE;
	}

	FakeSemaphore *semaphore = reinterpret_cast<FakeSemaphore *>(handle);
	std::unique_lock<std::mutex> lock(semaphore->mutex);

	auto ready = [semaphore]() { return semaphore->available || semaphore->deleted; };

	if (ticks == 0) {
		if (!ready()) {
			return pdFALSE;
		}
	} else if (ticks == portMAX_DELAY) {
		semaphore->condition.wait(lock, ready);
	} else {
		const auto timeout = std::chrono::milliseconds(static_cast<int64_t>(ticks));
		if (!semaphore->condition.wait_for(lock, timeout, ready)) {
			return pdFALSE;
		}
	}

	if (semaphore->deleted) {
		return pdFALSE;
	}

	if (!semaphore->available) {
		return pdFALSE;
	}

	semaphore->available = false;
	return pdTRUE;
}

extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
	if (handle == nullptr) {
		return pdFALSE;
	}

	FakeSemaphore *semaphore = reinterpret_cast<FakeSemaphore *>(handle);
	{
		std::lock_guard<std::mutex> lock(semaphore->mutex);
		if (semaphore->deleted) {
			return pdFALSE;
		}
		semaphore->available = true;
	}
	semaphore->condition.notify_one();
	return pdTRUE;
}

extern "C" void vSemaphoreDelete(SemaphoreHandle_t handle) {
	if (handle == nullptr) {
		return;
	}

	FakeSemaphore *semaphore = reinterpret_cast<FakeSemaphore *>(handle);
	{
		std::lock_guard<std::mutex> lock(semaphore->mutex);
		semaphore->deleted = true;
		semaphore->available = false;
	}
	semaphore->condition.notify_all();
	delete semaphore;
}

extern "C" BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char * /*name*/,
    uint32_t /*stackDepth*/,
    void *parameters,
    UBaseType_t /*priority*/,
    TaskHandle_t *createdTask,
    BaseType_t /*coreId*/
) {
	if (task == nullptr) {
		return pdFAIL;
	}

	FakeTask *fakeTask = new (std::nothrow) FakeTask{};
	if (fakeTask == nullptr) {
		return pdFAIL;
	}

	fakeTask->entry = task;
	fakeTask->arg = parameters;

	TaskHandle_t handle = reinterpret_cast<TaskHandle_t>(fakeTask);
	if (createdTask != nullptr) {
		*createdTask = handle;
	}

	fakeTask->worker = std::thread([fakeTask, handle]() {
		g_currentTaskHandle = handle;
		fakeTask->entry(fakeTask->arg);
		g_currentTaskHandle = nullptr;
	});

	g_createdTasks.fetch_add(1, std::memory_order_relaxed);
	return pdPASS;
}

extern "C" void vTaskDelete(TaskHandle_t task) {
	TaskHandle_t target = task;
	if (target == nullptr) {
		target = g_currentTaskHandle;
	}

	if (target == nullptr) {
		return;
	}

	FakeTask *fakeTask = reinterpret_cast<FakeTask *>(target);

	if (fakeTask->worker.joinable()) {
		if (std::this_thread::get_id() != fakeTask->worker.get_id()) {
			fakeTask->worker.join();
		} else {
			fakeTask->worker.detach();
		}
	}

	delete fakeTask;
	g_deletedTasks.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void vTaskDelay(TickType_t ticks) {
	std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(ticks)));
}

extern "C" TickType_t xTaskGetTickCount(void) {
	const auto elapsed = std::chrono::steady_clock::now() - g_startTime;
	return static_cast<TickType_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
	);
}

extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(void) {
	return g_currentTaskHandle;
}

namespace test_support {

void resetRuntime() {
	g_createdTasks.store(0, std::memory_order_relaxed);
	g_deletedTasks.store(0, std::memory_order_relaxed);
}

size_t createdTaskCount() {
	return g_createdTasks.load(std::memory_order_relaxed);
}

size_t deletedTaskCount() {
	return g_deletedTasks.load(std::memory_order_relaxed);
}

} // namespace test_support
