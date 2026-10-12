#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_INPUT_MAX_FIELDS 96
#define HID_INPUT_MAX_REPORT_BYTES 64

/* Pure C, independent of the radio and ESP-IDF. Report IDs are supplied by the
 * BLE Report Reference descriptor and are NOT stripped from the payload. */
typedef struct {
    uint16_t bit_offset;
    uint16_t usage_page;
    uint16_t usage;
    int32_t logical_min;
    int32_t logical_max;
    uint8_t report_id;
    uint8_t bit_size;
} hid_input_field_t;

typedef struct {
    hid_input_field_t fields[HID_INPUT_MAX_FIELDS];
    uint16_t report_bits[256];
    size_t count;
    unsigned skipped_fields;
} hid_input_map_t;

typedef struct {
    uint32_t buttons;             /* HID Button usage 1 -> bit 0, etc. */
    int32_t axes[6];              /* Generic Desktop X, Y, Z, Rx, Ry, Rz */
    uint8_t axes_present;
    int32_t hat;                  /* Raw HID value; -1 means neutral/absent. */
    uint8_t dpad;                 /* bit 0 up, 1 down, 2 right, 3 left */
} hid_input_state_t;

bool hid_input_parse(hid_input_map_t *map, const uint8_t *data, size_t length);
bool hid_input_decode(const hid_input_map_t *map, uint8_t report_id,
                      const uint8_t *data, size_t length, hid_input_state_t *state);
