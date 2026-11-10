#include "korvo_parallel.h"
#include "korvo_simd.h"
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* One persistent worker owns no VM references between calls. Audio (priority
 * 5) and MIDI (4) on Core 1 preempt this priority-3 raster worker. */
static TaskHandle_t worker;
static SemaphoreHandle_t available, request, finished;
static void (*function)(void *, nj_u4);
static void *argument;
static bool enabled = true;
static unsigned jobs;
static int worker_core = -1;
static uint64_t worker_us, wait_us;

static void render_worker(void *unused)
{
    (void)unused;
    worker_core = xPortGetCoreID();
    korvo_simd_prime();
    for (;;) {
        xSemaphoreTake(request, portMAX_DELAY);
        int64_t start = esp_timer_get_time();
        function(argument, 1);
        worker_us += esp_timer_get_time() - start;
        xSemaphoreGive(finished);
    }
}

void korvo_parallel_init(void)
{
    available = xSemaphoreCreateMutex();
    request = xSemaphoreCreateBinary();
    finished = xSemaphoreCreateBinary();
    if (!available || !request || !finished ||
        xTaskCreatePinnedToCore(render_worker, "j2me-render", 8192, NULL, 3, &worker, 1) != pdPASS) {
        if (available) vSemaphoreDelete(available);
        if (request) vSemaphoreDelete(request);
        if (finished) vSemaphoreDelete(finished);
        available = request = finished = NULL;
        worker = NULL;
        puts("PARALLEL unavailable; using serial rendering");
    }
}

void korvo_parallel_enable(bool value) { enabled = value; }
bool korvo_parallel_enabled(void) { return enabled && worker != NULL; }
void korvo_parallel_reset(void) { jobs = 0; worker_us = wait_us = 0; }
void korvo_parallel_stats(void)
{
    printf("PARALLEL enabled=%d worker_core=%d jobs=%u worker_us=%llu wait_us=%llu stack_free=%u\n",
           korvo_parallel_enabled(), worker_core, jobs,
           (unsigned long long)worker_us, (unsigned long long)wait_us,
           worker ? (unsigned)uxTaskGetStackHighWaterMark(worker) : 0);
}

void korvo_parallel_run(void *ud, void (*work)(void *, nj_u4), void *context, nj_u4 count)
{
    (void)ud;
    if (!korvo_parallel_enabled() || count != 2 || xPortGetCoreID() != 0) {
        for (nj_u4 i = 0; i < count; ++i) work(context, i);
        return;
    }
    xSemaphoreTake(available, portMAX_DELAY);
    function = work;
    argument = context;
    xSemaphoreGive(request);
    work(context, 0);
    int64_t start = esp_timer_get_time();
    xSemaphoreTake(finished, portMAX_DELAY);
    wait_us += esp_timer_get_time() - start;
    ++jobs;
    function = NULL;
    argument = NULL;
    xSemaphoreGive(available);
}

typedef struct { uint32_t *output, seed; int cores[2]; } test_job;
static uint32_t test_value(uint32_t seed, unsigned i)
{
    uint32_t v = seed ^ (i * 0x45d9f3bu);
    return (v ^ (v >> 16)) * 0x27d4eb2du;
}
static void test_work(void *context, nj_u4 part)
{
    test_job *job = context;
    unsigned first = part ? 2049 : 0, end = part ? 4099 : 2049;
    job->cores[part] = xPortGetCoreID();
    for (unsigned i = first; i < end; ++i) job->output[i] = test_value(job->seed, i);
}
void korvo_parallel_test(unsigned rounds)
{
    /* Odd boundaries share cache lines: verify real PSRAM visibility as well
     * as synchronous completion and repeated reuse of a stack-owned job. */
    uint32_t *output = heap_caps_malloc(4101 * sizeof(*output), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) { puts("PARALLEL_TEST allocation failed"); return; }
    unsigned bad = 0;
    test_job job = {.output = output + 1};
    output[0] = 0x12345678u; output[4100] = 0x87654321u;
    for (unsigned n = 0; n < rounds; ++n) {
        job.seed = n * 1664525u + 1013904223u;
        korvo_parallel_run(NULL, test_work, &job, 2u);
        for (unsigned i = 0; i < 4099; ++i)
            bad += job.output[i] != test_value(job.seed, i);
        bad += output[0] != 0x12345678u || output[4100] != 0x87654321u;
    }
    printf("PARALLEL_TEST rounds=%u mismatches=%u caller_core=%d worker_core=%d\n",
           rounds, bad, job.cores[0], job.cores[1]);
    heap_caps_free(output);
}
