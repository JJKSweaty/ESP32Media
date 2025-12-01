#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// RGB565 artwork buffer (80x80 = 12800 bytes)
#define ARTWORK_WIDTH 80
#define ARTWORK_HEIGHT 80
#define ARTWORK_BUF_SIZE (ARTWORK_WIDTH * ARTWORK_HEIGHT * 2)

struct SystemData {
    float cpu = 0.0f;
    float mem = 0.0f;
    float gpu = 0.0f;    // GPU usage percentage
    String procs[5];
    uint8_t procCount = 0;
    bool valid = false;
};

struct MediaData {
    String title;
    String artist;
    String album;
    int position = 0;    // seconds
    int duration = 0;    // seconds

    bool hasArtwork = false; // true if artwork is available

    bool valid = false;
};

typedef struct {
    float cpu;
    float mem;
    float gpu;           // GPU usage percentage
    uint8_t procCount;
    char procs[5][32];

    bool hasMedia;
    char title[64];
    char artist[64];
    char album[64];
    int position;
    int duration;
    bool hasArtwork;
} SnapshotMsg;

extern QueueHandle_t gSnapshotQueue;

void data_model_init();
void start_serial_task();
bool data_model_try_dequeue(SnapshotMsg &msg);

// Artwork handling functions
void artwork_start();
void artwork_chunk(const char* hexData, size_t len);
void artwork_end();
uint8_t* artwork_get_buffer();
bool artwork_is_ready();
size_t artwork_get_size();
void artwork_clear_ready();
