/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "structs.h"

/* What a stored config page turned out to hold. */
typedef enum {
    CONFIG_STORE_NONE = 0, /* nothing usable - the caller's defaults stand */
    CONFIG_STORE_KV,       /* key-value page, applied onto the target */
    CONFIG_STORE_LEGACY,   /* a raw config_t from before the key-value format */
} config_store_kind_t;

/* Serialise the writable half of api_field_map, read through base, into buffer.
   Returns the number of bytes used, including the trailing checksum. */
size_t config_store_pack(uint8_t *buffer, size_t buffer_len, const uint8_t *base);

/* Apply a stored page onto base. Only CONFIG_STORE_KV writes anything; the caller
   handles CONFIG_STORE_LEGACY with config_store_load_legacy. */
config_store_kind_t config_store_unpack(const uint8_t *stored, size_t stored_len, uint8_t *base);

/* Validate a pre key-value page and copy it out. False if it fails its own checks. */
bool config_store_load_legacy(const uint8_t *stored, size_t stored_len, config_t *out);
