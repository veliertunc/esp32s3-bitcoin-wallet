#include "btc_tx.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "base58.h"
#include "ecdsa.h"
#include "memzero.h"
#include "ripemd160.h"
#include "secp256k1.h"
#include "segwit_addr.h"
#include "sha2.h"

#include "wallet.h"

static const char *TAG = "btc_tx";

#define TX_VERSION      2
#define TX_SEQUENCE     0xFFFFFFFDu     /* opt-in RBF */
#define SIGHASH_ALL     1

/* ------------------------------------------------------------ helpers */

bool btc_hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)hex[2 * i]) || !isxdigit((unsigned char)hex[2 * i + 1]) ||
            sscanf(hex + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

void btc_bytes_to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = hx[in[i] >> 4];
        out[2 * i + 1] = hx[in[i] & 0xF];
    }
    out[2 * len] = 0;
}

bool btc_parse_amount(const char *s, int64_t *sats)
{
    while (*s == ' ') {
        s++;
    }
    int64_t whole = 0, frac = 0;
    int frac_digits = 0;
    bool dot = false, any = false;
    for (; *s; s++) {
        if (*s == '.' || *s == ',') {
            if (dot) {
                return false;
            }
            dot = true;
        } else if (isdigit((unsigned char)*s)) {
            any = true;
            if (dot) {
                if (++frac_digits > 8) {
                    return false;
                }
                frac = frac * 10 + (*s - '0');
            } else {
                whole = whole * 10 + (*s - '0');
                if (whole > 21000000) {
                    return false;
                }
            }
        } else if (*s == ' ') {
            continue;
        } else {
            return false;
        }
    }
    if (!any) {
        return false;
    }
    while (frac_digits++ < 8) {
        frac *= 10;
    }
    *sats = whole * 100000000LL + frac;
    return *sats > 0;
}

void btc_format_amount(int64_t sats, char *out, size_t out_len)
{
    const char *sign = "";
    if (sats < 0) {
        sign = "-";
        sats = -sats;
    }
    snprintf(out, out_len, "%s%lld.%08lld", sign, (long long)(sats / 100000000LL),
             (long long)(sats % 100000000LL));
}

static void dsha256(const uint8_t *d, size_t n, uint8_t out[32])
{
    uint8_t h[32];
    sha256_Raw(d, n, h);
    sha256_Raw(h, 32, out);
}

/* ----------------------------------------------------------- addresses */

esp_err_t btc_address_to_script(const char *addr, uint8_t *script, size_t *script_len)
{
    char a[100];
    size_t n = strlen(addr);
    if (n < 14 || n >= sizeof(a)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* strip optional "bitcoin:" URI prefix and query string */
    if (strncasecmp(addr, "bitcoin:", 8) == 0) {
        addr += 8;
    }
    strlcpy(a, addr, sizeof(a));
    char *q = strchr(a, '?');
    if (q) {
        *q = 0;
    }
    /* trim whitespace */
    n = strlen(a);
    while (n && isspace((unsigned char)a[n - 1])) {
        a[--n] = 0;
    }

    const char *hrp = wallet_hrp();
    size_t hl = strlen(hrp);
    if (strncasecmp(a, hrp, hl) == 0 && a[hl] == '1') {
        for (char *p = a; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        int ver;
        uint8_t prog[40];
        size_t plen;
        if (!segwit_addr_decode(&ver, prog, &plen, hrp, a)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (ver == 0 && plen != 20 && plen != 32) {
            return ESP_ERR_INVALID_ARG;
        }
        if (ver == 1 && plen != 32) {
            return ESP_ERR_INVALID_ARG;   /* only taproot v1 programs are standard */
        }
        if (ver > 1) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        script[0] = ver == 0 ? 0x00 : (uint8_t)(0x50 + ver);
        script[1] = (uint8_t)plen;
        memcpy(script + 2, prog, plen);
        *script_len = plen + 2;
        return ESP_OK;
    }

    uint8_t d[21];
    if (base58_decode_check(a, HASHER_SHA2D, d, sizeof(d)) != 21) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t p2pkh = wallet_is_testnet() ? 0x6F : 0x00;
    uint8_t p2sh = wallet_is_testnet() ? 0xC4 : 0x05;
    if (d[0] == p2pkh) {
        script[0] = 0x76;
        script[1] = 0xA9;
        script[2] = 0x14;
        memcpy(script + 3, d + 1, 20);
        script[23] = 0x88;
        script[24] = 0xAC;
        *script_len = 25;
        return ESP_OK;
    }
    if (d[0] == p2sh) {
        script[0] = 0xA9;
        script[1] = 0x14;
        memcpy(script + 2, d + 1, 20);
        script[22] = 0x87;
        *script_len = 23;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;   /* e.g. a mainnet address while on testnet */
}

/* -------------------------------------------------------- size + fees */

/* Worst-case virtual size for n P2WPKH inputs and the given outputs. */
static uint32_t estimate_vsize(int n_in, const size_t *out_script_lens, int n_out)
{
    uint32_t base = 4 + 4 + 1 + 1;                 /* version, locktime, vin/vout counts */
    base += 41u * (uint32_t)n_in;                  /* outpoint + empty scriptSig + sequence */
    for (int i = 0; i < n_out; i++) {
        base += 8 + 1 + (uint32_t)out_script_lens[i];
    }
    uint32_t witness = 2 + 108u * (uint32_t)n_in;  /* marker/flag + (count, 73 sig, 34 pubkey) */
    uint32_t weight = base * 4 + witness;
    return (weight + 3) / 4;
}

static int64_t fee_for(uint32_t vsize, double rate)
{
    return (int64_t)ceil((double)vsize * rate);
}

static int cmp_utxo_desc(const void *a, const void *b)
{
    const btc_utxo_t *x = a, *y = b;
    if (x->confirmed != y->confirmed) {
        return x->confirmed ? -1 : 1;          /* prefer confirmed coins */
    }
    return (x->value < y->value) - (x->value > y->value);
}

esp_err_t btc_plan_tx(const btc_utxo_t *utxos, int n_utxos, const char *dest_addr,
                      int64_t amount, double fee_rate, uint32_t change_index, btc_tx_plan_t *plan)
{
    memset(plan, 0, sizeof(*plan));
    if (fee_rate < 1.0) {
        fee_rate = 1.0;
    }
    plan->fee_rate = fee_rate;
    strlcpy(plan->dest_addr, dest_addr, sizeof(plan->dest_addr));

    btc_output_t *dst = &plan->outputs[0];
    esp_err_t err = btc_address_to_script(dest_addr, dst->script, &dst->script_len);
    if (err != ESP_OK) {
        return err;
    }

    btc_output_t *chg = &plan->outputs[1];
    uint8_t pkh[20];
    err = wallet_pubkey_hash(WALLET_CHAIN_CHANGE, change_index, pkh);
    if (err != ESP_OK) {
        return err;
    }
    chg->script[0] = 0x00;
    chg->script[1] = 0x14;
    memcpy(chg->script + 2, pkh, 20);
    chg->script_len = 22;
    plan->change_index = change_index;

    if (n_utxos <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    btc_utxo_t *sorted = malloc(sizeof(btc_utxo_t) * (size_t)n_utxos);
    if (!sorted) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(sorted, utxos, sizeof(btc_utxo_t) * (size_t)n_utxos);
    qsort(sorted, (size_t)n_utxos, sizeof(btc_utxo_t), cmp_utxo_desc);

    size_t lens1[1] = { dst->script_len };
    size_t lens2[2] = { dst->script_len, chg->script_len };
    int64_t sum = 0;
    err = ESP_ERR_INVALID_SIZE;   /* insufficient funds unless we succeed below */

    if (amount < 0) {
        /* send max: every coin, no change */
        int n = n_utxos > BTC_MAX_INPUTS ? BTC_MAX_INPUTS : n_utxos;
        for (int i = 0; i < n; i++) {
            plan->inputs[i] = sorted[i];
            sum += sorted[i].value;
        }
        plan->n_inputs = n;
        plan->vsize = estimate_vsize(n, lens1, 1);
        plan->fee = fee_for(plan->vsize, fee_rate);
        plan->amount = sum - plan->fee;
        if (plan->amount >= BTC_DUST_LIMIT) {
            dst->value = plan->amount;
            plan->n_outputs = 1;
            err = ESP_OK;
        }
    } else if (amount >= BTC_DUST_LIMIT) {
        plan->amount = amount;
        dst->value = amount;
        for (int i = 0; i < n_utxos && i < BTC_MAX_INPUTS; i++) {
            plan->inputs[i] = sorted[i];
            plan->n_inputs = i + 1;
            sum += sorted[i].value;

            uint32_t vs2 = estimate_vsize(plan->n_inputs, lens2, 2);
            int64_t fee2 = fee_for(vs2, fee_rate);
            if (sum >= amount + fee2 && sum - amount - fee2 >= BTC_DUST_LIMIT) {
                plan->has_change = true;
                chg->value = sum - amount - fee2;
                plan->fee = fee2;
                plan->vsize = vs2;
                plan->n_outputs = 2;
                err = ESP_OK;
                break;
            }
            uint32_t vs1 = estimate_vsize(plan->n_inputs, lens1, 1);
            int64_t fee1 = fee_for(vs1, fee_rate);
            if (sum >= amount + fee1) {
                /* leftover below dust goes to the miner */
                plan->fee = sum - amount;
                plan->vsize = vs1;
                plan->n_outputs = 1;
                err = ESP_OK;
                break;
            }
        }
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    free(sorted);

    if (err == ESP_OK) {
        plan->fee_rate = (double)plan->fee / plan->vsize;
        /* sanity: refuse absurd fees (more than 10% of amount and > 0.001 BTC) */
        if (plan->fee > 100000 && plan->fee * 10 > plan->amount) {
            ESP_LOGW(TAG, "fee %lld looks absurd, refusing", (long long)plan->fee);
            return ESP_ERR_INVALID_STATE;
        }
    }
    return err;
}

/* ------------------------------------------------------- serialisation */

typedef struct {
    uint8_t *p;
    size_t len, cap;
} buf_t;

static void put(buf_t *b, const void *d, size_t n)
{
    if (b->len + n <= b->cap) {
        memcpy(b->p + b->len, d, n);
    }
    b->len += n;
}

static void put_u32(buf_t *b, uint32_t v)
{
    uint8_t d[4] = { v, v >> 8, v >> 16, v >> 24 };
    put(b, d, 4);
}

static void put_u64(buf_t *b, uint64_t v)
{
    uint8_t d[8];
    for (int i = 0; i < 8; i++) {
        d[i] = (uint8_t)(v >> (8 * i));
    }
    put(b, d, 8);
}

static void put_varint(buf_t *b, uint64_t v)
{
    uint8_t c;
    if (v < 0xFD) {
        c = (uint8_t)v;
        put(b, &c, 1);
    } else if (v <= 0xFFFF) {
        uint8_t d[3] = { 0xFD, v, v >> 8 };
        put(b, d, 3);
    } else {
        c = 0xFE;
        put(b, &c, 1);
        put_u32(b, (uint32_t)v);
    }
}

static void put_outpoint(buf_t *b, const btc_utxo_t *u)
{
    uint8_t le[32];
    for (int i = 0; i < 32; i++) {
        le[i] = u->txid[31 - i];   /* display order -> internal byte order */
    }
    put(b, le, 32);
    put_u32(b, u->vout);
}

static void put_output(buf_t *b, const btc_output_t *o)
{
    put_u64(b, (uint64_t)o->value);
    put_varint(b, o->script_len);
    put(b, o->script, o->script_len);
}

typedef struct {
    uint8_t der[73];   /* DER + sighash byte */
    size_t  der_len;
    uint8_t pub[33];
} witness_t;

static void serialise(buf_t *b, const btc_tx_plan_t *plan, const witness_t *wit)
{
    put_u32(b, TX_VERSION);
    if (wit) {
        uint8_t mf[2] = { 0x00, 0x01 };
        put(b, mf, 2);
    }
    put_varint(b, (uint64_t)plan->n_inputs);
    for (int i = 0; i < plan->n_inputs; i++) {
        put_outpoint(b, &plan->inputs[i]);
        put_varint(b, 0);                  /* empty scriptSig */
        put_u32(b, TX_SEQUENCE);
    }
    put_varint(b, (uint64_t)plan->n_outputs);
    for (int i = 0; i < plan->n_outputs; i++) {
        put_output(b, &plan->outputs[i]);
    }
    if (wit) {
        for (int i = 0; i < plan->n_inputs; i++) {
            put_varint(b, 2);
            put_varint(b, wit[i].der_len);
            put(b, wit[i].der, wit[i].der_len);
            put_varint(b, 33);
            put(b, wit[i].pub, 33);
        }
    }
    put_u32(b, 0);                         /* locktime */
}

esp_err_t btc_sign_tx(const btc_tx_plan_t *plan, char **hex_out, char txid_hex[65])
{
    *hex_out = NULL;
    if (plan->n_inputs <= 0 || plan->n_outputs <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* BIP143 shared hashes */
    uint8_t hash_prevouts[32], hash_sequence[32], hash_outputs[32];
    {
        size_t cap = 36 * (size_t)plan->n_inputs + 4 * (size_t)plan->n_inputs + 128;
        buf_t b = { malloc(cap), 0, cap };
        if (!b.p) {
            return ESP_ERR_NO_MEM;
        }
        for (int i = 0; i < plan->n_inputs; i++) {
            put_outpoint(&b, &plan->inputs[i]);
        }
        dsha256(b.p, b.len, hash_prevouts);
        b.len = 0;
        for (int i = 0; i < plan->n_inputs; i++) {
            put_u32(&b, TX_SEQUENCE);
        }
        dsha256(b.p, b.len, hash_sequence);
        b.len = 0;
        for (int i = 0; i < plan->n_outputs; i++) {
            put_output(&b, &plan->outputs[i]);
        }
        dsha256(b.p, b.len, hash_outputs);
        free(b.p);
    }

    witness_t *wit = calloc((size_t)plan->n_inputs, sizeof(witness_t));
    if (!wit) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    for (int i = 0; i < plan->n_inputs && err == ESP_OK; i++) {
        const btc_utxo_t *u = &plan->inputs[i];
        uint8_t priv[32], pub[33], pkh[20], digest[32], sig[64];
        err = wallet_keypair(u->chain, u->index, priv, pub);
        if (err != ESP_OK) {
            break;
        }
        uint8_t h[32];
        sha256_Raw(pub, 33, h);
        ripemd160(h, 32, pkh);

        uint8_t pre[4 + 32 + 32 + 36 + 26 + 8 + 4 + 32 + 4 + 4];
        buf_t b = { pre, 0, sizeof(pre) };
        put_u32(&b, TX_VERSION);
        put(&b, hash_prevouts, 32);
        put(&b, hash_sequence, 32);
        put_outpoint(&b, u);
        uint8_t script_code[26] = { 0x19, 0x76, 0xA9, 0x14 };
        memcpy(script_code + 4, pkh, 20);
        script_code[24] = 0x88;
        script_code[25] = 0xAC;
        put(&b, script_code, 26);
        put_u64(&b, (uint64_t)u->value);
        put_u32(&b, TX_SEQUENCE);
        put(&b, hash_outputs, 32);
        put_u32(&b, 0);                    /* locktime */
        put_u32(&b, SIGHASH_ALL);
        dsha256(pre, b.len, digest);

        if (ecdsa_sign_digest(&secp256k1, priv, digest, sig, NULL, NULL) != 0 ||
            ecdsa_verify_digest(&secp256k1, pub, sig, digest) != 0) {
            err = ESP_FAIL;
        } else {
            wit[i].der_len = (size_t)ecdsa_sig_to_der(sig, wit[i].der);
            wit[i].der[wit[i].der_len++] = SIGHASH_ALL;
            memcpy(wit[i].pub, pub, 33);
        }
        memzero(priv, sizeof(priv));
        memzero(sig, sizeof(sig));
    }

    if (err == ESP_OK) {
        size_t cap = 64 + (size_t)plan->n_inputs * 160 + (size_t)plan->n_outputs * 64;
        buf_t b = { malloc(cap), 0, cap };
        if (!b.p) {
            err = ESP_ERR_NO_MEM;
        } else {
            /* txid = dSHA256(legacy serialisation), displayed reversed */
            serialise(&b, plan, NULL);
            uint8_t id[32], rid[32];
            dsha256(b.p, b.len, id);
            for (int i = 0; i < 32; i++) {
                rid[i] = id[31 - i];
            }
            btc_bytes_to_hex(rid, 32, txid_hex);

            b.len = 0;
            serialise(&b, plan, wit);
            if (b.len > cap) {
                err = ESP_ERR_INVALID_SIZE;
            } else {
                char *hex = malloc(b.len * 2 + 1);
                if (!hex) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    btc_bytes_to_hex(b.p, b.len, hex);
                    *hex_out = hex;
                    ESP_LOGI(TAG, "signed tx %s (%u bytes)", txid_hex, (unsigned)b.len);
                }
            }
            free(b.p);
        }
    }
    memzero(wit, sizeof(witness_t) * (size_t)plan->n_inputs);
    free(wit);
    return err;
}
