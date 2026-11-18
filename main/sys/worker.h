/*
 * Single background worker so slow jobs (KDF, network, signing, OTA) never
 * block the LVGL task. Jobs update the UI under ui_lock().
 */
#pragma once

#include <stdbool.h>

typedef void (*worker_fn_t)(void *arg);

void worker_start(void);
bool worker_run(worker_fn_t fn, void *arg);   /* false if a job is already running */
bool worker_busy(void);
