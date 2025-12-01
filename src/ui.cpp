/**
 * @file ui.cpp
 * LVGL 9.x UI for ESP32 Media Tracker
 * 
 * Uses arcs instead of meter (meter removed in LVGL 9)
 */

#include "ui.h"
#include "data_model.h"
#include "wifi_manager.h"
#include <math.h>
#include <WiFi.h>

// --- Styles ---
static lv_style_t style_screen_bg;
static lv_style_t style_card;
static lv_style_t style_label_primary;
static lv_style_t style_label_secondary;
static lv_style_t style_kill_btn;  // Style for kill buttons

// Artwork display - use lv_image with raw RGB565 data
static lv_image_dsc_t artwork_dsc;
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
    lv_obj_t *art_img;        // Image widget for artwork
    lv_obj_t *art_icon;       // Fallback icon label
    lv_obj_t *title_label;
    lv_obj_t *artist_label;
    lv_obj_t *album_label;
    lv_obj_t *progress_bar;
    lv_obj_t *progress_label;
    lv_obj_t *play_pause_btn;  // Single toggle button
    lv_obj_t *play_pause_label; // Label to update icon
    bool is_playing;           // Track current state
    // Shuffle/Repeat/Add-to-Playlist buttons
    lv_obj_t *shuffle_btn;
    lv_obj_t *shuffle_label;
    lv_obj_t *repeat_btn;
    lv_obj_t *repeat_label;
    lv_obj_t *add_playlist_btn;
    lv_obj_t *add_playlist_label;
    bool shuffle_state;        // Current shuffle state
    uint8_t repeat_state;      // 0=off, 1=track, 2=context
    // Queue page elements
    lv_obj_t *now_playing_page; // Main music page
    lv_obj_t *queue_page;       // Queue page container
    lv_obj_t *queue_btn;        // Button to open queue
    lv_obj_t *back_btn;         // Button to go back from queue
    lv_obj_t *queue_title;      // Queue page title
    lv_obj_t *queue_list;       // List widget for queue items
    lv_obj_t *playlist_label;   // Current playlist name on queue page
    // Local progress interpolation
    int last_server_position;   // Last position from server
    int last_server_duration;   // Last duration from server
    uint32_t last_update_ms;    // Timestamp of last server update
    int interpolated_position;  // Smoothly interpolated position
};
static MusicUI musicUi;

// Cache for queue to detect changes
static char gLastQueueItems[MAX_QUEUE_ITEMS][MAX_STR_ESP];
static uint8_t gLastQueueLen = 255;

// Command buffer size (must match data_model.h)
#define CMD_MAX_LEN 128

// Kill button callback for process list
static void kill_proc_event_cb(lv_event_t *e) {
    // Get the label passed as user_data
    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
    
    if (label && lv_obj_check_type(label, &lv_label_class)) {
        // Try to read PID from label's user data (preferred)
        int pid = 0;
        void *ud = lv_obj_get_user_data(label);
        if (ud) pid = (int)(intptr_t)ud;
        
        // Parse a PID at the start like "1234: ..." if user_data is empty
        if (pid == 0) {
            const char *txt = lv_label_get_text(label);
            if (txt) {
                const char *colon = strchr(txt, ':');
                if (colon) {
                    char buf[16];
                    int n = colon - txt;
                    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
                    memcpy(buf, txt, n);
                    buf[n] = '\0';
                    pid = atoi(buf);
                }
            }
        }
        if (pid > 0) {
            // Send JSON command to Python server
            char out[CMD_MAX_LEN];
            snprintf(out, sizeof(out), "{\"cmd\":\"kill\",\"pid\":%d}\n", pid);
            send_command(out);
        }
    }
}

// Display artwork from the global RGB565 buffer (decoded in data_model)
static void update_artwork() {
    if (!musicUi.art_img) return;
    if (!artwork_is_new()) return;
    
    // Get the decoded RGB565 data from global buffer
    uint8_t* rgb565_data = artwork_get_rgb565_buffer();
    
    // Setup image descriptor for LVGL
    artwork_dsc.header.w = ARTWORK_WIDTH;
    artwork_dsc.header.h = ARTWORK_HEIGHT;
    artwork_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    artwork_dsc.header.stride = ARTWORK_WIDTH * 2;
    artwork_dsc.data_size = ARTWORK_RGB565_SIZE;
    artwork_dsc.data = rgb565_data;

    // Set image source
    lv_image_set_src(musicUi.art_img, &artwork_dsc);

    // Show image, hide icon
    lv_obj_remove_flag(musicUi.art_img, LV_OBJ_FLAG_HIDDEN);
    if (musicUi.art_icon) lv_obj_add_flag(musicUi.art_icon, LV_OBJ_FLAG_HIDDEN);

    gArtworkDisplayed = true;
    artwork_clear_new();  // Mark as consumed
    Serial.println("[UI] Artwork displayed");
}

// Debounce for play button to prevent jitter
static uint32_t gLastPlayPressMs = 0;
static const uint32_t PLAY_DEBOUNCE_MS = 400;  // Ignore presses within 400ms

static void play_event_cb(lv_event_t *e) {
    (void)e;
    
    // Debounce - ignore rapid presses
    uint32_t now = lv_tick_get();
    if (now - gLastPlayPressMs < PLAY_DEBOUNCE_MS) {
        return;
    }
    gLastPlayPressMs = now;
    
    // Toggle play/pause locally for immediate UX feedback
    musicUi.is_playing = !musicUi.is_playing;
    if (musicUi.play_pause_label) {
        lv_label_set_text(musicUi.play_pause_label, musicUi.is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    // Send the appropriate command based on current state
    const char *cmd = musicUi.is_playing ? "{\"cmd\":\"play\"}\n" : "{\"cmd\":\"pause\"}\n";
    send_command(cmd);
}

// Debounce for next/prev buttons
static uint32_t gLastNavPressMs = 0;
static const uint32_t NAV_DEBOUNCE_MS = 300;

static void next_event_cb(lv_event_t *e) {
    (void)e;
    uint32_t now = lv_tick_get();
    if (now - gLastNavPressMs < NAV_DEBOUNCE_MS) return;
    gLastNavPressMs = now;
    send_command("{\"cmd\":\"next\"}\n");
}

static void prev_event_cb(lv_event_t *e) {
    (void)e;
    uint32_t now = lv_tick_get();
    if (now - gLastNavPressMs < NAV_DEBOUNCE_MS) return;
    gLastNavPressMs = now;
    send_command("{\"cmd\":\"previous\"}\n");
}

// Shuffle button callback - toggle shuffle state
static void shuffle_event_cb(lv_event_t *e) {
    (void)e;
    uint32_t now = lv_tick_get();
    if (now - gLastNavPressMs < NAV_DEBOUNCE_MS) return;
    gLastNavPressMs = now;
    
    // Toggle shuffle: send opposite of current state
    bool new_state = !musicUi.shuffle_state;
    
    // Immediate visual feedback
    musicUi.shuffle_state = new_state;
    if (new_state) {
        lv_obj_set_style_bg_color(musicUi.shuffle_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        lv_obj_set_style_bg_color(musicUi.shuffle_btn, lv_color_hex(0x404060), 0);
    }
    
    char out[CMD_MAX_LEN];
    snprintf(out, sizeof(out), "{\"cmd\":\"shuffle\",\"state\":%s}\n", new_state ? "true" : "false");
    send_command(out);
}

// Repeat button callback - cycle through off -> context -> track -> off
static void repeat_event_cb(lv_event_t *e) {
    (void)e;
    uint32_t now = lv_tick_get();
    if (now - gLastNavPressMs < NAV_DEBOUNCE_MS) return;
    gLastNavPressMs = now;
    
    // Cycle repeat state: 0=off -> 2=context -> 1=track -> 0=off
    uint8_t new_repeat = 0;
    const char *new_state = "off";
    if (musicUi.repeat_state == 0) {
        new_state = "context";
        new_repeat = 2;
    } else if (musicUi.repeat_state == 2) {
        new_state = "track";
        new_repeat = 1;
    } else {
        new_state = "off";
        new_repeat = 0;
    }
    
    // Immediate visual feedback
    musicUi.repeat_state = new_repeat;
    if (new_repeat == 1) {
        lv_obj_set_style_bg_color(musicUi.repeat_btn, lv_palette_main(LV_PALETTE_ORANGE), 0);
        lv_label_set_text(musicUi.repeat_label, "1");
    } else if (new_repeat == 2) {
        lv_obj_set_style_bg_color(musicUi.repeat_btn, lv_palette_main(LV_PALETTE_CYAN), 0);
        lv_label_set_text(musicUi.repeat_label, LV_SYMBOL_LOOP);
    } else {
        lv_obj_set_style_bg_color(musicUi.repeat_btn, lv_color_hex(0x404060), 0);
        lv_label_set_text(musicUi.repeat_label, LV_SYMBOL_LOOP);
    }
    
    char out[CMD_MAX_LEN];
    snprintf(out, sizeof(out), "{\"cmd\":\"repeat\",\"state\":\"%s\"}\n", new_state);
    send_command(out);
}

// Add to playlist button callback - adds current track to user's default playlist
static void add_playlist_event_cb(lv_event_t *e) {
    (void)e;
    uint32_t now = lv_tick_get();
    if (now - gLastNavPressMs < NAV_DEBOUNCE_MS) return;
    gLastNavPressMs = now;
    
    // Visual feedback - brief highlight
    lv_obj_set_style_bg_color(musicUi.add_playlist_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
    
    // Send add to playlist command
    char out[CMD_MAX_LEN];
    snprintf(out, sizeof(out), "{\"cmd\":\"add_to_playlist\"}\n");
    send_command(out);
    
    // Reset color after brief delay (will be reset on next UI update anyway)
}

// Debounce for queue item clicks (longer to prevent memory issues)
static uint32_t gLastQueueClickMs = 0;
static const uint32_t QUEUE_CLICK_DEBOUNCE_MS = 1000;  // 1 second debounce

// Queue item click - play this track immediately
static void queue_item_click_cb(lv_event_t *e) {
    // Debounce
    uint32_t now = lv_tick_get();
    if (now - gLastQueueClickMs < QUEUE_CLICK_DEBOUNCE_MS) {
        return;
    }
    gLastQueueClickMs = now;
    
    // Use current target (object the callback was attached to) for reliable userdata
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    // Fallback to event target if needed
    if (!obj) obj = (lv_obj_t *)lv_event_get_target(e);

    // The play button should have user_data set directly
    void *ud = lv_obj_get_user_data(obj);
    
    // If not found, try parent (in case label was clicked)
    if (!ud && obj) {
        lv_obj_t *parent = lv_obj_get_parent(obj);
        if (parent) ud = lv_obj_get_user_data(parent);
    }
    
    if (ud) {
        int idx = (int)(intptr_t)ud;
        Serial.print("[UI] Queue play clicked, index=");
        Serial.println(idx);
        char out[CMD_MAX_LEN];
        snprintf(out, sizeof(out), "{\"cmd\":\"queue_action\",\"action\":\"play_now\",\"index\":%d}\n", idx);
        send_command(out);
    } else {
        Serial.println("[UI] Queue play clicked but no userdata found");
    }
}

// ========== QUEUE REORDER WITH UP/DOWN ARROWS ==========

// Navigate to queue page
static void show_queue_page_cb(lv_event_t *e) {
    (void)e;
    if (musicUi.now_playing_page) lv_obj_add_flag(musicUi.now_playing_page, LV_OBJ_FLAG_HIDDEN);
    if (musicUi.queue_page) lv_obj_remove_flag(musicUi.queue_page, LV_OBJ_FLAG_HIDDEN);
}

// Navigate back to now playing page
static void show_now_playing_cb(lv_event_t *e) {
    (void)e;
    if (musicUi.queue_page) lv_obj_add_flag(musicUi.queue_page, LV_OBJ_FLAG_HIDDEN);
    if (musicUi.now_playing_page) lv_obj_remove_flag(musicUi.now_playing_page, LV_OBJ_FLAG_HIDDEN);
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
    
    // Kill button style - red background, white X, larger
    lv_style_init(&style_kill_btn);
    lv_style_set_bg_color(&style_kill_btn, lv_palette_main(LV_PALETTE_RED));
    lv_style_set_bg_opa(&style_kill_btn, LV_OPA_COVER);
    lv_style_set_radius(&style_kill_btn, 4);
    lv_style_set_text_color(&style_kill_btn, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_kill_btn, &lv_font_montserrat_16);
    lv_style_set_pad_all(&style_kill_btn, 2);
}

// --- MUSIC TAB ---
static void build_music_tab(lv_obj_t *parent) {
    lv_obj_add_style(parent, &style_screen_bg, 0);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ========== NOW PLAYING PAGE ==========
    lv_obj_t *now_playing_page = lv_obj_create(parent);
    lv_obj_remove_style_all(now_playing_page);
    lv_obj_set_size(now_playing_page, 320, 200);
    lv_obj_align(now_playing_page, LV_ALIGN_TOP_MID, 0, 0);
    musicUi.now_playing_page = now_playing_page;

    // Main card - now playing info
    lv_obj_t *card = lv_obj_create(now_playing_page);
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
    lv_obj_set_style_clip_corner(art_container, true, 0);
    lv_obj_remove_flag(art_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Image widget for artwork (hidden until we have data)
    lv_obj_t *art_img = lv_image_create(art_container);
    lv_obj_set_size(art_img, 80, 80);
    lv_obj_align(art_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);  // Hidden until we have artwork
    
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
    lv_obj_set_width(title, 180);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 90, 5);

    // Artist (scrolling single line)
    lv_obj_t *artist = lv_label_create(card);
    lv_obj_add_style(artist, &style_label_secondary, 0);
    lv_label_set_text(artist, "Artist");
    lv_label_set_long_mode(artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(artist, 180);
    lv_obj_align(artist, LV_ALIGN_TOP_LEFT, 90, 28);

    // Album (scrolling single line)
    lv_obj_t *album = lv_label_create(card);
    lv_obj_add_style(album, &style_label_secondary, 0);
    lv_label_set_text(album, "Album");
    lv_label_set_long_mode(album, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(album, 180);
    lv_obj_align(album, LV_ALIGN_TOP_LEFT, 90, 50);

    // Queue button - positioned below album art (left side)
    lv_obj_t *queue_btn = lv_btn_create(card);
    lv_obj_set_size(queue_btn, 80, 24);
    lv_obj_align(queue_btn, LV_ALIGN_TOP_LEFT, 0, 85);  // Below artwork
    lv_obj_set_style_bg_color(queue_btn, lv_color_hex(0x303050), 0);
    lv_obj_set_style_radius(queue_btn, 4, 0);
    lv_obj_t *queue_btn_label = lv_label_create(queue_btn);
    lv_label_set_text(queue_btn_label, LV_SYMBOL_LIST " Queue");
    lv_obj_set_style_text_font(queue_btn_label, &lv_font_montserrat_12, 0);
    lv_obj_center(queue_btn_label);
    lv_obj_add_event_cb(queue_btn, show_queue_page_cb, LV_EVENT_CLICKED, NULL);
    musicUi.queue_btn = queue_btn;

    // Progress bar - positioned above controls
    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, 290, 8);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -42);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x303050), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_palette_main(LV_PALETTE_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);

    // Time label - just below progress bar
    lv_obj_t *time_label = lv_label_create(card);
    lv_obj_add_style(time_label, &style_label_secondary, 0);
    lv_label_set_text(time_label, "0:00 / 0:00");
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_MID, 0, -52);

    musicUi.art_container = art_container;
    musicUi.art_img = art_img;
    musicUi.art_icon = icon;
    musicUi.title_label = title;
    musicUi.artist_label = artist;
    musicUi.album_label = album;
    musicUi.progress_bar = bar;
    musicUi.progress_label = time_label;

    // Secondary controls row: shuffle / like / repeat - positioned on the RIGHT side
    lv_obj_t *secondary_controls = lv_obj_create(card);
    lv_obj_remove_style_all(secondary_controls);
    lv_obj_set_size(secondary_controls, 110, 24);
    lv_obj_align(secondary_controls, LV_ALIGN_TOP_RIGHT, 0, 85);  // Right side, same row as queue
    lv_obj_set_layout(secondary_controls, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(secondary_controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(secondary_controls, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(secondary_controls, 5, 0);

    // Shuffle button
    lv_obj_t *shuffle_btn = lv_btn_create(secondary_controls);
    lv_obj_set_size(shuffle_btn, 32, 24);
    lv_obj_set_style_bg_color(shuffle_btn, lv_color_hex(0x404060), 0);  // Slightly lighter default
    lv_obj_set_style_radius(shuffle_btn, 4, 0);
    lv_obj_t *shuffle_label = lv_label_create(shuffle_btn);
    lv_label_set_text(shuffle_label, LV_SYMBOL_SHUFFLE);
    lv_obj_set_style_text_font(shuffle_label, &lv_font_montserrat_12, 0);
    lv_obj_center(shuffle_label);
    lv_obj_add_event_cb(shuffle_btn, shuffle_event_cb, LV_EVENT_CLICKED, NULL);
    musicUi.shuffle_btn = shuffle_btn;
    musicUi.shuffle_label = shuffle_label;
    musicUi.shuffle_state = false;

    // Add to playlist button (+)
    lv_obj_t *add_btn = lv_btn_create(secondary_controls);
    lv_obj_set_size(add_btn, 32, 24);
    lv_obj_set_style_bg_color(add_btn, lv_color_hex(0x404060), 0);
    lv_obj_set_style_radius(add_btn, 4, 0);
    lv_obj_t *add_label = lv_label_create(add_btn);
    lv_label_set_text(add_label, LV_SYMBOL_PLUS);  // Plus icon for add
    lv_obj_set_style_text_font(add_label, &lv_font_montserrat_12, 0);
    lv_obj_center(add_label);
    lv_obj_add_event_cb(add_btn, add_playlist_event_cb, LV_EVENT_CLICKED, NULL);
    musicUi.add_playlist_btn = add_btn;
    musicUi.add_playlist_label = add_label;

    // Repeat button
    lv_obj_t *repeat_btn = lv_btn_create(secondary_controls);
    lv_obj_set_size(repeat_btn, 32, 24);
    lv_obj_set_style_bg_color(repeat_btn, lv_color_hex(0x404060), 0);
    lv_obj_set_style_radius(repeat_btn, 4, 0);
    lv_obj_t *repeat_label = lv_label_create(repeat_btn);
    lv_label_set_text(repeat_label, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_font(repeat_label, &lv_font_montserrat_12, 0);
    lv_obj_center(repeat_label);
    lv_obj_add_event_cb(repeat_btn, repeat_event_cb, LV_EVENT_CLICKED, NULL);
    musicUi.repeat_btn = repeat_btn;
    musicUi.repeat_label = repeat_label;
    musicUi.repeat_state = 0;  // 0=off, 1=track, 2=context

    // Controls: back / play-pause / next - at bottom
    lv_obj_t *controls = lv_obj_create(card);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, 200, 30);
    lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_layout(controls, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(controls, 16, 0);

    lv_obj_t *prev_btn = lv_btn_create(controls);
    lv_obj_set_size(prev_btn, 50, 28);
    lv_obj_t *prev_label = lv_label_create(prev_btn);
    lv_label_set_text(prev_label, LV_SYMBOL_PREV);
    lv_obj_center(prev_label);
    lv_obj_add_event_cb(prev_btn, prev_event_cb, LV_EVENT_CLICKED, NULL);

    // Single play/pause toggle button
    lv_obj_t *play_btn = lv_btn_create(controls);
    lv_obj_set_size(play_btn, 50, 28);
    lv_obj_t *play_label = lv_label_create(play_btn);
    lv_label_set_text(play_label, LV_SYMBOL_PLAY);  // Default to play icon
    lv_obj_center(play_label);
    lv_obj_add_event_cb(play_btn, play_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Store references for updating the icon
    musicUi.play_pause_btn = play_btn;
    musicUi.play_pause_label = play_label;
    musicUi.is_playing = false;

    lv_obj_t *next_btn = lv_btn_create(controls);
    lv_obj_set_size(next_btn, 50, 28);
    lv_obj_t *next_label = lv_label_create(next_btn);
    lv_label_set_text(next_label, LV_SYMBOL_NEXT);
    lv_obj_center(next_label);
    lv_obj_add_event_cb(next_btn, next_event_cb, LV_EVENT_CLICKED, NULL);

    // ========== QUEUE PAGE (initially hidden) ==========
    lv_obj_t *queue_page = lv_obj_create(parent);
    lv_obj_remove_style_all(queue_page);
    lv_obj_set_size(queue_page, 320, 200);
    lv_obj_align(queue_page, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(queue_page, LV_OBJ_FLAG_HIDDEN);  // Start hidden
    musicUi.queue_page = queue_page;

    // Queue card
    lv_obj_t *queue_card = lv_obj_create(queue_page);
    lv_obj_remove_style_all(queue_card);
    lv_obj_add_style(queue_card, &style_card, 0);
    lv_obj_set_size(queue_card, 310, 185);
    lv_obj_align(queue_card, LV_ALIGN_TOP_MID, 0, 2);

    // Back button - top left
    lv_obj_t *back_btn = lv_btn_create(queue_card);
    lv_obj_set_size(back_btn, 70, 26);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x303050), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_12, 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, show_now_playing_cb, LV_EVENT_CLICKED, NULL);
    musicUi.back_btn = back_btn;

    // Queue title / playlist name - centered at top
    lv_obj_t *playlist_label = lv_label_create(queue_card);
    lv_obj_add_style(playlist_label, &style_label_primary, 0);
    lv_label_set_text(playlist_label, "Up Next");
    lv_label_set_long_mode(playlist_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(playlist_label, 150);
    lv_obj_align(playlist_label, LV_ALIGN_TOP_MID, 0, 4);
    musicUi.playlist_label = playlist_label;

    // Queue list - scrollable
    lv_obj_t *queue_list = lv_list_create(queue_card);
    lv_obj_set_size(queue_list, 294, 145);
    lv_obj_align(queue_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(queue_list, lv_color_hex(0x151525), 0);
    lv_obj_set_style_border_width(queue_list, 0, 0);
    lv_obj_set_style_pad_all(queue_list, 4, 0);
    lv_obj_set_style_pad_row(queue_list, 4, 0);
    musicUi.queue_list = queue_list;
    musicUi.queue_title = playlist_label;
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
    lv_obj_set_size(left_panel, 125, 180);
    // Nudge the left panel down slightly so its inner content aligns vertically
    // with the right panel's title and content
    lv_obj_align(left_panel, LV_ALIGN_TOP_LEFT, 2, 8);

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
    lv_obj_set_size(right_panel, 180, 180);
    // Match the vertical offset to the left panel for consistent alignment
    lv_obj_align(right_panel, LV_ALIGN_TOP_RIGHT, -2, 8);

    lv_obj_t *list_title = lv_label_create(right_panel);
    lv_obj_add_style(list_title, &style_label_secondary, 0);
    lv_label_set_text(list_title, "Top Processes");
    lv_obj_align(list_title, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t *list = lv_list_create(right_panel);
    // Move the list down slightly to avoid overlap with the title and ensure the top
    // row is visible (was getting cut off on some displays / fonts).
    lv_obj_set_size(list, 165, 150);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x151525), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_top(list, 6, 0);

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

// --- SETTINGS TAB with Network Info ---
// Settings UI holder
struct SettingsUI {
    lv_obj_t *wifi_status_label;
    lv_obj_t *wifi_ip_label;
    lv_obj_t *server_status_label;
    lv_obj_t *ssid_label;
    lv_obj_t *rssi_label;
    lv_obj_t *scan_btn;
    lv_obj_t *network_list;
    lv_obj_t *password_textarea;
    lv_obj_t *connect_btn;
    lv_obj_t *keyboard;
    lv_obj_t *password_popup;
    char selected_ssid[33];
    bool scan_pending;
};
static SettingsUI settingsUi;

// Forward declarations for settings callbacks
static void wifi_scan_btn_cb(lv_event_t *e);
static void network_item_click_cb(lv_event_t *e);
static void password_connect_cb(lv_event_t *e);
static void password_cancel_cb(lv_event_t *e);
static void keyboard_event_cb(lv_event_t *e);
static void update_wifi_status_display();
static void update_network_list();

// Password popup for connecting to networks
static void show_password_popup(const char* ssid) {
    strncpy(settingsUi.selected_ssid, ssid, 32);
    settingsUi.selected_ssid[32] = '\0';
    
    // Create fullscreen overlay for keyboard popup
    lv_obj_t *popup = lv_obj_create(lv_screen_active());
    lv_obj_set_size(popup, 320, 240);
    lv_obj_set_pos(popup, 0, 0);
    lv_obj_set_style_bg_color(popup, lv_color_hex(0x0d0d1a), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 0, 0);
    lv_obj_set_style_radius(popup, 0, 0);
    lv_obj_set_style_pad_all(popup, 5, 0);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    settingsUi.password_popup = popup;
    
    // Title - compact at top
    lv_obj_t *title = lv_label_create(popup);
    lv_label_set_text_fmt(title, LV_SYMBOL_WIFI " %s", ssid);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1db954), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 300);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    
    // Password input - below title
    lv_obj_t *ta = lv_textarea_create(popup);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, "Enter password...");
    lv_obj_set_size(ta, 240, 32);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x1db954), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(0xffffff), 0);
    settingsUi.password_textarea = ta;
    
    // Buttons row - next to text area
    lv_obj_t *cancel_btn = lv_button_create(popup);
    lv_obj_set_size(cancel_btn, 70, 28);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 5, 58);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(cancel_btn, 4, 0);
    lv_obj_add_event_cb(cancel_btn, password_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE " Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(cancel_lbl);
    
    lv_obj_t *connect_btn = lv_button_create(popup);
    lv_obj_set_size(connect_btn, 70, 28);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_RIGHT, -5, 58);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x1db954), 0);
    lv_obj_set_style_radius(connect_btn, 4, 0);
    lv_obj_add_event_cb(connect_btn, password_connect_cb, LV_EVENT_CLICKED, NULL);
    settingsUi.connect_btn = connect_btn;
    lv_obj_t *connect_lbl = lv_label_create(connect_btn);
    lv_label_set_text(connect_lbl, LV_SYMBOL_OK " Join");
    lv_obj_set_style_text_font(connect_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(connect_lbl);
    
    // Keyboard - fills bottom area
    lv_obj_t *kb = lv_keyboard_create(popup);
    lv_obj_set_size(kb, 310, 145);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x252540), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(0xffffff), LV_PART_ITEMS);
    // Handle checkmark (enter) and cancel keys
    lv_obj_add_event_cb(kb, keyboard_event_cb, LV_EVENT_READY, NULL);  // Checkmark pressed
    lv_obj_add_event_cb(kb, keyboard_event_cb, LV_EVENT_CANCEL, NULL); // X pressed
    settingsUi.keyboard = kb;
}

// Keyboard event handler for checkmark/cancel
static void keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        // Checkmark pressed - connect
        password_connect_cb(e);
    } else if (code == LV_EVENT_CANCEL) {
        // X pressed - cancel
        password_cancel_cb(e);
    }
}

static void password_cancel_cb(lv_event_t *e) {
    (void)e;
    if (settingsUi.password_popup) {
        lv_obj_delete(settingsUi.password_popup);
        settingsUi.password_popup = NULL;
    }
}

static void password_connect_cb(lv_event_t *e) {
    (void)e;
    const char* password = lv_textarea_get_text(settingsUi.password_textarea);
    
    // Connect to network
    if (wifiMgr.connect(settingsUi.selected_ssid, password, true)) {
        // Connected successfully
        Serial.printf("[UI] Connected to %s\n", settingsUi.selected_ssid);
    } else {
        Serial.printf("[UI] Failed to connect to %s\n", settingsUi.selected_ssid);
    }
    
    // Close popup
    password_cancel_cb(e);
    
    // Update status display
    update_wifi_status_display();
}

static void wifi_scan_btn_cb(lv_event_t *e) {
    (void)e;
    lv_label_set_text(lv_obj_get_child(settingsUi.scan_btn, 0), "Scanning...");
    lv_refr_now(NULL);  // Force immediate UI refresh to show "Scanning..."
    
    wifiMgr.startScan();  // Synchronous scan - blocks until complete
    
    lv_label_set_text(lv_obj_get_child(settingsUi.scan_btn, 0), LV_SYMBOL_REFRESH " Scan Networks");
    update_network_list();
    settingsUi.scan_pending = false;
}

static void network_item_click_cb(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t*)lv_event_get_target(e);
    const char* ssid = (const char*)lv_obj_get_user_data(btn);
    
    if (ssid) {
        // Check if this is a saved network
        char password[65];
        if (wifiMgr.findSavedPassword(ssid, password, sizeof(password))) {
            // Saved network - connect directly
            if (wifiMgr.connect(ssid, password, false)) {
                Serial.printf("[UI] Connected to saved network: %s\n", ssid);
            }
            update_wifi_status_display();
        } else {
            // New network - show password popup
            show_password_popup(ssid);
        }
    }
}

static void update_network_list() {
    if (!settingsUi.network_list) return;
    
    // Clear existing items
    lv_obj_clean(settingsUi.network_list);
    
    NetworkInfo networks[10];
    int count = wifiMgr.getScanResults(networks, 10);
    
    Serial.printf("[UI] Displaying %d networks\n", count);
    
    for (int i = 0; i < count; i++) {
        Serial.printf("[UI] Network %d: %s (%d dBm)\n", i, networks[i].ssid, networks[i].rssi);
        
        lv_obj_t *btn = lv_button_create(settingsUi.network_list);
        lv_obj_set_size(btn, 130, 22);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x252540), 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_pad_all(btn, 2, 0);
        
        // Store SSID in user_data (static memory needed)
        static char ssid_storage[10][33];
        strncpy(ssid_storage[i], networks[i].ssid, 32);
        ssid_storage[i][32] = '\0';
        lv_obj_set_user_data(btn, ssid_storage[i]);
        lv_obj_add_event_cb(btn, network_item_click_cb, LV_EVENT_CLICKED, NULL);
        
        // Network name with signal indicator
        lv_obj_t *lbl = lv_label_create(btn);
        char txt[48];
        const char* saved = networks[i].saved ? "*" : "";
        const char* signal = networks[i].rssi > -50 ? LV_SYMBOL_WIFI : 
                            (networks[i].rssi > -70 ? LV_SYMBOL_WIFI : LV_SYMBOL_WIFI);
        snprintf(txt, sizeof(txt), "%s%s %s", saved, signal, networks[i].ssid);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, 120);
        lv_obj_center(lbl);
    }
    
    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(settingsUi.network_list);
        lv_label_set_text(lbl, "No networks found");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    }
}

static void update_wifi_status_display() {
    if (!settingsUi.ssid_label) return;
    
    if (wifiMgr.isConnected()) {
        String ssid = wifiMgr.getConnectedSSID();
        char buf[64];
        snprintf(buf, sizeof(buf), "SSID: %s", ssid.c_str());
        lv_label_set_text(settingsUi.ssid_label, buf);
        
        lv_label_set_text(settingsUi.wifi_status_label, "WiFi: Connected");
        lv_obj_set_style_text_color(settingsUi.wifi_status_label, lv_color_hex(0x1db954), 0);
        
        IPAddress ip = wifiMgr.getIP();
        snprintf(buf, sizeof(buf), "IP: %s", ip.toString().c_str());
        lv_label_set_text(settingsUi.wifi_ip_label, buf);
        
        int32_t rssi = wifiMgr.getRSSI();
        snprintf(buf, sizeof(buf), "Signal: %ld dBm", rssi);
        lv_label_set_text(settingsUi.rssi_label, buf);
    } else {
        lv_label_set_text(settingsUi.ssid_label, "SSID: Not connected");
        lv_label_set_text(settingsUi.wifi_status_label, "WiFi: Disconnected");
        lv_obj_set_style_text_color(settingsUi.wifi_status_label, lv_color_hex(0xff4444), 0);
        lv_label_set_text(settingsUi.wifi_ip_label, "IP: ---.---.---.---");
        lv_label_set_text(settingsUi.rssi_label, "Signal: -- dBm");
    }
}

static void build_settings_tab(lv_obj_t *parent) {
    lv_obj_add_style(parent, &style_screen_bg, 0);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Main card
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_size(card, 310, 185);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 2);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_obj_add_style(title, &style_label_primary, 0);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS " Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    // ========== NETWORK SECTION ==========
    lv_obj_t *net_section = lv_obj_create(card);
    lv_obj_remove_style_all(net_section);
    lv_obj_set_size(net_section, 145, 130);
    lv_obj_align(net_section, LV_ALIGN_TOP_LEFT, 0, 25);
    lv_obj_set_style_bg_color(net_section, lv_color_hex(0x151525), 0);
    lv_obj_set_style_bg_opa(net_section, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(net_section, 6, 0);
    lv_obj_set_style_pad_all(net_section, 6, 0);
    lv_obj_remove_flag(net_section, LV_OBJ_FLAG_SCROLLABLE);

    // Network section title
    lv_obj_t *net_title = lv_label_create(net_section);
    lv_obj_add_style(net_title, &style_label_primary, 0);
    lv_label_set_text(net_title, LV_SYMBOL_WIFI " WiFi Status");
    lv_obj_set_style_text_font(net_title, &lv_font_montserrat_10, 0);
    lv_obj_align(net_title, LV_ALIGN_TOP_LEFT, 0, 0);

    // WiFi SSID (dynamic)
    lv_obj_t *ssid_label = lv_label_create(net_section);
    lv_obj_add_style(ssid_label, &style_label_secondary, 0);
    lv_label_set_text(ssid_label, "SSID: Connecting...");
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_10, 0);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 0, 14);
    lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ssid_label, 135);
    settingsUi.ssid_label = ssid_label;

    // WiFi status (connected/connecting/disconnected)
    lv_obj_t *wifi_status = lv_label_create(net_section);
    lv_obj_add_style(wifi_status, &style_label_secondary, 0);
    lv_label_set_text(wifi_status, "WiFi: Connecting...");
    lv_obj_set_style_text_font(wifi_status, &lv_font_montserrat_10, 0);
    lv_obj_align(wifi_status, LV_ALIGN_TOP_LEFT, 0, 28);
    settingsUi.wifi_status_label = wifi_status;

    // IP Address
    lv_obj_t *ip_label = lv_label_create(net_section);
    lv_obj_add_style(ip_label, &style_label_secondary, 0);
    lv_label_set_text(ip_label, "IP: ---.---.---.---");
    lv_obj_set_style_text_font(ip_label, &lv_font_montserrat_10, 0);
    lv_obj_align(ip_label, LV_ALIGN_TOP_LEFT, 0, 42);
    settingsUi.wifi_ip_label = ip_label;

    // Signal strength (RSSI)
    lv_obj_t *rssi_label = lv_label_create(net_section);
    lv_obj_add_style(rssi_label, &style_label_secondary, 0);
    lv_label_set_text(rssi_label, "Signal: -- dBm");
    lv_obj_set_style_text_font(rssi_label, &lv_font_montserrat_10, 0);
    lv_obj_align(rssi_label, LV_ALIGN_TOP_LEFT, 0, 56);
    settingsUi.rssi_label = rssi_label;

    // Server connection status
    lv_obj_t *server_status = lv_label_create(net_section);
    lv_obj_add_style(server_status, &style_label_secondary, 0);
    char server_txt[48];
    snprintf(server_txt, sizeof(server_txt), "Server: %s", TCP_SERVER_IP);
    lv_label_set_text(server_status, server_txt);
    lv_obj_set_style_text_font(server_status, &lv_font_montserrat_10, 0);
    lv_obj_align(server_status, LV_ALIGN_TOP_LEFT, 0, 70);
    lv_label_set_long_mode(server_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(server_status, 135);
    settingsUi.server_status_label = server_status;

    // ========== NETWORK SELECTION SECTION ==========
    lv_obj_t *scan_section = lv_obj_create(card);
    lv_obj_remove_style_all(scan_section);
    lv_obj_set_size(scan_section, 145, 130);
    lv_obj_align(scan_section, LV_ALIGN_TOP_RIGHT, 0, 25);
    lv_obj_set_style_bg_color(scan_section, lv_color_hex(0x151525), 0);
    lv_obj_set_style_bg_opa(scan_section, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scan_section, 6, 0);
    lv_obj_set_style_pad_all(scan_section, 6, 0);
    lv_obj_remove_flag(scan_section, LV_OBJ_FLAG_SCROLLABLE);

    // Scan section title with button
    lv_obj_t *scan_btn = lv_button_create(scan_section);
    lv_obj_set_size(scan_btn, 130, 20);
    lv_obj_align(scan_btn, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(0x1db954), 0);
    lv_obj_set_style_radius(scan_btn, 4, 0);
    lv_obj_add_event_cb(scan_btn, wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    settingsUi.scan_btn = scan_btn;
    
    lv_obj_t *scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, LV_SYMBOL_REFRESH " Scan Networks");
    lv_obj_set_style_text_font(scan_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(scan_lbl);

    // Network list container
    lv_obj_t *net_list = lv_obj_create(scan_section);
    lv_obj_remove_style_all(net_list);
    lv_obj_set_size(net_list, 135, 100);
    lv_obj_align(net_list, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(net_list, lv_color_hex(0x0d0d1a), 0);
    lv_obj_set_style_bg_opa(net_list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(net_list, 4, 0);
    lv_obj_set_style_pad_all(net_list, 3, 0);
    lv_obj_set_flex_flow(net_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(net_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(net_list, 3, 0);
    lv_obj_set_scrollbar_mode(net_list, LV_SCROLLBAR_MODE_AUTO);
    settingsUi.network_list = net_list;

    // Placeholder text
    lv_obj_t *placeholder = lv_label_create(net_list);
    lv_label_set_text(placeholder, "Tap scan to find\nWiFi networks");
    lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(placeholder, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER, 0);

    // ========== VERSION INFO ==========
    lv_obj_t *version_label = lv_label_create(card);
    lv_obj_add_style(version_label, &style_label_secondary, 0);
    lv_label_set_text(version_label, "v1.0.0 | WiFi Manager enabled");
    lv_obj_set_style_text_font(version_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(version_label, lv_color_hex(0x505050), 0);
    lv_obj_align(version_label, LV_ALIGN_BOTTOM_MID, 0, -5);

    // Initialize settings state
    settingsUi.scan_pending = false;
    settingsUi.password_popup = NULL;
    memset(settingsUi.selected_ssid, 0, sizeof(settingsUi.selected_ssid));
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
    
    // Disable swiping between tabs (use tab buttons only)
    // Get the content area of the tabview and disable scrolling/gestures
    lv_obj_t *content = lv_tabview_get_content(tabview);
    if (content) {
        lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLL_ONE);
        lv_obj_remove_flag(content, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_scroll_dir(content, LV_DIR_NONE);
    }

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

    // --- Check for new artwork (decoded in data_model, stored in global buffer) ---
    // Always check artwork_is_new() since artwork may arrive as a standalone message
    if (artwork_is_new()) {
        update_artwork();
    }

    // --- Update network status in settings (every 2 seconds) ---
    static uint32_t lastNetUpdate = 0;
    if (now - lastNetUpdate > 2000) {
        lastNetUpdate = now;
        update_wifi_status_display();
    }
    
    // --- Check for scan completion ---
    if (settingsUi.scan_pending && wifiMgr.isScanComplete()) {
        settingsUi.scan_pending = false;
        lv_label_set_text(lv_obj_get_child(settingsUi.scan_btn, 0), LV_SYMBOL_REFRESH " Scan Networks");
        update_network_list();
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
                
                // Create a custom row with kill button + text
                lv_obj_t *row = lv_obj_create(taskUi.proc_list);
                lv_obj_remove_style_all(row);
                // Make rows slightly taller to avoid label clipping on top/bottom
                lv_obj_set_size(row, LV_PCT(100), 34);
                lv_obj_set_layout(row, LV_LAYOUT_FLEX);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_pad_column(row, 4, 0);
                lv_obj_set_style_pad_all(row, 4, 0);
                
                // Kill button - bigger, red, with X
                lv_obj_t *kill_btn = lv_button_create(row);
                lv_obj_add_style(kill_btn, &style_kill_btn, 0);
                lv_obj_set_size(kill_btn, 28, 22);
                lv_obj_t *x_label = lv_label_create(kill_btn);
                lv_label_set_text(x_label, "X");
                lv_obj_center(x_label);
                
                // Process name label (store the full text for PID extraction)
                lv_obj_t *proc_label = lv_label_create(row);
                lv_obj_add_style(proc_label, &style_label_primary, 0);
                lv_label_set_text(proc_label, sys.procs[i].c_str());
                lv_obj_set_style_text_font(proc_label, &lv_font_montserrat_12, 0);
                // Clip long names horizontally, but prefer scrolling to avoid truncated display
                lv_label_set_long_mode(proc_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
                lv_obj_set_style_text_align(proc_label, LV_TEXT_ALIGN_LEFT, 0);
                lv_obj_set_style_pad_left(proc_label, 4, 0);
                lv_obj_set_flex_grow(proc_label, 1);
                
                // Store both label pointer as user data for previous logic AND store PID on the label user data
                // Attach callback
                lv_obj_set_user_data(proc_label, (void*)(intptr_t)sys.procPids[i]);
                lv_obj_add_event_cb(kill_btn, kill_proc_event_cb, LV_EVENT_CLICKED, proc_label);
                
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
        int server_pos = med.position;
        if (server_pos < 0) server_pos = 0;
        if (server_pos > dur) server_pos = dur;
        
        // Update server position tracking for interpolation
        uint32_t now_ms = lv_tick_get();
        if (server_pos != musicUi.last_server_position || dur != musicUi.last_server_duration) {
            // Server sent new position - sync immediately
            musicUi.last_server_position = server_pos;
            musicUi.last_server_duration = dur;
            musicUi.last_update_ms = now_ms;
            musicUi.interpolated_position = server_pos;
        }
        
        // Use interpolated position for display (updated in ui_tick)
        int display_pos = musicUi.interpolated_position;
        if (display_pos > dur) display_pos = dur;

        if (musicUi.progress_bar) {
            lv_bar_set_range(musicUi.progress_bar, 0, dur);
            lv_bar_set_value(musicUi.progress_bar, display_pos, LV_ANIM_OFF);
        }

        if (musicUi.progress_label) {
            char pos_str[16];
            char dur_str[16];
            format_time(pos_str, sizeof(pos_str), display_pos);
            format_time(dur_str, sizeof(dur_str), dur);
            snprintf(buf, sizeof(buf), "%s / %s", pos_str, dur_str);
            lv_label_set_text(musicUi.progress_label, buf);
        }
        
        // Update play/pause button icon based on current state
        if (musicUi.play_pause_label && med.isPlaying != musicUi.is_playing) {
            musicUi.is_playing = med.isPlaying;
            lv_label_set_text(musicUi.play_pause_label, med.isPlaying ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
        
        // Update shuffle button state (highlight when active)
        if (musicUi.shuffle_btn) {
            bool shuffle_active = med.shuffle;
            if (shuffle_active != musicUi.shuffle_state) {
                musicUi.shuffle_state = shuffle_active;
                if (shuffle_active) {
                    // Active: bright green
                    lv_obj_set_style_bg_color(musicUi.shuffle_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
                } else {
                    // Inactive: default gray
                    lv_obj_set_style_bg_color(musicUi.shuffle_btn, lv_color_hex(0x404060), 0);
                }
            }
        }
        
        // Update repeat button state and icon (0=off, 1=track, 2=context)
        if (musicUi.repeat_btn && musicUi.repeat_label) {
            if (med.repeat != musicUi.repeat_state) {
                musicUi.repeat_state = med.repeat;
                
                if (musicUi.repeat_state == 1) {
                    // Repeat one track - orange highlight + "1" indicator
                    lv_obj_set_style_bg_color(musicUi.repeat_btn, lv_palette_main(LV_PALETTE_ORANGE), 0);
                    lv_label_set_text(musicUi.repeat_label, "1");
                } else if (musicUi.repeat_state == 2) {
                    // Repeat playlist/context - cyan highlight
                    lv_obj_set_style_bg_color(musicUi.repeat_btn, lv_palette_main(LV_PALETTE_CYAN), 0);
                    lv_label_set_text(musicUi.repeat_label, LV_SYMBOL_LOOP);
                } else {
                    // Off - default gray
                    lv_obj_set_style_bg_color(musicUi.repeat_btn, lv_color_hex(0x404060), 0);
                    lv_label_set_text(musicUi.repeat_label, LV_SYMBOL_LOOP);
                }
            }
        }
        
        // Reset add-to-playlist button color (in case it was highlighted)
        if (musicUi.add_playlist_btn) {
            lv_obj_set_style_bg_color(musicUi.add_playlist_btn, lv_color_hex(0x404060), 0);
        }
        
        // Show/hide artwork placeholder based on whether we have displayed artwork
        if (!gArtworkDisplayed && musicUi.art_icon) {
            // No artwork yet - show icon
            lv_obj_remove_flag(musicUi.art_icon, LV_OBJ_FLAG_HIDDEN);
            if (musicUi.art_img) lv_obj_add_flag(musicUi.art_img, LV_OBJ_FLAG_HIDDEN);
        }

        // --- Update playlist/queue title ---
        if (musicUi.playlist_label) {
            if (med.hasPlaylist && strlen(med.playlist.name) > 0) {
                lv_label_set_text(musicUi.playlist_label, med.playlist.name);
            } else {
                lv_label_set_text(musicUi.playlist_label, "Up Next");
            }
        }

        // --- Update queue list ---
        if (musicUi.queue_list) {
            // Check if queue changed
            bool queueChanged = (med.queueLen != gLastQueueLen);
            if (!queueChanged) {
                for (uint8_t i = 0; i < med.queueLen && i < MAX_QUEUE_ITEMS; ++i) {
                    if (strcmp(med.queue[i].name, gLastQueueItems[i]) != 0) {
                        queueChanged = true;
                        break;
                    }
                }
            }
            
            if (queueChanged) {
                lv_obj_clean(musicUi.queue_list);
                
                for (uint8_t i = 0; i < med.queueLen && i < MAX_QUEUE_ITEMS; ++i) {
                    if (strlen(med.queue[i].name) == 0) continue;
                    
                    // Create queue item - simpler layout to avoid overlap
                    // Layout: [Art 32px] [Text ~160px] [Play 32px] [Drag 28px]
                    // Total width: ~280px (list is 294px)
                    lv_obj_t *item_btn = lv_obj_create(musicUi.queue_list);
                    lv_obj_remove_style_all(item_btn);
                    lv_obj_set_size(item_btn, LV_PCT(100), 40);
                    lv_obj_set_style_bg_color(item_btn, lv_color_hex(0x202040), 0);
                    lv_obj_set_style_bg_opa(item_btn, LV_OPA_COVER, 0);
                    lv_obj_set_style_radius(item_btn, 6, 0);
                    lv_obj_set_style_pad_all(item_btn, 2, 0);
                    lv_obj_remove_flag(item_btn, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_add_flag(item_btn, LV_OBJ_FLAG_CLICKABLE);
                    // Store index on item for drag callback
                    lv_obj_set_user_data(item_btn, (void*)(intptr_t)i);
                    
                    // === Artwork placeholder (left side) ===
                    lv_obj_t *art_placeholder = lv_obj_create(item_btn);
                    lv_obj_remove_style_all(art_placeholder);
                    lv_obj_set_size(art_placeholder, 32, 32);
                    lv_obj_align(art_placeholder, LV_ALIGN_LEFT_MID, 2, 0);
                    lv_obj_set_style_bg_color(art_placeholder, lv_color_hex(0x303050), 0);
                    lv_obj_set_style_bg_opa(art_placeholder, LV_OPA_COVER, 0);
                    lv_obj_set_style_radius(art_placeholder, 4, 0);
                    lv_obj_remove_flag(art_placeholder, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_remove_flag(art_placeholder, LV_OBJ_FLAG_CLICKABLE);
                    
                    // Music icon in placeholder
                    lv_obj_t *art_icon = lv_label_create(art_placeholder);
                    lv_label_set_text(art_icon, LV_SYMBOL_AUDIO);
                    lv_obj_set_style_text_font(art_icon, &lv_font_montserrat_12, 0);
                    lv_obj_set_style_text_color(art_icon, lv_color_hex(0x606080), 0);
                    lv_obj_center(art_icon);
                    
                    // === Track info ===
                    // Track name - single line, scrolls if too long
                    lv_obj_t *name_label = lv_label_create(item_btn);
                    lv_obj_add_style(name_label, &style_label_primary, 0);
                    lv_label_set_text(name_label, med.queue[i].name);
                    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_10, 0);
                    lv_label_set_long_mode(name_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
                    lv_obj_set_width(name_label, 180);  // Wider now without up/down buttons
                    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 38, -8);
                    
                    // Artist name (single line, scrolls if too long)
                    lv_obj_t *artist_label = lv_label_create(item_btn);
                    lv_obj_add_style(artist_label, &style_label_secondary, 0);
                    lv_label_set_text(artist_label, med.queue[i].artist);
                    lv_obj_set_style_text_font(artist_label, &lv_font_montserrat_10, 0);
                    lv_label_set_long_mode(artist_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
                    lv_obj_set_width(artist_label, 180);  // Wider now without up/down buttons
                    lv_obj_align(artist_label, LV_ALIGN_LEFT_MID, 38, 8);
                    
                    // === Play button (Spotify only - plays this track immediately) ===
                    lv_obj_t *play_btn = lv_button_create(item_btn);
                    lv_obj_set_size(play_btn, 32, 32);
                    lv_obj_align(play_btn, LV_ALIGN_RIGHT_MID, -36, 0);
                    lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x1db954), 0);
                    lv_obj_set_style_radius(play_btn, 16, 0);
                    lv_obj_set_style_pad_all(play_btn, 0, 0);
                    lv_obj_t *play_icon = lv_label_create(play_btn);
                    lv_label_set_text(play_icon, LV_SYMBOL_PLAY);
                    lv_obj_set_style_text_font(play_icon, &lv_font_montserrat_12, 0);
                    lv_obj_center(play_icon);
                    lv_obj_set_user_data(play_btn, (void*)(intptr_t)i);
                    lv_obj_add_event_cb(play_btn, queue_item_click_cb, LV_EVENT_CLICKED, NULL);
                    
                    // === Remove button (X) ===
                    lv_obj_t *remove_btn = lv_button_create(item_btn);
                    lv_obj_set_size(remove_btn, 32, 32);
                    lv_obj_align(remove_btn, LV_ALIGN_RIGHT_MID, -2, 0);
                    lv_obj_set_style_bg_color(remove_btn, lv_color_hex(0x802020), 0);
                    lv_obj_set_style_radius(remove_btn, 4, 0);
                    lv_obj_set_style_pad_all(remove_btn, 0, 0);
                    lv_obj_t *remove_icon = lv_label_create(remove_btn);
                    lv_label_set_text(remove_icon, LV_SYMBOL_CLOSE);
                    lv_obj_set_style_text_font(remove_icon, &lv_font_montserrat_12, 0);
                    lv_obj_center(remove_icon);
                    lv_obj_set_user_data(remove_btn, (void*)(intptr_t)i);
                    // TODO: Add remove callback if needed
                    
                    strncpy(gLastQueueItems[i], med.queue[i].name, MAX_STR_ESP - 1);
                    gLastQueueItems[i][MAX_STR_ESP - 1] = '\0';
                }
                gLastQueueLen = med.queueLen;
            }
        }
    }
}

// Smoothly interpolate progress bar between server updates
static uint32_t gLastTickMs = 0;

void ui_tick() {
    uint32_t now_ms = lv_tick_get();
    
    // Only update every 100ms for smooth 10fps interpolation
    if (now_ms - gLastTickMs < 100) return;
    uint32_t elapsed_ms = now_ms - gLastTickMs;
    gLastTickMs = now_ms;
    
    // Only interpolate if playing
    if (musicUi.is_playing && musicUi.last_server_duration > 0) {
        // Add elapsed time to interpolated position (convert ms to seconds)
        int elapsed_sec = elapsed_ms / 1000;
        if (elapsed_ms % 1000 >= 500) elapsed_sec++;  // Round
        
        // Use fractional accumulator for sub-second precision
        static uint32_t ms_accumulator = 0;
        ms_accumulator += elapsed_ms;
        if (ms_accumulator >= 1000) {
            musicUi.interpolated_position += ms_accumulator / 1000;
            ms_accumulator %= 1000;
        }
        
        // Clamp to duration
        if (musicUi.interpolated_position > musicUi.last_server_duration) {
            musicUi.interpolated_position = musicUi.last_server_duration;
        }
        
        // Update progress bar display
        if (musicUi.progress_bar) {
            lv_bar_set_value(musicUi.progress_bar, musicUi.interpolated_position, LV_ANIM_OFF);
        }
        
        // Update time label
        if (musicUi.progress_label) {
            char pos_str[16], dur_str[16], buf[48];
            format_time(pos_str, sizeof(pos_str), musicUi.interpolated_position);
            format_time(dur_str, sizeof(dur_str), musicUi.last_server_duration);
            snprintf(buf, sizeof(buf), "%s / %s", pos_str, dur_str);
            lv_label_set_text(musicUi.progress_label, buf);
        }
    }
}
// External API: set the play state and update UI accordingly
void ui_set_play_state(bool is_playing) {
    musicUi.is_playing = is_playing;
    if (musicUi.play_pause_label) {
        lv_label_set_text(musicUi.play_pause_label, musicUi.is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    Serial.print("[UI] ACK play_state=");
    Serial.println(musicUi.is_playing ? "1" : "0");
}