#include "data_model.h"
#include <ArduinoJson.h>
#include "mbedtls/base64.h"

QueueHandle_t gSnapshotQueue = nullptr;
static String gSerialLineBuf;

// Global artwork buffer (decoded RGB565) - NOT in the queue
static uint8_t gArtworkRgb565[ARTWORK_RGB565_SIZE];
static bool gArtworkNew = false;
static uint32_t gLastArtworkHash = 0;

// Simple hash for change detection
static uint32_t quickHash(const char* str, size_t len) {
    uint32_t h = 2166136261u;
    size_t maxLen = len > 100 ? 100 : len;
    for (size_t i = 0; i < maxLen; ++i) {
        h ^= (uint8_t)str[i];
        h *= 16777619u;
    }
    return h ^ len;
}

uint8_t* artwork_get_rgb565_buffer() {
    return gArtworkRgb565;
}

bool artwork_is_new() {
    return gArtworkNew;
}

void artwork_clear_new() {
    gArtworkNew = false;
}

// Helper to safely copy Strings into fixed buffers
static void safeStrCopy(char *dst, size_t dstSize, const String &src) {
    if (dstSize == 0) return;
    size_t n = src.length();
    if (n >= dstSize) n = dstSize - 1;
    memcpy(dst, src.c_str(), n);
    dst[n] = '\0';
}

// Decode base64 artwork directly into global buffer
static bool decodeArtworkB64(const char* b64, size_t b64Len) {
    // Check if it's the same artwork
    uint32_t h = quickHash(b64, b64Len);
    if (h == gLastArtworkHash) {
        return false;  // Same artwork, no update needed
    }
    
    size_t outLen = 0;
    int ret = mbedtls_base64_decode(
        gArtworkRgb565,
        sizeof(gArtworkRgb565),
        &outLen,
        (const unsigned char*)b64,
        b64Len
    );
    
    if (ret != 0 || outLen != ARTWORK_RGB565_SIZE) {
        Serial.printf("[ART] Decode failed: ret=%d, size=%u (expected %u)\n", ret, outLen, ARTWORK_RGB565_SIZE);
        return false;
    }
    
    gLastArtworkHash = h;
    gArtworkNew = true;
    Serial.println("[ART] Decoded successfully");
    return true;
}

// Parse JSON line into SnapshotMsg (POD struct) - artwork handled separately
static bool parse_json_into_msg(const String &input, SnapshotMsg &msg) {
    // Check if this is a standalone artwork message
    if (input.indexOf("artwork_b64") > 0 && input.indexOf("cpu_percent") < 0) {
        Serial.println("[ESP] Received artwork message!");
        // This is just artwork - parse and decode it, don't update msg
        DynamicJsonDocument doc(32768);
        DeserializationError err = deserializeJson(doc, input);
        if (err) {
            Serial.print("Artwork JSON parse error: ");
            Serial.println(err.f_str());
            return false;
        }
        if (doc.containsKey("artwork_b64")) {
            const char* b64 = doc["artwork_b64"] | "";
            size_t b64len = strlen(b64);
            Serial.printf("[ESP] artwork_b64 length: %u\n", b64len);
            if (b64len > 100) {
                if (decodeArtworkB64(b64, b64len)) {
                    Serial.println("[ESP] Artwork decoded successfully!");
                }
            }
        }
        return false;  // Don't queue this as a snapshot
    }
    
    // Regular system snapshot - use smaller buffer
    DynamicJsonDocument doc(2048);
    
    DeserializationError err = deserializeJson(doc, input);
    if (err) {
        Serial.print("JSON parse error: ");
        Serial.println(err.f_str());
        return false;
    }

    // --- System ---
    float cpu = 0.0f;
    if (doc.containsKey("cpu_percent_total"))
        cpu = doc["cpu_percent_total"].as<float>();
    else if (doc.containsKey("cpu_percent"))
        cpu = doc["cpu_percent"].as<float>();
    msg.cpu = cpu;
    msg.mem = doc.containsKey("mem_percent") ? doc["mem_percent"].as<float>() : 0.0f;
    msg.gpu = doc.containsKey("gpu_percent") ? doc["gpu_percent"].as<float>() : 0.0f;

    msg.procCount = 0;
    for (int i = 0; i < 5; ++i) {
        msg.procs[i][0] = '\0';
    }

    // Parse proc_top5 (rich objects) or fallback to cpu_top5_process (legacy string list)
    if (doc.containsKey("proc_top5") && doc["proc_top5"].is<JsonArray>()) {
        JsonArray arr = doc["proc_top5"].as<JsonArray>();
        uint8_t idx = 0;
        for (JsonVariant v : arr) {
            if (idx >= 5) break;
            if (v.is<JsonObject>()) {
                JsonObject o = v.as<JsonObject>();
                int pid = o["pid"] | 0;
                float mem = o["mem"] | 0.0f;
                const char *name = o["name"] | "";
                char buf[32];
                snprintf(buf, sizeof(buf), "%d: %.1f%% %s", pid, mem, name);
                safeStrCopy(msg.procs[idx], sizeof(msg.procs[idx]), String(buf));
                idx++;
            }
        }
        msg.procCount = idx;
    } else if (doc.containsKey("cpu_top5_process")) {
        JsonVariant procs = doc["cpu_top5_process"];
        if (procs.is<JsonArray>()) {
            JsonArray arr = procs.as<JsonArray>();
            uint8_t idx = 0;
            for (JsonVariant v : arr) {
                if (idx >= 5) break;
                String line;
                if (v.is<const char*>()) {
                    line = String(v.as<const char*>());
                } else {
                    serializeJson(v, line);
                }
                safeStrCopy(msg.procs[idx], sizeof(msg.procs[idx]), line);
                idx++;
            }
            msg.procCount = idx;
        }
    }

    // --- Media (optional "media" object from Python) ---
    msg.hasMedia = false;
    msg.hasArtwork = false;
    msg.artworkUpdated = false;
    msg.title[0]  = '\0';
    msg.artist[0] = '\0';
    msg.album[0]  = '\0';
    msg.position  = 0;
    msg.duration  = 0;
    msg.isPlaying = false;

    if (doc.containsKey("media") && doc["media"].is<JsonObject>()) {
        JsonObject media = doc["media"].as<JsonObject>();
        String title  = String(media["title"]   | "No media");
        String artist = String(media["artist"]  | "");
        String album  = String(media["album"]   | "");
        int pos       = media["position_seconds"]  | 0;
        int dur       = media["duration_seconds"]  | 0;
        bool playing  = media["is_playing"] | false;

        safeStrCopy(msg.title,  sizeof(msg.title),  title);
        safeStrCopy(msg.artist, sizeof(msg.artist), artist);
        safeStrCopy(msg.album,  sizeof(msg.album),  album);
        msg.position = pos;
        msg.duration = dur;
        msg.isPlaying = playing;
        msg.hasMedia = true;
        
        // Decode artwork directly into global buffer (not queued)
        if (media.containsKey("artwork_png_b64")) {
            const char* b64 = media["artwork_png_b64"] | "";
            size_t b64len = strlen(b64);
            if (b64len > 0) {
                msg.hasArtwork = true;
                if (decodeArtworkB64(b64, b64len)) {
                    msg.artworkUpdated = true;
                }
            }
        }
    }

    return true;
}

// RTOS task: producer – reads Serial, parses JSON, sends SnapshotMsg to queue
static void serial_task(void *pvParameters) {
    (void) pvParameters;
    Serial.println("[SerialTask] Started.");

    for (;;) {
        while (Serial.available() > 0) {
            char c = (char)Serial.read();

            if (c == '\n') {
                String line = gSerialLineBuf;
                gSerialLineBuf = "";
                line.trim();

                if (line.length() > 5) {
                    SnapshotMsg msg;
                    if (parse_json_into_msg(line, msg)) {
                        if (gSnapshotQueue) {
                            xQueueOverwrite(gSnapshotQueue, &msg);
                        }
                    }
                }
            } else if (c != '\r') {
                gSerialLineBuf += c;
                // Allow larger buffer for artwork JSON
                if (gSerialLineBuf.length() > 65535) {
                    gSerialLineBuf = "";
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void data_model_init() {
    // xQueueOverwrite requires queue size of 1
    gSnapshotQueue = xQueueCreate(1, sizeof(SnapshotMsg));
    if (!gSnapshotQueue) {
        Serial.println("[data_model] Failed to create snapshot queue!");
    }
}

void start_serial_task() {
    xTaskCreatePinnedToCore(
        serial_task,
        "SerialTask",
        12288,      // 12KB stack - enough for JSON parsing
        nullptr,
        1,          // priority
        nullptr,
        0           // core 0
    );
}

bool data_model_try_dequeue(SnapshotMsg &msg) {
    if (!gSnapshotQueue) return false;
    BaseType_t ok = xQueueReceive(gSnapshotQueue, &msg, 0);  // no block
    return (ok == pdTRUE);
}
