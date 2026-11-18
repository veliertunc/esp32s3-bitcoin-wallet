/*
 * Bitcoin transaction construction for a BIP84 (P2WPKH) wallet.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define BTC_DUST_LIMIT      546          /* conservative dust threshold (sats) */
#define BTC_MAX_INPUTS      60
#define BTC_MAX_SCRIPT      42

typedef struct {
    uint8_t  txid[32];        /* display order (as returned by the explorer) */
    uint32_t vout;
    int64_t  value;
    uint32_t chain;
    uint32_t index;
    bool     confirmed;
} btc_utxo_t;

typedef struct {
    uint8_t script[BTC_MAX_SCRIPT];
    size_t  script_len;
    int64_t value;
} btc_output_t;

typedef struct {
    /* inputs chosen by coin selection */
    btc_utxo_t inputs[BTC_MAX_INPUTS];
    int        n_inputs;
    /* outputs[0] = destination, outputs[1] = change (if has_change) */
    btc_output_t outputs[2];
    int        n_outputs;
    bool       has_change;
    uint32_t   change_index;
    int64_t    amount;
    int64_t    fee;
    uint32_t   vsize;
    double     fee_rate;
    char       dest_addr[96];
} btc_tx_plan_t;

/* Parse a mainnet/testnet address (bech32/bech32m/base58) into a scriptPubKey. */
esp_err_t btc_address_to_script(const char *addr, uint8_t *script, size_t *script_len);

/* Parse "0.001", "1", "0.00012345" BTC -> sats. Returns false on bad input. */
bool btc_parse_amount(const char *s, int64_t *sats);
/* Format sats as BTC string "0.00012345". */
void btc_format_amount(int64_t sats, char *out, size_t out_len);

/* Hex helpers */
bool btc_hex_to_bytes(const char *hex, uint8_t *out, size_t out_len);
void btc_bytes_to_hex(const uint8_t *in, size_t len, char *out);

/*
 * Coin selection. amount == -1 means "send everything".
 * change_index: receive index for the change output on chain 1.
 */
esp_err_t btc_plan_tx(const btc_utxo_t *utxos, int n_utxos, const char *dest_addr,
                      int64_t amount, double fee_rate, uint32_t change_index, btc_tx_plan_t *plan);

/* Sign every input with the wallet keys and serialise to a hex string
 * (malloc'd, caller frees). Also returns txid hex (64 chars + NUL). */
esp_err_t btc_sign_tx(const btc_tx_plan_t *plan, char **hex_out, char txid_hex[65]);
