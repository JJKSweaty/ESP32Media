#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>

#include "data_model.h"
#include "ui.h"

// --- Touch pins ---
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

SPIClass touchscreenSPI(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCREEN_WIDTH * 10]; // 10 lines

// Touch callback
static void touchscreen_read(lv_indev_drv_t * indev, lv_indev_data_t * data) {
    if (touchscreen.tirqTouched() && touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();
        int x = map(p.x, 178, 3895, 0, SCREEN_WIDTH - 1);
        int y = map(p.y, 318, 3851, 0, SCREEN_HEIGHT - 1);
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// LVGL flush -> TFT
static void my_disp_flush_cb(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p) {
    int32_t w = (area->x2 - area->x1 + 1);
    int32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushPixels((uint16_t *)color_p, w * h);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("Media Tracker: LVGL + Queue + RTOS");

    // TFT init
    tft.init();
    tft.setRotation(1);
    tft.setSwapBytes(true);   // matches LV_COLOR_16_SWAP 0
    tft.invertDisplay(false);

    // Touch init
    touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchscreenSPI);
    touchscreen.setRotation(1);

    // LVGL init + drivers (same as before)
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_WIDTH * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchscreen_read;
    lv_indev_drv_register(&indev_drv);

    data_model_init();
    ui_init();

    // Start the producer task
    start_serial_task();
}

void loop() {
    lv_timer_handler();
    lv_tick_inc(5);

    SnapshotMsg msg;
    if (data_model_try_dequeue(msg)) {
        // Map SnapshotMsg -> SystemData + MediaData for ui_update()
        SystemData sys;
        MediaData  med;

        sys.cpu = msg.cpu;
        sys.mem = msg.mem;
        sys.procCount = msg.procCount;
        sys.valid = true;
        for (uint8_t i = 0; i < msg.procCount && i < 5; ++i) {
            sys.procs[i] = String(msg.procs[i]);
        }

            if (msg.hasMedia) {
        med.title    = String(msg.title);
        med.artist   = String(msg.artist);
        med.album    = String(msg.album);
        med.position = msg.position;
        med.duration = msg.duration;

        // No artwork from queue yet -> just mark as absent
        med.artwork_b64 = "";
        med.hasArtwork  = false;

        med.valid = true;
    } else {
        med.valid       = false;
        med.hasArtwork  = false;
        med.artwork_b64 = "";
    }


        ui_update(sys, med);
    }

    delay(5);
}
