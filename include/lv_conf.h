/* Minimal LVGL config for ESP32Media-main
 * Based on lv_conf_template.h (LVGL v9.x)
 * This file provides a minimal set of configuration
 * macros required to ensure consistent behavior.
 *
 * NOTE: For full control, copy lv_conf_template.h from the
 * LVGL repo and edit the settings. This file intentionally
 * keeps changes minimal and uses lv_conf_internal.h defaults
 * for everything else.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* Color settings */
#define LV_COLOR_DEPTH 16
/* If your display requires swapped bytes for RGB565, set 1.
 * Many TFT drivers swap bytes internally — if you see inverted or wrong
 * colors after `tft.setSwapBytes(true)`, try setting LV_COLOR_16_SWAP to 0
 * (and the opposite if that doesn't work). */
#define LV_COLOR_16_SWAP 0

/* Memory pool size for lv_malloc (in bytes) */
#ifndef LV_MEM_SIZE
#define LV_MEM_SIZE (64 * 1024U)
#endif

/* Timing */
/* Default refresh period in ms; leave default if unsure */
/* #define LV_DEF_REFR_PERIOD 33 */

/* Use builtin C stdlib wrapper (malloc/strlen/etc.) */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

/* Fonts — enable commonly used Montserrat sizes used by the UI. */
/* LVGL will only compile the selected fonts; enabling ensures symbols like
 * `lv_font_montserrat_12/14/16` are available to `ui.cpp`. */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_28 1


/* Set default font (fallback if no explicit font passed) */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Disable OS usage (we use lv_tick_inc in the Arduino loop) */
#define LV_USE_OS LV_OS_NONE

/* Draw configuration: use built-in software renderer */
#define LV_USE_DRAW_SW 1

/* Keep additional features out: keep defaults in lv_conf_internal.h */

#endif /*LV_CONF_H*/
