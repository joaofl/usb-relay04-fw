/* Test firmware for the USB-RELAY04 (STM8S003F3): switches relay 1..4 on in turn,
 * 1 s each, and sends "R<n>\r\n" on UART1 (9600 8N1, to the PL2303) as it does. */

#include <stdint.h>

#define REG(addr) (*(volatile uint8_t *)(addr))

#define PA_ODR REG(0x5000)
#define PA_DDR REG(0x5002)
#define PA_CR1 REG(0x5003)
#define PC_ODR REG(0x500A)
#define PC_DDR REG(0x500C)
#define PC_CR1 REG(0x500D)

#define CLK_CKDIVR REG(0x50C6)

#define UART1_SR   REG(0x5230)
#define UART1_DR   REG(0x5231)
#define UART1_BRR1 REG(0x5232)
#define UART1_BRR2 REG(0x5233)
#define UART1_CR2  REG(0x5235)

#define TIM4_CR1  REG(0x5340)
#define TIM4_SR   REG(0x5344)
#define TIM4_PSCR REG(0x5347)
#define TIM4_ARR  REG(0x5348)

#define PA_RELAYS 0x0E /* PA1..PA3 */
#define PC_RELAYS 0x08 /* PC3 */

static void relay(uint8_t n, uint8_t on)
{
    volatile uint8_t *odr = n < 3 ? &PA_ODR : &PC_ODR;
    uint8_t mask = n < 3 ? (uint8_t)(0x02 << n) : PC_RELAYS;
    if (on)
        *odr |= mask;
    else
        *odr &= (uint8_t)~mask;
}

static void delay_ms(uint16_t ms)
{
    while (ms--) {
        while (!(TIM4_SR & 0x01))
            ;
        TIM4_SR = 0;
    }
}

static void uart_putc(char c)
{
    while (!(UART1_SR & 0x80))
        ;
    UART1_DR = c;
}

void main(void)
{
    CLK_CKDIVR = 0x00; /* HSI 16 MHz, no divider */

    PA_ODR &= (uint8_t)~PA_RELAYS;
    PA_CR1 |= PA_RELAYS;
    PA_DDR |= PA_RELAYS;
    PC_ODR &= (uint8_t)~PC_RELAYS;
    PC_CR1 |= PC_RELAYS;
    PC_DDR |= PC_RELAYS;

    TIM4_PSCR = 7; /* 16 MHz / 128 = 125 kHz */
    TIM4_ARR = 124; /* 1 ms */
    TIM4_CR1 = 0x01;

    /* 16 MHz / 9600 = 1667 = 0x0683; BRR2 must be written first */
    UART1_BRR2 = 0x03;
    UART1_BRR1 = 0x68;
    UART1_CR2 = 0x08; /* TEN */

    for (;;) {
        for (uint8_t n = 0; n < 4; n++) {
            uart_putc('R');
            uart_putc((char)('1' + n));
            uart_putc('\r');
            uart_putc('\n');
            relay(n, 1);
            delay_ms(1000);
            relay(n, 0);
            delay_ms(500);
        }
        delay_ms(2000);
    }
}
