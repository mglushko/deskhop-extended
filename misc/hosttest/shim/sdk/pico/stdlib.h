#pragma once
/* Stand-in for the SDK's stdlib. Only the clock, which keyboard.c stamps activity with. */

#include <stdint.h>

uint64_t time_us_64(void);
