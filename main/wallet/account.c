#include "account.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "sdkconfig.h"

#include "wallet.h"
#include "net/esplora.h"
#include "board/board_pins.h"

static const char *TAG = "account";

#define MAX_SCAN_INDEX  1000

typedef struct {
    uint32_t chain;
    uint32_t index;
    char     addr[WALLET_ADDR_MAX];
} used_addr_t;

static SemaphoreHandle_t s_mtx;
static account_summary_t s_sum;
static btc_utxo_t   *s_utxos;            /* ACCOUNT_MAX_UTXOS, PSRAM */
static account_tx_t  s_hist[ACCOUNT_MAX_HISTORY];
static uint32_t      s_rx_index;
static bool          s_rx_loaded;

static void lock(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGiveRecursive(s_mtx);
}

/* ------------------------------------------------------ persisted state */

static void state_path(char *p, size_t n)
{
    snprintf(p, n, SPIFFS_MOUNT_POINT "/acct_%s_%s.json", wallet_fingerprint_hex(),
             wallet_is_testnet() ? "t" : "m");
}

static void state_load(void)
{
    if (s_rx_loaded) {
        return;
    }
    s_rx_loaded = true;
    char path[64];
    state_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char buf[128] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    cJSON *j = cJSON_Parse(buf);
    if (j) {
        cJSON *rx = cJSON_GetObjectItem(j, "rx");
        if (cJSON_IsNumber(rx) && rx->valuedouble >= 0) {
            s_rx_index = (uint32_t)rx->valuedouble;
        }
        cJSON_Delete(j);
    }
}

static void state_save(void)
{
    char path[64];
    state_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "{\"rx\":%u}", (unsigned)s_rx_index);
        fclose(f);
    }
}

uint32_t account_receive_index(void)
{
    lock();
    state_load();
    uint32_t v = s_rx_index;
    unlock();
    return v;
}

void account_set_receive_index(uint32_t idx)
{
    lock();
    s_rx_index = idx;
    state_save();
    unlock();
}

void account_reset(void)
{
    lock();
    memset(&s_sum, 0, sizeof(s_sum));
    memset(s_hist, 0, sizeof(s_hist));
    if (s_utxos) {
        memset(s_utxos, 0, sizeof(btc_utxo_t) * ACCOUNT_MAX_UTXOS);
    }
    s_rx_index = 0;
    s_rx_loaded = false;
    unlock();
}

void account_summary(account_summary_t *out)
{
    lock();
    *out = s_sum;
    unlock();
}

int account_history(account_tx_t *out, int max)
{
    lock();
    int n = s_sum.n_history < max ? s_sum.n_history : max;
    memcpy(out, s_hist, sizeof(account_tx_t) * (size_t)n);
    unlock();
    return n;
}

/* ---------------------------------------------------------------- JSON */

static int64_t jnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}

/* ---------------------------------------------------------------- sync */

typedef struct {
    esplora_t   *api;
    used_addr_t *used;
    int          n_used, cap_used;
    btc_utxo_t  *utxos;
    int          n_utxos;
    int64_t      conf, unconf;
    account_tx_t hist[ACCOUNT_MAX_HISTORY];
    int          n_hist;
} sync_ctx_t;

static esp_err_t fetch_utxos(sync_ctx_t *ctx, uint32_t chain, uint32_t index, const char *addr)
{
    char path[160], *body = NULL;
    snprintf(path, sizeof(path), "/address/%s/utxo", addr);
    esp_err_t err = esplora_get(ctx->api, path, &body, NULL);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *arr = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *u;
    cJSON_ArrayForEach(u, arr) {
        if (ctx->n_utxos >= ACCOUNT_MAX_UTXOS) {
            ESP_LOGW(TAG, "too many UTXOs, ignoring the rest");
            break;
        }
        const cJSON *txid = cJSON_GetObjectItem(u, "txid");
        const cJSON *status = cJSON_GetObjectItem(u, "status");
        btc_utxo_t *x = &ctx->utxos[ctx->n_utxos];
        if (!cJSON_IsString(txid) || !btc_hex_to_bytes(txid->valuestring, x->txid, 32)) {
            continue;
        }
        x->vout = (uint32_t)jnum(u, "vout");
        x->value = jnum(u, "value");
        x->chain = chain;
        x->index = index;
        x->confirmed = cJSON_IsTrue(cJSON_GetObjectItem(status, "confirmed"));
        ctx->n_utxos++;
    }
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t scan_chain(sync_ctx_t *ctx, uint32_t chain, uint32_t *first_unused, account_progress_cb_t cb)
{
    uint32_t gap = 0;
    *first_unused = 0;
    bool seen_unused = false;
    for (uint32_t i = 0; i < MAX_SCAN_INDEX && gap < CONFIG_BTC_GAP_LIMIT; i++) {
        char addr[WALLET_ADDR_MAX], path[160], *body = NULL;
        esp_err_t err = wallet_address(chain, i, addr, sizeof(addr));
        if (err != ESP_OK) {
            return err;
        }
        if (cb) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Scanning %s #%u", chain ? "change" : "receive", (unsigned)i);
            cb(msg);
        }
        snprintf(path, sizeof(path), "/address/%s", addr);
        err = esplora_get(ctx->api, path, &body, NULL);
        if (err != ESP_OK) {
            return err;
        }
        cJSON *j = cJSON_Parse(body);
        free(body);
        if (!j) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const cJSON *cs = cJSON_GetObjectItem(j, "chain_stats");
        const cJSON *ms = cJSON_GetObjectItem(j, "mempool_stats");
        int64_t txc = jnum(cs, "tx_count") + jnum(ms, "tx_count");
        int64_t cbal = jnum(cs, "funded_txo_sum") - jnum(cs, "spent_txo_sum");
        int64_t mbal = jnum(ms, "funded_txo_sum") - jnum(ms, "spent_txo_sum");
        int64_t funded_unspent = jnum(cs, "funded_txo_count") + jnum(ms, "funded_txo_count") -
                                 jnum(cs, "spent_txo_count") - jnum(ms, "spent_txo_count");
        cJSON_Delete(j);

        if (txc > 0) {
            gap = 0;
            ctx->conf += cbal;
            ctx->unconf += mbal;
            if (ctx->n_used < ctx->cap_used) {
                used_addr_t *ua = &ctx->used[ctx->n_used++];
                ua->chain = chain;
                ua->index = i;
                strlcpy(ua->addr, addr, sizeof(ua->addr));
            }
            if (funded_unspent > 0) {
                err = fetch_utxos(ctx, chain, i, addr);
                if (err != ESP_OK) {
                    return err;
                }
            }
        } else {
            if (!seen_unused) {
                *first_unused = i;
                seen_unused = true;
            }
            gap++;
        }
    }
    return ESP_OK;
}

static bool is_ours(const sync_ctx_t *ctx, const char *addr)
{
    if (!addr) {
        return false;
    }
    for (int i = 0; i < ctx->n_used; i++) {
        if (strcmp(ctx->used[i].addr, addr) == 0) {
            return true;
        }
    }
    return false;
}

static void add_history_from_json(sync_ctx_t *ctx, cJSON *arr)
{
    const cJSON *tx;
    cJSON_ArrayForEach(tx, arr) {
        const cJSON *txid = cJSON_GetObjectItem(tx, "txid");
        if (!cJSON_IsString(txid)) {
            continue;
        }
        bool dup = false;
        for (int k = 0; k < ctx->n_hist; k++) {
            if (strcmp(ctx->hist[k].txid, txid->valuestring) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        int64_t net = 0;
        const cJSON *v;
        cJSON_ArrayForEach(v, cJSON_GetObjectItem(tx, "vin")) {
            const cJSON *po = cJSON_GetObjectItem(v, "prevout");
            const cJSON *a = cJSON_GetObjectItem(po, "scriptpubkey_address");
            if (cJSON_IsString(a) && is_ours(ctx, a->valuestring)) {
                net -= jnum(po, "value");
            }
        }
        cJSON_ArrayForEach(v, cJSON_GetObjectItem(tx, "vout")) {
            const cJSON *a = cJSON_GetObjectItem(v, "scriptpubkey_address");
            if (cJSON_IsString(a) && is_ours(ctx, a->valuestring)) {
                net += jnum(v, "value");
            }
        }
        const cJSON *st = cJSON_GetObjectItem(tx, "status");
        account_tx_t t = {0};
        strlcpy(t.txid, txid->valuestring, sizeof(t.txid));
        t.net = net;
        t.confirmed = cJSON_IsTrue(cJSON_GetObjectItem(st, "confirmed"));
        t.height = (uint32_t)jnum(st, "block_height");
        t.time = (uint32_t)jnum(st, "block_time");

        /* insert sorted: unconfirmed first, then newest height first */
        int pos = ctx->n_hist;
        for (int k = 0; k < ctx->n_hist; k++) {
            const account_tx_t *o = &ctx->hist[k];
            bool newer = (!t.confirmed && o->confirmed) ||
                         (t.confirmed == o->confirmed && t.height > o->height);
            if (newer) {
                pos = k;
                break;
            }
        }
        if (pos >= ACCOUNT_MAX_HISTORY) {
            continue;
        }
        int n = ctx->n_hist < ACCOUNT_MAX_HISTORY ? ctx->n_hist : ACCOUNT_MAX_HISTORY - 1;
        memmove(&ctx->hist[pos + 1], &ctx->hist[pos], sizeof(account_tx_t) * (size_t)(n - pos));
        ctx->hist[pos] = t;
        if (ctx->n_hist < ACCOUNT_MAX_HISTORY) {
            ctx->n_hist++;
        }
    }
}

static void fetch_history(sync_ctx_t *ctx, account_progress_cb_t cb)
{
    for (int i = 0; i < ctx->n_used; i++) {
        if (cb) {
            cb("Loading history");
        }
        char path[160], *body = NULL;
        snprintf(path, sizeof(path), "/address/%s/txs", ctx->used[i].addr);
        if (esplora_get(ctx->api, path, &body, NULL) != ESP_OK) {
            continue;
        }
        cJSON *arr = cJSON_Parse(body);
        free(body);
        if (cJSON_IsArray(arr)) {
            add_history_from_json(ctx, arr);
        }
        cJSON_Delete(arr);
    }
}

static void fetch_fees(esplora_t *api, account_summary_t *sum)
{
    char *body = NULL;
    if (esplora_get(api, "/fee-estimates", &body, NULL) == ESP_OK) {
        cJSON *j = cJSON_Parse(body);
        if (j) {
            const cJSON *f1 = cJSON_GetObjectItem(j, "2");
            const cJSON *f6 = cJSON_GetObjectItem(j, "6");
            const cJSON *f144 = cJSON_GetObjectItem(j, "144");
            sum->fee_fast = cJSON_IsNumber(f1) ? f1->valuedouble : 5;
            sum->fee_normal = cJSON_IsNumber(f6) ? f6->valuedouble : 3;
            sum->fee_slow = cJSON_IsNumber(f144) ? f144->valuedouble : 1;
            cJSON_Delete(j);
        }
        free(body);
    }
    if (sum->fee_fast < 1) sum->fee_fast = 1;
    if (sum->fee_normal < 1) sum->fee_normal = 1;
    if (sum->fee_slow < 1) sum->fee_slow = 1;

    body = NULL;
    if (esplora_get(api, "/blocks/tip/height", &body, NULL) == ESP_OK) {
        sum->tip_height = (uint32_t)strtoul(body, NULL, 10);
        free(body);
    }
}

esp_err_t account_sync(account_progress_cb_t cb)
{
    if (wallet_state() != WALLET_STATE_UNLOCKED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_utxos) {
        s_utxos = calloc(ACCOUNT_MAX_UTXOS, sizeof(btc_utxo_t));
        if (!s_utxos) {
            return ESP_ERR_NO_MEM;
        }
    }
    sync_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    ctx->cap_used = 400;
    ctx->used = calloc((size_t)ctx->cap_used, sizeof(used_addr_t));
    ctx->utxos = calloc(ACCOUNT_MAX_UTXOS, sizeof(btc_utxo_t));
    ctx->api = esplora_open();
    esp_err_t err = (ctx->used && ctx->utxos && ctx->api) ? ESP_OK : ESP_ERR_NO_MEM;

    account_summary_t sum = {0};
    if (err == ESP_OK) {
        err = scan_chain(ctx, WALLET_CHAIN_RECEIVE, &sum.next_receive, cb);
    }
    if (err == ESP_OK) {
        err = scan_chain(ctx, WALLET_CHAIN_CHANGE, &sum.next_change, cb);
    }
    if (err == ESP_OK) {
        if (cb) {
            cb("Fetching fee estimates");
        }
        fetch_fees(ctx->api, &sum);
        fetch_history(ctx, cb);
    }

    if (err == ESP_OK) {
        lock();
        state_load();
        sum.synced = true;
        sum.confirmed = ctx->conf;
        sum.unconfirmed = ctx->unconf;
        sum.n_utxos = ctx->n_utxos;
        sum.n_history = ctx->n_hist;
        sum.synced_at_s = esp_timer_get_time() / 1000000;
        s_sum = sum;
        memcpy(s_utxos, ctx->utxos, sizeof(btc_utxo_t) * ACCOUNT_MAX_UTXOS);
        memcpy(s_hist, ctx->hist, sizeof(s_hist));
        /* never show an address that has already received funds */
        if (s_rx_index < sum.next_receive) {
            s_rx_index = sum.next_receive;
            state_save();
        }
        unlock();
        ESP_LOGI(TAG, "sync ok: %lld conf, %lld unconf, %d utxos, next rx %u",
                 (long long)sum.confirmed, (long long)sum.unconfirmed, sum.n_utxos,
                 (unsigned)sum.next_receive);
    } else {
        ESP_LOGE(TAG, "sync failed: %s", esp_err_to_name(err));
    }

    esplora_close(ctx->api);
    free(ctx->used);
    free(ctx->utxos);
    free(ctx);
    return err;
}

esp_err_t account_plan_send(const char *dest, int64_t amount, double fee_rate, btc_tx_plan_t *plan)
{
    lock();
    if (!s_sum.synced || !s_utxos) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    int n = s_sum.n_utxos;
    btc_utxo_t *copy = malloc(sizeof(btc_utxo_t) * (size_t)(n ? n : 1));
    if (!copy) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, s_utxos, sizeof(btc_utxo_t) * (size_t)n);
    uint32_t change_idx = s_sum.next_change;
    unlock();

    esp_err_t err = btc_plan_tx(copy, n, dest, amount, fee_rate, change_idx, plan);
    free(copy);
    return err;
}

esp_err_t account_sign_and_broadcast(const btc_tx_plan_t *plan, char *txid_out, char *msg, size_t msg_len)
{
    char *hex = NULL;
    char txid[65];
    msg[0] = 0;
    esp_err_t err = btc_sign_tx(plan, &hex, txid);
    if (err != ESP_OK) {
        snprintf(msg, msg_len, "Signing failed (%s)", esp_err_to_name(err));
        return err;
    }
    esplora_t *api = esplora_open();
    if (!api) {
        free(hex);
        return ESP_ERR_NO_MEM;
    }
    char *body = NULL;
    int status = 0;
    err = esplora_post(api, "/tx", hex, &body, &status);
    free(hex);
    esplora_close(api);

    if (err != ESP_OK) {
        snprintf(msg, msg_len, "Network error (%s)", esp_err_to_name(err));
    } else if (status != 200) {
        snprintf(msg, msg_len, "Rejected (HTTP %d): %.200s", status, body ? body : "");
        err = ESP_FAIL;
    } else {
        strlcpy(txid_out, txid, 65);
        if (body && strlen(body) >= 64 && strncmp(body, txid, 64) != 0) {
            ESP_LOGW(TAG, "server txid %.64s differs from local %s", body, txid);
        }
        /* mark change as used so the next send uses a fresh change address */
        lock();
        if (plan->has_change && s_sum.next_change <= plan->change_index) {
            s_sum.next_change = plan->change_index + 1;
        }
        unlock();
    }
    free(body);
    return err;
}
