/* Flash writing shared by the bootloader and the firmware. */
#ifndef IAP_H
#define IAP_H

#include "stm8s.h"

#define BLOCK 64

static int8_t nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* Parses 2*n hex digits into out; returns the position after them, or 0 on bad input. */
static const char *hex_decode(const char *p, uint8_t *out, uint8_t n)
{
    while (n--) {
        int8_t hi = nibble(p[0]), lo = hi < 0 ? -1 : nibble(p[1]);
        if (lo < 0)
            return 0;
        *out++ = (uint8_t)(hi << 4 | lo);
        p += 2;
    }
    return p;
}

/* Word programming may run from flash: the CPU stalls until each word is written.
 * Words go last to first, so a vector table's reset entry changes last. */
static uint8_t flash_write_block(uint16_t addr, const uint8_t *data)
{
    volatile uint8_t *p = (volatile uint8_t *)addr;
    FLASH_PUKR = 0x56;
    FLASH_PUKR = 0xAE;
    while (!(FLASH_IAPSR & IAPSR_PUL))
        IWDG_KR = 0xAA;
    for (int8_t i = BLOCK - 4; i >= 0; i -= 4) {
        FLASH_CR2 = CR2_WPRG;
        FLASH_NCR2 = (uint8_t)~CR2_WPRG;
        p[i] = data[i];
        p[i + 1] = data[i + 1];
        p[i + 2] = data[i + 2];
        p[i + 3] = data[i + 3];
        while (!(FLASH_IAPSR & (IAPSR_EOP | IAPSR_WR_PG_DIS)))
            IWDG_KR = 0xAA;
    }
    FLASH_IAPSR &= (uint8_t)~IAPSR_PUL;
    for (uint8_t i = 0; i < BLOCK; i++)
        if (p[i] != data[i])
            return 0;
    return 1;
}

#endif
