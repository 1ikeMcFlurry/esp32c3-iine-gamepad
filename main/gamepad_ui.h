#pragma once
#include "gamepad_hid.h"

typedef enum { UI_DEVICES, UI_CONNECTING, UI_LIVE, UI_FORGET } ui_page_t;
typedef struct {
    ui_page_t page;
    gamepad_link_t link;
    gamepad_peer_t peers[GAMEPAD_MAX_PEERS], active;
    uint32_t seen_ms[GAMEPAD_MAX_PEERS];
    unsigned count, selected;
    bool show_all, decoded;
    char error[96];
    hid_input_state_t input;
    int32_t minimum[6], maximum[6], hat_min, hat_max;
    uint8_t mapped_axes, report_id, raw_length;
    uint32_t mapped_buttons, reports;
    uint8_t raw[HID_INPUT_MAX_REPORT_BYTES];
    bool vendor, raw_view;
    iine_stage_t protocol_stage;
    uint32_t packets, bad_packets, last_input_ms;
    uint32_t changed_ms[15];
    uint32_t button_pressed_ms[12], dpad_pressed_ms[4];
} gamepad_ui_t;
typedef struct { bool send; gamepad_command_t command; gamepad_peer_t peer; } ui_action_t;
void gamepad_ui_init(gamepad_ui_t *ui);
void gamepad_ui_event(gamepad_ui_t *ui, const gamepad_event_t *event, uint32_t now);
/* key: 0 up, 1 down, 2 confirm; long press applies only to confirm. */
ui_action_t gamepad_ui_key(gamepad_ui_t *ui, int key, bool long_press);
unsigned gamepad_ui_visible(const gamepad_ui_t *ui, unsigned *indices);
int gamepad_ui_axis(const gamepad_ui_t *ui, unsigned axis, int radius);
uint8_t gamepad_ui_dpad(const gamepad_ui_t *ui);
/* Render a stripe into native-endian RGB565, independent of ESP-IDF. */
void gamepad_ui_render(const gamepad_ui_t *ui, uint32_t now, uint16_t *pixels, int y, int height);

void gamepad_screen_start(void);
void gamepad_screen_event(const gamepad_event_t *event);
