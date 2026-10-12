#pragma once
#include <stdint.h>
#include "hid_input.h"

#define GAMEPAD_MAX_PEERS 16
typedef enum { GP_SCANNING, GP_CONNECTING, GP_PAIRING, GP_DISCOVERING, GP_READY, GP_ERROR } gamepad_link_t;
typedef struct {
    char name[40];
    uint8_t address[6], address_type;
    int8_t rssi;
    bool candidate, saved, seen;
} gamepad_peer_t;
typedef enum { GAMEPAD_STATUS, GAMEPAD_LAYOUT, GAMEPAD_INPUT, GAMEPAD_PEER,
               GAMEPAD_LINK, GAMEPAD_SCAN_CLEAR, GAMEPAD_FORGOTTEN,
               GAMEPAD_PROTOCOL } gamepad_event_type_t;
typedef enum { IINE_OFF, IINE_IDENTIFY, IINE_CAPABILITIES, IINE_STARTING,
               IINE_WAITING, IINE_RECEIVING, IINE_NO_INPUT, IINE_UNKNOWN } iine_stage_t;
typedef struct {
    gamepad_event_type_t type;
    char message[96];
    uint8_t report_id;
    uint8_t length;
    uint8_t raw[HID_INPUT_MAX_REPORT_BYTES];
    bool decoded;
    hid_input_state_t state;
    hid_input_field_t field;
    gamepad_peer_t peer;
    gamepad_link_t link;
    iine_stage_t protocol_stage;
    uint32_t packets, bad_packets;
} gamepad_event_t;

/* Called from the NimBLE host task. Consumer must copy and return promptly. */
typedef void (*gamepad_event_cb_t)(const gamepad_event_t *event);
void gamepad_hid_start(gamepad_event_cb_t callback);
/* Commands are copied to a queue; all BLE operations run on the host task. */
typedef enum { GP_CMD_CONNECT, GP_CMD_DISCONNECT, GP_CMD_REFRESH, GP_CMD_FORGET } gamepad_command_t;
bool gamepad_hid_command(gamepad_command_t command, const gamepad_peer_t *peer);
