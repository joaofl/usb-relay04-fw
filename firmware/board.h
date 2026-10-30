/* Relay pin table of the board selected with -DBOARD_H="boards/<name>.h" (make BOARD=<name>). */
#ifndef BOARD_INCLUDED
#define BOARD_INCLUDED

#include "stm8s.h"

#ifndef BOARD_H
#error "build with make BOARD=<name>, see firmware/boards/"
#endif

struct relay_pin {
    uint16_t port;
    uint8_t mask;
};

#define PIN(port, bit) { PORT_##port, 1u << (bit) }

#include BOARD_H

static const struct relay_pin relay_pins[] = RELAY_PINS;

#define NRELAYS (sizeof(relay_pins) / sizeof(relay_pins[0]))

/* Relay states are one bit each in a uint8_t. */
typedef char nrelays_at_most_8[NRELAYS <= 8 ? 1 : -1];

static void relay_pins_init(void)
{
    for (uint8_t i = 0; i < NRELAYS; i++) {
        uint16_t p = relay_pins[i].port;
        uint8_t m = relay_pins[i].mask;
        REG(p) &= (uint8_t)~m;
        REG(p + 3) |= m;
        REG(p + 2) |= m;
    }
}

/* bit n of state = relay n+1 on */
static void relay_pins_write(uint8_t state)
{
    for (uint8_t i = 0; i < NRELAYS; i++) {
        uint16_t p = relay_pins[i].port;
        uint8_t m = relay_pins[i].mask;
        if (state & (1u << i))
            REG(p) |= m;
        else
            REG(p) &= (uint8_t)~m;
    }
}

#endif
