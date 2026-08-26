/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 *
 * How often the host port polls an interrupt endpoint. See usb_polling.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

uint8_t usb_polling_interval(uint8_t attr,
                             uint8_t epaddr,
                             uint8_t declared,
                             bool full_speed,
                             bool force_fast);
