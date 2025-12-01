#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// === Data the UI actually uses ===
struct SystemData {
    float cpu = 0.0f;
    float mem = 0.0f;
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

    // For album art support (even if unused for now)
    String artwork_b64;      // base64-encoded PNG from Python
    bool hasArtwork = false; // true if artwork_b64 has valid data

    bool valid = false;
};

// === Queue payload: fixed-size POD struct (safe to copy in RTOS queue) ===
typedef struct {
    float cpu;
    float mem;
    uint8_t procCount;
    char procs[5][32];    // "12.3% chrome.exe" etc.

    bool hasMedia;
    char title[64];
    char artist[64];
    char album[64];
    int position;
    int duration;
} SnapshotMsg;

// Queue handle (created in data_model.cpp)
extern QueueHandle_t gSnapshotQueue;

// Init + start RTOS task
void data_model_init();
void start_serial_task();

// Consumer: try get one snapshot from the queue (non-blocking)
bool data_model_try_dequeue(SnapshotMsg &msg);
