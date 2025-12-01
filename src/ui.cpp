#include "ui.h"
#include <math.h>
#include "mbedtls/base64.h"

// --- Styles ---
static lv_style_t style_screen_bg;
static lv_style_t style_card;
static lv_style_t style_label_primary;
static lv_style_t style_label_secondary;

// --- Task UI holder ---
struct TaskUI {
    lv_obj_t *meter;
    lv_meter_indicator_t *cpu_arc;
    lv_meter_indicator_t *mem_arc;
    lv_obj_t *cpu_label;
    lv_obj_t *mem_label;
    lv_obj_t *proc_list;
};
static TaskUI taskUi;

// --- Music UI holder ---
struct MusicUI {
    lv_obj_t *art_img;
    lv_obj_t *title_label;
    lv_obj_t *artist_label;
    lv_obj_t *album_label;
    lv_obj_t *progress_bar;
    lv_obj_t *progress_label;
};
static MusicUI musicUi;

// Artwork buffer + descriptor
static uint8_t gArtworkPngBuf[32768];
static lv_img_dsc_t gArtworkImg;
static bool gArtworkImgInited = false;
static uint32_t gLastArtworkHash = 0;

// Simple hash to avoid decoding same image repeatedly
static uint32_t simpleHash(const String &s) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s.length(); ++i) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static void format_time(char *buf, size_t buf_len, int seconds) {
    if (seconds < 0) seconds = 0;
    int m = seconds / 60;
    int s = seconds % 60;
    snprintf(buf, buf_len, "%d:%02d", m, s);
}

// --- Styles: simple high-contrast dark theme ---
static void init_styles() {
    lv_style_init(&style_screen_bg);
    lv_style_set_bg_color(&style_screen_bg, lv_color_hex(0x000000));
    lv_style_set_bg_grad_color(&style_screen_bg, lv_color_hex(0x101010));
    lv_style_set_bg_grad_dir(&style_screen_bg, LV_GRAD_DIR_VER);

    lv_style_init(&style_card);
    lv_style_set_radius(&style_card, 8);
    lv_style_set_bg_color(&style_card, lv_color_hex(0x202020));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_pad_all(&style_card, 8);
    lv_style_set_shadow_width(&style_card, 8);
    lv_style_set_shadow_color(&style_card, lv_color_hex(0x000000));
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_border_color(&style_card, lv_color_hex(0x404040));

    lv_style_init(&style_label_primary);
    lv_style_set_text_color(&style_label_primary, lv_color_hex(0xFFFFFF));

    lv_style_init(&style_label_secondary);
    lv_style_set_text_color(&style_label_secondary, lv_color_hex(0xA0A0A0));
}

// --- MUSIC TAB ---
static void build_music_tab(lv_obj_t *parent) {
    lv_obj_add_style(parent, &style_screen_bg, 0);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Card: artwork + text
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_size(card, 320 - 20, 150);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *art = lv_img_create(card);
    lv_obj_set_size(art, 96, 96);
    lv_obj_set_style_radius(art, 8, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(0x404040), 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);

    lv_obj_t *text_col = lv_obj_create(card);
    lv_obj_remove_style_all(text_col);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_width(text_col, 320 - 20 - 96 - 30);

    lv_obj_t *title = lv_label_create(text_col);
    lv_obj_add_style(title, &style_label_primary, 0);
    lv_label_set_text(title, "No media");
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, lv_pct(100));

    lv_obj_t *artist = lv_label_create(text_col);
    lv_obj_add_style(artist, &style_label_secondary, 0);
    lv_label_set_text(artist, "Artist");

    lv_obj_t *album = lv_label_create(text_col);
    lv_obj_add_style(album, &style_label_secondary, 0);
    lv_label_set_text(album, "Album");

    // Progress card
    lv_obj_t *progress_card = lv_obj_create(parent);
    lv_obj_add_style(progress_card, &style_card, 0);
    lv_obj_set_size(progress_card, 320 - 20, 60);
    lv_obj_set_flex_flow(progress_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(progress_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *bar = lv_bar_create(progress_card);
    lv_obj_set_size(bar, 320 - 40, 12);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    lv_obj_t *time_label = lv_label_create(progress_card);
    lv_obj_add_style(time_label, &style_label_secondary, 0);
    lv_label_set_text(time_label, "0:00 / 0:00");

    musicUi.art_img = art;
    musicUi.title_label = title;
    musicUi.artist_label = artist;
    musicUi.album_label = album;
    musicUi.progress_bar = bar;
    musicUi.progress_label = time_label;
}

// --- TASK TAB ---
static void build_task_tab(lv_obj_t *parent) {
    lv_obj_add_style(parent, &style_screen_bg, 0);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *left_card = lv_obj_create(parent);
    lv_obj_add_style(left_card, &style_card, 0);
    lv_obj_set_size(left_card, 150, 240 - 60);
    lv_obj_align(left_card, LV_ALIGN_LEFT_MID, 5, 10);

    lv_obj_t *meter = lv_meter_create(left_card);
    lv_obj_set_size(meter, 120, 120);
    lv_obj_center(meter);

    lv_meter_scale_t *scale = lv_meter_add_scale(meter);
    lv_meter_set_scale_ticks(meter, scale, 11, 2, 10, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(meter, scale, 1, 2, 30, lv_color_hex3(0xeee), 15);
    lv_meter_set_scale_range(meter, scale, 0, 100, 270, 90);

    lv_meter_indicator_t *cpu_arc = lv_meter_add_arc(meter, scale, 8, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_meter_indicator_t *mem_arc = lv_meter_add_arc(meter, scale, 8, lv_palette_main(LV_PALETTE_ORANGE), -20);

    lv_obj_t *cpu_label = lv_label_create(left_card);
    lv_obj_add_style(cpu_label, &style_label_primary, 0);
    lv_label_set_text(cpu_label, "CPU: 0%");
    lv_obj_align(cpu_label, LV_ALIGN_BOTTOM_LEFT, 5, -30);

    lv_obj_t *mem_label = lv_label_create(left_card);
    lv_obj_add_style(mem_label, &style_label_primary, 0);
    lv_label_set_text(mem_label, "MEM: 0%");
    lv_obj_align(mem_label, LV_ALIGN_BOTTOM_LEFT, 5, -10);

    lv_obj_t *right_card = lv_obj_create(parent);
    lv_obj_add_style(right_card, &style_card, 0);
    lv_obj_set_size(right_card, 320 - 150 - 15, 240 - 60);
    lv_obj_align(right_card, LV_ALIGN_RIGHT_MID, -5, 10);

    lv_obj_t *list = lv_list_create(right_card);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));

    taskUi.meter = meter;
    taskUi.cpu_arc = cpu_arc;
    taskUi.mem_arc = mem_arc;
    taskUi.cpu_label = cpu_label;
    taskUi.mem_label = mem_label;
    taskUi.proc_list = list;
}

// --- DISCORD TAB ---
static void build_discord_tab(lv_obj_t *parent) {
    lv_obj_add_style(parent, &style_screen_bg, 0);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_size(card, 320 - 20, 240 - 80);
    lv_obj_center(card);

    lv_obj_t *label = lv_label_create(card);
    lv_obj_add_style(label, &style_label_primary, 0);
    lv_label_set_text(label, "Discord overlay\nComing soon");
    lv_obj_center(label);
}

// --- SETTINGS TAB ---
static void build_settings_tab(lv_obj_t *parent) {
    lv_obj_add_style(parent, &style_screen_bg, 0);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_size(card, 320 - 20, 240 - 80);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_add_style(title, &style_label_primary, 0);
    lv_label_set_text(title, "Settings");

    lv_obj_t *desc = lv_label_create(card);
    lv_obj_add_style(desc, &style_label_secondary, 0);
    lv_label_set_text(desc, "Future options: theme, brightness,\nupdate interval, etc.");
}

// --- Artwork decode + update ---
static void update_artwork(const MediaData &med) {
    if (!musicUi.art_img) return;
    if (!med.hasArtwork || med.artwork_b64.length() == 0) return;

    uint32_t h = simpleHash(med.artwork_b64);
    if (gArtworkImgInited && h == gLastArtworkHash) {
        return; // same image as last time
    }

    size_t out_len = 0;
    int ret = mbedtls_base64_decode(
        gArtworkPngBuf,
        sizeof(gArtworkPngBuf),
        &out_len,
        (const unsigned char *)med.artwork_b64.c_str(),
        med.artwork_b64.length()
    );

    if (ret != 0) {
        Serial.println("Base64 decode failed for artwork");
        return;
    }

    memset(&gArtworkImg, 0, sizeof(gArtworkImg));
    gArtworkImg.header.always_zero = 0;
    gArtworkImg.header.w = 0;
    gArtworkImg.header.h = 0;
    gArtworkImg.header.cf = LV_IMG_CF_RAW;   // PNG in memory
    gArtworkImg.data_size = out_len;
    gArtworkImg.data = gArtworkPngBuf;

    gArtworkImgInited = true;
    gLastArtworkHash = h;

    lv_img_set_src(musicUi.art_img, &gArtworkImg);
}

// --- Public API ---

void ui_init() {
    // register PNG decoder (once)
    lv_png_init();
    init_styles();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_add_style(scr, &style_screen_bg, 0);

    lv_obj_t *tabview = lv_tabview_create(scr, LV_DIR_TOP, 40);
    lv_obj_set_size(tabview, 320, 240);
    lv_obj_align(tabview, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *tab_music   = lv_tabview_add_tab(tabview, "Music");
    lv_obj_t *tab_discord = lv_tabview_add_tab(tabview, "Discord");
    lv_obj_t *tab_tasks   = lv_tabview_add_tab(tabview, "Tasks");
    lv_obj_t *tab_settings= lv_tabview_add_tab(tabview, "Settings");

    build_music_tab(tab_music);
    build_discord_tab(tab_discord);
    build_task_tab(tab_tasks);
    build_settings_tab(tab_settings);
}

void ui_update(const SystemData &sys, const MediaData &med) {
    char buf[64];

    // --- Tasks ---
    int cpu_i = (int)round(sys.cpu);
    int mem_i = (int)round(sys.mem);

    if (taskUi.cpu_label) {
        snprintf(buf, sizeof(buf), "CPU: %d%%", cpu_i);
        lv_label_set_text(taskUi.cpu_label, buf);
    }
    if (taskUi.mem_label) {
        snprintf(buf, sizeof(buf), "MEM: %d%%", mem_i);
        lv_label_set_text(taskUi.mem_label, buf);
    }
    if (taskUi.meter && taskUi.cpu_arc)
        lv_meter_set_indicator_end_value(taskUi.meter, taskUi.cpu_arc, cpu_i);
    if (taskUi.meter && taskUi.mem_arc)
        lv_meter_set_indicator_end_value(taskUi.meter, taskUi.mem_arc, mem_i);

    if (taskUi.proc_list) {
        lv_obj_clean(taskUi.proc_list);
        for (uint8_t i = 0; i < sys.procCount; ++i) {
            lv_list_add_text(taskUi.proc_list, sys.procs[i].c_str());
        }
    }

    // --- Music ---
    if (med.valid) {
        if (musicUi.title_label)
            lv_label_set_text(musicUi.title_label, med.title.c_str());
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

        if (med.hasArtwork) {
            update_artwork(med);
        }
    }
}
