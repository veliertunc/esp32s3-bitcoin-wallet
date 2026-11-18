/*
 * trezor-crypto random source backed by the ESP32-S3 hardware RNG.
 *
 * esp_fill_random() is a true RNG only while an entropy source is active
 * (Wi-Fi/BT RF running, or the SAR ADC noise source enabled by
 * bootloader_random_enable()). The wallet code calls
 * trng_entropy_begin()/trng_entropy_end() around key generation to guarantee
 * that, and additionally mixes user/touch timing entropy (see wallet.c).
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_random.h"
#include "rand.h"


void random_buffer(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
}
