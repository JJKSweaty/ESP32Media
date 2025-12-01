/**
 * @file main.cpp
 * ESP32 Media Tracker with LVGL 9.x
 */

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <WiFi.h>

#include "data_model.h"
#include "ui.h"

// Transport mode: "serial", "wifi", or "both"
// Set to 1 to use WiFi TCP connection instead of serial
#define USE_WIFI_TRANSPORT 1

// Set to 1 to also keep serial for debugging (only when USE_WIFI_TRANSPORT is 1)
#define KEEP_SERIAL_DEBUG 1

// Increase Arduino loop task stack size (default is 8KB, we need more for LVGL 9)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// --- Touch pins for CYD ---
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// Hardware
SPIClass touchscreenSPI(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

// LVGL 9 display buffer - reduced size to save memory
static lv_color_t draw_buf[SCREEN_WIDTH * 10];

// LVGL 9 display flush callback
static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushPixels((uint16_t *)px_map, w * h);
    tft.endWrite();

    lv_display_flush_ready(disp);
}

// LVGL 9 touch read callback
static void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
    if (touchscreen.tirqTouched() && touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();
        // Use calibrated values from previous working setup
        int x = map(p.x, 178, 3895, 0, SCREEN_WIDTH - 1);
        int y = map(p.y, 318, 3851, 0, SCREEN_HEIGHT - 1);
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("ESP32 Media Tracker - LVGL 9");

    // Initialize TFT
    tft.init();
    tft.setRotation(1);
    tft.setSwapBytes(true);   // Important for correct colors with LVGL
    tft.invertDisplay(false); // Keep colors not inverted
    tft.fillScreen(TFT_BLACK);

    // Initialize touch
    touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchscreenSPI);
    touchscreen.setRotation(1);  // Match TFT rotation for easier coordinate handling

    // Initialize LVGL
    lv_init();

    // Create display (LVGL 9 API)
    lv_display_t *disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Create touch input device (LVGL 9 API)
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

    // Initialize data model and UI
    data_model_init();
    ui_init();

#if USE_WIFI_TRANSPORT
    // Start WiFi task for TCP data (WiFi credentials managed by WiFiManager)
    Serial.println("Starting WiFi transport...");
    start_wifi_task(TCP_SERVER_IP, TCP_SERVER_PORT);
    
    #if !KEEP_SERIAL_DEBUG
    // Optionally disable serial task if not needed
    #else
    // Keep serial for debugging but not for data
    Serial.println("Serial kept for debug output.");
    #endif
#else
    // Use serial for data transport
    start_serial_task();
#endif

    Serial.println("Setup complete.");
}

// Heap monitoring
static uint32_t lastHeapLog = 0;
static size_t minHeapSeen = 999999;

void loop() {
    lv_tick_inc(5);
    lv_timer_handler();
    
    // Smooth progress bar interpolation (call frequently)
    ui_tick();
    
    // Periodic heap monitoring (every 30 seconds)
    uint32_t now = millis();
    if (now - lastHeapLog > 30000) {
        lastHeapLog = now;
        size_t freeHeap = ESP.getFreeHeap();
        size_t minFree = ESP.getMinFreeHeap();
        if (freeHeap < minHeapSeen) minHeapSeen = freeHeap;
        Serial.printf("[HEAP] Free: %d, Min: %d, Session Min: %d\n", freeHeap, minFree, minHeapSeen);
        
        // Warn if heap is getting low
        if (freeHeap < 30000) {
            Serial.println("[HEAP] WARNING: Low memory!");
        }
    }

    SnapshotMsg msg;
    if (data_model_try_dequeue(msg)) {
        SystemData sys;
        MediaData med;

        sys.cpu = msg.cpu;
        sys.mem = msg.mem;
        sys.gpu = msg.gpu;
        sys.procCount = msg.procCount;
        sys.valid = true;
        
        for (uint8_t i = 0; i < msg.procCount && i < 5; ++i) {
            sys.procs[i] = String(msg.procs[i]);
                sys.procPids[i] = msg.procPids[i];
        }

        if (msg.hasMedia) {
            med.title = String(msg.title);
            med.artist = String(msg.artist);
            med.album = String(msg.album);
            med.source = String(msg.source);
            med.trackUri = String(msg.trackUri);
            med.position = msg.position;
            med.duration = msg.duration;
            med.isPlaying = msg.isPlaying;
            med.shuffle = msg.shuffle;
            med.repeat = msg.repeat;
            med.isLiked = msg.isLiked;
            med.valid = true;
            
            // Artwork is decoded directly into global buffer by data_model
            med.hasArtwork = msg.hasArtwork;
            med.artworkUpdated = msg.artworkUpdated;
            
            // Copy queue data
            med.hasQueue = msg.hasQueue;
            med.queueLen = msg.queueLen;
            for (uint8_t i = 0; i < msg.queueLen && i < MAX_QUEUE_ITEMS; ++i) {
                med.queue[i] = msg.queue[i];
            }
            
            // Copy playlist context
            med.hasPlaylist = msg.hasPlaylist;
            if (msg.hasPlaylist) {
                med.playlist = msg.playlist;
            }
        } else {
            med.valid = false;
            med.hasArtwork = false;
            med.artworkUpdated = false;
            med.hasQueue = false;
            med.hasPlaylist = false;
            med.shuffle = false;
            med.repeat = 0;
            med.isLiked = false;
        }

        ui_update(sys, med);
    }

    delay(5);
}
