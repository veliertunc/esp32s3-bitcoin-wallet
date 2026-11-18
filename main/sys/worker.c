#include "worker.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
    worker_fn_t fn;
    void *arg;
} job_t;

static QueueHandle_t s_q;
static volatile bool s_busy;

static void worker_task(void *p)
{
    job_t j;
    for (;;) {
        if (xQueueReceive(s_q, &j, portMAX_DELAY) == pdTRUE) {
            j.fn(j.arg);
            s_busy = false;
        }
    }
}

void worker_start(void)
{
    s_q = xQueueCreate(1, sizeof(job_t));
    /* big stack: PBKDF2, JSON parsing and TLS run here */
    xTaskCreatePinnedToCore(worker_task, "worker", 16384, NULL, 3, NULL, 0);
}

bool worker_run(worker_fn_t fn, void *arg)
{
    if (s_busy) {
        return false;
    }
    s_busy = true;
    job_t j = { fn, arg };
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        s_busy = false;
        return false;
    }
    return true;
}

bool worker_busy(void)
{
    return s_busy;
}
