#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

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

    String artwork_b64;      // base64 PNG from Python (optional)
    bool hasArtwork = false; // true if artwork_b64 is valid

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
} SnapshotMsg;

extern QueueHandle_t gSnapshotQueue;

void data_model_init();
void start_serial_task();
bool data_model_try_dequeue(SnapshotMsg &msg);
