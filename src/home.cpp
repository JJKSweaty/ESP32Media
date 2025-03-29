#include "home.h"
#include "lvgl.h"

HomePage::HomePage():headerLabel(nullptr),spotifyLabel(nullptr),youtubeLabel(nullptr){

}

void HomePage::create(){
    lv_obj_t * home_screen=lv_obj_create(NULL);
    // Clearing the scroll flag
    lv_obj_clear_flag(home_screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(home_screen);
    
    // Header
    headerLabel=lv_label_create(home_screen);
    lv_label_set_text(headerLabel,"Media Dashboard");
    lv_obj_align(headerLabel,LV_ALIGN_OUT_TOP_MID,10,5);

    // Container
    lv_obj_t * cont =lv_obj_create(home_screen);
    lv_obj_set_size(cont, lv_pct(80), lv_pct(75));
    lv_obj_center(cont);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(cont, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(cont, 20, 0);
    
    //Spotify
    lv_obj_t * spotifyCont = lv_obj_create(cont);
    lv_obj_set_size(spotifyCont, lv_pct(100), lv_pct(50));
    lv_obj_set_style_bg_color(spotifyCont, lv_color_hex(0x1DB954), 0); // Spotify green
    lv_obj_set_style_pad_all(spotifyCont, 10, 0);
    spotifyLabel = lv_label_create(spotifyCont);
    lv_label_set_text_fmt(spotifyLabel, "Spotify:\nTrack: %s\nArtist: %s", "Song Title", "Artist Name");
    lv_obj_center(spotifyLabel);

    // Create the YouTube panel
    lv_obj_t * youtubeCont = lv_obj_create(cont);
    lv_obj_set_size(youtubeCont, lv_pct(100), lv_pct(50));
    lv_obj_set_style_bg_color(youtubeCont, lv_color_hex(0xFF0000), 0); // YouTube red
    lv_obj_set_style_pad_all(youtubeCont, 10, 0);
    youtubeLabel = lv_label_create(youtubeCont);
    lv_label_set_text_fmt(youtubeLabel, "YouTube:\nVideo: %s\nChannel: %s", "Video Title", "Channel Name");
    lv_obj_center(youtubeLabel);



}

