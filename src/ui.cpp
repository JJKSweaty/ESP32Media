/**
 * @file ui.cpp
 * LVGL 9.x UI for ESP32 Media Tracker
 * 
 * Uses arcs instead of meter (meter removed in LVGL 9)
 */

#include "ui.h"
#include "data_model.h"
#include <math.h>

// --- Styles ---
static lv_style_t style_screen_bg;
static lv_style_t style_card;
static lv_style_t style_label_primary;
static lv_style_t style_label_secondary;

// Canvas buffer for artwork (must be static to persist)
static lv_color_t canvas_buf[80 * 80];
static bool gArtworkDisplayed = false;

// --- Task UI holder (using arcs instead of meter in LVGL 9) ---
struct TaskUI {
    lv_obj_t *cpu_arc;
    lv_obj_t *mem_arc;
    lv_obj_t *gpu_arc;
    lv_obj_t *cpu_label;
    lv_obj_t *mem_label;
    lv_obj_t *gpu_label;
    lv_obj_t *proc_list;
};
static TaskUI taskUi;

// --- Music UI holder ---
struct MusicUI {
    lv_obj_t *art_container;  // Container for artwork
    lv_obj_t *art_canvas;     // Canvas for actual image
    lv_obj_t *art_icon;       // Fallback icon label
    lv_obj_t *title_label;
    lv_obj_t *artist_label;
    lv_obj_t *album_label;
    lv_obj_t *progress_bar;
    lv_obj_t *progress_label;
};
static MusicUI musicUi;

// Kill button callback for process list
static void kill_proc_event_cb(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    // Find child label
    lv_obj_t *label = nullptr;
    uint32_t child_cnt = lv_obj_get_child_cnt(btn);
    for (uint32_t i = 0; i < child_cnt; ++i) {
        lv_obj_t *c = lv_obj_get_child(btn, i);
        if (c && lv_obj_check_type(c, &lv_label_class)) {
            label = c;
            break;
        }
    }
    if (!label) {
        // fallback: try child 0
        label = lv_obj_get_child(btn, 0);
    }
    if (label) {
        const char *txt = lv_label_get_text(label);
        Serial.print("[UI] Kill requested for: ");
        Serial.println(txt);
        // Parse a PID at the start like "1234: ..."
        int pid = 0;
        if (txt) {
            // find colon
            const char *colon = strchr(txt, ':');
            if (colon) {
                // parse integer before colon
                char buf[16];
                int n = colon - txt;
                if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
                memcpy(buf, txt, n);
                buf[n] = '\0';
                pid = atoi(buf);
            }
        }
        if (pid > 0) {
            // Send JSON command over serial to host: {"cmd":"kill","pid":1234}\n
            char out[64];
            snprintf(out, sizeof(out), "{\"cmd\":\"kill\",\"pid\":%d}\n", pid);
            Serial.print(out);
        } else {
            Serial.println("[UI] Could not parse PID from label.");
        }
    }
}

static void play_event_cb(lv_event_t *e) {
    (void)e;
    const char *cmd = "{\"cmd\":\"play\"}\n";
    Serial.print(cmd);
}

static void pause_event_cb(lv_event_t *e) {
    (void)e;
    const char *cmd = "{\"cmd\":\"pause\"}\n";
    Serial.print(cmd);
}

static void next_event_cb(lv_event_t *e) {
    (void)e;
    const char *cmd = "{\"cmd\":\"next\"}\n";
    Serial.print(cmd);
}

static void prev_event_cb(lv_event_t *e) {
    (void)e;
    const char *cmd = "{\"cmd\":\"previous\"}\n";
    Serial.print(cmd);
}

// Throttle UI updates
static uint32_t gLastUpdateMs = 0;
static const uint32_t UPDATE_INTERVAL_MS = 100;  // 100ms = 10 updates/sec

// Cache last values to avoid redundant updates
static int gLastCpu = -1, gLastMem = -1, gLastGpu = -1;
static String gLastProcs[5];
static uint8_t gLastProcCount = 255;
static String gLastTitle = "";

static void format_time(char *buf, size_t buf_len, int seconds) {
    if (seconds < 0) seconds = 0;
    int m = seconds / 60;
    int s = seconds % 60;
    snprintf(buf, buf_len, "%d:%02d", m, s);
}

// --- Styles ---
static void init_styles() {
    lv_style_init(&style_screen_bg);
    lv_style_set_bg_color(&style_screen_bg, lv_color_hex(0x101018));

    lv_style_init(&style_card);
    lv_style_set_radius(&style_card, 8);
    lv_style_set_bg_color(&style_card, lv_color_hex(0x1a1a2e));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_pad_all(&style_card, 8);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_border_color(&style_card, lv_color_hex(0x303050));

    lv_style_init(&style_label_primary);
    lv_style_set_text_color(&style_label_primary, lv_color_hex(0xFFFFFF));

    lv_style_init(&style_label_secondary);
    lv_style_set_text_color(&style_label_secondary, lv_color_hex(0x909090));
}

// --- MUSIC TAB ---
static void build_music_tab(lv_obj_t *parent) {
    lv_obj_add_style(parent, &style_screen_bg, 0);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Main card
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_size(card, 310, 185);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 2);

    // Artwork container
    lv_obj_t *art_container = lv_obj_create(card);
    lv_obj_set_size(art_container, 80, 80);
    lv_obj_align(art_container, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(art_container, 6, 0);
    lv_obj_set_style_bg_color(art_container, lv_color_hex(0x303050), 0);
    lv_obj_set_style_border_width(art_container, 0, 0);
    lv_obj_set_style_pad_all(art_container, 0, 0);
    lv_obj_remove_flag(art_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Canvas for actual artwork image
    lv_obj_t *art_canvas = lv_canvas_create(art_container);
    lv_canvas_set_buffer(art_canvas, canvas_buf, 80, 80, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(art_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(art_canvas, LV_OBJ_FLAG_HIDDEN);  // Hidden until we have artwork
    
    // Music icon placeholder (shown when no artwork)
    lv_obj_t *icon = lv_label_create(art_container);
    lv_label_set_text(icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x606080), 0);
    lv_obj_center(icon);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_obj_add_style(title, &style_label_primary, 0);
    lv_label_set_text(title, "No media playing");
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, 210);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 90, 5);

    // Artist
    lv_obj_t *artist = lv_label_create(card);
    lv_obj_add_style(artist, &style_label_secondary, 0);
    lv_label_set_text(artist, "Artist");
    lv_obj_set_width(artist, 210);
    lv_obj_align(artist, LV_ALIGN_TOP_LEFT, 90, 28);

    // Album
    lv_obj_t *album = lv_label_create(card);
    lv_obj_add_style(album, &style_label_secondary, 0);
    lv_label_set_text(album, "Album");
    lv_obj_set_width(album, 210);
    lv_obj_align(album, LV_ALIGN_TOP_LEFT, 90, 50);

    // Progress bar
    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, 290, 10);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x303050), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_palette_main(LV_PALETTE_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);

    // Time label
    lv_obj_t *time_label = lv_label_create(card);
    lv_obj_add_style(time_label, &style_label_secondary, 0);
    lv_label_set_text(time_label, "0:00 / 0:00");
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_MID, 0, -5);

    musicUi.art_container = art_container;
    musicUi.art_canvas = art_canvas;
    musicUi.art_icon = icon;
    musicUi.title_label = title;
    musicUi.artist_label = artist;
    musicUi.album_label = album;
    musicUi.progress_bar = bar;
    musicUi.progress_label = time_label;

    // Controls: back / play / next
    lv_obj_t *controls = lv_obj_create(card);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, 260, 32);
    lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_layout(controls, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(controls, 4, 0);

    lv_obj_t *prev_btn = lv_btn_create(controls);
    lv_obj_set_size(prev_btn, 44, 28);
    lv_obj_t *prev_label = lv_label_create(prev_btn);
    lv_label_set_text(prev_label, LV_SYMBOL_PREV);
    lv_obj_center(prev_label);
    lv_obj_add_event_cb(prev_btn, prev_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *play_btn = lv_btn_create(controls);
    lv_obj_set_size(play_btn, 44, 28);
    lv_obj_t *play_label = lv_label_create(play_btn);
    lv_label_set_text(play_label, LV_SYMBOL_PLAY);
    lv_obj_center(play_label);
    lv_obj_add_event_cb(play_btn, play_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *pause_btn = lv_btn_create(controls);
    lv_obj_set_size(pause_btn, 44, 28);
    lv_obj_t *pause_label = lv_label_create(pause_btn);
    lv_label_set_text(pause_label, LV_SYMBOL_PAUSE);
    lv_obj_center(pause_label);
    lv_obj_add_event_cb(pause_btn, pause_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next_btn = lv_btn_create(controls);
    lv_obj_set_size(next_btn, 44, 28);
    lv_obj_t *next_label = lv_label_create(next_btn);
    lv_label_set_text(next_label, LV_SYMBOL_NEXT);
    lv_obj_center(next_label);
    lv_obj_add_event_cb(next_btn, next_event_cb, LV_EVENT_CLICKED, NULL);
}

// --- TASK TAB --- (using arcs for LVGL 9)
static void build_task_tab(lv_obj_t *parent) {
    lv_obj_add_style(parent, &style_screen_bg, 0);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Left panel: arcs + labels
    lv_obj_t *left_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(left_panel);
    lv_obj_add_style(left_panel, &style_card, 0);
    lv_obj_set_size(left_panel, 125, 185);
    lv_obj_align(left_panel, LV_ALIGN_TOP_LEFT, 2, 2);

    // CPU Arc (outermost, cyan)
    lv_obj_t *cpu_arc = lv_arc_create(left_panel);
    lv_obj_set_size(cpu_arc, 90, 90);
    lv_obj_align(cpu_arc, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_rotation(cpu_arc, 135);
    lv_arc_set_bg_angles(cpu_arc, 0, 270);
    lv_arc_set_range(cpu_arc, 0, 100);
    lv_arc_set_value(cpu_arc, 0);
    lv_obj_remove_style(cpu_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(cpu_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(cpu_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(cpu_arc, lv_color_hex(0x303050), LV_PART_MAIN);
    lv_obj_set_style_arc_color(cpu_arc, lv_palette_main(LV_PALETTE_CYAN), LV_PART_INDICATOR);
    lv_obj_remove_flag(cpu_arc, LV_OBJ_FLAG_CLICKABLE);

    // MEM Arc (middle, orange)
    lv_obj_t *mem_arc = lv_arc_create(left_panel);
    lv_obj_set_size(mem_arc, 70, 70);
    lv_obj_align(mem_arc, LV_ALIGN_TOP_MID, 0, 10);
    lv_arc_set_rotation(mem_arc, 135);
    lv_arc_set_bg_angles(mem_arc, 0, 270);
    lv_arc_set_range(mem_arc, 0, 100);
    lv_arc_set_value(mem_arc, 0);
    lv_obj_remove_style(mem_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(mem_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(mem_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mem_arc, lv_color_hex(0x303050), LV_PART_MAIN);
    lv_obj_set_style_arc_color(mem_arc, lv_palette_main(LV_PALETTE_ORANGE), LV_PART_INDICATOR);
    lv_obj_remove_flag(mem_arc, LV_OBJ_FLAG_CLICKABLE);

    // GPU Arc (innermost, green)
    lv_obj_t *gpu_arc = lv_arc_create(left_panel);
    lv_obj_set_size(gpu_arc, 50, 50);
    lv_obj_align(gpu_arc, LV_ALIGN_TOP_MID, 0, 20);
    lv_arc_set_rotation(gpu_arc, 135);
    lv_arc_set_bg_angles(gpu_arc, 0, 270);
    lv_arc_set_range(gpu_arc, 0, 100);
    lv_arc_set_value(gpu_arc, 0);
    lv_obj_remove_style(gpu_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(gpu_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(gpu_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(gpu_arc, lv_color_hex(0x303050), LV_PART_MAIN);
    lv_obj_set_style_arc_color(gpu_arc, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);
    lv_obj_remove_flag(gpu_arc, LV_OBJ_FLAG_CLICKABLE);

    // Labels below arcs
    lv_obj_t *cpu_label = lv_label_create(left_panel);
    lv_obj_add_style(cpu_label, &style_label_primary, 0);
    lv_label_set_text(cpu_label, "CPU: 0%");
    lv_obj_set_style_text_color(cpu_label, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_align(cpu_label, LV_ALIGN_BOTTOM_LEFT, 5, -45);

    lv_obj_t *mem_label = lv_label_create(left_panel);
    lv_obj_add_style(mem_label, &style_label_primary, 0);
    lv_label_set_text(mem_label, "MEM: 0%");
    lv_obj_set_style_text_color(mem_label, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_align(mem_label, LV_ALIGN_BOTTOM_LEFT, 5, -25);

    lv_obj_t *gpu_label = lv_label_create(left_panel);
    lv_obj_add_style(gpu_label, &style_label_primary, 0);
    lv_label_set_text(gpu_label, "GPU: 0%");
    lv_obj_set_style_text_color(gpu_label, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_align(gpu_label, LV_ALIGN_BOTTOM_LEFT, 5, -5);

    // Right panel: process list
    lv_obj_t *right_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(right_panel);
    lv_obj_add_style(right_panel, &style_card, 0);
    lv_obj_set_size(right_panel, 180, 185);
    lv_obj_align(right_panel, LV_ALIGN_TOP_RIGHT, -2, 0);

    lv_obj_t *list_title = lv_label_create(right_panel);
    lv_obj_add_style(list_title, &style_label_secondary, 0);
    lv_label_set_text(list_title, "Top Processes");
    lv_obj_align(list_title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *list = lv_list_create(right_panel);
    lv_obj_set_size(list, 165, 155);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x151525), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);

    taskUi.cpu_arc = cpu_arc;
    taskUi.mem_arc = mem_arc;
    taskUi.gpu_arc = gpu_arc;
    taskUi.cpu_label = cpu_label;
    taskUi.mem_label = mem_label;
    taskUi.gpu_label = gpu_label;
    taskUi.proc_list = list;
}

// --- DISCORD TAB ---
static void build_discord_tab(lv_obj_t *parent) {
    lv_obj_add_style(parent, &style_screen_bg, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_size(card, 300, 160);
    lv_obj_center(card);

    lv_obj_t *label = lv_label_create(card);
    lv_obj_add_style(label, &style_label_primary, 0);
    lv_label_set_text(label, "Discord\nComing soon");
    lv_obj_center(label);
}

// --- SETTINGS TAB ---
static void build_settings_tab(lv_obj_t *parent) {
    lv_obj_add_style(parent, &style_screen_bg, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_size(card, 300, 160);
    lv_obj_center(card);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_add_style(title, &style_label_primary, 0);
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *desc = lv_label_create(card);
    lv_obj_add_style(desc, &style_label_secondary, 0);
    lv_label_set_text(desc, "Theme, brightness,\nupdate interval...");
    lv_obj_align(desc, LV_ALIGN_TOP_LEFT, 0, 25);
}

// --- Public API ---

void ui_init() {
    init_styles();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_add_style(scr, &style_screen_bg, 0);

    // LVGL 9 tabview API
    lv_obj_t *tabview = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(tabview, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tabview, 35);
    lv_obj_set_size(tabview, 320, 240);
    lv_obj_align(tabview, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *tab_music   = lv_tabview_add_tab(tabview, "Music");
    lv_obj_t *tab_tasks   = lv_tabview_add_tab(tabview, "Tasks");
    lv_obj_t *tab_discord = lv_tabview_add_tab(tabview, "Discord");
    lv_obj_t *tab_settings= lv_tabview_add_tab(tabview, "Settings");

    build_music_tab(tab_music);
    build_task_tab(tab_tasks);
    build_discord_tab(tab_discord);
    build_settings_tab(tab_settings);
}

void ui_update(const SystemData &sys, const MediaData &med) {
    // Throttle updates
    uint32_t now = millis();
    if (now - gLastUpdateMs < UPDATE_INTERVAL_MS) {
        return;
    }
    gLastUpdateMs = now;

    char buf[64];

    // --- Check for new artwork ---
    if (artwork_is_ready() && musicUi.art_canvas) {
        // Copy RGB565 data to canvas buffer
        uint8_t *src = artwork_get_buffer();
        size_t srcSize = artwork_get_size();
        
        if (srcSize == ARTWORK_BUF_SIZE) {
            // Direct copy - RGB565 data is already in correct format
            memcpy(canvas_buf, src, srcSize);
            
            // Show canvas, hide icon
            lv_obj_remove_flag(musicUi.art_canvas, LV_OBJ_FLAG_HIDDEN);
            if (musicUi.art_icon) lv_obj_add_flag(musicUi.art_icon, LV_OBJ_FLAG_HIDDEN);
            
            // Invalidate canvas to trigger redraw
            lv_obj_invalidate(musicUi.art_canvas);
            
            gArtworkDisplayed = true;
            Serial.println("[UI] Artwork displayed");
        }
        artwork_clear_ready();  // Mark as consumed
    }

    // --- Tasks ---
    int cpu_i = (int)round(sys.cpu);
    int mem_i = (int)round(sys.mem);
    int gpu_i = (int)round(sys.gpu);

    // Update arcs
    if (taskUi.cpu_arc) lv_arc_set_value(taskUi.cpu_arc, cpu_i);
    if (taskUi.mem_arc) lv_arc_set_value(taskUi.mem_arc, mem_i);
    if (taskUi.gpu_arc) lv_arc_set_value(taskUi.gpu_arc, gpu_i);

    // Update labels only if changed
    if (cpu_i != gLastCpu && taskUi.cpu_label) {
        snprintf(buf, sizeof(buf), "CPU: %d%%", cpu_i);
        lv_label_set_text(taskUi.cpu_label, buf);
        gLastCpu = cpu_i;
    }
    if (mem_i != gLastMem && taskUi.mem_label) {
        snprintf(buf, sizeof(buf), "MEM: %d%%", mem_i);
        lv_label_set_text(taskUi.mem_label, buf);
        gLastMem = mem_i;
    }
    if (gpu_i != gLastGpu && taskUi.gpu_label) {
        snprintf(buf, sizeof(buf), "GPU: %d%%", gpu_i);
        lv_label_set_text(taskUi.gpu_label, buf);
        gLastGpu = gpu_i;
    }

    // Update process list only if changed
    if (taskUi.proc_list) {
        bool procsChanged = (sys.procCount != gLastProcCount);
        if (!procsChanged) {
            for (uint8_t i = 0; i < sys.procCount && i < 5; ++i) {
                if (sys.procs[i] != gLastProcs[i]) {
                    procsChanged = true;
                    break;
                }
            }
        }
        if (procsChanged) {
            lv_obj_clean(taskUi.proc_list);
            for (uint8_t i = 0; i < sys.procCount; ++i) {
                if (!sys.procs[i].length()) continue;
                // Button with close icon + process text
                lv_obj_t *btn = lv_list_add_button(taskUi.proc_list, LV_SYMBOL_CLOSE, sys.procs[i].c_str());
                lv_obj_add_event_cb(btn, kill_proc_event_cb, LV_EVENT_CLICKED, NULL);
                gLastProcs[i] = sys.procs[i];
            }
            gLastProcCount = sys.procCount;
        }
    }

    // --- Music ---
    if (med.valid) {
        // Only update title if changed
        if (med.title != gLastTitle && musicUi.title_label) {
            lv_label_set_text(musicUi.title_label, med.title.c_str());
            gLastTitle = med.title;
        }
        if (musicUi.artist_label)
            lv_label_set_text(musicUi.artist_label, med.artist.c_str());
        if (musicUi.album_label)
            lv_label_set_text(musicUi.album_label, med.album.c_str());

        int dur = med.duration > 0 ? med.duration : 1;
        int pos = med.position;
        if (pos < 0) pos = 0;
        if (pos > dur) pos = dur;

        if (musicUi.progress_bar) {
            lv_bar_set_range(musicUi.progress_bar, 0, dur);
            lv_bar_set_value(musicUi.progress_bar, pos, LV_ANIM_OFF);
        }

        if (musicUi.progress_label) {
            char pos_str[16];
            char dur_str[16];
            format_time(pos_str, sizeof(pos_str), pos);
            format_time(dur_str, sizeof(dur_str), dur);
            snprintf(buf, sizeof(buf), "%s / %s", pos_str, dur_str);
            lv_label_set_text(musicUi.progress_label, buf);
        }
        
        // Show/hide artwork placeholder based on whether we have displayed artwork
        if (!gArtworkDisplayed && musicUi.art_icon) {
            // No artwork yet - show icon
            lv_obj_remove_flag(musicUi.art_icon, LV_OBJ_FLAG_HIDDEN);
            if (musicUi.art_canvas) lv_obj_add_flag(musicUi.art_canvas, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
