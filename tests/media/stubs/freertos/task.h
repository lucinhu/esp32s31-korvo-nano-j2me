#pragma once
#include "FreeRTOS.h"
void vTaskDelay(unsigned ticks);
void vTaskDelete(TaskHandle_t task);
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, unsigned stack,
    void *arg, unsigned priority, TaskHandle_t *handle, int core);
