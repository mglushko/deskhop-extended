/* Exercises the on-flash config format off-device.
 *
 * Builds the real src/config_store.c against the real api_field_map, with a byte array
 * standing in for the flash page. Run it with misc/hosttest/run.sh. */

#include "main.h"

#include <stdio.h>

static int failures = 0;

static void check(const char *name, int ok, const char *detail) {
    printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name,
           ok ? "" : "  <- ", ok ? "" : (detail ? detail : ""));
    if (!ok)
        failures++;
}

/* api_field_map offsets are into device_t, so that is what pack and unpack read and
   write through - exactly as handle_api_msgs does. */
static device_t device_a, device_b;
static uint8_t page[FLASH_PAGE_SIZE];

/* Give every writable field a distinct value so a mix-up cannot go unnoticed. */
static void fill(device_t *state, uint8_t seed) {
    for (size_t i = 0; i < get_field_map_length(); i++) {
        const field_map_t *map = get_field_map_index(i);

        if (map->readonly)
            continue;

        for (uint32_t b = 0; b < map->len; b++)
            ((uint8_t *)state)[map->offset + b] = (uint8_t)(seed + i * 7 + b);
    }
}

static int writable_fields_match(const device_t *x, const device_t *y, int skip_version) {
    for (size_t i = 0; i < get_field_map_length(); i++) {
        const field_map_t *map = get_field_map_index(i);

        if (map->readonly || (skip_version && map->idx == CONFIG_VERSION_KEY))
            continue;

        if (memcmp((const uint8_t *)x + map->offset, (const uint8_t *)y + map->offset, map->len))
            return 0;
    }
    return 1;
}

int main(void) {
    char detail[128];

    /* ---- round trip ---------------------------------------------------- */
    fill(&device_a, 11);
    size_t used = config_store_pack(page, sizeof(page), (const uint8_t *)&device_a);

    snprintf(detail, sizeof(detail), "%zu bytes of %d", used, FLASH_PAGE_SIZE);
    check("worst case payload fits one flash page", used > 0 && used <= FLASH_PAGE_SIZE, detail);

    fill(&device_b, 200);
    check("unpack reports a key-value page",
          config_store_unpack(page, sizeof(page), (uint8_t *)&device_b) == CONFIG_STORE_KV, "");
    check("round trip restores every writable field",
          writable_fields_match(&device_a, &device_b, 1), "");

    /* ---- config.version is never stored, so never restored -------------- */
    device_a.config.version = 9;
    device_b.config.version = CURRENT_CONFIG_VERSION;
    config_store_pack(page, sizeof(page), (const uint8_t *)&device_a);
    config_store_unpack(page, sizeof(page), (uint8_t *)&device_b);
    check("config.version is left alone by a restore",
          device_b.config.version == CURRENT_CONFIG_VERSION, "");

    /* ---- a key this firmware does not know is skipped ------------------- */
    fill(&device_a, 33);
    used = config_store_pack(page, sizeof(page), (const uint8_t *)&device_a);
    {
        config_store_header_t h;
        memcpy(&h, page, sizeof(h));

        size_t at = sizeof(config_store_header_t) + h.length;
        page[at] = 233;      /* key no api_field_map entry claims */
        page[at + 1] = 1;
        page[at + 2] = 0x5A;
        h.length += 3;
        h.count += 1;
        memcpy(page, &h, sizeof(h));

        size_t end = sizeof(config_store_header_t) + h.length;
        uint32_t crc = calc_crc32(page, end);
        memcpy(&page[end], &crc, sizeof(crc));

        device_t before = device_b;
        check("unknown key does not abort the page",
              config_store_unpack(page, sizeof(page), (uint8_t *)&device_b) == CONFIG_STORE_KV, "");
        check("known keys still applied alongside an unknown one",
              writable_fields_match(&device_a, &device_b, 1), "");
        (void)before;
    }

    /* ---- a key absent from the page keeps whatever was there ------------ */
    {
        fill(&device_a, 77);
        config_store_pack(page, sizeof(page), (const uint8_t *)&device_a);

        /* Drop the first entry by walking it out of the payload. */
        config_store_header_t h;
        memcpy(&h, page, sizeof(h));
        size_t first = sizeof(config_store_header_t);
        uint8_t dropped_key = page[first], dropped_len = page[first + 1];
        size_t entry = 2 + dropped_len;
        size_t end = sizeof(config_store_header_t) + h.length;

        memmove(&page[first], &page[first + entry], end - first - entry);
        h.length -= entry;
        h.count -= 1;
        memcpy(page, &h, sizeof(h));
        end = sizeof(config_store_header_t) + h.length;
        uint32_t crc = calc_crc32(page, end);
        memcpy(&page[end], &crc, sizeof(crc));

        const field_map_t *map = get_field_map_entry(dropped_key);
        memset((uint8_t *)&device_b + map->offset, 0xC7, map->len);

        config_store_unpack(page, sizeof(page), (uint8_t *)&device_b);

        int untouched = 1;
        for (uint32_t b = 0; b < map->len; b++)
            untouched &= ((uint8_t *)&device_b)[map->offset + b] == 0xC7;

        check("a missing key leaves the existing value in place", untouched, "");
    }

    /* ---- corruption is rejected wholesale ------------------------------- */
    fill(&device_a, 5);
    config_store_pack(page, sizeof(page), (const uint8_t *)&device_a);
    page[sizeof(config_store_header_t) + 4] ^= 0xFF;
    check("a bad checksum yields nothing usable",
          config_store_unpack(page, sizeof(page), (uint8_t *)&device_b) == CONFIG_STORE_NONE, "");

    config_store_pack(page, sizeof(page), (const uint8_t *)&device_a);
    page[0] ^= 0xFF;
    check("a bad magic header yields nothing usable",
          config_store_unpack(page, sizeof(page), (uint8_t *)&device_b) == CONFIG_STORE_NONE, "");

    {   /* A length past the end of the page must not be believed. */
        config_store_pack(page, sizeof(page), (const uint8_t *)&device_a);
        config_store_header_t h;
        memcpy(&h, page, sizeof(h));
        h.length = FLASH_PAGE_SIZE;
        memcpy(page, &h, sizeof(h));
        check("an oversized length is rejected",
              config_store_unpack(page, sizeof(page), (uint8_t *)&device_b) == CONFIG_STORE_NONE, "");
    }

    /* ---- migration from the pre key-value layout ------------------------ */
    {
        config_t legacy;
        memset(&legacy, 0, sizeof(legacy));
        legacy.magic_header = CONFIG_MAGIC_HEADER;
        legacy.version = CURRENT_CONFIG_VERSION;
        legacy.jump_threshold = 1234;
        legacy.output[1].screen_count = 3;
        legacy.checksum = calc_crc32((uint8_t *)&legacy, sizeof(config_t) - sizeof(uint32_t));

        memset(page, 0, sizeof(page));
        memcpy(page, &legacy, sizeof(legacy));

        check("a pre key-value page is recognised as legacy",
              config_store_unpack(page, sizeof(page), (uint8_t *)&device_b) == CONFIG_STORE_LEGACY, "");

        config_t out;
        memset(&out, 0, sizeof(out));
        check("legacy page loads", config_store_load_legacy(page, sizeof(page), &out), "");
        check("legacy values survive the migration",
              out.jump_threshold == 1234 && out.output[1].screen_count == 3, "");

        legacy.checksum ^= 0xFF;
        memcpy(page, &legacy, sizeof(legacy));
        check("a corrupt legacy page is refused",
              !config_store_load_legacy(page, sizeof(page), &out), "");
    }

    /* ---- an erased sector reads as nothing ------------------------------ */
    memset(page, 0xFF, sizeof(page));
    check("an erased page yields nothing usable",
          config_store_unpack(page, sizeof(page), (uint8_t *)&device_b) == CONFIG_STORE_NONE, "");

    printf("\n%s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}
