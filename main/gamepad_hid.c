#include "gamepad_hid.h"
#include "iine_link.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

/* ESP-IDF's NimBLE store implementation exports this without a public prototype,
 * as used in its blecent example. */
void ble_store_config_init(void);

#define MAX_CHARS 24
#define MAX_MAP_BYTES 1024
#define SCAN_MS 10000

typedef struct {
    struct ble_gatt_chr chr;
    uint16_t cccd, reference;
    uint8_t id, type;
    bool subscribed;
    size_t last_length;
    uint8_t last[HID_INPUT_MAX_REPORT_BYTES];
} report_chr_t;

typedef struct {
    bool used, connectable, logged, hid;
    ble_addr_t address;
    char name[40];
} scan_peer_t;

static gamepad_event_cb_t s_callback;
static uint8_t s_own_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_connecting, s_failing, s_discovering, s_have_address;
static uint8_t s_filter_address[6];
static struct ble_npl_callout s_retry, s_timeout;
static struct ble_gatt_svc s_hid;
static struct ble_gatt_svc s_vendor;
static bool s_vendor_ready;
static report_chr_t s_chars[MAX_CHARS];
static size_t s_char_count, s_index, s_map_length;
static uint16_t s_map_handle, s_protocol_handle;
static uint8_t s_map_data[MAX_MAP_BYTES];
static hid_input_map_t s_map;
static hid_input_state_t s_state;
static scan_peer_t s_peers[16];
static size_t s_peer_next;
static QueueHandle_t s_commands;
static struct ble_npl_callout s_command_poll;
static bool s_synced, s_user_cancel;
static gamepad_peer_t s_selected, s_saved;
static bool s_have_saved;
typedef struct { gamepad_command_t command; gamepad_peer_t peer; } command_t;

static int gap_event(struct ble_gap_event *event, void *arg);
static void scan_start(struct ble_npl_event *event);
static void descriptors_next(void);
static void subscribe_next(void);
static void status(const char *format, ...);

static void vendor_event(const gamepad_event_t *event);

static void link_state(gamepad_link_t link, const char *message)
{
    gamepad_event_t event = {.type = GAMEPAD_LINK, .link = link, .peer = s_selected};
    if (message) snprintf(event.message, sizeof(event.message), "%s", message);
    s_callback(&event);
}

static void peer_event(const gamepad_peer_t *peer)
{
    gamepad_event_t event = {.type = GAMEPAD_PEER, .peer = *peer};
    s_callback(&event);
}

static bool same_peer(const gamepad_peer_t *a, const gamepad_peer_t *b)
{
    return (a->address_type & 1) == (b->address_type & 1) && !memcmp(a->address, b->address, 6);
}

static void save_peer(void)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn, &desc) || !desc.sec_state.bonded) return;
    s_saved = s_selected;
    memcpy(s_saved.address, desc.peer_id_addr.val, 6);
    s_saved.address_type = desc.peer_id_addr.type;
    s_saved.saved = true;
    s_saved.seen = false;
    s_have_saved = true;
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("iine_demo", NVS_READWRITE, &handle);
    if (rc == ESP_OK) {
        rc = nvs_set_blob(handle, "last_peer", &s_saved, sizeof(s_saved));
        if (rc == ESP_OK) rc = nvs_commit(handle);
        nvs_close(handle);
    }
    if (rc != ESP_OK) status("Could not persist peer metadata rc=%d", rc);
    peer_event(&s_saved);
}

static void vendor_event(const gamepad_event_t *event)
{
    if (event->type == GAMEPAD_PROTOCOL && event->protocol_stage == IINE_RECEIVING && !s_vendor_ready) {
        s_vendor_ready = true;
        save_peer();
    }
    s_callback(event);
}

static void status(const char *format, ...)
{
    gamepad_event_t event = {.type = GAMEPAD_STATUS};
    va_list args;
    va_start(args, format);
    vsnprintf(event.message, sizeof(event.message), format, args);
    va_end(args);
    s_callback(&event);
}

static void retry(void)
{
    ble_npl_callout_reset(&s_retry, ble_npl_time_ms_to_ticks32(2000));
}

static bool current(uint16_t connection)
{
    return connection == s_conn && s_conn != BLE_HS_CONN_HANDLE_NONE && !s_failing;
}

static void fail(const char *operation, int rc)
{
    if (s_failing) return;
    s_failing = true;
    status("FAIL %s rc=%d; retry in 2s", operation, rc);
    link_state(GP_ERROR, operation);
    ble_npl_callout_stop(&s_timeout);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        int result = ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        if (result == 0) return; /* DISCONNECT owns the restart. */
        status("Terminate rc=%d", result);
        if (result != BLE_HS_ENOTCONN) {
            /* Keep ownership of a live connection until termination succeeds. */
            ble_npl_callout_reset(&s_timeout, ble_npl_time_ms_to_ticks32(2000));
            return;
        }
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_connecting = false;
    retry();
}

static void setup_timeout(struct ble_npl_event *event)
{
    (void)event;
    if (s_failing) s_failing = false; /* Retry a previously failed terminate. */
    fail("pairing / HID discovery timed out", BLE_HS_ETIMEOUT);
}

static int subscribed(uint16_t connection, const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (!current(connection)) return 0;
    if (error->status) { fail("enable input notifications", error->status); return 0; }
    s_chars[s_index].subscribed = true;
    status("Subscribed input report id=%u handle=0x%04x",
           s_chars[s_index].id, s_chars[s_index].chr.val_handle);
    ++s_index;
    subscribe_next();
    return 0;
}

static int reference_read(uint16_t connection, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (!current(connection)) return 0;
    uint8_t reference[2];
    if (error->status || !attr || OS_MBUF_PKTLEN(attr->om) != sizeof(reference) ||
        ble_hs_mbuf_to_flat(attr->om, reference, sizeof(reference), NULL)) {
        fail("read Report Reference (0x2908)", error->status); return 0;
    }
    report_chr_t *r = &s_chars[s_index];
    r->id = reference[0];
    r->type = reference[1];
    if (r->type != 1) { /* Output and Feature reports are never subscribed. */
        ++s_index;
        subscribe_next();
        return 0;
    }
    if (!r->cccd || !(r->chr.properties & BLE_GATT_CHR_PROP_NOTIFY)) {
        fail("input report missing notification/CCCD", BLE_HS_ENOTSUP); return 0;
    }
    const uint8_t enable[] = {1, 0};
    int rc = ble_gattc_write_flat(s_conn, r->cccd, enable, sizeof(enable), subscribed, NULL);
    if (rc) fail("write CCCD", rc);
    return 0;
}

static void subscribe_next(void)
{
    while (s_index < s_char_count) {
        report_chr_t *r = &s_chars[s_index];
        if (ble_uuid_u16(&r->chr.uuid.u) != 0x2a4d) { ++s_index; continue; }
        if (!r->reference) { fail("report has no Report Reference", BLE_HS_ENOENT); return; }
        int rc = ble_gattc_read(s_conn, r->reference, reference_read, NULL);
        if (rc) fail("request Report Reference", rc);
        return;
    }
    unsigned inputs = 0;
    for (size_t i = 0; i < s_char_count; ++i) inputs += s_chars[i].subscribed;
    if (!inputs) { fail("no HID input reports", BLE_HS_ENOENT); return; }
    ble_npl_callout_stop(&s_timeout);
    status("READY: %u input reports; press buttons / move stick", inputs);
    save_peer();
    link_state(GP_READY, NULL);
}

static int protocol_written(uint16_t connection, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (!current(connection)) return 0;
    if (error->status) { fail("set Report Protocol", error->status); return 0; }
    s_index = 0;
    subscribe_next();
    return 0;
}

static void map_ready(void)
{
    bool parsed = hid_input_parse(&s_map, s_map_data, s_map_length);
    status("Report Map: %u bytes, %u fields, %u skipped; %s",
           (unsigned)s_map_length, (unsigned)s_map.count, s_map.skipped_fields,
           parsed ? "descriptor decoding" : "RAW ONLY (unsupported map)");
    for (size_t i = 0; i < s_map.count; ++i) {
        const hid_input_field_t *f = &s_map.fields[i];
        gamepad_event_t event = {.type = GAMEPAD_LAYOUT, .field = *f};
        snprintf(event.message, sizeof(event.message),
                 "id=%u usage=0x%02x bits=%u offset=%u min=%ld max=%ld",
                 f->report_id, f->usage, f->bit_size, f->bit_offset,
                 (long)f->logical_min, (long)f->logical_max);
        s_callback(&event);
    }
    /* Protocol Mode is optional; Report mode is the HOGP default. */
    if (s_protocol_handle) {
        const uint8_t report_mode = 1;
        int rc = 0;
        for (size_t i = 0; i < s_char_count; ++i) {
            if (s_chars[i].chr.val_handle != s_protocol_handle) continue;
            if (s_chars[i].chr.properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) {
                rc = ble_gattc_write_no_rsp_flat(s_conn, s_protocol_handle, &report_mode, 1);
                if (rc) { fail("set Report Protocol", rc); return; }
            } else if (s_chars[i].chr.properties & BLE_GATT_CHR_PROP_WRITE) {
                rc = ble_gattc_write_flat(s_conn, s_protocol_handle, &report_mode, 1, protocol_written, NULL);
                if (rc) fail("set Report Protocol", rc);
                return;
            }
        }
    }
    s_index = 0;
    subscribe_next();
}

static int map_read(uint16_t connection, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (!current(connection)) return 0;
    if (error->status == BLE_HS_EDONE) { map_ready(); return 0; }
    if (error->status || !attr) { fail("read Report Map", error->status); return 0; }
    size_t length = OS_MBUF_PKTLEN(attr->om);
    if (attr->offset != s_map_length || length > sizeof(s_map_data) - s_map_length) {
        fail("Report Map exceeds 1024 bytes or invalid offset", BLE_HS_EMSGSIZE);
        return BLE_HS_EMSGSIZE;
    }
    int rc = ble_hs_mbuf_to_flat(attr->om, s_map_data + s_map_length, length, NULL);
    if (rc) { fail("copy Report Map", rc); return rc; }
    s_map_length += length;
    return 0;
}

static int descriptor_found(uint16_t connection, const struct ble_gatt_error *error,
                            uint16_t value_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)value_handle; (void)arg;
    if (!current(connection)) return 0;
    if (error->status == BLE_HS_EDONE) { ++s_index; descriptors_next(); return 0; }
    if (error->status) { fail("discover descriptors", error->status); return 0; }
    if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) s_chars[s_index].cccd = dsc->handle;
    if (ble_uuid_u16(&dsc->uuid.u) == 0x2908) s_chars[s_index].reference = dsc->handle;
    return 0;
}

static void descriptors_next(void)
{
    while (s_index < s_char_count) {
        report_chr_t *r = &s_chars[s_index];
        if (ble_uuid_u16(&r->chr.uuid.u) != 0x2a4d) { ++s_index; continue; }
        uint16_t end = s_index + 1 < s_char_count ? s_chars[s_index + 1].chr.def_handle - 1 : s_hid.end_handle;
        if (end <= r->chr.val_handle) { fail("report missing descriptors", BLE_HS_ENOENT); return; }
        int rc = ble_gattc_disc_all_dscs(s_conn, r->chr.val_handle, end, descriptor_found, NULL);
        if (rc) fail("request descriptors", rc);
        return;
    }
    if (!s_map_handle) { fail("missing HID Report Map (0x2a4b)", BLE_HS_ENOENT); return; }
    int rc = ble_gattc_read_long(s_conn, s_map_handle, 0, map_read, NULL);
    if (rc) fail("request Report Map", rc);
}

static int characteristic_found(uint16_t connection, const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (!current(connection)) return 0;
    if (error->status == BLE_HS_EDONE) { s_index = 0; descriptors_next(); return 0; }
    if (error->status) { fail("discover characteristics", error->status); return 0; }
    if (s_char_count == MAX_CHARS) { fail("too many HID characteristics", BLE_HS_ENOMEM); return BLE_HS_ENOMEM; }
    s_chars[s_char_count++].chr = *chr;
    if (ble_uuid_u16(&chr->uuid.u) == 0x2a4b) s_map_handle = chr->val_handle;
    if (ble_uuid_u16(&chr->uuid.u) == 0x2a4e) s_protocol_handle = chr->val_handle;
    return 0;
}

static int diagnostic_characteristic(uint16_t connection, const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (!current(connection)) return 0;
    if (error->status == BLE_HS_EDONE || error->status) {
        fail("no BLE HID service 0x1812; check controller mode", BLE_HS_ENOENT);
    } else {
        char uuid[BLE_UUID_STR_LEN];
        status("GATT characteristic %s value=0x%04x properties=0x%02x",
               ble_uuid_to_str(&chr->uuid.u, uuid), chr->val_handle, chr->properties);
    }
    return 0;
}

static int service_found(uint16_t connection, const struct ble_gatt_error *error,
                         const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (!current(connection)) return 0;
    if (error->status == BLE_HS_EDONE) {
        if (!s_hid.start_handle) {
            if (s_vendor.start_handle) {
                status("IINE private service found; starting bounded protocol probe");
                ble_npl_callout_stop(&s_timeout);
                iine_link_start(s_conn, &s_vendor);
                return 0;
            }
            status("No HID service; enumerating characteristics for diagnosis (no vendor writes)");
            int rc = ble_gattc_disc_all_chrs(s_conn, 1, 0xffff, diagnostic_characteristic, NULL);
            if (rc) fail("no BLE HID service 0x1812; check controller mode", rc);
            return 0;
        }
        int rc = ble_gattc_disc_all_chrs(s_conn, s_hid.start_handle, s_hid.end_handle, characteristic_found, NULL);
        if (rc) fail("request characteristics", rc);
    } else if (error->status) {
        fail("discover HID service", error->status);
    } else {
        char uuid[BLE_UUID_STR_LEN];
        status("GATT service %s handles=0x%04x-0x%04x",
               ble_uuid_to_str(&svc->uuid.u, uuid), svc->start_handle, svc->end_handle);
        const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
        if (!s_hid.start_handle && ble_uuid_cmp(&svc->uuid.u, &hid_uuid.u) == 0)
            s_hid = *svc; /* One gamepad / first HID service in this demo. */
        if (!s_vendor.start_handle && iine_service_matches(&svc->uuid.u)) s_vendor = *svc;
    }
    return 0;
}

static int mtu_exchanged(uint16_t connection, const struct ble_gatt_error *error,
                         uint16_t mtu, void *arg)
{
    (void)arg;
    if (!current(connection)) return 0;
    status("MTU=%u status=%d; discovering HID", mtu, error->status);
    int rc = ble_gattc_disc_all_svcs(s_conn, service_found, NULL);
    if (rc) fail("request HID service", rc);
    return 0;
}

static void discover(void)
{
    if (s_discovering) return;
    s_discovering = true;
    int rc = ble_gattc_exchange_mtu(s_conn, mtu_exchanged, NULL);
    if (rc) { /* A previously negotiated MTU is sufficient. */
        struct ble_gatt_error error = {.status = rc};
        mtu_exchanged(s_conn, &error, ble_att_mtu(s_conn), NULL);
    }
}

static bool name_matches(const char *name)
{
    if (CONFIG_IINE_PEER_NAME[0] && strcmp(name, CONFIG_IINE_PEER_NAME) == 0) return true;
    char lower[40];
    size_t i = 0;
    for (; name[i] && i < sizeof(lower) - 1; ++i) lower[i] = tolower((unsigned char)name[i]);
    lower[i] = 0;
    return strstr(lower, "gamepad") || strstr(lower, "controller") || strstr(lower, "iine");
}

static void scan_result(const struct ble_gap_disc_desc *disc)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE || s_connecting) return;
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data)) return;
    scan_peer_t *peer = NULL;
    for (size_t i = 0; i < 16; ++i) {
        if (s_peers[i].used && !ble_addr_cmp(&s_peers[i].address, &disc->addr)) { peer = &s_peers[i]; break; }
    }
    if (!peer) {
        peer = &s_peers[s_peer_next++ % 16];
        memset(peer, 0, sizeof(*peer));
        peer->used = true;
        peer->address = disc->addr;
    }
    if (disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
        disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) peer->connectable = true;
    if (fields.name && fields.name_len) {
        size_t n = fields.name_len < sizeof(peer->name) - 1 ? fields.name_len : sizeof(peer->name) - 1;
        memcpy(peer->name, fields.name, n);
        peer->name[n] = 0;
    }
    for (unsigned i = 0; i < fields.num_uuids16; ++i)
        if (ble_uuid_u16(&fields.uuids16[i].u) == 0x1812) peer->hid = true;
    if (!peer->logged && peer->name[0]) {
        const uint8_t *a = disc->addr.val;
        status("BLE %02X:%02X:%02X:%02X:%02X:%02X type=%u RSSI=%d %.39s",
               a[5], a[4], a[3], a[2], a[1], a[0], disc->addr.type, disc->rssi, peer->name);
        peer->logged = true;
    }
    if (!peer->connectable || (!peer->name[0] && !peer->hid)) return;
    if (s_have_address && memcmp(disc->addr.val, s_filter_address, 6)) return;
    gamepad_peer_t found = {.address_type = disc->addr.type, .rssi = disc->rssi,
                           .candidate = peer->hid || name_matches(peer->name), .seen = true};
    memcpy(found.address, disc->addr.val, 6);
    snprintf(found.name, sizeof(found.name), "%s", peer->name[0] ? peer->name : "BLE HID");
    found.saved = s_have_saved && same_peer(&found, &s_saved);
    peer_event(&found);
}

static void input_report(struct ble_gap_event *event)
{
    if (!current(event->notify_rx.conn_handle)) return;
    if (iine_link_notification(event->notify_rx.conn_handle, event->notify_rx.attr_handle, event->notify_rx.om)) return;
    for (size_t i = 0; i < s_char_count; ++i) {
        report_chr_t *r = &s_chars[i];
        if (r->chr.val_handle != event->notify_rx.attr_handle || r->type != 1) continue;
        gamepad_event_t output = {.type = GAMEPAD_INPUT, .report_id = r->id};
        size_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (!length || length > sizeof(output.raw)) {
            status("Report id=%u length=%u exceeds 64-byte input limit", r->id, (unsigned)length); return;
        }
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, output.raw, length, NULL)) return;
        if (r->last_length == length && !memcmp(r->last, output.raw, length)) return;
        memcpy(r->last, output.raw, length);
        r->last_length = length;
        output.length = length;
        output.decoded = hid_input_decode(&s_map, r->id, output.raw, length, &s_state);
        output.state = s_state;
        s_callback(&output);
        return;
    }
}

static void clear_connection(void)
{
    iine_link_reset();
    memset(&s_hid, 0, sizeof(s_hid));
    memset(&s_vendor, 0, sizeof(s_vendor));
    s_vendor_ready = false;
    memset(s_chars, 0, sizeof(s_chars));
    memset(&s_map, 0, sizeof(s_map));
    memset(&s_state, 0, sizeof(s_state));
    s_state.hat = -1;
    s_char_count = s_index = s_map_length = 0;
    s_map_handle = s_protocol_handle = 0;
    s_failing = s_discovering = false;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: scan_result(&event->disc); break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!s_connecting && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            status("Scan finished. L1241: Pairing + A for 3s, select IINE GAMEPAD.");
            retry();
        }
        break;
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status) {
            status("Connection failed rc=%d", event->connect.status);
            if (!s_user_cancel) link_state(GP_ERROR, "Connection timed out / failed");
            retry(); break;
        }
        clear_connection();
        s_conn = event->connect.conn_handle;
        if (s_user_cancel) { ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM); break; }
        status("Connected; pairing / restoring encrypted bond ...");
        link_state(GP_PAIRING, NULL);
        ble_npl_callout_reset(&s_timeout, ble_npl_time_ms_to_ticks32(45000));
        {
            int rc = ble_gap_security_initiate(s_conn);
            if (rc && rc != BLE_HS_EALREADY) fail("start security", rc);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (!current(event->enc_change.conn_handle)) break;
        if (event->enc_change.status) {
            struct ble_gap_conn_desc desc;
            if (event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) &&
                !ble_gap_conn_find(s_conn, &desc)) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
                status("Peer lost its bond; removed stale peer keys. Re-enter pairing mode.");
            }
            fail("encryption", event->enc_change.status);
        } else {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn, &desc) || !desc.sec_state.encrypted) fail("link not encrypted", BLE_HS_EAUTHEN);
            else { status("Encrypted; discovering reports ..."); link_state(GP_DISCOVERING, NULL); discover(); }
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (current(event->repeat_pairing.conn_handle) && !ble_gap_conn_find(s_conn, &desc)) {
            int rc = ble_store_util_delete_peer(&desc.peer_id_addr);
            status("Peer requests re-pairing; old bond removal rc=%d", rc);
            if (!rc) return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        fail("peer requires passkey; use L1241 iOS pairing mode", BLE_HS_ENOTSUP);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: input_report(event); break;
    case BLE_GAP_EVENT_DISCONNECT: {
        bool failed = s_failing;
        ble_npl_callout_stop(&s_timeout);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_connecting = false;
        clear_connection();
        status("DISCONNECTED reason=%d; input state cleared; scanning again", event->disconnect.reason);
        if (!failed || s_user_cancel) link_state(GP_SCANNING, "Disconnected");
        retry();
        break;
    }
    default: break;
    }
    return 0;
}

static void scan_start(struct ble_npl_event *event)
{
    (void)event;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE || s_connecting || ble_gap_disc_active()) return;
    memset(s_peers, 0, sizeof(s_peers));
    s_peer_next = 0;
    struct ble_gap_disc_params params = {0};
    params.itvl = 160;
    params.window = 80;
    params.passive = 0; /* Names often appear ONLY in the scan response. */
    params.filter_duplicates = 0;
    status("SCANNING for selectable BLE devices; no automatic connection");
    int rc = ble_gap_disc(s_own_type, SCAN_MS, &params, gap_event, NULL);
    if (rc) { status("Scan start rc=%d", rc); link_state(GP_ERROR, "Scan failed"); retry(); }
}

bool gamepad_hid_command(gamepad_command_t command, const gamepad_peer_t *peer)
{
    if (!s_commands) return false;
    command_t item = {.command = command};
    if (peer) item.peer = *peer;
    return xQueueSend(s_commands, &item, 0) == pdTRUE;
}

static void command_poll(struct ble_npl_event *event)
{
    (void)event;
    command_t item;
    if (s_synced && xQueueReceive(s_commands, &item, 0) == pdTRUE) {
        if (item.command == GP_CMD_CONNECT && !s_connecting && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            s_selected = item.peer;
            s_connecting = true;
            s_user_cancel = false;
            s_failing = false;
            ble_npl_callout_stop(&s_retry);
            int rc = ble_gap_disc_active() ? ble_gap_disc_cancel() : 0;
            if (!rc || rc == BLE_HS_EALREADY) {
                ble_addr_t address = {.type = item.peer.address_type};
                memcpy(address.val, item.peer.address, 6);
                link_state(GP_CONNECTING, NULL);
                status("Connecting to %.39s (user selected)", item.peer.name);
                rc = ble_gap_connect(s_own_type, &address, 10000, NULL, gap_event, NULL);
            }
            if (rc) { s_connecting = false; fail("connect request", rc); }
        } else if (item.command == GP_CMD_DISCONNECT) {
            s_user_cancel = true;
            ble_npl_callout_stop(&s_timeout);
            if (s_connecting) ble_gap_conn_cancel();
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
                if (!iine_link_disconnect()) {
                    int rc = ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
                    if (rc) fail("disconnect request", rc);
                }
            } else if (!s_connecting) scan_start(NULL);
        } else if (item.command == GP_CMD_REFRESH && !s_connecting && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            if (ble_gap_disc_active()) ble_gap_disc_cancel();
            gamepad_event_t clear = {.type = GAMEPAD_SCAN_CLEAR};
            s_callback(&clear);
            if (s_have_saved) peer_event(&s_saved);
            link_state(GP_SCANNING, NULL);
            scan_start(NULL);
        } else if (item.command == GP_CMD_FORGET && !s_connecting && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            ble_addr_t address = {.type = (uint8_t)(item.peer.address_type & 1)};
            memcpy(address.val, item.peer.address, 6);
            int rc = ble_store_util_delete_peer(&address);
            if (!rc || rc == BLE_HS_ENOENT) {
                if (s_have_saved && same_peer(&item.peer, &s_saved)) {
                    nvs_handle_t handle;
                    if (nvs_open("iine_demo", NVS_READWRITE, &handle) == ESP_OK) {
                        esp_err_t result = nvs_erase_key(handle, "last_peer");
                        if (result == ESP_OK) result = nvs_commit(handle);
                        nvs_close(handle);
                        if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
                            status("Forget metadata failed rc=%d", result);
                    }
                    s_have_saved = false;
                }
                gamepad_event_t forgotten = {.type = GAMEPAD_FORGOTTEN, .peer = item.peer};
                s_callback(&forgotten);
                status("Selected peer bond removed; other NVS data preserved");
            } else { status("Forget peer failed rc=%d", rc); link_state(GP_ERROR, "Forget failed"); }
        }
    }
    ble_npl_callout_reset(&s_command_poll, ble_npl_time_ms_to_ticks32(50));
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (!rc) rc = ble_hs_id_infer_auto(0, &s_own_type);
    if (rc) { status("BLE address setup failed rc=%d", rc); link_state(GP_ERROR, "BLE initialization failed"); return; }
    s_synced = true;
    link_state(GP_SCANNING, NULL);
    if (s_have_saved) peer_event(&s_saved);
    scan_start(NULL);
}

static void on_reset(int reason)
{
    ble_npl_callout_stop(&s_retry);
    ble_npl_callout_stop(&s_timeout);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_connecting = false;
    s_synced = false;
    clear_connection();
    status("Bluetooth host reset reason=%d; waiting for sync", reason);
    link_state(GP_ERROR, "Bluetooth reset; waiting for sync");
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void gamepad_hid_start(gamepad_event_cb_t callback)
{
    s_callback = callback;
    s_commands = xQueueCreate(8, sizeof(command_t));
    configASSERT(s_commands);
    if (CONFIG_IINE_PEER_ADDRESS[0]) {
        unsigned a[6];
        int consumed = 0;
        if (strlen(CONFIG_IINE_PEER_ADDRESS) != 17 ||
            sscanf(CONFIG_IINE_PEER_ADDRESS, "%2x:%2x:%2x:%2x:%2x:%2x%n",
                   &a[5], &a[4], &a[3], &a[2], &a[1], &a[0], &consumed) != 6 || consumed != 17) {
            status("Invalid IINE_PEER_ADDRESS: use AA:BB:CC:DD:EE:FF");
            link_state(GP_ERROR, "Invalid address configuration"); return;
        }
        for (size_t i = 0; i < 6; ++i) s_filter_address[i] = a[i];
        s_have_address = true;
    }
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        /* No automatic erase: this NVS partition belongs to the existing badge. */
        status("NVS init failed 0x%x. Existing data preserved; no automatic erase.", err);
        link_state(GP_ERROR, "NVS initialization failed"); return;
    }
    nvs_handle_t saved_handle;
    if (nvs_open("iine_demo", NVS_READONLY, &saved_handle) == ESP_OK) {
        size_t size = sizeof(s_saved);
        if (nvs_get_blob(saved_handle, "last_peer", &s_saved, &size) == ESP_OK &&
            size == sizeof(s_saved) && s_saved.address_type <= 3 && s_saved.name[0]) {
            s_saved.name[sizeof(s_saved.name) - 1] = 0;
            s_saved.saved = true;
            s_saved.seen = false;
            s_have_saved = true;
        }
        nvs_close(saved_handle);
    }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("Folo IINE HID Host");
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    ble_npl_callout_init(&s_retry, nimble_port_get_dflt_eventq(), scan_start, NULL);
    ble_npl_callout_init(&s_timeout, nimble_port_get_dflt_eventq(), setup_timeout, NULL);
    ble_npl_callout_init(&s_command_poll, nimble_port_get_dflt_eventq(), command_poll, NULL);
    iine_link_init(vendor_event, fail);
    ble_npl_callout_reset(&s_command_poll, ble_npl_time_ms_to_ticks32(50));
    status("IINE L1241 v0.3.1 / ESP32-C3: measured private BLE input + LCD controls.");
    nimble_port_freertos_init(host_task);
}
