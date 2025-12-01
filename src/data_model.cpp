#include "data_model.h"
#include <ArduinoJson.h>

QueueHandle_t gSnapshotQueue = nullptr;
static String gSerialLineBuf;

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
    StaticJsonDocument<4096> doc;
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

    msg.procCount = 0;
    for (int i = 0; i < 5; ++i) {
        msg.procs[i][0] = '\0';
    }

    if (doc.containsKey("cpu_top5_process")) {
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
                    // Serial.print("[SerialTask] RX: ");
                    // Serial.println(line);

                    SnapshotMsg msg;
                    if (parse_json_into_msg(line, msg)) {
                        if (gSnapshotQueue) {
                            // Keep only the latest snapshot in the queue
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
    // Queue size 1–3 is enough; we usually only care about the latest snapshot
    gSnapshotQueue = xQueueCreate(3, sizeof(SnapshotMsg));
    if (!gSnapshotQueue) {
        Serial.println("[data_model] Failed to create snapshot queue!");
    }
}

void start_serial_task() {
    xTaskCreatePinnedToCore(
        serial_task,
        "SerialTask",
        8192,
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
