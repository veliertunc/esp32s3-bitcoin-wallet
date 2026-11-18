/*
 * Online view of the unlocked wallet: balances, UTXOs, history, fee estimates.
 * Populated by account_sync() which scans BIP84 receive/change chains via Esplora.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "btc_tx.h"

#define ACCOUNT_MAX_UTXOS   120
#define ACCOUNT_MAX_HISTORY 40

typedef struct {
    char     txid[65];
    int64_t  net;          /* + received, - sent (incl. fee) */
    bool     confirmed;
    uint32_t height;
    uint32_t time;
} account_tx_t;

typedef struct {
    bool     synced;
    int64_t  confirmed;
    int64_t  unconfirmed;
    uint32_t next_receive;     /* first unused receive index */
    uint32_t next_change;      /* first unused change index */
    uint32_t tip_height;
    double   fee_fast, fee_normal, fee_slow;   /* sat/vB */
    int      n_utxos;
    int      n_history;
    int64_t  synced_at_s;      /* uptime seconds of last successful sync */
} account_summary_t;

typedef void (*account_progress_cb_t)(const char *msg);

void      account_reset(void);               /* call on lock */
esp_err_t account_sync(account_progress_cb_t cb);
void      account_summary(account_summary_t *out);
int       account_history(account_tx_t *out, int max);

/* Address currently shown on the Receive screen (persisted per wallet in SPIFFS). */
uint32_t  account_receive_index(void);
void      account_set_receive_index(uint32_t idx);

/* Build a transaction plan from the current UTXO set. amount=-1 sends everything. */
esp_err_t account_plan_send(const char *dest, int64_t amount, double fee_rate, btc_tx_plan_t *plan);

/* Sign + broadcast. txid_out >= 65 bytes, msg receives server error text on failure. */
esp_err_t account_sign_and_broadcast(const btc_tx_plan_t *plan, char *txid_out, char *msg, size_t msg_len);
