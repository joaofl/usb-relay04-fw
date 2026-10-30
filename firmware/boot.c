/* USB-RELAY04 bootloader, 0x8000-0x87FF. Prints a BOOT line, then starts the application at
 * APP_START unless the application jumped here with BOOT_REQUEST or there is none yet.
 * When it stays, it starts the application after TIMEOUT_MS without a command, if there is one.
 * Commands: version, write <aaaa> <64 bytes hex>, read <aaaa>, run. */

#include "stm8s.h"

#define VERSION "1.1"
#define BLOCK 64
#define TIMEOUT_MS 3000

/* Interrupts run through this vector table: forward each IRQ to the application's entry. */
#define FWD(n, addr) void fwd##n(void) __interrupt(n) __naked { __asm__("jp " #addr); }
FWD(0, 0x8808)  FWD(1, 0x880C)  FWD(2, 0x8810)  FWD(3, 0x8814)  FWD(4, 0x8818)
FWD(5, 0x881C)  FWD(6, 0x8820)  FWD(7, 0x8824)  FWD(8, 0x8828)  FWD(9, 0x882C)
FWD(10, 0x8830) FWD(11, 0x8834) FWD(12, 0x8838) FWD(13, 0x883C) FWD(14, 0x8840)
FWD(15, 0x8844) FWD(16, 0x8848) FWD(17, 0x884C) FWD(18, 0x8850) FWD(19, 0x8854)
FWD(20, 0x8858) FWD(21, 0x885C) FWD(22, 0x8860) FWD(23, 0x8864) FWD(24, 0x8868)
FWD(25, 0x886C) FWD(26, 0x8870) FWD(27, 0x8874) FWD(28, 0x8878) FWD(29, 0x887C)

#define START_APP() __asm__("ldw x, #0x03ff\n ldw sp, x\n jp 0x8800")

static char line[150];
static uint8_t block[BLOCK];

/* The IWDG keeps running when the application started it; refresh it in every wait. */
#define KICK() (IWDG_KR = 0xAA)

static void putch(char c)
{
    while (!(UART1_SR & SR_TXE))
        KICK();
    UART1_DR = c;
}

static void puts_(const char *s)
{
    while (*s)
        putch(*s++);
}

static void reply(const char *s)
{
    puts_(s);
    puts_("\r\n");
}

static uint8_t app_present(void)
{
    return *(volatile uint8_t *)APP_START == 0x82;
}

static void start_app(void)
{
    while (!(UART1_SR & SR_TC))
        KICK();
    UART1_CR2 = 0;
    TIM4_CR1 = 0;
    TIM4_SR = 0;
    START_APP();
}

/* Reads one line into line; returns 0 after TIMEOUT_MS of silence if there is an application. */
static uint8_t gets_(void)
{
    uint8_t n = 0;
    uint16_t idle_ms = 0;
    for (;;) {
        while (!(UART1_SR & SR_RXNE)) {
            KICK();
            if (TIM4_SR & 0x01) {
                TIM4_SR = 0;
                if (++idle_ms >= TIMEOUT_MS && app_present())
                    return 0;
            }
        }
        idle_ms = 0;
        char c = UART1_DR;
        if (c == '\r' || c == '\n') {
            if (n) {
                line[n] = 0;
                return 1;
            }
        } else if (n < sizeof(line) - 1) {
            line[n++] = c;
        }
    }
}

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
static const char *hex(const char *p, uint8_t *out, uint8_t n)
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

/* "<aaaa>" at p: a block-aligned application address; returns 0 if invalid. */
static uint16_t block_addr(const char *p, const char **end)
{
    uint8_t a[2];
    *end = hex(p, a, 2);
    if (!*end)
        return 0;
    uint16_t addr = (uint16_t)(a[0] << 8 | a[1]);
    if (addr % BLOCK || addr < APP_START || addr >= FLASH_END)
        return 0;
    return addr;
}

static uint8_t starts(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return 0;
    return 1;
}

/* Word programming may run from flash: the CPU stalls until each word is written. */
static uint8_t write_block(uint16_t addr)
{
    volatile uint8_t *p = (volatile uint8_t *)addr;
    FLASH_PUKR = 0x56;
    FLASH_PUKR = 0xAE;
    while (!(FLASH_IAPSR & IAPSR_PUL))
        KICK();
    for (uint8_t i = 0; i < BLOCK; i += 4) {
        FLASH_CR2 = CR2_WPRG;
        FLASH_NCR2 = (uint8_t)~CR2_WPRG;
        p[i] = block[i];
        p[i + 1] = block[i + 1];
        p[i + 2] = block[i + 2];
        p[i + 3] = block[i + 3];
        while (!(FLASH_IAPSR & (IAPSR_EOP | IAPSR_WR_PG_DIS)))
            KICK();
    }
    FLASH_IAPSR &= (uint8_t)~IAPSR_PUL;
    for (uint8_t i = 0; i < BLOCK; i++)
        if (p[i] != block[i])
            return 0;
    return 1;
}

static void cmd_write(void)
{
    const char *p;
    uint16_t addr = block_addr(line + 6, &p);
    if (!addr || *p != ' ' || !(p = hex(p + 1, block, BLOCK)) || *p) {
        reply("ERR usage: write <aaaa> <64 bytes hex>");
        return;
    }
    reply(write_block(addr) ? "OK" : "ERR verify");
}

static void cmd_read(void)
{
    const char *p;
    uint16_t addr = block_addr(line + 5, &p);
    if (!addr || *p) {
        reply("ERR usage: read <aaaa>");
        return;
    }
    puts_("OK ");
    for (uint8_t i = 0; i < BLOCK; i++) {
        uint8_t b = ((volatile uint8_t *)addr)[i];
        putch("0123456789abcdef"[b >> 4]);
        putch("0123456789abcdef"[b & 15]);
    }
    reply("");
}

void main(void)
{
    uint8_t requested = TIM4_ARR == BOOT_REQUEST;

    clock_and_uart_init(CR2_TEN | CR2_REN);
    if (!requested && app_present()) {
        reply("BOOT relay04-boot " VERSION ", starting application");
        start_app();
    }

    TIM4_PSCR = 7; /* 125 kHz */
    TIM4_ARR = 124; /* 1 ms */
    TIM4_EGR = 0x01;
    TIM4_SR = 0;
    TIM4_CR1 = 0x01;

    if (app_present())
        reply("BOOT relay04-boot " VERSION ", application starts after 3 s without a command");
    else
        reply("BOOT relay04-boot " VERSION ", no application");

    for (;;) {
        if (!gets_()) {
            reply("BOOT timeout, starting application");
            start_app();
        }
        if (starts(line, "version"))
            reply("OK relay04-boot " VERSION);
        else if (starts(line, "write "))
            cmd_write();
        else if (starts(line, "read "))
            cmd_read();
        else if (starts(line, "run")) {
            if (!app_present()) {
                reply("ERR no application");
                continue;
            }
            reply("OK");
            start_app();
        } else
            reply("ERR commands: version, write <aaaa> <hex>, read <aaaa>, run");
    }
}
