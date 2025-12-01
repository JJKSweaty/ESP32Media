#pragma once
#include <lvgl.h>
#include "data_model.h"

// Initialize LVGL UI (tabview, styles, widgets)
void ui_init();

// Update UI elements from the latest data models
void ui_update(const SystemData &sys, const MediaData &med);

// Set play/pause state programmatically (used for ack from server)
void ui_set_play_state(bool is_playing);

// Call frequently (e.g., in main loop) to smoothly interpolate progress bar
void ui_tick();
