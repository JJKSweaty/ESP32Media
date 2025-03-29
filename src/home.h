#ifndef HOME_H
#define HOME_H

#include <lvgl.h>
#include <string>
using namespace std;

class HomePage {
public:
    // Constructor
    HomePage();

    
    void create();

    
    void update(const char* spotifyTrack, const char* spotifyArtist,
                const char* youtubeVideo, const char* youtubeChannel);

private:
    // Pointers to LVGL objects that you might need to update later
    lv_obj_t * headerLabel;
    lv_obj_t * spotifyLabel;
    lv_obj_t * youtubeLabel;
    string ytname;
    string ytAlbum;
    string ytImg;



    // Internal helper function to style a container or label
    void setupStyle(lv_obj_t * obj);
};

#endif // HOME_H
