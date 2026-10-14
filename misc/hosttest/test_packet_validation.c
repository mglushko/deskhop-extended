/* Exercises the config endpoint packet allowlist off-device. */

#include "main.h"

#include <stdio.h>

static int failures = 0;

static void check(const char *name, bool actual, bool expected) {
    printf("  %s  %s\n", actual == expected ? "PASS" : "FAIL", name);
    if (actual != expected)
        failures++;
}

static bool accepted(uint8_t outer_type, uint8_t inner_type) {
    uart_packet_t packet = {
        .type = outer_type,
        .data = {inner_type},
    };

    return validate_packet(&packet);
}

int main(void) {
    check("an allowed config packet is accepted", accepted(GET_VAL_MSG, 0), true);
    check("a firmware-upgrade packet is rejected", accepted(FIRMWARE_UPGRADE_MSG, 0), false);
    check("an allowed packet can be proxied once", accepted(PROXY_PACKET_MSG, GET_VAL_MSG), true);
    check("a nested proxy packet is rejected", accepted(PROXY_PACKET_MSG, PROXY_PACKET_MSG), false);

    return failures ? 1 : 0;
}
