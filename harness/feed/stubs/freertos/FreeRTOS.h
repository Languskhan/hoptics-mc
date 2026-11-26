#pragma once
#include <stdint.h>
typedef void *SemaphoreHandle_t; typedef void *TaskHandle_t; typedef uint32_t TickType_t; typedef int BaseType_t; typedef unsigned UBaseType_t;
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define tskNO_AFFINITY 0x7fffffff
