#include "hid_input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Synthetic HID fixtures, not captured from an L1241. */
static const uint8_t descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, /* Gamepad application */
    0x85, 0x01,                         /* report ID 1, OUT OF BAND for BLE */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0c, /* 12 buttons */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0c, 0x81, 0x02,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03, /* 4 padding bits */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, /* signed X/Y */
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0x09, 0x39, 0x15, 0x01, 0x25, 0x08, /* hat 1..8, neutral 0 */
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x75, 0x04, 0x81, 0x03,
    0x85, 0x02, 0x09, 0x32,             /* report 2: unsigned 16-bit Z */
    0x15, 0x00, 0x26, 0xff, 0xff, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
    0xc0,
};

static void test_reports(void)
{
    hid_input_map_t map;
    hid_input_state_t state = {.hat = -1};
    assert(hid_input_parse(&map, descriptor, sizeof(descriptor)));
    assert(map.count == 16 && map.report_bits[1] == 40 && map.report_bits[2] == 16);
    /* First payload byte is 1, equal to report ID: must NOT strip it. */
    const uint8_t input[] = {1, 8, 0x81, 0x7f, 8};
    assert(hid_input_decode(&map, 1, input, sizeof(input), &state));
    assert(state.buttons == 0x801 && state.axes[0] == -127 && state.axes[1] == 127);
    assert(state.hat == 8 && state.axes_present == 3);
    hid_input_state_t before = state;
    assert(!hid_input_decode(&map, 1, input, 4, &state));
    assert(!memcmp(&before, &state, sizeof(state)));
    assert(!hid_input_decode(&map, 7, input, sizeof(input), &state));
    const uint8_t trigger[] = {0xfe, 0xff};
    assert(hid_input_decode(&map, 2, trigger, 2, &state));
    assert(state.axes[2] == 65534 && state.buttons == 0x801 && state.axes_present == 7);
    const uint8_t released[] = {0, 0, 0, 0, 0};
    assert(hid_input_decode(&map, 1, released, 5, &state));
    assert(state.buttons == 0 && state.hat == -1 && state.axes[2] == 65534);
}

static void test_bit_fields_and_push_pop(void)
{
    const uint8_t desc[] = {
        0x05, 1, 0x09, 4, 0xa1, 1,
        0x15, 0, 0x25, 1, 0x75, 3, 0x95, 1, 0x81, 3, /* padding */
        0x75, 0x0c, 0x16, 0x00, 0xf8, 0x26, 0xff, 7,
        0x09, 0x30, 0x81, 2, /* unaligned signed 12-bit X */
        0xa4,               /* Push globals */
        0x05, 9, 0x09, 1, 0x75, 1, 0x15, 0, 0x25, 1, 0x81, 2,
        0xb4,               /* Pop restores page, size, range */
        0x09, 0x31, 0x81, 2, 0xc0,
    };
    hid_input_map_t map;
    hid_input_state_t state = {.hat = -1};
    assert(hid_input_parse(&map, desc, sizeof(desc)));
    assert(map.count == 3 && map.fields[2].bit_offset == 16);
    const uint8_t input[] = {0xf8, 0xff, 0xff, 0x07};
    assert(hid_input_decode(&map, 0, input, sizeof(input), &state));
    assert(state.axes[0] == -1 && state.axes[1] == 2047 && state.buttons == 1);
}

static void test_collection_and_arrays(void)
{
    const uint8_t desc[] = {
        0x05, 1, 0x09, 5, 0xa1, 1,
        0x05, 9, 0x19, 1, 0x29, 8, 0x15, 0, 0x25, 8,
        0x75, 8, 0x95, 1, 0x81, 0, /* Button array: raw only, advances offset */
        0x05, 1, 0x09, 0x90, 0x25, 1, 0x75, 1, 0x81, 2,
        0xc0,
        0x05, 1, 0x09, 2, 0xa1, 1,  /* Mouse, must not be exposed as gamepad X */
        0x09, 0x30, 0x75, 8, 0x81, 2, 0xc0,
    };
    hid_input_map_t map;
    hid_input_state_t state = {.hat = -1};
    assert(hid_input_parse(&map, desc, sizeof(desc)));
    assert(map.count == 1 && map.skipped_fields == 1 && map.fields[0].bit_offset == 8);
    const uint8_t input[] = {7, 1, 0};
    assert(hid_input_decode(&map, 0, input, 3, &state));
    assert(state.dpad == 1 && state.buttons == 0 && state.axes_present == 0);
}

static void test_malformed(void)
{
    hid_input_map_t map;
    assert(!hid_input_parse(NULL, descriptor, sizeof(descriptor)));
    assert(!hid_input_parse(&map, NULL, 1));
    for (size_t n = 0; n < sizeof(descriptor); ++n) {
        assert(!hid_input_parse(&map, descriptor, n));
        assert(map.count == 0);
    }
    const uint8_t pop[] = {0xb4};
    const uint8_t long_item[] = {0xfe, 10, 0, 1};
    const uint8_t huge[] = {0x75, 32, 0x97, 0xff, 0xff, 0xff, 0xff, 0x81, 2};
    const uint8_t bad_id[] = {0x85, 0};
    const uint8_t bad_size[] = {0x75, 33, 0x95, 1, 0x81, 2};
    assert(!hid_input_parse(&map, pop, sizeof(pop)));
    assert(!hid_input_parse(&map, long_item, sizeof(long_item)));
    assert(!hid_input_parse(&map, huge, sizeof(huge)));
    assert(!hid_input_parse(&map, bad_id, sizeof(bad_id)));
    assert(!hid_input_parse(&map, bad_size, sizeof(bad_size)));
}

static void test_random_maps(void)
{
    uint32_t seed = 0x1241;
    uint8_t bytes[128];
    hid_input_map_t map;
    for (unsigned n = 0; n < 20000; ++n) {
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            seed = seed * 1664525u + 1013904223u;
            bytes[i] = seed >> 24;
        }
        size_t len = seed % sizeof(bytes);
        if (hid_input_parse(&map, bytes, len)) {
            hid_input_state_t state = {.hat = -1};
            for (unsigned id = 0; id < 256; ++id)
                hid_input_decode(&map, id, bytes, sizeof(bytes), &state);
        }
    }
}

int main(void)
{
    test_reports();
    test_bit_fields_and_push_pop();
    test_collection_and_arrays();
    test_malformed();
    test_random_maps();
    puts("PASS: HID reports, bit fields, signed axes, report IDs, collections, malformed maps, 20000 random maps");
    return 0;
}
