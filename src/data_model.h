#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ============= TCP Server Configuration =============
// The TCP server IP/port should match your Python server
#define TCP_SERVER_IP "192.168.1.168"   
#define TCP_SERVER_PORT 5555

// WiFi credentials are now managed by WiFiManager (stored in NVS)
// See wifi_manager.h for runtime network selection

// Artwork dimensions
#define ARTWORK_WIDTH 80
#define ARTWORK_HEIGHT 80
#define ARTWORK_RGB565_SIZE (ARTWORK_WIDTH * ARTWORK_HEIGHT * 2)  // 12800 bytes

// Queue/Playlist limits (memory-constrained)
#define MAX_QUEUE_ITEMS 5
#define MAX_STR_ESP 48

// Discord voice call limits
#define MAX_DISCORD_USERS 5
#define DISCORD_NAME_LEN 16
#define DISCORD_CHANNEL_LEN 20

// Discord user in voice channel
struct DiscordUser {
    char name[DISCORD_NAME_LEN];
    bool muted;
    bool deafened;
    bool speaking;  // Simulated - true if not muted
};

// Discord voice call state
struct DiscordState {
    bool inCall;
    char channelName[DISCORD_CHANNEL_LEN];
    bool selfMuted;
    bool selfDeafened;
    uint8_t userCount;
    DiscordUser users[MAX_DISCORD_USERS];
};

struct SystemData {
    float cpu = 0.0f;
    float mem = 0.0f;
    float gpu = 0.0f;    // GPU usage percentage
    String procs[5];
    int procPids[5];
    uint8_t procCount = 0;
    bool valid = false;
};

// Queue item for upcoming tracks
struct QueueItem {
    char id[64];           // Spotify URI
    char source[16];       // "spotify" or "local"
    char name[MAX_STR_ESP];
    char artist[MAX_STR_ESP];
    char album[MAX_STR_ESP];
    uint16_t duration;     // seconds
    bool isLocal;
};

// Playlist context info
struct PlaylistInfo {
    char id[64];
    char name[MAX_STR_ESP];
    char snapshotId[48];
    uint16_t totalTracks;
    bool isPublic;
    bool isCollaborative;
    bool hasImage;
};

struct MediaData {
    String title;
    String artist;
    String album;
    int position = 0;    // seconds
    int duration = 0;    // seconds
    bool isPlaying = false;
    String source;       // "spotify", "youtube", "browser"
    String trackUri;     // Spotify track URI for like/unlike

    bool hasArtwork = false;
    bool artworkUpdated = false;  // Set true when new artwork is ready
    
    // Spotify playback state
    bool shuffle = false;
    uint8_t repeat = 0;  // 0=off, 1=track (repeat one), 2=context (repeat all)
    bool isLiked = false;

    // Queue data
    bool hasQueue = false;
    uint8_t queueLen = 0;
    QueueItem queue[MAX_QUEUE_ITEMS];
    
    // Playlist context
    bool hasPlaylist = false;
    PlaylistInfo playlist;

    // Discord voice call state (for UI convenience)
    bool hasDiscord = false;
    DiscordState discord;

    bool valid = false;
};

// Keep SnapshotMsg small - no artwork in the queue!
typedef struct {
    float cpu;
    float mem;
    float gpu;           // GPU usage percentage
    uint8_t procCount;
    char procs[5][32];
    int procPids[5];

    bool hasMedia;
    char title[64];
    char artist[64];
    char album[64];
    char source[16];     // Media source
    char trackUri[80];   // Spotify track URI for like/unlike
    int position;
    int duration;
    bool isPlaying;
    
    // Spotify playback state
    bool shuffle;
    uint8_t repeat;      // 0=off, 1=track, 2=context
    bool isLiked;
    
    bool hasArtwork;       // Indicates artwork was parsed
    bool artworkUpdated;   // Indicates new artwork in global buffer
    
    // Queue data
    bool hasQueue;
    uint8_t queueLen;
    QueueItem queue[MAX_QUEUE_ITEMS];
    
    // Playlist context
    bool hasPlaylist;
    PlaylistInfo playlist;
    
    // Discord voice call state
    bool hasDiscord;
    DiscordState discord;
} SnapshotMsg;

extern QueueHandle_t gSnapshotQueue;

void data_model_init();
void start_serial_task();
void start_wifi_task(const char* host, uint16_t port);  // WiFi managed by WiFiManager
bool data_model_try_dequeue(SnapshotMsg &msg);

// Send a command to the Python server (non-blocking, uses WiFi if available)
void send_command(const char* cmd);

// Artwork buffer access (global static buffer, not in queue)
uint8_t* artwork_get_rgb565_buffer();
bool artwork_is_new();
void artwork_clear_new();
