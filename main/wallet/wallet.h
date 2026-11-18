/*
 * HD wallet (BIP39 mnemonic -> BIP32 -> BIP84 native segwit P2WPKH) whose
 * secret is stored encrypted on the microSD card.
 *
 *  /sdcard/btcwallet/wallet.dat   encrypted mnemonic + optional BIP39 passphrase
 *
 * While unlocked only the BIP84 account key (m/84'/coin'/0') and the two chain
 * keys live in RAM. wallet_lock() wipes them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WALLET_DIR          "/sdcard/btcwallet"
#define WALLET_FILE         WALLET_DIR "/wallet.dat"
#define WALLET_ADDR_MAX     91          /* bech32 max length + NUL */
#define WALLET_CHAIN_RECEIVE 0
#define WALLET_CHAIN_CHANGE  1

typedef enum {
    WALLET_STATE_NO_SD = 0,
    WALLET_STATE_EMPTY,       /* SD ok, no wallet file */
    WALLET_STATE_LOCKED,
    WALLET_STATE_UNLOCKED,
} wallet_state_t;

/* Network constants for the compiled-in network. */
bool        wallet_is_testnet(void);
const char *wallet_hrp(void);           /* "bc" / "tb" */
const char *wallet_network_name(void);

/* BIP84 known-answer self-test (cached). Wallet ops refuse to run if it fails. */
bool        wallet_selftest(void);

wallet_state_t wallet_refresh_state(void);   /* re-checks SD + file */
wallet_state_t wallet_state(void);

/* Add extra entropy (e.g. touch timings) to the pool used for new mnemonics. */
void wallet_entropy_add(const void *data, size_t len);

/* Generate a fresh mnemonic of 12 or 24 words into out (>= 256 bytes). */
esp_err_t wallet_generate_mnemonic(int words, char *out, size_t out_len);
bool      wallet_mnemonic_valid(const char *mnemonic);
/* Normalise user input: lowercase, single spaces, trimmed. */
void      wallet_mnemonic_normalise(const char *in, char *out, size_t out_len);

/* Encrypt + store on SD and unlock. password >= 6 chars. */
esp_err_t wallet_create(const char *mnemonic, const char *bip39_passphrase, const char *password);

/* ESP_OK, ESP_ERR_INVALID_CRC (wrong password), ESP_ERR_INVALID_STATE (throttled),
 * ESP_ERR_NOT_FOUND (no file) or other I/O errors. */
esp_err_t wallet_unlock(const char *password);
/* Seconds the user must wait before the next unlock attempt (0 = none). */
int       wallet_unlock_wait_seconds(void);
void      wallet_lock(void);
bool      wallet_verify_password(const char *password);

/* Decrypt and return the mnemonic (for backup display). Caller must memzero. */
esp_err_t wallet_reveal_mnemonic(const char *password, char *out, size_t out_len);

/* Remove the wallet file from the SD card (requires password). */
esp_err_t wallet_wipe(const char *password);

/* ---- key access (unlocked only) ---- */
esp_err_t wallet_address(uint32_t chain, uint32_t index, char *addr, size_t addr_len);
esp_err_t wallet_pubkey_hash(uint32_t chain, uint32_t index, uint8_t pkh[20]);
esp_err_t wallet_keypair(uint32_t chain, uint32_t index, uint8_t priv[32], uint8_t pub[33]);
/* SLIP-132 account xpub (zpub / vpub) */
esp_err_t wallet_account_xpub(char *out, size_t out_len);
/* 4-byte master key fingerprint as hex (identifies the wallet). */
const char *wallet_fingerprint_hex(void);
