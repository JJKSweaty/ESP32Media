#include "data_model.h"
#include <ArduinoJson.h>

QueueHandle_t gSnapshotQueue = nullptr;
static String gSerialLineBuf;

// Artwork buffer and state (static storage)
static uint8_t artwork_rgb565[80 * 80 * 2];  // 12800 bytes
static size_t artworkSize = 0;
static bool artworkReady = false;
static size_t artworkWritePos = 0;  // Position for chunk assembly

// Helper: convert hex char to nibble
static uint8_t hexCharToNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return 0;
}

// Artwork chunk handling functions
void artwork_start() {
    artworkWritePos = 0;
    artworkReady = false;
    memset(artwork_rgb565, 0, sizeof(artwork_rgb565));
    Serial.println("[ART] Start receiving artwork");
}

void artwork_chunk(const char* hexData, size_t len) {
    // Convert hex string to bytes and append to buffer
    // Each 2 hex chars = 1 byte
    for (size_t i = 0; i + 1 < len && artworkWritePos < sizeof(artwork_rgb565); i += 2) {
        uint8_t hi = hexCharToNibble(hexData[i]);
        uint8_t lo = hexCharToNibble(hexData[i + 1]);
        artwork_rgb565[artworkWritePos++] = (hi << 4) | lo;
    }
}

void artwork_end() {
    artworkSize = artworkWritePos;
    artworkReady = true;
    Serial.printf("[ART] Complete: %u bytes\n", artworkSize);
}

uint8_t* artwork_get_buffer() {
    return artwork_rgb565;
}

bool artwork_is_ready() {
    return artworkReady;
}

size_t artwork_get_size() {
    return artworkSize;
}

void artwork_clear_ready() {
    artworkReady = false;
}

// Helper to safely copy Strings into fixed buffers
static void safeStrCopy(char *dst, size_t dstSize, const String &src) {
    if (dstSize == 0) return;
    size_t n = src.length();
    if (n >= dstSize) n = dstSize - 1;
    memcpy(dst, src.c_str(), n);
    dst[n] = '\0';
}

// Parse JSON line into SnapshotMsg (POD struct)
static bool parse_json_into_msg(const String &input, SnapshotMsg &msg) {
    StaticJsonDocument<2048> doc;
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
                // Format: "1234: 45.2% name"
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
    msg.title[0]  = '\0';
    msg.artist[0] = '\0';
    msg.album[0]  = '\0';
    msg.position  = 0;
    msg.duration  = 0;

    if (doc.containsKey("media") && doc["media"].is<JsonObject>()) {
        JsonObject media = doc["media"].as<JsonObject>();
        String title  = String(media["title"]   | "No media");
        String artist = String(media["artist"]  | "");
        String album  = String(media["album"]   | "");
        int pos       = media["position_seconds"]  | 0;
        int dur       = media["duration_seconds"]  | 0;

        safeStrCopy(msg.title,  sizeof(msg.title),  title);
        safeStrCopy(msg.artist, sizeof(msg.artist), artist);
        safeStrCopy(msg.album,  sizeof(msg.album),  album);
        msg.position = pos;
        msg.duration = dur;
        msg.hasMedia = true;
        // Artwork detection (just check if present, don't store base64 - too large)
        msg.hasArtwork = media.containsKey("artwork_b64");
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

                // Handle artwork protocol
                if (line.startsWith("ART_START")) {
                    artwork_start();
                } else if (line.startsWith("ART_CHUNK:")) {
                    // Extract hex data after "ART_CHUNK:"
                    const char* hexData = line.c_str() + 10;  // Skip "ART_CHUNK:"
                    size_t hexLen = line.length() - 10;
                    artwork_chunk(hexData, hexLen);
                } else if (line.startsWith("ART_END")) {
                    artwork_end();
                } else if (line.length() > 5) {
                    // Regular JSON data
                    SnapshotMsg msg;
                    if (parse_json_into_msg(line, msg)) {
                        if (gSnapshotQueue) {
                            xQueueOverwrite(gSnapshotQueue, &msg);
                        }
                    }
                }
            } else if (c != '\r') {
                gSerialLineBuf += c;
                if (gSerialLineBuf.length() > 4095) {
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
        4096,       // Reduced stack - struct is smaller now
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
