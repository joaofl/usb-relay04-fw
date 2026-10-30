/* Relay firmware, from APP_START. Lines at 115200 8N1, replies "OK ..." or "ERR ...".
 * <bits> is one character per relay, relay 1 first, 1 = on:
 *   on|off <relays>      relays: "all" or numbers, e.g. "1 3" or "13"
 *   set <bits>           all relays at once
 *   pulse <relays> <t>   on for t (ms, or "10s"), then off; sends "DONE <n> <bits>"
 *   status               "OK <bits> [t<n>=<ms left>] up=<s> reset=<cause>"
 *   version              "OK relay04 <version> <board> <relays>"
 *   boot                 restarts into the bootloader for a firmware update
 *   h, help, ?           command list with examples, then "OK"
 * Prints "READY relay04 <version> <board> <relays> reset=<cause>" at start; all relays start off.
 * Input is echoed like a terminal: replies and notices start on a line of their own. */

#include "board.h"

#define VERSION "1.2.0"
#define MAX_PULSE_MS 86400000UL

static volatile uint32_t ticks;
static volatile char rx[64];
static volatile uint8_t rx_head, rx_tail;

static char line[48];
static uint8_t line_len;
static uint8_t after_cr;
static uint8_t state;     /* bit n = relay n+1 on */
static uint8_t pulsing;   /* relays with a pulse running */
static uint32_t pulse_end[NRELAYS];
static const char *reset_cause;

void tim4_isr(void) __interrupt(IRQ_TIM4)
{
    TIM4_SR = 0;
    ticks++;
}

void uart_rx_isr(void) __interrupt(IRQ_UART1_RX)
{
    char c = UART1_DR;
    uint8_t next = (rx_head + 1) & (sizeof(rx) - 1);
    if (next != rx_tail) {
        rx[rx_head] = c;
        rx_head = next;
    }
}

static uint32_t now(void)
{
    uint32_t t;
    __asm__("sim");
    t = ticks;
    __asm__("rim");
    return t;
}

static void apply(void)
{
    relay_pins_write(state);
}

static void putch(char c)
{
    while (!(UART1_SR & SR_TXE))
        ;
    UART1_DR = c;
}

static void puts_(const char *s)
{
    while (*s)
        putch(*s++);
}

static void putu(uint32_t v)
{
    char buf[10];
    uint8_t n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        putch(buf[--n]);
}

static void put_state(void)
{
    for (uint8_t i = 0; i < NRELAYS; i++)
        putch(state & (1 << i) ? '1' : '0');
}

static void reply_state(void)
{
    puts_("OK ");
    put_state();
    puts_("\r\n");
}

static void error(const char *msg)
{
    puts_("ERR ");
    puts_(msg);
    puts_("\r\n");
}

static uint8_t equal(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void put_id(void)
{
    puts_("relay04 " VERSION " " BOARD_NAME " ");
    putu(NRELAYS);
}

static void cmd_help(void)
{
    puts_("relays: 1-");
    putu(NRELAYS);
    puts_(" or all; states: one digit per relay, relay 1 first, 1 = on\r\n"
          "  on 1 2         switch relays 1 and 2 on\r\n"
          "  off all        switch every relay off\r\n"
          "  set ");
    for (uint8_t i = 0; i < NRELAYS; i++)
        putch(i & 1 ? '1' : '0');
    puts_("       set every relay at once\r\n"
          "  pulse 1 10s    relay 1 on for 10 s, then \"DONE 1 <state>\"\r\n"
          "  pulse 2 500ms  same in ms, up to 24h\r\n"
          "  status         state, ms left on pulses, uptime, reset cause\r\n"
          "  version        firmware, board, number of relays\r\n"
          "  boot           start the bootloader, for a firmware update\r\n"
          "OK\r\n");
}

/* "all", or relay numbers 1..NRELAYS, optionally separated by spaces or commas; 0 if invalid. */
static uint8_t parse_relays(const char *p)
{
    uint8_t mask = 0;
    if (equal(p, "all"))
        return (uint8_t)((1u << NRELAYS) - 1);
    for (; *p; p++) {
        if (*p >= '1' && *p < '1' + NRELAYS)
            mask |= (uint8_t)(1 << (*p - '1'));
        else if (*p != ' ' && *p != ',')
            return 0;
    }
    return mask;
}

/* "<n>", "<n>ms" or "<n>s"; 0 if invalid or above MAX_PULSE_MS. */
static uint32_t parse_duration(const char *p)
{
    uint32_t v = 0;
    uint8_t digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 8)
            return 0;
        v = v * 10 + (uint8_t)(*p++ - '0');
    }
    if (!digits)
        return 0;
    if (equal(p, "s")) {
        if (v > MAX_PULSE_MS / 1000)
            return 0;
        v *= 1000;
    } else if (!equal(p, "") && !equal(p, "ms")) {
        return 0;
    }
    return v <= MAX_PULSE_MS ? v : 0;
}

static void cmd_status(void)
{
    uint32_t t = now();
    puts_("OK ");
    put_state();
    for (uint8_t i = 0; i < NRELAYS; i++) {
        if (pulsing & (1 << i)) {
            puts_(" t");
            putch((char)('1' + i));
            putch('=');
            putu(pulse_end[i] - t);
        }
    }
    puts_(" up=");
    putu(t / 1000);
    puts_(" reset=");
    puts_(reset_cause);
    puts_("\r\n");
}

static void cmd_pulse(char *args)
{
    char *last = 0;
    for (char *p = args; *p; p++)
        if (*p == ' ')
            last = p;
    if (!last) {
        error("usage: pulse <relays> <ms|Ns>");
        return;
    }
    *last = 0;
    uint8_t mask = parse_relays(args);
    uint32_t ms = parse_duration(last + 1);
    if (!mask || !ms) {
        error("usage: pulse <relays> <ms|Ns>, at most 24h");
        return;
    }
    uint32_t end = now() + ms;
    for (uint8_t i = 0; i < NRELAYS; i++)
        if (mask & (1 << i))
            pulse_end[i] = end;
    pulsing |= mask;
    state |= mask;
    apply();
    reply_state();
}

static void cmd_set(const char *bits)
{
    uint8_t s = 0;
    for (uint8_t i = 0; i < NRELAYS; i++) {
        if (bits[i] == '1')
            s |= (uint8_t)(1 << i);
        else if (bits[i] != '0')
            break;
        if (i == NRELAYS - 1 && !bits[i + 1]) {
            state = s;
            pulsing = 0;
            apply();
            reply_state();
            return;
        }
    }
    error("usage: set <one 0|1 per relay, relay 1 first>");
}

static void handle(char *cmd)
{
    char *args = cmd;
    while (*args && *args != ' ')
        args++;
    if (*args)
        *args++ = 0;
    while (*args == ' ')
        args++;

    if (equal(cmd, "on") || equal(cmd, "off")) {
        uint8_t mask = parse_relays(args);
        if (!mask) {
            error("usage: on|off <relays>, e.g. on 1 3, off all");
            return;
        }
        pulsing &= (uint8_t)~mask;
        if (cmd[1] == 'n')
            state |= mask;
        else
            state &= (uint8_t)~mask;
        apply();
        reply_state();
    } else if (equal(cmd, "set")) {
        cmd_set(args);
    } else if (equal(cmd, "pulse")) {
        cmd_pulse(args);
    } else if (equal(cmd, "status")) {
        cmd_status();
    } else if (equal(cmd, "h") || equal(cmd, "help") || equal(cmd, "?")) {
        cmd_help();
    } else if (equal(cmd, "version")) {
        puts_("OK ");
        put_id();
        puts_("\r\n");
    } else if (equal(cmd, "boot")) {
        state = pulsing = 0;
        apply();
        puts_("OK boot\r\n");
        while (!(UART1_SR & SR_TC))
            ;
        /* A jump, not a reset: a reset under an attached SWIM probe stalls the CPU. */
        __asm__("sim");
        UART1_CR2 = 0;
        TIM4_CR1 = 0;
        TIM4_IER = 0;
        TIM4_ARR = BOOT_REQUEST;
        __asm__("ldw x, #0x03ff\n ldw sp, x\n jp 0x8000");
    } else {
        error("unknown command, h for help");
    }
}

static void finish_pulses(void)
{
    uint32_t t = now();
    for (uint8_t i = 0; i < NRELAYS; i++) {
        uint8_t bit = (uint8_t)(1 << i);
        if ((pulsing & bit) && (int32_t)(t - pulse_end[i]) >= 0) {
            pulsing &= (uint8_t)~bit;
            state &= (uint8_t)~bit;
            apply();
            if (line_len)
                puts_("\r\n");
            puts_("DONE ");
            putch((char)('1' + i));
            putch(' ');
            put_state();
            puts_("\r\n");
            for (uint8_t j = 0; j < line_len; j++)
                putch(line[j]);
        }
    }
}

static void poll_rx(void)
{
    while (rx_tail != rx_head) {
        char c = rx[rx_tail];
        rx_tail = (rx_tail + 1) & (sizeof(rx) - 1);
        /* "\r\n" from a program is one line end, not an extra empty line */
        uint8_t lf_after_cr = c == '\n' && after_cr;
        after_cr = c == '\r';
        if (lf_after_cr)
            continue;
        if (c == '\r' || c == '\n') {
            puts_("\r\n");
            while (line_len && line[line_len - 1] == ' ')
                line_len--;
            line[line_len] = 0;
            uint8_t len = line_len;
            line_len = 0;
            if (len)
                handle(line);
        } else if (c == '\b' || c == 0x7F) {
            if (line_len) {
                line_len--;
                puts_("\b \b");
            }
        } else if (c == 0x03) {
            line_len = 0;
            puts_("^C\r\n");
        } else if (c >= ' ' && c <= '~' && line_len < sizeof(line) - 1) {
            putch(c);
            line[line_len++] = c >= 'A' && c <= 'Z' ? c + 32 : c;
        }
    }
}

void main(void)
{
    uint8_t rst = RST_SR;
    RST_SR = 0x1F;
    reset_cause = rst & RST_IWDGF ? "watchdog" : rst & RST_ILLOPF ? "illegal-opcode"
                : rst & RST_EMCF ? "emc" : rst & RST_SWIMF ? "swim" : "power";

    /* ~0.5 s: LSI 128 kHz / 256, reload 255 */
    IWDG_KR = 0xCC;
    IWDG_KR = 0x55;
    IWDG_PR = 6;
    IWDG_RLR = 0xFF;
    IWDG_KR = 0xAA;

    relay_pins_init();

    TIM4_PSCR = 7; /* 125 kHz */
    TIM4_ARR = 124; /* 1 ms */
    TIM4_IER = 0x01;
    TIM4_CR1 = 0x01;

    clock_and_uart_init(CR2_RIEN | CR2_TEN | CR2_REN);
    __asm__("rim");

    puts_("READY ");
    put_id();
    puts_(" reset=");
    puts_(reset_cause);
    puts_("\r\n");

    for (;;) {
        IWDG_KR = 0xAA;
        finish_pulses();
        poll_rx();
    }
}
