#include "wallet.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "bootloader_random.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "bip39.h"
#include "bignum.h"
#include "base58.h"
#include "ecdsa.h"
#include "hmac.h"
#include "memzero.h"
#include "pbkdf2.h"
#include "ripemd160.h"
#include "rfc7539.h"
#include "secp256k1.h"
#include "segwit_addr.h"
#include "sha2.h"

#include "board/board.h"
#include "net/wifi_mgr.h"

static const char *TAG = "wallet";

/* ------------------------------------------------------------ file format */

#define FILE_MAGIC      "BTCWLT01"
#define FILE_VERSION    1
#define FLAG_BOUND      0x01
#define PT_LEN          512
#define MAX_PASSPHRASE  256

typedef struct __attribute__((packed)) {
    char     magic[8];
    uint8_t  version;
    uint8_t  flags;
    uint8_t  network;     /* 0 = mainnet, 1 = testnet */
    uint8_t  reserved;
    uint32_t iterations;
    uint8_t  salt[16];
    uint8_t  nonce[12];
} file_hdr_t;

/* ------------------------------------------------------------ HD node */

typedef struct {
    uint8_t  key[32];
    uint8_t  chain[32];
    uint8_t  pub[33];
    uint8_t  depth;
    uint32_t child;
    uint8_t  parent_fp[4];
} hdnode_t;

static SemaphoreHandle_t s_mtx;
static wallet_state_t s_state = WALLET_STATE_NO_SD;
static hdnode_t s_account;          /* m/84'/coin'/0'           */
static hdnode_t s_chain[2];         /* m/84'/coin'/0'/{0,1}     */
static uint8_t  s_master_fp[4];
static char     s_fp_hex[9];
static uint8_t  s_entropy_pool[32];
static int64_t  s_last_fail_us;

#define LOCK()   xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_mtx)

static void ensure_mutex(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateRecursiveMutex();
    }
}

bool wallet_is_testnet(void)
{
#if CONFIG_BTC_NETWORK_MAINNET
    return false;
#else
    return true;
#endif
}

const char *wallet_hrp(void)          { return wallet_is_testnet() ? "tb" : "bc"; }
const char *wallet_network_name(void) { return wallet_is_testnet() ? "Testnet4" : "Mainnet"; }
static uint32_t coin_type(void)       { return wallet_is_testnet() ? 1 : 0; }

static void hash160(const uint8_t *in, size_t len, uint8_t out[20])
{
    uint8_t h[32];
    sha256_Raw(in, len, h);
    ripemd160(h, 32, out);
    memzero(h, sizeof(h));
}

/* --------------------------------------------------------------- BIP32 */

static bool node_from_seed(const uint8_t seed[64], hdnode_t *n)
{
    uint8_t I[64];
    hmac_sha512((const uint8_t *)"Bitcoin seed", 12, seed, 64, I);
    bignum256 k;
    bn_read_be(I, &k);
    bool ok = !bn_is_zero(&k) && bn_is_less(&k, &secp256k1.order);
    memset(n, 0, sizeof(*n));
    memcpy(n->key, I, 32);
    memcpy(n->chain, I + 32, 32);
    ecdsa_get_public_key33(&secp256k1, n->key, n->pub);
    memzero(I, sizeof(I));
    memzero(&k, sizeof(k));
    return ok;
}

static bool node_ckd_priv(const hdnode_t *p, uint32_t i, hdnode_t *c, bool want_pub)
{
    uint8_t data[37];
    uint8_t I[64];
    if (i & 0x80000000u) {
        data[0] = 0;
        memcpy(data + 1, p->key, 32);
    } else {
        memcpy(data, p->pub, 33);
    }
    data[33] = i >> 24;
    data[34] = i >> 16;
    data[35] = i >> 8;
    data[36] = i;
    hmac_sha512(p->chain, 32, data, sizeof(data), I);

    bignum256 a, b;
    bn_read_be(I, &a);
    bool ok = bn_is_less(&a, &secp256k1.order);
    bn_read_be(p->key, &b);
    bn_add(&a, &b);
    bn_mod(&a, &secp256k1.order);
    ok = ok && !bn_is_zero(&a);

    hdnode_t tmp = {0};
    bn_write_be(&a, tmp.key);
    memcpy(tmp.chain, I + 32, 32);
    tmp.depth = p->depth + 1;
    tmp.child = i;
    uint8_t h[20];
    hash160(p->pub, 33, h);
    memcpy(tmp.parent_fp, h, 4);
    if (want_pub) {
        ecdsa_get_public_key33(&secp256k1, tmp.key, tmp.pub);
    }
    *c = tmp;

    memzero(&tmp, sizeof(tmp));
    memzero(data, sizeof(data));
    memzero(I, sizeof(I));
    memzero(&a, sizeof(a));
    memzero(&b, sizeof(b));
    return ok;
}

/* m/84'/coin'/0' plus its /0 and /1 chain nodes */
static bool derive_account_ex(const uint8_t seed[64], uint32_t coin, hdnode_t *acct, hdnode_t chains[2],
                              uint8_t master_fp[4])
{
    hdnode_t m, n1, n2;
    bool ok = node_from_seed(seed, &m);
    uint8_t h[20];
    hash160(m.pub, 33, h);
    memcpy(master_fp, h, 4);

    ok = ok && node_ckd_priv(&m, 0x80000000u | 84, &n1, true);
    ok = ok && node_ckd_priv(&n1, 0x80000000u | coin, &n2, true);
    ok = ok && node_ckd_priv(&n2, 0x80000000u | 0, acct, true);
    ok = ok && node_ckd_priv(acct, 0, &chains[0], true);
    ok = ok && node_ckd_priv(acct, 1, &chains[1], true);
    memzero(&m, sizeof(m));
    memzero(&n1, sizeof(n1));
    memzero(&n2, sizeof(n2));
    return ok;
}

static bool derive_account(const uint8_t seed[64])
{
    bool ok = derive_account_ex(seed, coin_type(), &s_account, s_chain, s_master_fp);
    snprintf(s_fp_hex, sizeof(s_fp_hex), "%02x%02x%02x%02x", s_master_fp[0], s_master_fp[1], s_master_fp[2],
             s_master_fp[3]);
    return ok;
}

static void p2wpkh_address(const uint8_t pub[33], const char *hrp, char *out)
{
    uint8_t pkh[20];
    hash160(pub, 33, pkh);
    segwit_addr_encode(out, hrp, 0, pkh, 20);
}

/*
 * Known-answer test with the official BIP84 vectors. Runs at boot; if it fails
 * the wallet refuses to create/unlock so funds are never sent to wrong addresses.
 */
static int s_selftest = -1;

bool wallet_selftest(void)
{
    if (s_selftest >= 0) {
        return s_selftest == 1;
    }
    static const char *mn = "abandon abandon abandon abandon abandon abandon abandon abandon "
                            "abandon abandon abandon about";
    static const char *want[3] = {
        "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu",   /* m/84'/0'/0'/0/0 */
        "bc1qnjg0jd8228aq7egyzacy8cys3knf9xvrerkf9g",   /* m/84'/0'/0'/0/1 */
        "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el",   /* m/84'/0'/0'/1/0 */
    };
    static const char *want_zpub =
        "zpub6rFR7y4Q2AijBEqTUquhVz398htDFrtymD9xYYfG1m4wAcvPhXNfE3EfH1r1ADqtfSdVCToUG868RvUUkgDKf31mGDtKsAYz2oz2AGutZYs";

    uint8_t seed[64], fp[4];
    hdnode_t *acct = calloc(4, sizeof(hdnode_t));
    if (!acct) {
        return false;
    }
    hdnode_t *chains = acct + 1, *child = acct + 3;
    bool ok = mnemonic_check(mn);
    mnemonic_to_seed(mn, "", seed, NULL);
    ok = ok && derive_account_ex(seed, 0, acct, chains, fp);

    char addr[WALLET_ADDR_MAX];
    const uint32_t path[3][2] = { {0, 0}, {0, 1}, {1, 0} };
    for (int i = 0; i < 3 && ok; i++) {
        ok = node_ckd_priv(&chains[path[i][0]], path[i][1], child, true);
        p2wpkh_address(child->pub, "bc", addr);
        if (strcmp(addr, want[i]) != 0) {
            ESP_LOGE(TAG, "selftest: address %d = %s, expected %s", i, addr, want[i]);
            ok = false;
        }
    }
    if (ok) {
        /* xpub serialisation */
        uint8_t d[78] = { 0x04, 0xB2, 0x47, 0x46, acct->depth };
        memcpy(d + 5, acct->parent_fp, 4);
        d[9] = acct->child >> 24;
        d[10] = acct->child >> 16;
        d[11] = acct->child >> 8;
        d[12] = acct->child;
        memcpy(d + 13, acct->chain, 32);
        memcpy(d + 45, acct->pub, 33);
        char x[120];
        base58_encode_check(d, sizeof(d), HASHER_SHA2D, x, sizeof(x));
        if (strcmp(x, want_zpub) != 0) {
            ESP_LOGE(TAG, "selftest: zpub mismatch %s", x);
            ok = false;
        }
    }
    if (ok) {
        /* sign / verify round trip with the derived key */
        uint8_t digest[32], sig[64];
        sha256_Raw((const uint8_t *)"selftest", 8, digest);
        ok = ecdsa_sign_digest(&secp256k1, child->key, digest, sig, NULL, NULL) == 0 &&
             ecdsa_verify_digest(&secp256k1, child->pub, sig, digest) == 0;
        sig[5] ^= 1;
        ok = ok && ecdsa_verify_digest(&secp256k1, child->pub, sig, digest) != 0;
    }
    memzero(seed, sizeof(seed));
    memzero(acct, 4 * sizeof(hdnode_t));
    free(acct);
    s_selftest = ok ? 1 : 0;
    ESP_LOGI(TAG, "BIP39/BIP32/BIP84 self-test %s", ok ? "PASSED" : "FAILED");
    return ok;
}

/* ------------------------------------------------------------- state */

wallet_state_t wallet_state(void)
{
    return s_state;
}

wallet_state_t wallet_refresh_state(void)
{
    ensure_mutex();
    LOCK();
    if (s_state != WALLET_STATE_UNLOCKED) {
        struct stat st;
        if (!board_sd_mounted()) {
            s_state = WALLET_STATE_NO_SD;
        } else if (stat(WALLET_FILE, &st) == 0 && st.st_size > (off_t)sizeof(file_hdr_t)) {
            s_state = WALLET_STATE_LOCKED;
        } else {
            s_state = WALLET_STATE_EMPTY;
        }
    }
    wallet_state_t s = s_state;
    UNLOCK();
    return s;
}

/* ------------------------------------------------------------- entropy */

void wallet_entropy_add(const void *data, size_t len)
{
    SHA256_CTX c;
    sha256_Init(&c);
    sha256_Update(&c, s_entropy_pool, sizeof(s_entropy_pool));
    sha256_Update(&c, data, len);
    int64_t t = esp_timer_get_time();
    sha256_Update(&c, (const uint8_t *)&t, sizeof(t));
    sha256_Final(&c, s_entropy_pool);
}

static void hw_entropy(uint8_t *out, size_t len)
{
    /* esp_fill_random() is only a TRNG while RF or the ADC noise source is on. */
    if (wifi_mgr_radio_on()) {
        esp_fill_random(out, len);
    } else {
        board_adc_lock();
        bootloader_random_enable();
        esp_fill_random(out, len);
        bootloader_random_disable();
        board_adc_unlock();
    }
}

esp_err_t wallet_generate_mnemonic(int words, char *out, size_t out_len)
{
    if ((words != 12 && words != 24) || out_len < 256) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hw[64], ent[32];
    hw_entropy(hw, sizeof(hw));
    SHA256_CTX c;
    sha256_Init(&c);
    sha256_Update(&c, hw, sizeof(hw));
    sha256_Update(&c, s_entropy_pool, sizeof(s_entropy_pool));
    sha256_Final(&c, ent);

    const char *m = mnemonic_from_data(ent, words == 24 ? 32 : 16);
    esp_err_t err = ESP_FAIL;
    if (m) {
        strlcpy(out, m, out_len);
        err = ESP_OK;
    }
    mnemonic_clear();
    memzero(hw, sizeof(hw));
    memzero(ent, sizeof(ent));
    memzero(&c, sizeof(c));
    /* never reuse pool state */
    wallet_entropy_add("used", 4);
    return err;
}

void wallet_mnemonic_normalise(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    bool space = false;
    for (const char *p = in; *p && o + 1 < out_len; p++) {
        char ch = (char)tolower((unsigned char)*p);
        if (ch >= 'a' && ch <= 'z') {
            if (space && o > 0) {
                out[o++] = ' ';
                if (o + 1 >= out_len) {
                    break;
                }
            }
            out[o++] = ch;
            space = false;
        } else {
            space = true;
        }
    }
    out[o] = 0;
}

bool wallet_mnemonic_valid(const char *mnemonic)
{
    return mnemonic_check(mnemonic) != 0;
}

/* ------------------------------------------------------- device binding */

static esp_err_t get_pepper(uint8_t pepper[32], bool create)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wallet", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = 32;
    err = nvs_get_blob(h, "pepper", pepper, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND && create) {
        hw_entropy(pepper, 32);
        err = nvs_set_blob(h, "pepper", pepper, 32);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    } else if (err == ESP_OK && len != 32) {
        err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(h);
    return err;
}

static esp_err_t derive_file_key(const char *password, const file_hdr_t *hdr, uint8_t key[32], bool create)
{
    uint8_t in[32];
    const uint8_t *pw = (const uint8_t *)password;
    size_t pwlen = strlen(password);

    if (hdr->flags & FLAG_BOUND) {
        uint8_t pepper[32];
        esp_err_t err = get_pepper(pepper, create);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "device secret unavailable (%s) - wallet is bound to another board?",
                     esp_err_to_name(err));
            return ESP_ERR_INVALID_STATE;
        }
        hmac_sha256(pepper, 32, pw, pwlen, in);
        memzero(pepper, sizeof(pepper));
        pw = in;
        pwlen = 32;
    }
    uint8_t out[64];
    pbkdf2_hmac_sha512(pw, pwlen, hdr->salt, sizeof(hdr->salt), hdr->iterations, out, sizeof(out));
    memcpy(key, out, 32);
    memzero(out, sizeof(out));
    memzero(in, sizeof(in));
    return ESP_OK;
}

static void aead(bool enc, const uint8_t key[32], const file_hdr_t *hdr,
                 const uint8_t *in, uint8_t *out, size_t len, uint8_t tag[16])
{
    chacha20poly1305_ctx ctx;
    rfc7539_init(&ctx, key, hdr->nonce);
    rfc7539_auth(&ctx, (const uint8_t *)hdr, sizeof(*hdr));
    if (enc) {
        chacha20poly1305_encrypt(&ctx, in, out, len);
    } else {
        chacha20poly1305_decrypt(&ctx, in, out, len);
    }
    rfc7539_finish(&ctx, sizeof(*hdr), len, tag);
    memzero(&ctx, sizeof(ctx));
}

static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

/* Reads + decrypts the wallet file into pt[PT_LEN]. */
static esp_err_t read_and_decrypt(const char *password, uint8_t *pt)
{
    FILE *f = fopen(WALLET_FILE, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    file_hdr_t hdr;
    uint8_t ct[PT_LEN], tag[16], calc[16], key[32];
    size_t r = fread(&hdr, 1, sizeof(hdr), f);
    r += fread(ct, 1, sizeof(ct), f);
    r += fread(tag, 1, sizeof(tag), f);
    fclose(f);
    if (r != sizeof(hdr) + sizeof(ct) + sizeof(tag) || memcmp(hdr.magic, FILE_MAGIC, 8) != 0 ||
        hdr.version != FILE_VERSION || hdr.iterations < 1000) {
        ESP_LOGE(TAG, "wallet file corrupt or unsupported");
        return ESP_ERR_INVALID_VERSION;
    }
    esp_err_t err = derive_file_key(password, &hdr, key, false);
    if (err != ESP_OK) {
        return err;
    }
    aead(false, key, &hdr, ct, pt, PT_LEN, calc);
    memzero(key, sizeof(key));
    if (!ct_equal(calc, tag, 16)) {
        memzero(pt, PT_LEN);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static bool parse_plaintext(const uint8_t *pt, char *mn, size_t mn_len, char *pp, size_t pp_len)
{
    size_t a = pt[0] | (pt[1] << 8);
    if (a >= mn_len || a + 4 > PT_LEN) {
        return false;
    }
    memcpy(mn, pt + 2, a);
    mn[a] = 0;
    size_t b = pt[2 + a] | (pt[3 + a] << 8);
    if (b >= pp_len || 4 + a + b > PT_LEN) {
        return false;
    }
    memcpy(pp, pt + 4 + a, b);
    pp[b] = 0;
    return true;
}

/* ------------------------------------------------------------- create */

esp_err_t wallet_create(const char *mnemonic, const char *bip39_passphrase, const char *password)
{
    ensure_mutex();
    if (!board_sd_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wallet_selftest()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wallet_mnemonic_valid(mnemonic) || strlen(password) < 6) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bip39_passphrase) {
        bip39_passphrase = "";
    }
    size_t a = strlen(mnemonic), b = strlen(bip39_passphrase);
    if (b > MAX_PASSPHRASE || 4 + a + b > PT_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t pt[PT_LEN], ct[PT_LEN], tag[16], key[32];
    hw_entropy(pt, sizeof(pt));                      /* random padding */
    pt[0] = a & 0xFF;
    pt[1] = a >> 8;
    memcpy(pt + 2, mnemonic, a);
    pt[2 + a] = b & 0xFF;
    pt[3 + a] = b >> 8;
    memcpy(pt + 4 + a, bip39_passphrase, b);

    file_hdr_t hdr = {0};
    memcpy(hdr.magic, FILE_MAGIC, 8);
    hdr.version = FILE_VERSION;
#if CONFIG_WALLET_BIND_TO_DEVICE
    hdr.flags = FLAG_BOUND;
#endif
    hdr.network = wallet_is_testnet() ? 1 : 0;
    hdr.iterations = CONFIG_WALLET_KDF_ITERATIONS;
    hw_entropy(hdr.salt, sizeof(hdr.salt));
    hw_entropy(hdr.nonce, sizeof(hdr.nonce));

    esp_err_t err = derive_file_key(password, &hdr, key, true);
    if (err != ESP_OK) {
        memzero(pt, sizeof(pt));
        return err;
    }
    aead(true, key, &hdr, pt, ct, PT_LEN, tag);
    memzero(key, sizeof(key));
    memzero(pt, sizeof(pt));

    mkdir(WALLET_DIR, 0775);
    const char *tmp = WALLET_DIR "/wallet.tmp";
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t w = fwrite(&hdr, 1, sizeof(hdr), f);
    w += fwrite(ct, 1, sizeof(ct), f);
    w += fwrite(tag, 1, sizeof(tag), f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (w != sizeof(hdr) + sizeof(ct) + sizeof(tag)) {
        unlink(tmp);
        return ESP_FAIL;
    }
    unlink(WALLET_FILE);
    if (rename(tmp, WALLET_FILE) != 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wallet written to %s", WALLET_FILE);

    /* Derive keys directly so the user lands in the unlocked wallet. */
    uint8_t seed[64];
    mnemonic_to_seed(mnemonic, bip39_passphrase, seed, NULL);
    LOCK();
    bool ok = derive_account(seed);
    memzero(seed, sizeof(seed));
    s_state = ok ? WALLET_STATE_UNLOCKED : WALLET_STATE_LOCKED;
    UNLOCK();
    return ok ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------- unlock */

static uint32_t nvs_get_fails(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open("wallet", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "fails", &v);
        nvs_close(h);
    }
    return v;
}

static void nvs_set_fails(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open("wallet", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "fails", v);
        nvs_commit(h);
        nvs_close(h);
    }
}

int wallet_unlock_wait_seconds(void)
{
    uint32_t fails = nvs_get_fails();
    if (fails < CONFIG_WALLET_MAX_PIN_ATTEMPTS) {
        return 0;
    }
    uint32_t over = fails - CONFIG_WALLET_MAX_PIN_ATTEMPTS;
    int64_t delay = 5LL << (over > 7 ? 7 : over);     /* 5 s .. 640 s */
    int64_t since = (esp_timer_get_time() - s_last_fail_us) / 1000000;
    if (s_last_fail_us == 0) {
        since = esp_timer_get_time() / 1000000;       /* after reboot: count from boot */
    }
    return since >= delay ? 0 : (int)(delay - since);
}

esp_err_t wallet_unlock(const char *password)
{
    ensure_mutex();
    if (!wallet_selftest()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (wallet_unlock_wait_seconds() > 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t pt[PT_LEN];
    char mn[BIP39_MAX_MNEMONIC_LEN + 1], pp[MAX_PASSPHRASE + 1];
    esp_err_t err = read_and_decrypt(password, pt);
    if (err == ESP_ERR_INVALID_CRC) {
        nvs_set_fails(nvs_get_fails() + 1);
        s_last_fail_us = esp_timer_get_time();
        return err;
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_fails(0);
    bool ok = parse_plaintext(pt, mn, sizeof(mn), pp, sizeof(pp));
    memzero(pt, sizeof(pt));
    if (!ok) {
        return ESP_ERR_INVALID_VERSION;
    }
    uint8_t seed[64];
    mnemonic_to_seed(mn, pp, seed, NULL);
    memzero(mn, sizeof(mn));
    memzero(pp, sizeof(pp));

    LOCK();
    ok = derive_account(seed);
    memzero(seed, sizeof(seed));
    if (ok) {
        s_state = WALLET_STATE_UNLOCKED;
    }
    UNLOCK();
    ESP_LOGI(TAG, "unlocked wallet %s", s_fp_hex);
    return ok ? ESP_OK : ESP_FAIL;
}

bool wallet_verify_password(const char *password)
{
    uint8_t pt[PT_LEN];
    esp_err_t err = read_and_decrypt(password, pt);
    memzero(pt, sizeof(pt));
    return err == ESP_OK;
}

void wallet_lock(void)
{
    ensure_mutex();
    LOCK();
    memzero(&s_account, sizeof(s_account));
    memzero(s_chain, sizeof(s_chain));
    if (s_state == WALLET_STATE_UNLOCKED) {
        s_state = WALLET_STATE_LOCKED;
    }
    UNLOCK();
    wallet_refresh_state();
}

esp_err_t wallet_reveal_mnemonic(const char *password, char *out, size_t out_len)
{
    uint8_t pt[PT_LEN];
    char pp[MAX_PASSPHRASE + 1];
    esp_err_t err = read_and_decrypt(password, pt);
    if (err == ESP_OK && !parse_plaintext(pt, out, out_len, pp, sizeof(pp))) {
        err = ESP_ERR_INVALID_SIZE;
    }
    memzero(pt, sizeof(pt));
    memzero(pp, sizeof(pp));
    return err;
}

esp_err_t wallet_wipe(const char *password)
{
    if (!wallet_verify_password(password)) {
        return ESP_ERR_INVALID_CRC;
    }
    wallet_lock();
    /* overwrite before unlinking (best effort on FAT/flash media) */
    FILE *f = fopen(WALLET_FILE, "r+b");
    if (f) {
        uint8_t z[64] = {0};
        for (int i = 0; i < 16; i++) {
            fwrite(z, 1, sizeof(z), f);
        }
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    int r = unlink(WALLET_FILE);
    wallet_refresh_state();
    return r == 0 ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------- keys */

esp_err_t wallet_keypair(uint32_t chain, uint32_t index, uint8_t priv[32], uint8_t pub[33])
{
    if (chain > 1 || (index & 0x80000000u)) {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    if (s_state != WALLET_STATE_UNLOCKED) {
        UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    hdnode_t c;
    bool ok = node_ckd_priv(&s_chain[chain], index, &c, true);
    UNLOCK();
    if (ok) {
        if (priv) {
            memcpy(priv, c.key, 32);
        }
        if (pub) {
            memcpy(pub, c.pub, 33);
        }
    }
    memzero(&c, sizeof(c));
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t wallet_pubkey_hash(uint32_t chain, uint32_t index, uint8_t pkh[20])
{
    uint8_t pub[33];
    esp_err_t err = wallet_keypair(chain, index, NULL, pub);
    if (err == ESP_OK) {
        hash160(pub, 33, pkh);
    }
    return err;
}

esp_err_t wallet_address(uint32_t chain, uint32_t index, char *addr, size_t addr_len)
{
    uint8_t pkh[20];
    if (addr_len < WALLET_ADDR_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = wallet_pubkey_hash(chain, index, pkh);
    if (err != ESP_OK) {
        return err;
    }
    return segwit_addr_encode(addr, wallet_hrp(), 0, pkh, 20) ? ESP_OK : ESP_FAIL;
}

esp_err_t wallet_account_xpub(char *out, size_t out_len)
{
    uint8_t d[78];
    uint32_t ver = wallet_is_testnet() ? 0x045F1CF6u /* vpub */ : 0x04B24746u /* zpub */;
    LOCK();
    if (s_state != WALLET_STATE_UNLOCKED) {
        UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    d[0] = ver >> 24;
    d[1] = ver >> 16;
    d[2] = ver >> 8;
    d[3] = ver;
    d[4] = s_account.depth;
    memcpy(d + 5, s_account.parent_fp, 4);
    d[9] = s_account.child >> 24;
    d[10] = s_account.child >> 16;
    d[11] = s_account.child >> 8;
    d[12] = s_account.child;
    memcpy(d + 13, s_account.chain, 32);
    memcpy(d + 45, s_account.pub, 33);
    UNLOCK();
    return base58_encode_check(d, sizeof(d), HASHER_SHA2D, out, out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

const char *wallet_fingerprint_hex(void)
{
    return s_fp_hex;
}
