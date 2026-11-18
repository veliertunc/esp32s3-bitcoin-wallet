# ESP32-S3 SD-card Bitcoin wallet (ONX3248G035)

ESP-IDF firmware that turns the **ONX3248G035** board (ESP32-S3R8, 3.5" 320×480 ST7796 touch LCD, CST826
touch, microSD) into a Bitcoin wallet with an LVGL touch UI:

- **Create** a new BIP39 wallet (12/24 words, hardware TRNG + touch entropy) or **restore** an existing phrase
- Wallet secret stored **encrypted on the microSD card** (`/btcwallet/wallet.dat`)
- **Receive**: BIP84 native segwit (`bc1q…` / `tb1q…`) addresses with QR code
- **Send**: to bech32 / bech32m (taproot) / legacy / P2SH addresses, fee presets, "send all", change
  handling, BIP143 signing on the device, RBF enabled, broadcast over HTTPS
- **History**, balance (confirmed + pending), watch-only **zpub/vpub** export, show recovery phrase
- **Firmware update from the SD card** (`/firmware.bin`, A/B OTA slots with automatic rollback)
- Battery level, Wi-Fi setup screen, auto-lock, brightness

> ⚠️ **Defaults to Bitcoin testnet4.** Test creating, receiving, sending and restoring with testnet coins
> first. Switch to mainnet in `menuconfig` only once you're confident. This is hobby firmware without
> a secure element; see *Security model* below before trusting it with real money.

## Hardware mapping (from schematic V1.2)

| Function | Pins |
|---|---|
| LCD ST7796 (SPI2) | MOSI IO1, SCLK IO5, CS IO2, DC IO3, RST = PCF8574 P5, backlight IO6 (PWM) |
| Touch CST826 (I2C 0x15) | SCL IO7, SDA IO8, INT = PCF8574 P4, RST shared with LCD |
| IO expander PCF8574A (0x38) | P0 I2S_CTRL, P1 RTC_INT, P2 CHG_N, P3 CAM_PWDN, P4 TP_INT, P5 LCD_RST, P6 SD_CS |
| microSD (SDMMC 1-bit) | CLK IO11, CMD IO10, D0 IO9, DAT3/CS = PCF8574 P6 (held high) |
| Battery | IO4 (ADC1), 1 MΩ / 2.2 MΩ divider |

See [main/board/board_pins.h](main/board/board_pins.h).

## Build & flash

Requires ESP-IDF ≥ 5.4 (developed against v6.1). Dependencies (LVGL 9.2, cJSON) are fetched by the
component manager; the crypto library ([trezor-crypto](components/trezor-crypto), MIT) is vendored.

```powershell
idf.py set-target esp32s3
idf.py menuconfig        # "Bitcoin Wallet" menu: network, Esplora URL, KDF iterations, auto-lock...
idf.py build flash monitor
```

On first boot the log should show `BIP39/BIP32/BIP84 self-test PASSED`. If it fails, wallet creation
and unlocking are disabled.

### Updating firmware from the SD card

Copy `build/esp32s3-bitcoin-wallet.bin` to the root of the card as `firmware.bin`, then
*Settings → Update firmware from SD*. The image goes into the inactive OTA slot, is validated, and the
board reboots into it; the file is renamed to `firmware.bin.done`. If the new firmware doesn't start the
bootloader rolls back.

## Usage

1. Insert a FAT32 microSD card and power on.
2. **New wallet** → write down the words → confirm 3 of them → choose a password (≥ 6 chars, longer is
   better) → the encrypted wallet is written to the SD card.
3. *Settings → Wi-Fi* → scan, pick your network, enter password (saved in NVS).
4. *Home → Sync* scans your addresses (gap limit 10) via the Esplora API.
5. *Receive* shows a fresh address + QR. *Send*: address, amount in BTC (or "send entire balance"), fee
   preset → **Review** → check every detail on the device screen → **Sign & Send**.

Storage layout:

| Where | What |
|---|---|
| SD `/btcwallet/wallet.dat` | encrypted mnemonic + optional BIP39 passphrase (572 bytes) |
| SD `/firmware.bin` | optional firmware update image |
| flash SPIFFS `/spiffs/acct_<fp>_<net>.json` | non-secret state (current receive index) |
| flash NVS | Wi-Fi credentials, device-binding secret, failed-unlock counter |

## Security model — read this

- **Encryption**: key = PBKDF2-HMAC-SHA512(password, 16-byte salt, 50 000 iterations by default) →
  ChaCha20-Poly1305 with the file header as associated data. Plaintext is padded to a fixed 512 bytes.
- **Device binding** (default on, `WALLET_BIND_TO_DEVICE`): a random 32-byte secret in this board's NVS
  is mixed into the key, so a stolen SD card alone cannot be brute-forced. Consequence: the file only
  opens on *this* board. **Your written recovery phrase is the real backup** — restore it on any BIP84
  wallet (Sparrow, Electrum, BlueWallet…) if the board or card dies.
- For real protection of that NVS secret and the firmware, enable **flash encryption + NVS encryption
  + secure boot** (ESP-IDF security features). Without them, someone with the board *and* the card can
  read the NVS secret over USB and brute-force your password offline — use a long password.
- While unlocked, only the account/chain keys live in RAM; locking (button, or auto-lock after 180 s)
  wipes them. The mnemonic is decrypted only to unlock or when you ask to display it.
- Wrong passwords trigger an increasing delay (persisted across reboots).
- Every signature is verified before broadcast; the confirm screen shows destination, amount, fee,
  fee rate and change.
- **Privacy**: syncing sends all your addresses to the configured Esplora server
  (mempool.space by default). Point `BTC_ESPLORA_URL` at your own mempool/electrs instance for privacy.
- Not implemented: secure element, PSBT/air-gapped signing, multisig, coin control, Lightning.

## Project layout

```
main/
  main.c               boot sequence
  board/               pins, PCF8574, backlight, battery ADC, SD/SPIFFS, ST7796 + CST826 drivers
  gui/                 LVGL port (flush/touch/lock) and the UI screens
  wallet/wallet.c      BIP39/BIP32/BIP84 keys, encrypted SD storage, self-test
  wallet/btc_tx.c      address decoding, coin selection, BIP143 signing, serialisation
  wallet/account.c     Esplora sync: balances, UTXOs, history, fee estimates, broadcast
  net/                 Wi-Fi manager, Esplora HTTPS client
  sys/                 background worker task, firmware update from SD
components/trezor-crypto   vendored crypto (MIT)
partitions.csv       16 MB: nvs, otadata, 2 × 4 MB OTA app slots, ~7.9 MB SPIFFS
```
