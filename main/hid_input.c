#include "hid_input.h"
#include <limits.h>
#include <string.h>

typedef struct {
    uint32_t page, size, count;
    int32_t min, max;
    uint8_t id;
} globals_t;

static int32_t signed_item(uint32_t v, unsigned bytes)
{
    if (bytes == 1) return (int8_t)v;
    if (bytes == 2) return (int16_t)v;
    return (int32_t)v;
}

static uint32_t full_usage(uint32_t value, unsigned bytes, uint32_t page)
{
    return bytes == 4 ? value : (page << 16) | value;
}

static bool parse(hid_input_map_t *map, const uint8_t *data, size_t length)
{
    globals_t g = {0}, stack[4];
    size_t sp = 0, depth = 0, usage_count = 0;
    bool collection_gamepad[16] = {0}, gamepad = false, have_min = false;
    uint32_t usages[32], usage_min = 0, usage_max = 0;
    for (size_t at = 0; at < length;) {
        uint8_t prefix = data[at++];
        if (prefix == 0xfe) { /* Bounded skip of a HID long item. */
            if (length - at < 2) return false;
            size_t bytes = data[at];
            at += 2;
            if (bytes > length - at) return false;
            at += bytes;
            continue;
        }
        unsigned bytes = prefix & 3;
        if (bytes == 3) bytes = 4;
        if (bytes > length - at) return false;
        uint32_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value |= (uint32_t)data[at++] << (8 * i);
        unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: if (value > UINT16_MAX) return false; g.page = value; break;
            case 1: g.min = signed_item(value, bytes); break;
            case 2:
                if (g.min >= 0 && value > INT32_MAX) return false;
                g.max = g.min < 0 ? signed_item(value, bytes) : (int32_t)value;
                break;
            case 7: g.size = value; break;
            case 8: if (value == 0 || value > 255) return false; g.id = value; break;
            case 9: g.count = value; break;
            case 10:
                if (sp == 4) return false;
                stack[sp++] = g;
                break;
            case 11:
                if (!sp) return false;
                g = stack[--sp];
                break;
            default: break;
            }
        } else if (type == 2) {
            value = full_usage(value, bytes, g.page);
            if (tag == 0) {
                if (usage_count == 32) return false;
                usages[usage_count++] = value;
            } else if (tag == 1) {
                usage_min = usage_max = value;
                have_min = true;
            } else if (tag == 2) {
                if (!have_min || value < usage_min || (value >> 16) != (usage_min >> 16)) return false;
                usage_max = value;
            } else if (tag == 10) {
                return false; /* Delimiter alternative usage sets are unsupported. */
            }
        } else if (type == 0) {
            if (tag == 10) { /* Collection: only joystick/gamepad application fields. */
                if (depth == 16) return false;
                collection_gamepad[depth++] = gamepad;
                uint32_t usage = usage_count ? usages[0] : usage_min;
                if (value == 1) gamepad = usage == 0x10004 || usage == 0x10005;
            } else if (tag == 12) {
                if (!depth) return false;
                gamepad = collection_gamepad[--depth];
            } else if (tag == 8) { /* Input offsets include padding and unknown usages. */
                if (!g.size || g.size > 32 || !g.count ||
                    g.count > HID_INPUT_MAX_REPORT_BYTES * 8 / g.size) return false;
                uint32_t bits = g.size * g.count;
                if (bits + map->report_bits[g.id] > HID_INPUT_MAX_REPORT_BYTES * 8) return false;
                if (gamepad && !(value & 1)) {
                    for (uint32_t i = 0; i < g.count; ++i) {
                        uint32_t usage = 0;
                        if (usage_count) usage = usages[i < usage_count ? i : usage_count - 1];
                        else if (have_min) usage = usage_min + (i <= usage_max - usage_min ? i : usage_max - usage_min);
                        uint16_t page = usage >> 16, id = usage & 0xffff;
                        bool supported = (page == 9 && id >= 1 && id <= 32) ||
                            (page == 1 && ((id >= 0x30 && id <= 0x35) || id == 0x39 ||
                                          (id >= 0x90 && id <= 0x93)));
                        if (!(value & 2) || (value & 4) || !supported) {
                            ++map->skipped_fields; /* Arrays/relative/vendor fields: raw only. */
                            continue;
                        }
                        if (g.max < g.min || map->count == HID_INPUT_MAX_FIELDS) return false;
                        map->fields[map->count++] = (hid_input_field_t){
                            .bit_offset = map->report_bits[g.id] + i * g.size,
                            .usage_page = page, .usage = id, .logical_min = g.min,
                            .logical_max = g.max, .report_id = g.id, .bit_size = g.size,
                        };
                    }
                }
                map->report_bits[g.id] += bits;
            }
            usage_count = 0;
            have_min = false;
            usage_min = usage_max = 0;
        }
    }
    return !depth && !sp && map->count > 0;
}

bool hid_input_parse(hid_input_map_t *map, const uint8_t *data, size_t length)
{
    if (!map) return false;
    memset(map, 0, sizeof(*map));
    if (!data || !length || !parse(map, data, length)) {
        memset(map, 0, sizeof(*map));
        return false;
    }
    return true;
}

bool hid_input_decode(const hid_input_map_t *map, uint8_t report_id,
                      const uint8_t *data, size_t length, hid_input_state_t *state)
{
    if (!map || !data || !state || !map->report_bits[report_id] ||
        length < (map->report_bits[report_id] + 7u) / 8u) return false;
    bool found = false;
    for (size_t i = 0; i < map->count; ++i) {
        const hid_input_field_t *f = &map->fields[i];
        if (f->report_id != report_id) continue;
        uint32_t raw = 0;
        for (unsigned b = 0; b < f->bit_size; ++b) {
            unsigned bit = f->bit_offset + b;
            raw |= (uint32_t)((data[bit / 8] >> (bit % 8)) & 1) << b;
        }
        if (f->logical_min < 0 && f->bit_size < 32 && (raw & (1u << (f->bit_size - 1))))
            raw |= UINT32_MAX << f->bit_size;
        int32_t value = (int32_t)raw;
        if (f->usage_page == 9) {
            uint32_t mask = 1u << (f->usage - 1);
            state->buttons = (state->buttons & ~mask) | (value ? mask : 0);
        } else if (f->usage >= 0x30 && f->usage <= 0x35) {
            unsigned axis = f->usage - 0x30;
            state->axes[axis] = value;
            state->axes_present |= 1u << axis;
        } else if (f->usage == 0x39) {
            state->hat = value < f->logical_min || value > f->logical_max ? -1 : value;
        } else if (f->usage >= 0x90 && f->usage <= 0x93) {
            uint8_t mask = 1u << (f->usage - 0x90);
            state->dpad = (state->dpad & ~mask) | (value ? mask : 0);
        }
        found = true;
    }
    return found;
}
