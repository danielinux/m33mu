#ifndef TEST_STM32U585_WOLFSSL_PKA_USER_SETTINGS_H
#define TEST_STM32U585_WOLFSSL_PKA_USER_SETTINGS_H

#include <stdint.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Target identification
 * --------------------------------------------------------------------- */
#define WOLFSSL_STM32U5
#define WOLFSSL_STM32_PKA

/* Disable STM32 hardware hash, AES acceleration, and stdlib RNG — we
 * only use PKA. Without these, stm32.h won't pull in CRYP/HASH HAL types
 * and random.c won't try to use the Standard Peripheral Library RNG. */
#define NO_STM32_HASH
#define NO_STM32_CRYPTO
#define NO_STM32_RNG

/* Custom RNG seed via STM32U585 TRNG hardware (direct register access) */
static inline int stm32u5_rng_generate_block(unsigned char *buf, unsigned int sz)
{
    /* STM32U585 RNG: AHB2ENR bit 18 = RNGEN, base 0x420C0800 */
    volatile uint32_t *rcc_ahb2enr = (volatile uint32_t *)(0x46020C00U + 0x8CU);
    volatile uint32_t *rng_cr  = (volatile uint32_t *)0x420C0800U;
    volatile uint32_t *rng_sr  = (volatile uint32_t *)0x420C0804U;
    volatile uint32_t *rng_dr  = (volatile uint32_t *)0x420C0808U;
    unsigned int i = 0;
    /* Enable RNG clock */
    *rcc_ahb2enr |= (1U << 18);
    /* Enable RNG: set RNGEN (bit 2) and IE (bit 3) */
    *rng_cr |= (1U << 2) | (1U << 3);
    while (i < sz) {
        /* Wait for DRDY */
        while ((*rng_sr & (1U << 0)) == 0U) { }
        uint32_t r = *rng_dr;
        unsigned int j;
        for (j = 0; j < 4U && i < sz; ++j, ++i) {
            buf[i] = (unsigned char)(r & 0xFFU);
            r >>= 8;
        }
    }
    return 0;
}
#define CUSTOM_RAND_GENERATE_BLOCK  stm32u5_rng_generate_block
/* Hash_DRBG_Generate uses WC_RESEED_INTERVAL; define it since
 * CUSTOM_RAND_GENERATE_BLOCK suppresses the definition in random.h */
#ifndef WC_RESEED_INTERVAL
#define WC_RESEED_INTERVAL 1000000
#endif

/* -----------------------------------------------------------------------
 * Basic wolfSSL settings
 * --------------------------------------------------------------------- */
#ifndef WOLFSSL_USER_SETTINGS
    /* Should already be defined in CMake flags */
    #define WOLFSSL_USER_SETTINGS
#endif
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define NO_WRITEV
#define WOLFSSL_NO_SOCK
#define NO_MAIN_DRIVER
#define WOLFSSL_LOG_PRINTF

/* -----------------------------------------------------------------------
 * ECC and ASN
 * --------------------------------------------------------------------- */
#define HAVE_ECC
#define HAVE_ECC256
#define ECC_TIMING_RESISTANT
#define HAVE_ASN

/* -----------------------------------------------------------------------
 * Hash / RNG
 * --------------------------------------------------------------------- */
#define HAVE_HASHDRBG
#define NO_OLD_RNGNAME
#define WC_NO_HARDEN

/* -----------------------------------------------------------------------
 * Math
 * --------------------------------------------------------------------- */
#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_SP_SMALL
#define WOLFSSL_HAVE_SP_ECC
#define SP_WORD_SIZE 32
#define TFM_TIMING_RESISTANT
#define TFM_ARM

/* -----------------------------------------------------------------------
 * Reduce binary size
 * --------------------------------------------------------------------- */
#define WOLFSSL_SMALL_STACK
#define WOLFSSL_GENERAL_ALIGNMENT 4
#define NO_DES3
#define NO_DSA
#define NO_RC4
#define NO_MD4
#define NO_MD5
#define NO_SHA
#define NO_SHA512
#define NO_PKCS12
#define NO_RSA
#define NO_DH
#define NO_ASN_TIME
#define NO_OLD_TLS

#define HAVE_AES_DECRYPT
#define HAVE_SHA256

/* -----------------------------------------------------------------------
 * Time overrides (no OS)
 * --------------------------------------------------------------------- */
extern volatile uint32_t systick_ms;
static inline long XTIME(long *x) { (void)x; return (long)systick_ms; }
#define WOLFSSL_USER_CURRTIME

/* -----------------------------------------------------------------------
 * PKA peripheral base address (STM32U585 non-secure alias)
 * --------------------------------------------------------------------- */
#define STM32U5_PKA_BASE  0x420C2000u

/* -----------------------------------------------------------------------
 * Minimal STM32 HAL types required by wolfssl/wolfcrypt/src/port/st/stm32.c
 *
 * The real CubeMX HAL for STM32U5 is not available in this bare-metal
 * environment.  We provide lightweight inline stubs that directly program
 * the PKA peripheral registers the same way the emulator expects them.
 * --------------------------------------------------------------------- */

/* HAL status codes */
typedef enum {
    HAL_OK      = 0x00U,
    HAL_ERROR   = 0x01U,
    HAL_BUSY    = 0x02U,
    HAL_TIMEOUT = 0x03U
} HAL_StatusTypeDef;

#define HAL_MAX_DELAY 0xFFFFFFFFU

/* --- PKA register offsets ------------------------------------------- */
#define PKA_CR_OFFSET   0x000U
#define PKA_SR_OFFSET   0x004U
#define PKA_CLRFR_OFFSET 0x008U

#define PKA_CR_EN       (1U << 0)
#define PKA_CR_START    (1U << 1)
#define PKA_CR_MODE_SHIFT 8U
#define PKA_SR_PROCENDF (1U << 17)
#define PKA_SR_RAMERRF  (1U << 19)
#define PKA_SR_ADDRERRF (1U << 20)
#define PKA_SR_OPERRF   (1U << 21)

#define PKA_CLRFR_PROCENDFC (1U << 17)
#define PKA_CLRFR_RAMERRFC  (1U << 19)
#define PKA_CLRFR_ADDRERRFC (1U << 20)
#define PKA_CLRFR_OPERRFC   (1U << 21)

/* PKA RAM parameter offsets (from PKA base) - match pka.c */
#define PKA_RAM_ECC_N_LEN        0x400U
#define PKA_RAM_ECC_P_LEN        0x408U
#define PKA_RAM_A_SIGN           0x410U
#define PKA_RAM_A_COEFF          0x418U

/* ECDSA sign input offsets */
#define PKA_SIGN_N_LEN           0x400U
#define PKA_SIGN_P_LEN           0x408U
#define PKA_SIGN_A_SIGN          0x410U
#define PKA_SIGN_A_COEFF         0x418U
#define PKA_SIGN_P               0x1088U
#define PKA_SIGN_K               0x12A0U
#define PKA_SIGN_GX              0x578U
#define PKA_SIGN_GY              0x470U
#define PKA_SIGN_HASH            0xFE8U
#define PKA_SIGN_D               0xF28U
#define PKA_SIGN_N               0xF88U
#define PKA_SIGN_R               0x730U
#define PKA_SIGN_S               0x788U
#define PKA_SIGN_ERR             0xFE0U

/* ECDSA verify input offsets */
#define PKA_VERIF_N_LEN          0x408U
#define PKA_VERIF_P_LEN          0x4C8U
#define PKA_VERIF_A_SIGN         0x468U
#define PKA_VERIF_A              0x470U
#define PKA_VERIF_P              0x4D0U
#define PKA_VERIF_GX             0x678U
#define PKA_VERIF_GY             0x6D0U
#define PKA_VERIF_QX             0x12F8U
#define PKA_VERIF_QY             0x1350U
#define PKA_VERIF_R              0x10E0U
#define PKA_VERIF_S              0xC68U
#define PKA_VERIF_HASH           0x13A8U
#define PKA_VERIF_N              0x1088U
#define PKA_VERIF_RES            0x5D0U

/* ECC scalar mul input offsets */
#define PKA_ECCMUL_P_LEN         0x408U
#define PKA_ECCMUL_N_LEN         0x400U
#define PKA_ECCMUL_A_SIGN        0x410U
#define PKA_ECCMUL_A_COEFF       0x418U
#define PKA_ECCMUL_P             0x1088U
#define PKA_ECCMUL_K             0x12A0U
#define PKA_ECCMUL_X             0x578U
#define PKA_ECCMUL_Y             0x470U
#define PKA_ECCMUL_N             0xF88U
#define PKA_ECCMUL_B             0x520U  /* PKA_V2: coefB */
#define PKA_ECCMUL_RES_X         0x578U
#define PKA_ECCMUL_RES_Y         0x5D0U
#define PKA_ECCMUL_ERR           0x680U

/* PKA operation mode codes */
#define PKA_MODE_ECDSA_SIGN      0x24U
#define PKA_MODE_ECDSA_VERIF     0x26U
#define PKA_MODE_ECC_MUL         0x20U

#define PKA_STATUS_OK  0xD60DU

/* --- HAL PKA handle ------------------------------------------------- */
typedef struct {
    volatile uint32_t *Instance;   /* Points to PKA base address */
    uint32_t          sz;          /* Last operation modulus size in bytes */
} PKA_HandleTypeDef;

/* PKA_V2: STM32U5 uses the "V2" PKA that has coefB + primeOrder in ECCMul */
#define PKA_ECC_SCALAR_MUL_IN_B_COEFF 1  /* triggers WOLFSSL_STM32_PKA_V2 */

/* --- ECC scalar multiply structs ------------------------------------ */
typedef struct {
    uint32_t modulusSize;
    uint32_t coefSign;
    const uint8_t *coefA;
    const uint8_t *coefB;        /* PKA_V2 */
    const uint8_t *modulus;
    const uint8_t *pointX;
    const uint8_t *pointY;
    uint32_t scalarMulSize;
    const uint8_t *scalarMul;
    const uint8_t *primeOrder;   /* PKA_V2 */
} PKA_ECCMulInTypeDef;

typedef struct {
    uint8_t *ptX;
    uint8_t *ptY;
} PKA_ECCMulOutTypeDef;

/* --- ECDSA sign structs --------------------------------------------- */
typedef struct {
    uint32_t primeOrderSize;
    uint32_t modulusSize;
    uint32_t coefSign;
    const uint8_t *coef;
    const uint8_t *coefB;        /* PKA_V2 */
    const uint8_t *modulus;
    const uint8_t *basePointX;
    const uint8_t *basePointY;
    const uint8_t *primeOrder;
    const uint8_t *hash;
    const uint8_t *integer;
    const uint8_t *privateKey;
} PKA_ECDSASignInTypeDef;

typedef struct {
    uint8_t *RSign;
    uint8_t *SSign;
} PKA_ECDSASignOutTypeDef;

/* --- ECDSA verify structs ------------------------------------------- */
typedef struct {
    uint32_t primeOrderSize;
    uint32_t modulusSize;
    uint32_t coefSign;
    const uint8_t *coef;
    const uint8_t *modulus;
    const uint8_t *basePointX;
    const uint8_t *basePointY;
    const uint8_t *primeOrder;
    const uint8_t *pPubKeyCurvePtX;
    const uint8_t *pPubKeyCurvePtY;
    const uint8_t *RSign;
    const uint8_t *SSign;
    const uint8_t *hash;
} PKA_ECDSAVerifInTypeDef;

/* -----------------------------------------------------------------------
 * Inline PKA helper utilities
 * --------------------------------------------------------------------- */
static inline volatile uint32_t *pka_reg(uint32_t off)
{
    return (volatile uint32_t *)(STM32U5_PKA_BASE + off);
}

static inline void pka_ram_write_be(uint32_t ram_off, const uint8_t *src, uint32_t size)
{
    /* Write big-endian byte array into PKA RAM (little-endian words, LE byte order per word) */
    uint32_t i;
    volatile uint32_t *dst = (volatile uint32_t *)(STM32U5_PKA_BASE + ram_off);
    /* PKA RAM stores limbs in little-endian word order with big-endian bytes within each word */
    uint32_t words = (size + 3U) / 4U;
    for (i = 0; i < words; ++i) {
        uint32_t widx = words - 1U - i; /* reverse word order: MSW at lowest RAM addr */
        uint32_t bbase = i * 4U;
        uint32_t b0 = (bbase < size)     ? src[bbase]     : 0U;
        uint32_t b1 = (bbase+1U < size)  ? src[bbase+1U]  : 0U;
        uint32_t b2 = (bbase+2U < size)  ? src[bbase+2U]  : 0U;
        uint32_t b3 = (bbase+3U < size)  ? src[bbase+3U]  : 0U;
        /* Each 4-byte group is stored big-endian within the word,
         * but groups are in reverse order (LSG at highest address) */
        dst[widx] = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
    }
    /* Zero-terminate */
    dst[words] = 0U;
}

static inline void pka_ram_read_be(uint32_t ram_off, uint8_t *dst, uint32_t size)
{
    uint32_t i;
    const volatile uint32_t *src = (const volatile uint32_t *)(STM32U5_PKA_BASE + ram_off);
    uint32_t words = (size + 3U) / 4U;
    for (i = 0; i < words; ++i) {
        uint32_t widx = words - 1U - i;
        uint32_t w = src[widx];
        uint32_t bbase = i * 4U;
        if (bbase < size)     dst[bbase]     = (uint8_t)((w >> 24U) & 0xFFU);
        if (bbase+1U < size)  dst[bbase+1U]  = (uint8_t)((w >> 16U) & 0xFFU);
        if (bbase+2U < size)  dst[bbase+2U]  = (uint8_t)((w >> 8U)  & 0xFFU);
        if (bbase+3U < size)  dst[bbase+3U]  = (uint8_t)( w         & 0xFFU);
    }
}

static inline void pka_write32(uint32_t ram_off, uint32_t val)
{
    *((volatile uint32_t *)(STM32U5_PKA_BASE + ram_off)) = val;
}

static inline uint32_t pka_read32(uint32_t ram_off)
{
    return *((volatile uint32_t *)(STM32U5_PKA_BASE + ram_off));
}

static inline HAL_StatusTypeDef pka_run(uint32_t mode, uint32_t timeout)
{
    uint32_t cr;
    (void)timeout;
    /* Enable PKA + set mode + start */
    cr = PKA_CR_EN | (mode << PKA_CR_MODE_SHIFT) | PKA_CR_START;
    *pka_reg(PKA_CR_OFFSET) = cr;
    /* Poll for PROCENDF */
    while ((*pka_reg(PKA_SR_OFFSET) & PKA_SR_PROCENDF) == 0U) {
        /* spin */
    }
    /* Check for errors */
    if (*pka_reg(PKA_SR_OFFSET) & (PKA_SR_RAMERRF | PKA_SR_ADDRERRF | PKA_SR_OPERRF)) {
        *pka_reg(PKA_CLRFR_OFFSET) = PKA_CLRFR_PROCENDFC | PKA_CLRFR_RAMERRFC |
                                      PKA_CLRFR_ADDRERRFC | PKA_CLRFR_OPERRFC;
        return HAL_ERROR;
    }
    return HAL_OK;
}

/* -----------------------------------------------------------------------
 * HAL_PKA_RAMReset  — clear PKA RAM and status flags
 * --------------------------------------------------------------------- */
static inline void HAL_PKA_RAMReset(PKA_HandleTypeDef *hpka)
{
    uint32_t i;
    volatile uint32_t *ram = (volatile uint32_t *)(STM32U5_PKA_BASE + 0x400U);
    (void)hpka;
    for (i = 0; i < (5336U / 4U); ++i) {
        ram[i] = 0U;
    }
    *pka_reg(PKA_CLRFR_OFFSET) = PKA_CLRFR_PROCENDFC | PKA_CLRFR_RAMERRFC |
                                   PKA_CLRFR_ADDRERRFC | PKA_CLRFR_OPERRFC;
}

/* -----------------------------------------------------------------------
 * HAL_PKA_ECCMul  — ECC scalar multiplication (mode 0x20)
 *
 * Parameter layout in PKA RAM (pka.c defines):
 *   PKA_RAM_ECC_P_LEN  0x408  => modulus bit-length
 *   PKA_RAM_ECC_N_LEN  0x400  => order bit-length
 *   PKA_RAM_A_SIGN     0x410  => coefA sign (1 = negative)
 *   PKA_RAM_A_COEFF    0x418  => coefA bytes
 *   PKA_RAM_PC_B       0x520  => coefB bytes (PKA_V2)
 *   PKA_RAM_ECC_P      0x1088 => prime field modulus
 *   PKA_RAM_ECC_K      0x12A0 => scalar
 *   PKA_RAM_ECC_X      0x578  => point X (input)
 *   PKA_RAM_ECC_Y      0x470  => point Y (input)
 *   PKA_RAM_ECC_N      0xF88  => curve order
 *   Results written to same X/Y offsets.
 * --------------------------------------------------------------------- */
static inline HAL_StatusTypeDef HAL_PKA_ECCMul(PKA_HandleTypeDef *hpka,
    PKA_ECCMulInTypeDef *in, uint32_t timeout)
{
    uint32_t sz = in->modulusSize;
    uint32_t bits = sz * 8U;
    (void)hpka;

    hpka->sz = (uint32_t)sz;
    pka_write32(PKA_RAM_ECC_P_LEN,  bits);
    pka_write32(PKA_RAM_ECC_N_LEN,  bits);
    pka_write32(PKA_RAM_A_SIGN,     in->coefSign);
    pka_ram_write_be(PKA_RAM_A_COEFF, in->coefA, sz);
    if (in->coefB != NULL) pka_ram_write_be(PKA_ECCMUL_B, in->coefB, sz);
    pka_ram_write_be(PKA_ECCMUL_P,  in->modulus, sz);
    pka_ram_write_be(PKA_ECCMUL_K,  in->scalarMul, in->scalarMulSize);
    pka_ram_write_be(PKA_ECCMUL_X,  in->pointX, sz);
    pka_ram_write_be(PKA_ECCMUL_Y,  in->pointY, sz);
    if (in->primeOrder != NULL) pka_ram_write_be(PKA_ECCMUL_N, in->primeOrder, sz);

    return pka_run(PKA_MODE_ECC_MUL, timeout);
}

static inline void HAL_PKA_ECCMul_GetResult(PKA_HandleTypeDef *hpka,
    PKA_ECCMulOutTypeDef *out)
{
    uint32_t sz = hpka->sz ? hpka->sz : 32U;
    pka_ram_read_be(PKA_ECCMUL_RES_X, out->ptX, sz);
    pka_ram_read_be(PKA_ECCMUL_RES_Y, out->ptY, sz);
}

/* -----------------------------------------------------------------------
 * HAL_PKA_ECDSASign  — ECDSA sign (mode 0x24)
 *
 * Parameter layout in PKA RAM (pka.c):
 *   PKA_RAM_ECC_P_LEN  0x408  => prime bit-length
 *   PKA_RAM_ECC_N_LEN  0x400  => order bit-length
 *   PKA_RAM_A_SIGN     0x410  => coefA sign
 *   PKA_RAM_A_COEFF    0x418  => coefA
 *   PKA_RAM_SIGN_P     0x1088 => prime field p
 *   PKA_RAM_SIGN_K     0x12A0 => random k
 *   PKA_RAM_SIGN_GX    0x578  => base point Gx
 *   PKA_RAM_SIGN_GY    0x470  => base point Gy
 *   PKA_RAM_SIGN_HASH  0xFE8  => hash
 *   PKA_RAM_SIGN_D     0xF28  => private key d
 *   PKA_RAM_SIGN_N     0xF88  => order n
 *   Results: R at 0x730, S at 0x788
 * --------------------------------------------------------------------- */
static inline HAL_StatusTypeDef HAL_PKA_ECDSASign(PKA_HandleTypeDef *hpka,
    PKA_ECDSASignInTypeDef *in, uint32_t timeout)
{
    uint32_t plen = in->modulusSize;
    uint32_t nlen = in->primeOrderSize;
    uint32_t pbits = plen * 8U;
    uint32_t nbits = nlen * 8U;

    hpka->sz = nlen;
    pka_write32(PKA_RAM_ECC_P_LEN, pbits);
    pka_write32(PKA_RAM_ECC_N_LEN, nbits);
    pka_write32(PKA_RAM_A_SIGN,    in->coefSign);
    pka_ram_write_be(PKA_RAM_A_COEFF, in->coef, plen);
    pka_ram_write_be(PKA_SIGN_P,      in->modulus,    plen);
    pka_ram_write_be(PKA_SIGN_K,      in->integer,    nlen);
    pka_ram_write_be(PKA_SIGN_GX,     in->basePointX, plen);
    pka_ram_write_be(PKA_SIGN_GY,     in->basePointY, plen);
    pka_ram_write_be(PKA_SIGN_HASH,   in->hash,       nlen);
    pka_ram_write_be(PKA_SIGN_D,      in->privateKey, nlen);
    pka_ram_write_be(PKA_SIGN_N,      in->primeOrder, nlen);

    return pka_run(PKA_MODE_ECDSA_SIGN, timeout);
}

static inline void HAL_PKA_ECDSASign_GetResult(PKA_HandleTypeDef *hpka,
    PKA_ECDSASignOutTypeDef *out, void *param)
{
    uint32_t sz = hpka->sz ? hpka->sz : 32U;
    (void)param;
    pka_ram_read_be(PKA_SIGN_R, out->RSign, sz);
    pka_ram_read_be(PKA_SIGN_S, out->SSign, sz);
}

/* -----------------------------------------------------------------------
 * HAL_PKA_ECDSAVerif  — ECDSA verify (mode 0x26)
 *
 * Parameter layout in PKA RAM (pka.c):
 *   PKA_RAM_VERIF_P_LEN  0x4C8 => prime bit-length
 *   PKA_RAM_VERIF_N_LEN  0x408 => order bit-length
 *   PKA_RAM_VERIF_A_SIGN 0x468 => coefA sign
 *   PKA_RAM_VERIF_A      0x470 => coefA
 *   PKA_RAM_VERIF_P      0x4D0 => prime
 *   PKA_RAM_VERIF_GX     0x678 => Gx
 *   PKA_RAM_VERIF_GY     0x6D0 => Gy
 *   PKA_RAM_VERIF_QX     0x12F8 => pub key X
 *   PKA_RAM_VERIF_QY     0x1350 => pub key Y
 *   PKA_RAM_VERIF_R      0x10E0 => signature R
 *   PKA_RAM_VERIF_S      0xC68 => signature S
 *   PKA_RAM_VERIF_HASH   0x13A8 => hash
 *   PKA_RAM_VERIF_N      0x1088 => order n
 *   Result: PKA_RAM_VERIF_RES 0x5D0 (0xD60D = OK)
 * --------------------------------------------------------------------- */
static inline HAL_StatusTypeDef HAL_PKA_ECDSAVerif(PKA_HandleTypeDef *hpka,
    PKA_ECDSAVerifInTypeDef *in, uint32_t timeout)
{
    uint32_t plen = in->modulusSize;
    uint32_t nlen = in->primeOrderSize;
    uint32_t pbits = plen * 8U;
    uint32_t nbits = nlen * 8U;
    (void)hpka;

    pka_write32(PKA_VERIF_P_LEN,  pbits);
    pka_write32(PKA_VERIF_N_LEN,  nbits);
    pka_write32(PKA_VERIF_A_SIGN, in->coefSign);
    pka_ram_write_be(PKA_VERIF_A,    in->coef,          plen);
    pka_ram_write_be(PKA_VERIF_P,    in->modulus,        plen);
    pka_ram_write_be(PKA_VERIF_GX,   in->basePointX,    plen);
    pka_ram_write_be(PKA_VERIF_GY,   in->basePointY,    plen);
    pka_ram_write_be(PKA_VERIF_QX,   in->pPubKeyCurvePtX, plen);
    pka_ram_write_be(PKA_VERIF_QY,   in->pPubKeyCurvePtY, plen);
    pka_ram_write_be(PKA_VERIF_R,    in->RSign,         nlen);
    pka_ram_write_be(PKA_VERIF_S,    in->SSign,         nlen);
    pka_ram_write_be(PKA_VERIF_HASH, in->hash,          nlen);
    pka_ram_write_be(PKA_VERIF_N,    in->primeOrder,    nlen);

    return pka_run(PKA_MODE_ECDSA_VERIF, timeout);
}

static inline uint32_t HAL_PKA_ECDSAVerif_IsValidSignature(PKA_HandleTypeDef *hpka)
{
    uint32_t res;
    (void)hpka;
    res = pka_read32(PKA_VERIF_RES);
    return (res == PKA_STATUS_OK) ? 1U : 0U;
}

/* The hpka global instance declared in wolfssl stm32.c as "extern PKA_HandleTypeDef hpka" */
/* We define it in main.c */

/* wolfSSL checks for this to detect PKA_V2 (coefB field present) */
/* already defined above: #define PKA_ECC_SCALAR_MUL_IN_B_COEFF 1 */

#endif /* TEST_STM32U585_WOLFSSL_PKA_USER_SETTINGS_H */
