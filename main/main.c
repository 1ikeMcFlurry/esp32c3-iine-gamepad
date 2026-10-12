#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "gamepad_hid.h"
#include "gamepad_ui.h"

static QueueHandle_t s_events;
static unsigned s_dropped;
static portMUX_TYPE s_drop_lock = portMUX_INITIALIZER_UNLOCKED;

static void on_gamepad(const gamepad_event_t *event)
{
    gamepad_screen_event(event);
    if (event->type == GAMEPAD_PEER || event->type == GAMEPAD_SCAN_CLEAR || event->type == GAMEPAD_FORGOTTEN || event->type == GAMEPAD_PROTOCOL) return;
    /* USB logging must never block the Bluetooth host. */
    if (xQueueSend(s_events, event, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_drop_lock);
        ++s_dropped;
        portEXIT_CRITICAL(&s_drop_lock);
    }
}

static void log_task(void *arg)
{
    (void)arg;
    gamepad_event_t event;
    for (;;) {
        if (xQueueReceive(s_events, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (event.type == GAMEPAD_LINK) {
                ESP_LOGI("iine", "LINK state=%d %s", event.link, event.message);
            } else if (event.type != GAMEPAD_INPUT) {
                ESP_LOGI("iine", "%s", event.message);
            } else {
                bool log_raw = !event.decoded;
#ifdef CONFIG_IINE_LOG_RAW
                log_raw = true;
#endif
                if (log_raw) {
                    char hex[HID_INPUT_MAX_REPORT_BYTES * 3 + 1];
                    for (unsigned i = 0; i < event.length; ++i)
                        snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", event.raw[i]);
                    ESP_LOGI("iine", "RAW id=%u len=%u %s", event.report_id, event.length, hex);
                }
                if (event.decoded) {
                    const hid_input_state_t *s = &event.state;
                    ESP_LOGI("iine", "INPUT id=%u buttons=%08lX axes_mask=%02X "
                             "X=%ld Y=%ld Z=%ld Rx=%ld Ry=%ld Rz=%ld hat=%ld dpad=%X",
                             event.report_id, (unsigned long)s->buttons, s->axes_present,
                             (long)s->axes[0], (long)s->axes[1], (long)s->axes[2],
                             (long)s->axes[3], (long)s->axes[4], (long)s->axes[5],
                             (long)s->hat, s->dpad);
                }
            }
        }
        portENTER_CRITICAL(&s_drop_lock);
        unsigned dropped = s_dropped;
        s_dropped = 0;
        portEXIT_CRITICAL(&s_drop_lock);
        if (dropped) ESP_LOGW("iine", "%u log events dropped; USB consumer too slow", dropped);
    }
}

void app_main(void)
{
    s_events = xQueueCreate(32, sizeof(gamepad_event_t));
    configASSERT(s_events);
    BaseType_t rc = xTaskCreate(log_task, "gamepad_log", 4096, NULL, 2, NULL);
    configASSERT(rc == pdPASS);
    gamepad_screen_start();
    gamepad_hid_start(on_gamepad);
}
