#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include "UI/ui.h"

// --- Touchscreen pin definitions ---
#define XPT2046_IRQ  36  // Touch interrupt
#define XPT2046_MOSI 32  // Touch SPI MOSI
#define XPT2046_MISO 39  // Touch SPI MISO
#define XPT2046_CLK  25  // Touch SPI Clock
#define XPT2046_CS   33  // Touch chip select

// --- Display size ---
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// --- Global objects ---
SPIClass touchscreenSPI(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

// LVGL draw buffer for display flushing
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCREEN_WIDTH * 10]; // Buffer for 10 lines

// A label to show touch info on the LVGL screen
static lv_obj_t * text_label_touch = nullptr;

// Calibrated touch coordinates (global variables)
int x, y, z;

// --- Helper function to print touch info to Serial ---

// --- LVGL Input Read Callback ---
// This function is called periodically by LVGL to fetch touch data.
static void touchscreen_read(lv_indev_drv_t * indev, lv_indev_data_t * data) {
  if(touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();

    // Calibrate the raw values:
    // Raw X range: 178 (min) to 3895 (max)
    // Raw Y range: 318 (min) to 3851 (max)
    int calibratedX = map(p.x, 178, 3895, 0, SCREEN_WIDTH - 1);
    int calibratedY = map(p.y, 318, 3851, 0, SCREEN_HEIGHT - 1);

    data->point.x = calibratedX;
    data->point.y = calibratedY;
    data->state = LV_INDEV_STATE_PRESSED;
    x = calibratedX;
    y = calibratedY;
    z = p.z;

    
  } 
}

// --- LVGL Display Flush Callback ---
static void my_disp_flush_cb(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p) {
  int32_t w = (area->x2 - area->x1 + 1);
  int32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushPixels((uint16_t *)color_p, w * h);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

void create_tabview_gui() {

  lv_obj_t * tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 50);
  
  lv_obj_set_width(tabview, SCREEN_WIDTH);
  lv_obj_align(tabview, LV_ALIGN_TOP_MID, 0, 0);

  // Add four tabs
  lv_obj_t * tab_music   = lv_tabview_add_tab(tabview, "Music");
  lv_obj_t * tab_discord = lv_tabview_add_tab(tabview, "Discord");
  lv_obj_t * tab_tasks   = lv_tabview_add_tab(tabview, "Tasks");
  lv_obj_t * tab_settings= lv_tabview_add_tab(tabview, "Settings");

  // Populate each tab with a simple label
  lv_obj_t * lbl_music = lv_label_create(tab_music);
  lv_label_set_text(lbl_music, "Music Page");
  lv_obj_center(lbl_music);

  lv_obj_t * lbl_discord = lv_label_create(tab_discord);
  lv_label_set_text(lbl_discord, "Discord Page");
  lv_obj_center(lbl_discord);

  lv_obj_t * lbl_tasks = lv_label_create(tab_tasks);
  lv_label_set_text(lbl_tasks, "Task Manager Page");
  lv_obj_center(lbl_tasks);

  lv_obj_t * lbl_settings = lv_label_create(tab_settings);
  lv_label_set_text(lbl_settings, "Settings Page");
  lv_obj_center(lbl_settings);
}


void setup() {
  Serial.begin(115200);
  Serial.println("Starting LVGL touchscreen test...");

  // --- Initialize TFT_eSPI display ---
  tft.init();
  tft.setRotation(1); 

  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
 
  touchscreen.setRotation(1);

  // --- Initialize LVGL ---
  lv_init();

  // Create a draw buffer for LVGL display flushing
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_WIDTH * 10);

  // Register display driver with LVGL
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_WIDTH;
  disp_drv.ver_res = SCREEN_HEIGHT;
  disp_drv.flush_cb = my_disp_flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // Register input device (touch) with LVGL
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touchscreen_read;
  lv_indev_drv_register(&indev_drv);
  tft.invertDisplay(false);
  ui_init();
  
  
}

void loop() {
  lv_timer_handler();  
  lv_tick_inc(5);      
  delay(5);
}
