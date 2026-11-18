/*
 * Tiny HTTPS client for an Esplora-compatible REST API (mempool.space, blockstream.info, electrs).
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

typedef struct esplora_s esplora_t;

esplora_t *esplora_open(void);
void       esplora_close(esplora_t *e);

/* GET base+path. *body is malloc'd and NUL-terminated; caller frees.
 * Returns ESP_OK only for HTTP 200. */
esp_err_t esplora_get(esplora_t *e, const char *path, char **body, size_t *len);

/* POST text body to base+path. *status receives the HTTP status code. */
esp_err_t esplora_post(esplora_t *e, const char *path, const char *data, char **body, int *status);
