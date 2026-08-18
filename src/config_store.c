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
 * On-flash representation of the device configuration.
 *
 * Settings are stored as {key, length, value} triples keyed by api_field_map
 * (protocol.c), which names a field rather than its position in config_t. A struct
 * that gains, loses or reorders fields therefore does not invalidate what is already
 * in flash: keys this firmware no longer knows are skipped, keys it has gained keep
 * their default. That is the whole point of the format - a raw struct dump had to be
 * thrown away wholesale on any layout change.
 *
 * Kept free of hardware headers so the format can be exercised on the host; the flash
 * access itself lives in utils.c. See misc/test-config-store.c.
 */

#include "main.h"

size_t config_store_pack(uint8_t *buffer, size_t buffer_len, const uint8_t *base) {
    config_store_header_t *header = (config_store_header_t *)buffer;
    size_t pos = sizeof(config_store_header_t);
    uint8_t count = 0;

    if (buffer_len < sizeof(config_store_header_t) + sizeof(uint32_t))
        return 0;

    memset(buffer, 0, buffer_len);

    for (size_t i = 0; i < get_field_map_length(); i++) {
        const field_map_t *map = get_field_map_index(i);

        if (map->readonly || map->idx == CONFIG_VERSION_KEY)
            continue;

        /* Stop rather than run past the buffer and take the checksum with it. The
           field map fits with room to spare today; this is here so a larger one
           degrades to dropped settings instead of a corrupt page. */
        if (pos + 2 + map->len + sizeof(uint32_t) > buffer_len)
            break;

        buffer[pos++] = (uint8_t)map->idx;
        buffer[pos++] = (uint8_t)map->len;
        memcpy(&buffer[pos], base + map->offset, map->len);
        pos += map->len;
        count++;
    }

    header->magic_header = CONFIG_MAGIC_HEADER;
    header->format       = CONFIG_STORE_FORMAT;
    header->length       = (uint16_t)(pos - sizeof(config_store_header_t));
    header->count        = count;

    uint32_t checksum = calc_crc32(buffer, pos);
    memcpy(&buffer[pos], &checksum, sizeof(checksum));

    return pos + sizeof(checksum);
}

config_store_kind_t config_store_unpack(const uint8_t *stored, size_t stored_len, uint8_t *base) {
    config_store_header_t header;

    if (stored_len < sizeof(config_store_header_t) + sizeof(uint32_t))
        return CONFIG_STORE_NONE;

    memcpy(&header, stored, sizeof(header));

    if (header.magic_header != CONFIG_MAGIC_HEADER)
        return CONFIG_STORE_NONE;

    if (header.format != CONFIG_STORE_FORMAT)
        return CONFIG_STORE_LEGACY;

    size_t end = sizeof(config_store_header_t) + header.length;

    /* The length came out of flash, so it is not to be trusted until checked. */
    if (end + sizeof(uint32_t) > stored_len)
        return CONFIG_STORE_NONE;

    uint32_t stored_checksum;
    memcpy(&stored_checksum, &stored[end], sizeof(stored_checksum));

    if (calc_crc32(stored, end) != stored_checksum)
        return CONFIG_STORE_NONE;

    for (size_t pos = sizeof(config_store_header_t); pos + 2 <= end; ) {
        uint8_t key = stored[pos];
        uint8_t len = stored[pos + 1];

        pos += 2;

        /* Truncated trailing entry - keep what has been applied and stop. */
        if (pos + len > end)
            break;

        const field_map_t *map = get_field_map_entry(key);

        /* Skip anything this firmware does not know, must not restore, or has since
           changed the width of. The default already in place stands in for it. */
        if (map != NULL && !map->readonly && map->len == len && map->idx != CONFIG_VERSION_KEY)
            memcpy(base + map->offset, &stored[pos], len);

        pos += len;
    }

    return CONFIG_STORE_KV;
}

bool config_store_load_legacy(const uint8_t *stored, size_t stored_len, config_t *out) {
    config_t legacy;

    if (stored_len < sizeof(config_t))
        return false;

    memcpy(&legacy, stored, sizeof(config_t));

    if (legacy.magic_header != CONFIG_MAGIC_HEADER || legacy.version != CURRENT_CONFIG_VERSION)
        return false;

    if (legacy.checksum != calc_crc32((uint8_t *)&legacy, sizeof(config_t) - sizeof(uint32_t)))
        return false;

    memcpy(out, &legacy, sizeof(config_t));
    return true;
}
