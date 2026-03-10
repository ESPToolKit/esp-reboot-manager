#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t configSTACK_DEPTH_TYPE;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef uint32_t StackType_t;

typedef struct StaticSemaphore {
	uintptr_t storage[8];
} StaticSemaphore_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0

#define portMAX_DELAY ((TickType_t) - 1)
#define portTICK_PERIOD_MS 1
#define tskNO_AFFINITY (-1)

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#ifdef __cplusplus
}
#endif
