#include "gamepad_ui.h"
#include <string.h>

static bool same(const gamepad_peer_t *a, const gamepad_peer_t *b)
{
    return (a->address_type & 1) == (b->address_type & 1) && !memcmp(a->address, b->address, 6);
}

static void clear_input(gamepad_ui_t *ui)
{
    memset(&ui->input, 0, sizeof(ui->input));
    ui->input.hat = -1;
    ui->mapped_axes = 0;
    ui->mapped_buttons = ui->reports = 0;
    ui->hat_min = ui->hat_max = -1;
    ui->decoded = false;
    ui->raw_length = 0;
    ui->vendor = false;
    ui->raw_view = false;
    ui->protocol_stage = IINE_OFF;
    ui->packets = ui->bad_packets = ui->last_input_ms = 0;
    memset(ui->changed_ms, 0, sizeof(ui->changed_ms));
    memset(ui->button_pressed_ms, 0, sizeof(ui->button_pressed_ms));
    memset(ui->dpad_pressed_ms, 0, sizeof(ui->dpad_pressed_ms));
}

void gamepad_ui_init(gamepad_ui_t *ui)
{
    memset(ui, 0, sizeof(*ui));
    clear_input(ui);
}

unsigned gamepad_ui_visible(const gamepad_ui_t *ui, unsigned *indices)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ui->count; ++i)
        if (ui->show_all || ui->peers[i].candidate || ui->peers[i].saved) indices[n++] = i;
    return n;
}

void gamepad_ui_event(gamepad_ui_t *ui, const gamepad_event_t *e, uint32_t now)
{
    if (e->type == GAMEPAD_PEER) {
        unsigned i;
        for (i = 0; i < ui->count; ++i) if (same(&e->peer, &ui->peers[i])) break;
        if (i == GAMEPAD_MAX_PEERS) {
            if (!e->peer.candidate && !e->peer.saved) return;
            for (i = 0; i < ui->count; ++i)
                if (!ui->peers[i].candidate && !ui->peers[i].saved) break;
            if (i == ui->count) return;
        }
        if (i == ui->count) ++ui->count;
        bool seen = i < ui->count && ui->peers[i].seen && same(&ui->peers[i], &e->peer);
        ui->peers[i] = e->peer;
        if (e->peer.seen) ui->seen_ms[i] = now;
        else if (seen) ui->peers[i].seen = true;
    } else if (e->type == GAMEPAD_SCAN_CLEAR) {
        ui->count = ui->selected = 0;
        memset(ui->peers, 0, sizeof(ui->peers));
    } else if (e->type == GAMEPAD_FORGOTTEN) {
        for (unsigned i = 0; i < ui->count; ++i)
            if (same(&e->peer, &ui->peers[i])) ui->peers[i].saved = false;
        ui->selected = 0;
        ui->error[0] = 0;
    } else if (e->type == GAMEPAD_LINK) {
        ui->link = e->link;
        if (e->link == GP_CONNECTING) {
            clear_input(ui);
            ui->active = e->peer;
            ui->error[0] = 0;
            ui->page = UI_CONNECTING;
        } else if (e->link == GP_READY) ui->page = UI_LIVE;
        else if (e->link == GP_SCANNING || e->link == GP_ERROR) {
            clear_input(ui);
            ui->page = UI_DEVICES;
            if (e->link == GP_ERROR) memcpy(ui->error, e->message, sizeof(ui->error));
            else ui->error[0] = 0;
        }
    } else if (e->type == GAMEPAD_PROTOCOL) {
        ui->vendor = true;
        ui->page = UI_LIVE;
        ui->protocol_stage = e->protocol_stage;
        ui->packets = e->packets;
        ui->bad_packets = e->bad_packets;
    } else if (e->type == GAMEPAD_LAYOUT) {
        const hid_input_field_t *f = &e->field;
        if (f->usage_page == 1 && f->usage >= 0x30 && f->usage <= 0x35) {
            unsigned a = f->usage - 0x30;
            ui->mapped_axes |= 1u << a;
            ui->minimum[a] = f->logical_min;
            ui->maximum[a] = f->logical_max;
        } else if (f->usage_page == 1 && f->usage == 0x39) {
            ui->hat_min = f->logical_min;
            ui->hat_max = f->logical_max;
        } else if (f->usage_page == 9 && f->usage >= 1 && f->usage <= 32)
            ui->mapped_buttons |= 1u << (f->usage - 1);
    } else if (e->type == GAMEPAD_INPUT) {
        if (e->decoded) {
            for (unsigned i = 0; i < 12; ++i)
                if ((e->state.buttons & (1u << i)) && !(ui->input.buttons & (1u << i))) ui->button_pressed_ms[i] = now;
            for (unsigned i = 0; i < 4; ++i)
                if ((e->state.dpad & (1u << i)) && !(ui->input.dpad & (1u << i))) ui->dpad_pressed_ms[i] = now;
        }
        for (unsigned i = 0; i < 15 && i < e->length; ++i)
            if (i >= ui->raw_length || ui->raw[i] != e->raw[i]) ui->changed_ms[i] = now;
        ui->last_input_ms = now;
        ui->input = e->state;
        ui->decoded = e->decoded;
        ui->report_id = e->report_id;
        ui->raw_length = e->length;
        memcpy(ui->raw, e->raw, sizeof(ui->raw));
        ++ui->reports;
    }
}

ui_action_t gamepad_ui_key(gamepad_ui_t *ui, int key, bool long_press)
{
    ui_action_t action = {0};
    unsigned indices[GAMEPAD_MAX_PEERS], n = gamepad_ui_visible(ui, indices);
    unsigned rows = n + 2; /* Refresh and visibility toggle. */
    if (ui->selected >= rows) ui->selected = 0;
    if (ui->page == UI_FORGET) {
        if (key == 2) {
            if (!long_press) { action.send = true; action.command = GP_CMD_FORGET; action.peer = ui->active; }
            ui->page = UI_DEVICES;
        }
    } else if (ui->page == UI_DEVICES) {
        if (long_press && key == 2 && ui->selected < n && ui->peers[indices[ui->selected]].saved) {
            ui->active = ui->peers[indices[ui->selected]];
            ui->page = UI_FORGET;
        } else if (!long_press) {
            if (key == 0) ui->selected = (ui->selected + rows - 1) % rows;
            if (key == 1) ui->selected = (ui->selected + 1) % rows;
            if (key == 2) {
                if (ui->selected < n) {
                    action.send = true;
                    action.command = GP_CMD_CONNECT;
                    action.peer = ui->peers[indices[ui->selected]];
                    ui->active = action.peer;
                    ui->link = GP_CONNECTING;
                    ui->page = UI_CONNECTING;
                } else if (ui->selected == n) { action.send = true; action.command = GP_CMD_REFRESH; }
                else { ui->show_all = !ui->show_all; ui->selected = 0; }
            }
        }
    } else if (ui->page == UI_LIVE && ui->vendor && key == 2 && !long_press) {
        ui->raw_view = !ui->raw_view;
    } else if (key == 2 && long_press) {
        action.send = true;
        action.command = GP_CMD_DISCONNECT;
        clear_input(ui);
        ui->page = UI_DEVICES;
        ui->link = GP_SCANNING;
    }
    return action;
}

int gamepad_ui_axis(const gamepad_ui_t *ui, unsigned a, int radius)
{
    if (a >= 6 || !(ui->input.axes_present & (1u << a)) || !(ui->mapped_axes & (1u << a))) return 0;
    int64_t lo = ui->minimum[a], hi = ui->maximum[a], value = ui->input.axes[a];
    if (hi <= lo) return 0;
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    return (int)((value - lo) * 2 * radius / (hi - lo) - radius);
}

uint8_t gamepad_ui_dpad(const gamepad_ui_t *ui)
{
    uint8_t result = ui->input.dpad;
    int64_t range = (int64_t)ui->hat_max - ui->hat_min + 1;
    int64_t hat = (int64_t)ui->input.hat - ui->hat_min;
    static const uint8_t directions[] = {1, 5, 4, 6, 2, 10, 8, 9};
    if (ui->input.hat >= 0 && hat >= 0 && hat < range && (range == 8 || range == 4))
        result |= directions[hat * (range == 4 ? 2 : 1)];
    return result;
}
