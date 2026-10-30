/* Wiring test: switches each relay of the board on in turn, 1 s each, and prints
 * "R<n>" at 115200 8N1 as it does. Runs from 0x8000, without the bootloader. */

#include "board.h"

static void delay_ms(uint16_t ms)
{
    while (ms--) {
        while (!(TIM4_SR & 0x01))
            ;
        TIM4_SR = 0;
    }
}

static void putch(char c)
{
    while (!(UART1_SR & SR_TXE))
        ;
    UART1_DR = c;
}

void main(void)
{
    relay_pins_init();

    TIM4_PSCR = 7; /* 125 kHz */
    TIM4_ARR = 124; /* 1 ms */
    TIM4_CR1 = 0x01;

    clock_and_uart_init(CR2_TEN);

    for (;;) {
        for (uint8_t n = 0; n < NRELAYS; n++) {
            putch('R');
            putch((char)('1' + n));
            putch('\r');
            putch('\n');
            relay_pins_write((uint8_t)(1u << n));
            delay_ms(1000);
            relay_pins_write(0);
            delay_ms(500);
        }
        delay_ms(2000);
    }
}
