#include "gamepad_ui.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static gamepad_ui_t s_ui;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_generation;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_dma_done;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_calibration;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }

void gamepad_screen_event(const gamepad_event_t *event)
{
    if (event->type == GAMEPAD_STATUS) return;
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    gamepad_ui_event(&s_ui, event, now);
    ++s_generation;
    portEXIT_CRITICAL(&s_lock);
}

static bool IRAM_ATTR dma_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *arg)
{
    (void)io; (void)edata; (void)arg;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(s_dma_done, &wake);
    return wake == pdTRUE;
}

static void lcd_init(void)
{
    s_dma_done = xSemaphoreCreateBinary();
    configASSERT(s_dma_done);
    spi_bus_config_t bus = {.mosi_io_num=9,.sclk_io_num=8,.miso_io_num=-1,
        .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=240*16*2};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t config = {.cs_gpio_num=1,.dc_gpio_num=20,.pclk_hz=80000000,
        .spi_mode=0,.lcd_cmd_bits=8,.lcd_param_bits=8,.trans_queue_depth=1,
        .on_color_trans_done=dma_done};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&config,&io));
    esp_lcd_panel_dev_config_t panel = {.reset_gpio_num=-1,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io,&panel,&s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* Same T200H7-C14-05 panel tuning and orientation as this badge's driver. */
    static const struct { uint8_t cmd,len,data[14]; } tuning[] = {
        {0xB2,5,{5,5,0,0x33,0x33}},{0xB7,1,{0x35}},{0xBB,1,{0x21}},
        {0xC0,1,{0x2C}},{0xC2,1,{1}},{0xC3,1,{0x0B}},{0xC4,1,{0x20}},
        {0xC6,1,{0x0F}},{0xD0,2,{0xA7,0xA1}},{0xD0,2,{0xA4,0xA1}},{0xD6,1,{0xA1}},
        {0xE0,14,{0xD0,4,8,0x0A,9,5,0x2D,0x43,0x49,9,0x16,0x15,0x26,0x2B}},
        {0xE1,14,{0xD0,3,9,0x0A,0x0A,6,0x2E,0x44,0x40,0x3A,0x15,0x15,0x26,0x2A}}
    };
    for(unsigned i=0;i<sizeof(tuning)/sizeof(tuning[0]);++i)
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io,tuning[i].cmd,tuning[i].data,tuning[i].len));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel,true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel,false,false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel,0,0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel,true));
    ledc_timer_config_t timer = {.speed_mode=LEDC_LOW_SPEED_MODE,.timer_num=LEDC_TIMER_0,
        .duty_resolution=LEDC_TIMER_10_BIT,.freq_hz=5000,.clk_cfg=LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t backlight = {.gpio_num=21,.speed_mode=LEDC_LOW_SPEED_MODE,
        .channel=LEDC_CHANNEL_0,.timer_sel=LEDC_TIMER_0,.duty=0};
    ESP_ERROR_CHECK(ledc_channel_config(&backlight));
}

static void screen_task(void *arg)
{
    (void)arg;
    uint16_t *stripe=heap_caps_malloc(240*16*2,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    configASSERT(stripe);
    gamepad_ui_t frame;
    uint32_t rendered=UINT32_MAX,last=0;
    bool first=true;
    for(;;) {
        uint32_t now=now_ms(),generation;
        portENTER_CRITICAL(&s_lock);
        generation=s_generation;
        frame=s_ui;
        portEXIT_CRITICAL(&s_lock);
        if(generation!=rendered || now-last>=500) {
            for(int y=0;y<320;y+=16) {
                gamepad_ui_render(&frame,now,stripe,y,16);
                for(unsigned i=0;i<240*16;++i) stripe[i]=(stripe[i]>>8)|(stripe[i]<<8);
                ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel,0,y,240,y+16,stripe));
                configASSERT(xSemaphoreTake(s_dma_done,pdMS_TO_TICKS(1000))==pdTRUE);
            }
            rendered=generation;
            last=now;
            if(first) {
                ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,818));
                ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0));
                ESP_LOGI("iine_ui","LCD first frame complete; backlight 80%%; 240x320; ADC keys enabled");
                ESP_LOGI("iine_ui","Free heap: %u bytes",(unsigned)esp_get_free_heap_size());
                first=false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void handle_key(int key,bool long_press)
{
    portENTER_CRITICAL(&s_lock);
    ui_action_t action=gamepad_ui_key(&s_ui,key,long_press);
    ++s_generation;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI("iine_ui","KEY %s %s",key==0?"UP":key==1?"DOWN":"OK",long_press?"hold":"click");
    if(action.send && !gamepad_hid_command(action.command,&action.peer)) {
        gamepad_event_t error={.type=GAMEPAD_LINK,.link=GP_ERROR,.message="Command queue busy; retry"};
        gamepad_screen_event(&error);
    }
}

static void keys_task(void *arg)
{
    (void)arg;
    int candidate=-1,stable=-1,samples=0;
    bool armed=false,long_sent=false;
    uint32_t pressed=0;
    for(;;) {
        int total=0,valid=0;
        for(int i=0;i<4;++i) {
            int raw;
            if(adc_oneshot_read(s_adc,ADC_CHANNEL_0,&raw)==ESP_OK) { total+=raw; ++valid; }
        }
        if(valid) {
            int raw=total/valid,mv=raw*2500/4095;
            if(s_calibration) adc_cali_raw_to_voltage(s_calibration,raw,&mv);
            int key=mv<150?0:mv<447?1:mv<1900?2:-1;
            if(key!=candidate) { candidate=key; samples=1; }
            else if(samples<4) ++samples;
            if(samples>=4) {
                if(key==-1) armed=true; /* Ignore a button held during boot. */
                if(armed && key!=stable) {
                    if(stable==2 && key==-1 && !long_sent) handle_key(2,false);
                    stable=key;
                    pressed=now_ms();
                    long_sent=false;
                    if(key==0 || key==1) handle_key(key,false);
                }
                if(armed && stable==2 && !long_sent && now_ms()-pressed>=900) {
                    handle_key(2,true);
                    long_sent=true;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void gamepad_screen_start(void)
{
    gamepad_ui_init(&s_ui);
    lcd_init();
    adc_oneshot_unit_init_cfg_t unit={.unit_id=ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit,&s_adc));
    adc_oneshot_chan_cfg_t channel={.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_DEFAULT};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc,ADC_CHANNEL_0,&channel));
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t calibration={.unit_id=ADC_UNIT_1,.chan=ADC_CHANNEL_0,
        .atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_DEFAULT};
    esp_err_t rc=adc_cali_create_scheme_curve_fitting(&calibration,&s_calibration);
#else
    adc_cali_line_fitting_config_t calibration={.unit_id=ADC_UNIT_1,
        .atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_DEFAULT};
    esp_err_t rc=adc_cali_create_scheme_line_fitting(&calibration,&s_calibration);
#endif
    if(rc!=ESP_OK) ESP_LOGW("iine_ui","ADC calibration unavailable; using C3 2.5V estimate");
    configASSERT(xTaskCreate(screen_task,"gamepad_lcd",6144,NULL,2,NULL)==pdPASS);
    configASSERT(xTaskCreate(keys_task,"gamepad_keys",3072,NULL,3,NULL)==pdPASS);
}
