#include "iine_link.h"
#include "iine_protocol.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"

/* Only this configuration service is opened. FF12 OTA is deliberately absent. */
#define IINE_UUID(id) BLE_UUID128_INIT(0xb5,0xda,0x8b,0x14,0xc4,0x19,0xc3,0x96,0xe9,0x46,0x0d,0x66,id,0x10,0xf0,0xd7)
static const ble_uuid128_t service_uuid = IINE_UUID(0xe0);
static const ble_uuid128_t write_uuid = IINE_UUID(0xe1);
static const ble_uuid128_t notify_uuid = IINE_UUID(0xe2);
static const uint8_t mode_payload[] = {2, 0xdf, 0xab};
static const uint8_t test_payload[] = {5, 0xdf, 0xab, 0, 6, 5};
static const uint8_t caps_payload[] = {0, 1};

typedef enum { IDLE, DISCOVERY, INFO, CAPS, START, TEST, LISTEN, STOPPED, CLOSING } step_t;
static struct {
    uint16_t conn, write, notify, cccd, descriptor_end, service_end;
    uintptr_t generation;
    bool active, busy, replied, mode_sent, test_sent, known_layout;
    uint8_t serial_counter, serial, opcode, attempts;
    uint8_t last[IINE_PAYLOAD_MAX], last_length;
    uint32_t packets, bad, inputs;
    int64_t deadline, started, last_input, next_status, next_keepalive;
    step_t step;
    iine_stage_t stage;
    iine_packet_t reply;
    hid_input_state_t input;
} s;
static struct ble_npl_callout timer;
static gamepad_event_cb_t callback;
static void (*failure)(const char *, int);

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static bool valid(uint16_t conn, void *arg)
{
    return s.active && s.conn == conn && s.generation == (uintptr_t)arg;
}
static void log_message(const char *format, ...)
{
    gamepad_event_t e = {.type = GAMEPAD_STATUS};
    va_list args; va_start(args, format);
    vsnprintf(e.message, sizeof(e.message), format, args);
    va_end(args); callback(&e);
}
static void progress(iine_stage_t stage)
{
    bool changed = stage != s.stage;
    s.stage = stage;
    gamepad_event_t e = {.type = GAMEPAD_PROTOCOL, .protocol_stage = stage,
                        .packets = s.packets, .bad_packets = s.bad};
    callback(&e);
    if (changed) log_message("IINE stage=%u packets=%lu bad=%lu inputs=%lu", stage,
                            (unsigned long)s.packets, (unsigned long)s.bad, (unsigned long)s.inputs);
}
static void abort_link(const char *message, int rc)
{
    ble_npl_callout_stop(&timer);
    s.active = false;
    failure(message, rc);
}
static void terminate(void)
{
    int rc = ble_gap_terminate(s.conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc && rc != BLE_HS_ENOTCONN) abort_link("IINE disconnect", rc);
    else { ble_npl_callout_stop(&timer); s.active = false; }
}

static int written(uint16_t conn, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    if (!valid(conn, arg)) return 0;
    s.busy = false;
    if (s.step == CLOSING) { terminate(); return 0; }
    if (error->status) abort_link("IINE configuration write", error->status);
    return 0;
}

static bool send_request(uint8_t opcode, const uint8_t *payload, size_t length)
{
    if (s.busy || !s.active || !iine_request_allowed(opcode, payload, length)) return false;
    uint8_t wire[IINE_PACKET_MAX];
    ++s.serial_counter;
    s.serial = (s.serial_counter & 0x7f) | ((s.serial_counter & 1) ? 0 : 0x80);
    size_t size = iine_packet_build(wire, sizeof(wire), opcode, s.serial, esp_random(), payload, length);
    if (!size) return false;
    s.opcode = opcode; s.replied = false; s.busy = true;
    s.deadline = now_ms() + 4500;
    int rc = ble_gattc_write_flat(s.conn, s.write, wire, size, written, (void *)s.generation);
    if (rc) { s.busy = false; abort_link("IINE send request", rc); return false; }
    if (s.step != LISTEN) log_message("IINE TX opcode=%02X serial=%02X payload=%u", opcode, s.serial, (unsigned)length);
    return true;
}

static void advance(step_t step)
{
    s.step = step; s.attempts = 0; s.replied = false; s.deadline = 0;
}

static void tick(struct ble_npl_event *event)
{
    (void)event;
    if (!s.active) return;
    int64_t now = now_ms();
    if (s.step == CLOSING) {
        if (now >= s.deadline) terminate();
    } else if (s.busy) {
        if (now > s.deadline) abort_link("IINE GATT write timed out", BLE_HS_ETIMEOUT);
    } else if (s.step == DISCOVERY) {
        if (now > s.deadline) abort_link("IINE service setup timed out", BLE_HS_ETIMEOUT);
    } else if (s.step == INFO || s.step == CAPS) {
        if (s.replied) {
            if (s.step == INFO) {
                if (s.reply.length < 6) { advance(STOPPED); progress(IINE_UNKNOWN); }
                else {
                    const uint8_t *p = s.reply.payload;
                    log_message("IINE identity VID=%02X%02X PID=%02X%02X FW=%X.%02X",p[0],p[1],p[2],p[3],p[4],p[5]);
                    const uint8_t measured[] = {7, 0x10, 2, 0x20, 0x27, 2};
                    s.known_layout = s.reply.length == 8 && !memcmp(p, measured, sizeof(measured));
                    if (s.known_layout) {
                        log_message("IINE L1241 FW 27.02 measured input decoder enabled");
                        for (unsigned axis = 0; axis < 6; ++axis) {
                            gamepad_event_t e = {.type = GAMEPAD_LAYOUT,
                                .field = {.usage_page = 1, .usage = 0x30 + axis, .logical_min = 0, .logical_max = 255}};
                            callback(&e);
                        }
                        for (unsigned button = 1; button <= 12; ++button) {
                            gamepad_event_t e = {.type = GAMEPAD_LAYOUT, .field = {.usage_page = 9, .usage = button}};
                            callback(&e);
                        }
                    }
                    advance(CAPS); progress(IINE_CAPABILITIES);
                }
            } else { advance(START); progress(IINE_STARTING); }
        } else if (!s.deadline || now >= s.deadline) {
            if (s.attempts >= 2) {
                if (s.step == INFO) { advance(STOPPED); progress(IINE_UNKNOWN); }
                else { advance(START); progress(IINE_STARTING); }
            } else {
                ++s.attempts;
                send_request(s.step == INFO ? 0x91 : 0xb0,
                             s.step == CAPS ? caps_payload : NULL, s.step == CAPS ? sizeof(caps_payload) : 0);
            }
        }
    } else if (s.step == START) {
        if (send_request(0x1d, mode_payload, sizeof(mode_payload))) {
            s.mode_sent = true; s.started = now; s.step = LISTEN;
            s.next_keepalive = now + 500;
            progress(IINE_WAITING);
        }
    } else if (s.step == TEST) {
        if (send_request(0x1d, test_payload, sizeof(test_payload))) {
            s.test_sent = true; s.started = now; s.step = LISTEN;
            s.next_keepalive = now + 1000;
            log_message("IINE button-test page requested");
        }
    } else if (s.step == LISTEN) {
        if (now >= s.next_keepalive && !s.test_sent) advance(TEST);
        else if (!s.inputs && now - s.started > 12000 && s.stage != IINE_NO_INPUT) progress(IINE_NO_INPUT);
        /* A test-page keepalive is only needed after using that page. */
        if (s.test_sent && s.step == LISTEN && now >= s.next_keepalive) {
            send_request(0x1d, test_payload, sizeof(test_payload));
            s.next_keepalive = now + 1000;
        }
        /* Input is sent on changes: a quiet pad is still connected, and a held
         * button must not be cleared merely because it has not changed. */
    }
    if (s.active) {
        if (s.step != CLOSING && now >= s.next_status) { progress(s.stage); s.next_status = now + 1000; }
        ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(50));
    }
}

static int subscribed(uint16_t conn, const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    if (!valid(conn, arg)) return 0;
    if (error->status) { abort_link("IINE notification subscription", error->status); return 0; }
    log_message("IINE notifications enabled; querying protocol identity");
    advance(INFO); progress(IINE_IDENTIFY);
    ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(50));
    return 0;
}

static int descriptor(uint16_t conn, const struct ble_gatt_error *error,
                      uint16_t value, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)value;
    if (!valid(conn, arg)) return 0;
    if (error->status == BLE_HS_EDONE) {
        if (!s.cccd) { abort_link("IINE CCCD missing", BLE_HS_ENOENT); return 0; }
        const uint8_t enable[] = {1, 0};
        int rc = ble_gattc_write_flat(conn, s.cccd, enable, sizeof(enable), subscribed, arg);
        if (rc) abort_link("IINE subscribe request", rc);
    } else if (error->status) abort_link("IINE descriptors", error->status);
    else if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) s.cccd = dsc->handle;
    return 0;
}

static int characteristic(uint16_t conn, const struct ble_gatt_error *error,
                          const struct ble_gatt_chr *chr, void *arg)
{
    if (!valid(conn, arg)) return 0;
    if (error->status == BLE_HS_EDONE) {
        if (!s.write || !s.notify || s.descriptor_end <= s.notify) {
            abort_link("IINE characteristic layout unsupported", BLE_HS_ENOTSUP); return 0;
        }
        int rc = ble_gattc_disc_all_dscs(conn, s.notify, s.descriptor_end, descriptor, arg);
        if (rc) abort_link("IINE descriptor request", rc);
    } else if (error->status) abort_link("IINE characteristics", error->status);
    else {
        if (s.notify && chr->def_handle > s.notify && chr->def_handle - 1 < s.descriptor_end)
            s.descriptor_end = chr->def_handle - 1;
        if (!ble_uuid_cmp(&chr->uuid.u, &write_uuid.u) && (chr->properties & BLE_GATT_CHR_PROP_WRITE)) s.write = chr->val_handle;
        if (!ble_uuid_cmp(&chr->uuid.u, &notify_uuid.u) && (chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) s.notify = chr->val_handle;
    }
    return 0;
}

bool iine_service_matches(const ble_uuid_t *uuid) { return !ble_uuid_cmp(uuid, &service_uuid.u); }

void iine_link_init(gamepad_event_cb_t cb, void (*fail_cb)(const char *, int))
{
    callback = cb; failure = fail_cb;
    ble_npl_callout_init(&timer, nimble_port_get_dflt_eventq(), tick, NULL);
}

void iine_link_reset(void)
{
    ble_npl_callout_stop(&timer);
    uintptr_t generation = s.generation + 1;
    memset(&s, 0, sizeof(s)); s.generation = generation;
    s.input.hat = -1;
}

void iine_link_start(uint16_t conn, const struct ble_gatt_svc *service)
{
    iine_link_reset();
    s.active = true; s.conn = conn; s.step = DISCOVERY;
    s.deadline = now_ms() + 12000;
    s.service_end = s.descriptor_end = service->end_handle;
    progress(IINE_IDENTIFY);
    int rc = ble_gattc_disc_all_chrs(conn, service->start_handle, service->end_handle, characteristic, (void *)s.generation);
    if (rc) abort_link("IINE characteristic request", rc);
    else ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(50));
}

bool iine_link_notification(uint16_t conn, uint16_t handle, struct os_mbuf *data)
{
    if (!s.active || s.conn != conn || s.notify != handle) return false;
    if (s.step == CLOSING) return true;
    ++s.packets;
    size_t length = OS_MBUF_PKTLEN(data);
    uint8_t wire[IINE_PACKET_MAX];
    iine_packet_t p;
    if (length > sizeof(wire) || ble_hs_mbuf_to_flat(data, wire, length, NULL) || !iine_packet_parse(wire, length, &p)) {
        ++s.bad;
        if (s.bad < 5) {
            uint8_t raw[20]; char hex[61] = {0};
            unsigned kept = length < sizeof(raw) ? length : sizeof(raw);
            if (!os_mbuf_copydata(data, 0, kept, raw))
                for (unsigned i = 0; i < kept; ++i) snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", raw[i]);
            log_message("IINE invalid len=%u raw=%s", (unsigned)length, hex);
        }
        return true;
    }
    if (p.opcode == 0x19 && p.serial == s.serial && !s.replied) {
        s.reply = p; s.replied = true;
        char hex[46] = {0};
        for (unsigned i = 0; i < p.length; ++i) snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", p.payload[i]);
        if (s.step != LISTEN || s.packets < 10)
            log_message("IINE reply to=%02X serial=%02X data=%s", s.opcode, p.serial, hex);
    } else if (p.opcode == 0x1f && s.mode_sent && p.length) {
        ++s.inputs; s.last_input = now_ms();
        if (s.stage != IINE_RECEIVING) progress(IINE_RECEIVING);
        /* Sequence / nonce / CRC are removed before change detection. Never
         * mistake changing transport bytes for physical button activity. */
        if (s.last_length == p.length && !memcmp(s.last, p.payload, p.length)) return true;
        memcpy(s.last, p.payload, p.length); s.last_length = p.length;
        gamepad_event_t e = {.type = GAMEPAD_INPUT, .report_id = p.opcode,
                            .length = p.length, .state = {.hat = -1}};
        memcpy(e.raw, p.payload, p.length);
        e.decoded = s.known_layout && iine_input_decode(p.payload, p.length, &s.input);
        e.state = s.input;
        callback(&e);
    } else if (s.packets < 12) log_message("IINE packet opcode=%02X serial=%02X len=%u", p.opcode, p.serial, p.length);
    return true;
}

bool iine_link_disconnect(void)
{
    if (!s.active) return false;
    bool busy = s.busy;
    s.step = CLOSING;
    if (s.mode_sent && !busy) {
        if (!send_request(0x1e, mode_payload, sizeof(mode_payload))) { terminate(); return true; }
        s.deadline = now_ms() + 600;
    } else { terminate(); return true; }
    ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(50));
    return true;
}
