#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Artwork dimensions
#define ARTWORK_WIDTH 80
#define ARTWORK_HEIGHT 80
#define ARTWORK_RGB565_SIZE (ARTWORK_WIDTH * ARTWORK_HEIGHT * 2)  // 12800 bytes

// Queue/Playlist limits (memory-constrained)
#define MAX_QUEUE_ITEMS 5
#define MAX_STR_ESP 48

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

    bool hasArtwork = false;
    bool artworkUpdated = false;  // Set true when new artwork is ready

    // Queue data
    bool hasQueue = false;
    uint8_t queueLen = 0;
    QueueItem queue[MAX_QUEUE_ITEMS];
    
    // Playlist context
    bool hasPlaylist = false;
    PlaylistInfo playlist;

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
    int position;
    int duration;
    bool isPlaying;
    
    bool hasArtwork;       // Indicates artwork was parsed
    bool artworkUpdated;   // Indicates new artwork in global buffer
    
    // Queue data
    bool hasQueue;
    uint8_t queueLen;
    QueueItem queue[MAX_QUEUE_ITEMS];
    
    // Playlist context
    bool hasPlaylist;
    PlaylistInfo playlist;
} SnapshotMsg;

extern QueueHandle_t gSnapshotQueue;

void data_model_init();
void start_serial_task();
bool data_model_try_dequeue(SnapshotMsg &msg);

// Artwork buffer access (global static buffer, not in queue)
uint8_t* artwork_get_rgb565_buffer();
bool artwork_is_new();
void artwork_clear_new();
