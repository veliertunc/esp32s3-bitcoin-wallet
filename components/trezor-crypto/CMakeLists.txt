# Vendored subset of trezor-crypto (MIT) - see src/LICENSE and VERSION.
# Only the files needed for a BIP39/BIP32/BIP84 secp256k1 wallet are compiled.
set(TC src)
idf_component_register(
    SRCS
        ${TC}/address.c
        ${TC}/base58.c
        ${TC}/bignum.c
        ${TC}/bip39.c
        ${TC}/bip39_english.c
        ${TC}/blake256.c
        ${TC}/blake2b.c
        ${TC}/consteq.c
        ${TC}/ecdsa.c
        ${TC}/fault_handler_noop.c
        ${TC}/hasher.c
        ${TC}/hmac.c
        ${TC}/hmac_drbg.c
        ${TC}/memzero.c
        ${TC}/pbkdf2.c
        ${TC}/rand.c
        ${TC}/rfc6979.c
        ${TC}/ripemd160.c
        ${TC}/script.c
        ${TC}/secp256k1.c
        ${TC}/segwit_addr.c
        ${TC}/sha2.c
        ${TC}/sha3.c
        ${TC}/chacha20poly1305/chacha20poly1305.c
        ${TC}/chacha20poly1305/chacha_merged.c
        ${TC}/chacha20poly1305/poly1305-donna.c
        ${TC}/chacha20poly1305/rfc7539.c
        port/random_esp32.c
    INCLUDE_DIRS ${TC} ${TC}/chacha20poly1305
    REQUIRES esp_hw_support)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    USE_BIP32_25519_CURVES=0
    USE_BIP39_CACHE=0
    USE_ETHEREUM=0
    USE_MONERO=0
    USE_CARDANO=0
    USE_NEM=0
    USE_KECCAK=1
    USE_PRECOMPUTED_CP=1
    RAND_PLATFORM_INDEPENDENT=1
    # avoid clashing with wpa_supplicant exports
    hmac_sha256=tc_hmac_sha256
    hmac_sha512=tc_hmac_sha512
    hmac_sha256_Init=tc_hmac_sha256_Init
    hmac_sha256_Update=tc_hmac_sha256_Update
    hmac_sha256_Final=tc_hmac_sha256_Final)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function -Wno-unused-variable -Wno-sign-compare
    -Wno-maybe-uninitialized -Wno-implicit-fallthrough -O2)
