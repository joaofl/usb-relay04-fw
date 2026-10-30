/* STM8S003F3 registers used by the relay firmware and bootloader (RM0016). */
#ifndef STM8S_H
#define STM8S_H

#include <stdint.h>

#define REG(addr) (*(volatile uint8_t *)(addr))

#define PA_ODR REG(0x5000)
#define PA_DDR REG(0x5002)
#define PA_CR1 REG(0x5003)
#define PC_ODR REG(0x500A)
#define PC_DDR REG(0x500C)
#define PC_CR1 REG(0x500D)

#define FLASH_CR2   REG(0x505B)
#define FLASH_NCR2  REG(0x505C)
#define FLASH_IAPSR REG(0x505F)
#define FLASH_PUKR  REG(0x5062)
#define IAPSR_WR_PG_DIS 0x01
#define IAPSR_PUL       0x02
#define IAPSR_EOP       0x04
#define CR2_WPRG        0x40

#define RST_SR     REG(0x50B3)
#define RST_WWDGF  0x01
#define RST_IWDGF  0x02
#define RST_ILLOPF 0x04
#define RST_SWIMF  0x08
#define RST_EMCF   0x10

#define CLK_CKDIVR REG(0x50C6)

#define IWDG_KR  REG(0x50E0)
#define IWDG_PR  REG(0x50E1)
#define IWDG_RLR REG(0x50E2)

#define UART1_SR   REG(0x5230)
#define UART1_DR   REG(0x5231)
#define UART1_BRR1 REG(0x5232)
#define UART1_BRR2 REG(0x5233)
#define UART1_CR2  REG(0x5235)
#define SR_TXE  0x80
#define SR_TC   0x40
#define SR_RXNE 0x20
#define CR2_RIEN 0x20
#define CR2_TEN  0x08
#define CR2_REN  0x04

#define TIM4_CR1  REG(0x5340)
#define TIM4_IER  REG(0x5343)
#define TIM4_SR   REG(0x5344)
#define TIM4_PSCR REG(0x5347)
#define TIM4_ARR  REG(0x5348)

#define IRQ_UART1_RX 18
#define IRQ_TIM4     23

#define APP_START 0x8800u
#define FLASH_END 0xA000u

/* The application jumps to the bootloader with this in TIM4_ARR (0xFF after any reset). */
#define BOOT_REQUEST 0xB0

/* 16 MHz HSI, 115200 8N1: divider 139 = 0x008B */
static inline void clock_and_uart_init(uint8_t cr2)
{
    CLK_CKDIVR = 0x00;
    UART1_BRR2 = 0x0B;
    UART1_BRR1 = 0x08;
    UART1_CR2 = cr2;
}

#endif
