#pragma once
#include "gamepad_hid.h"
#include "host/ble_gatt.h"

bool iine_service_matches(const ble_uuid_t *uuid);
void iine_link_init(gamepad_event_cb_t callback, void (*failure)(const char *, int));
void iine_link_start(uint16_t connection, const struct ble_gatt_svc *service);
void iine_link_reset(void);
bool iine_link_notification(uint16_t connection, uint16_t handle, struct os_mbuf *data);
/* Returns true when this backend owns the asynchronous disconnect. */
bool iine_link_disconnect(void);
