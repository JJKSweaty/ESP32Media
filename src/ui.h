#pragma once
#include <lvgl.h>
#include "data_model.h"

// Initialize LVGL UI (tabview, styles, widgets)
void ui_init();

// Update UI elements from the latest data models
void ui_update(const SystemData &sys, const MediaData &med);
