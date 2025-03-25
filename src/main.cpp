#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>


#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

SPIClass mySpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

TFT_eSPI tft = TFT_eSPI(); // Display


static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[320 * 10]; // Example: 10 lines of 320 pixels each


void my_disp_flush_cb(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p)
{

  int32_t w = (area->x2 - area->x1 + 1);
  int32_t h = (area->y2 - area->y1 + 1);


  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushPixels((uint16_t *)&color_p[0], w * h);
  tft.endWrite();

  // Tell LVGL we're done
  lv_disp_flush_ready(disp);
}



static void my_touch_read_cb(lv_indev_drv_t * indev, lv_indev_data_t * data)
{
  // Check if touched
  if (ts.tirqTouched() && ts.touched())
  {
    TS_Point p = ts.getPoint();
    // Map raw to screen (adjust raw min/max to your panel)
    int screenX = map(p.x, 200, 3800, 0, 320);
    int screenY = map(p.y, 240, 3800, 0, 240);

    // In some rotations you might invert axes or swap X<->Y
    // e.g. screenX = 320 - screenX; etc. if it appears mirrored

    data->point.x = screenX;
    data->point.y = screenY;
    data->state   = LV_INDEV_STATE_PR; // pressed
  }
  else
  {
    data->state = LV_INDEV_STATE_REL; // not pressed
  }
}



void setup() {
  Serial.begin(115200);

  // Initialize the TFT
  tft.init();
  tft.setRotation(1); // e.g. landscape

  // Initialize the Touch
  mySpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(mySpi);
  ts.setRotation(1); // match orientation

  // Initialize LVGL
  lv_init();

  // Create a draw buffer (we'll do single buffering here)
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, 320 * 10);

  // Register display driver
  static lv_disp_drv_t disp_drv; // Must be static or global
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 320;
  disp_drv.ver_res = 240;
  disp_drv.flush_cb = my_disp_flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // Register input driver (touch)
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read_cb;
  lv_indev_drv_register(&indev_drv);


  lv_obj_t * label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "Hello LVGL");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}


void loop() {
  
  lv_timer_handler();  
  delay(5);            
  tft.drawCircle(160, 120, 50, TFT_RED);

}
