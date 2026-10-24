/* m33mu -- an ARMv8-M Emulator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * wolfCrypt configuration for the WASM build. Only selects algorithms; the
 * wolfSSL sources come from M33MU_WOLFSSL_SRC and are never vendored.
 * Mirrors what the native build gets from the distro wolfSSL for the APIs
 * the crypto peripheral models call.
 */
#ifndef M33MU_WASM_WOLFSSL_USER_SETTINGS_H
#define M33MU_WASM_WOLFSSL_USER_SETTINGS_H

#define WOLFCRYPT_ONLY
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define NO_WRITEV
#define WOLFSSL_NO_SOCK
#define HAVE_HASHDRBG
/* Seed from getentropy() (crypto.getRandomValues under Emscripten). */
int m33mu_wasm_rng_seed(unsigned char *output, unsigned int sz);
#define CUSTOM_RAND_GENERATE_SEED m33mu_wasm_rng_seed

#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_SP_INT_NEGATIVE
#define SP_INT_BITS 4096
#define WOLFSSL_PUBLIC_MP

#define WOLFSSL_AES_DIRECT
#define HAVE_AES_ECB
#define HAVE_AES_CBC
#define WOLFSSL_AES_COUNTER
#define HAVE_AESGCM
#define WOLFSSL_AESGCM_STREAM
#define HAVE_AESCCM
#define WOLFSSL_CMAC
#define WOLFSSL_AES_128
#define WOLFSSL_AES_192
#define WOLFSSL_AES_256

#define WOLFSSL_SHA224
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
#define WOLFSSL_SHA3
#define HAVE_HKDF
/* wc_Sha256Transform (LPC55S69 HASHCRYPT) is only declared with OPENSSL_EXTRA. */
#define OPENSSL_EXTRA

#define HAVE_ECC
#define ECC_TIMING_RESISTANT
#define HAVE_ECC_CDH
#define HAVE_ECC384
#define HAVE_ECC521
#define HAVE_ECC_BRAINPOOL
#define HAVE_ECC_KOBLITZ
#define ECC_USER_CURVES
#define HAVE_ECC192
#define HAVE_ECC224
#define HAVE_ECC256
#define WOLFSSL_KEY_GEN
#define WOLFSSL_CUSTOM_CURVES

#define WC_RSA_PSS
#define WC_RSA_BLINDING
#define WOLFSSL_RSA_KEY_CHECK

#define NO_DSA
#define NO_DH
#define NO_RC4
#define NO_MD4
#define NO_MD5
#define NO_DES3
#define NO_PSK
#define NO_OLD_TLS

#endif
