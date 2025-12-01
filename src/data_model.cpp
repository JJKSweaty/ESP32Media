#include "data_model.h"
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include "ui.h"
#include <WiFi.h>
#include <WiFiClient.h>

QueueHandle_t gSnapshotQueue = nullptr;
static String gSerialLineBuf;

// Command queue for sending to Python server (non-blocking)
static QueueHandle_t gCommandQueue = nullptr;
#define CMD_QUEUE_SIZE 8
#define CMD_MAX_LEN 128

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
        return false;
    }
    
    gLastArtworkHash = h;
    gArtworkNew = true;
    return true;
}

// Parse JSON line into SnapshotMsg (POD struct) - artwork handled separately
static bool parse_json_into_msg(const String &input, SnapshotMsg &msg) {
    // Check if this is a standalone artwork message
    if (input.indexOf("artwork_b64") > 0 && input.indexOf("cpu_percent") < 0) {
        // This is just artwork - parse and decode it, don't update msg
        DynamicJsonDocument doc(32768);
        DeserializationError err = deserializeJson(doc, input);
        if (err) {
            return false;
        }
        if (doc.containsKey("artwork_b64")) {
            const char* b64 = doc["artwork_b64"] | "";
            size_t b64len = strlen(b64);
            if (b64len > 100) {
                decodeArtworkB64(b64, b64len);
            }
        }
        return false;  // Don't queue this as a snapshot
    }

    // If the message is a one-off 'ack' command (acknowledgement), apply quick UI updates
    // Parse minimal JSON for ack
    if (input.indexOf("\"ack\"") > 0) {
        DynamicJsonDocument ackDoc(256);
        DeserializationError ackErr = deserializeJson(ackDoc, input);
        if (!ackErr && ackDoc.containsKey("ack")) {
            const char* ackVal = ackDoc["ack"] | "";
            if (strcmp(ackVal, "play") == 0) {
                ui_set_play_state(true);
            } else if (strcmp(ackVal, "pause") == 0) {
                ui_set_play_state(false);
            }
        }
        return false; // Not a full snapshot
    }
    
    // Regular system snapshot - need larger buffer for queue data
    DynamicJsonDocument doc(6144);
    
    DeserializationError err = deserializeJson(doc, input);
    if (err) {
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
        msg.procPids[i] = 0;
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
                const char *display = o["display_name"] | name;
                char buf[32];
                // Display string should hide PID and remove ".exe" suffix
                // The Python backend provides `display_name`, but fallback to `name`.
                snprintf(buf, sizeof(buf), "%.1f%% %s", mem, display);
                safeStrCopy(msg.procs[idx], sizeof(msg.procs[idx]), String(buf));
                msg.procPids[idx] = pid;
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
                // Try to strip PID/EXE in fallback string
                // If line contains a percent and text, we assume it's in format "<mem>% <name>"
                // Keep as-is but remove .exe if present
                String clean = line;
                // Remove .exe suffixs in fallback
                int posExe = clean.indexOf(".exe");
                if (posExe >= 0) {
                    // remove .exe
                    clean.remove(posExe, 4);
                }
                safeStrCopy(msg.procs[idx], sizeof(msg.procs[idx]), clean);
                msg.procPids[idx] = 0; // unknown PID in fallback
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
    msg.source[0] = '\0';
    msg.position  = 0;
    msg.duration  = 0;
    msg.isPlaying = false;
    
    // Initialize queue/playlist
    msg.hasQueue = false;
    msg.queueLen = 0;
    msg.hasPlaylist = false;
    memset(&msg.playlist, 0, sizeof(msg.playlist));
    for (int i = 0; i < MAX_QUEUE_ITEMS; ++i) {
        memset(&msg.queue[i], 0, sizeof(QueueItem));
    }

    if (doc.containsKey("media") && doc["media"].is<JsonObject>()) {
        JsonObject media = doc["media"].as<JsonObject>();
        String title  = String(media["title"]   | "No media");
        String artist = String(media["artist"]  | "");
        String album  = String(media["album"]   | "");
        String source = String(media["source"]  | "");
        int pos       = media["position_seconds"]  | 0;
        int dur       = media["duration_seconds"]  | 0;
        bool playing  = media["is_playing"] | false;

        safeStrCopy(msg.title,  sizeof(msg.title),  title);
        safeStrCopy(msg.artist, sizeof(msg.artist), artist);
        safeStrCopy(msg.album,  sizeof(msg.album),  album);
        safeStrCopy(msg.source, sizeof(msg.source), source);
        msg.position = pos;
        msg.duration = dur;
        msg.isPlaying = playing;
        msg.hasMedia = true;
        
        // Parse playlist context if present
        if (media.containsKey("playlist") && media["playlist"].is<JsonObject>()) {
            JsonObject pl = media["playlist"].as<JsonObject>();
            msg.hasPlaylist = true;
            safeStrCopy(msg.playlist.id, sizeof(msg.playlist.id), String(pl["id"] | ""));
            safeStrCopy(msg.playlist.name, sizeof(msg.playlist.name), String(pl["name"] | ""));
            safeStrCopy(msg.playlist.snapshotId, sizeof(msg.playlist.snapshotId), String(pl["snapshot_id"] | ""));
            msg.playlist.totalTracks = pl["total_tracks"] | 0;
            msg.playlist.isPublic = pl["is_public"] | false;
            msg.playlist.isCollaborative = pl["is_collaborative"] | false;
            msg.playlist.hasImage = pl.containsKey("image_thumb_jpg_b64") && strlen(pl["image_thumb_jpg_b64"] | "") > 0;
        }
        
        // Parse queue if present
        if (media.containsKey("queue") && media["queue"].is<JsonArray>()) {
            JsonArray qArr = media["queue"].as<JsonArray>();
            msg.hasQueue = true;
            uint8_t idx = 0;
            for (JsonVariant v : qArr) {
                if (idx >= MAX_QUEUE_ITEMS) break;
                if (!v.is<JsonObject>()) continue;
                JsonObject q = v.as<JsonObject>();
                
                QueueItem &item = msg.queue[idx];
                safeStrCopy(item.id, sizeof(item.id), String(q["id"] | ""));
                safeStrCopy(item.source, sizeof(item.source), String(q["source"] | "spotify"));
                safeStrCopy(item.name, sizeof(item.name), String(q["name"] | ""));
                safeStrCopy(item.artist, sizeof(item.artist), String(q["artist"] | ""));
                safeStrCopy(item.album, sizeof(item.album), String(q["album"] | ""));
                item.duration = q["duration_seconds"] | 0;
                item.isLocal = q["is_local"] | false;
                idx++;
            }
            msg.queueLen = idx;
        }
        
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
                if (gSerialLineBuf.length() > 65535) {
                    gSerialLineBuf = "";
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void data_model_init() {
    gSnapshotQueue = xQueueCreate(1, sizeof(SnapshotMsg));
    gCommandQueue = xQueueCreate(CMD_QUEUE_SIZE, CMD_MAX_LEN);
    if (!gCommandQueue) {
        Serial.println("[data_model] Failed to create command queue!");
    }
}

// Non-blocking command send - sends via Serial immediately (most reliable)
void send_command(const char* cmd) {
    if (!cmd) return;
    // Ensure newline termination
    size_t len = strlen(cmd);
    bool has_newline = (len > 0 && cmd[len - 1] == '\n');
    
    // Send directly via Serial - this is immediate and reliable
    if (has_newline) {
        Serial.write((const uint8_t*)cmd, len);
    } else {
        Serial.write((const uint8_t*)cmd, len);
        Serial.write((const uint8_t*)"\n", 1);
    }
    
    // Also queue for WiFi if available (optional)
    if (gCommandQueue) {
        char buf[CMD_MAX_LEN];
        strncpy(buf, cmd, CMD_MAX_LEN - 1);
        buf[CMD_MAX_LEN - 1] = '\0';
        xQueueSend(gCommandQueue, buf, 0);  // Non-blocking, drop if full
    }
}

void start_serial_task() {
    xTaskCreatePinnedToCore(
        serial_task,
        "SerialTask",
        16384,      // 16KB stack - enough for JSON parsing with queue data
        nullptr,
        1,          // priority
        nullptr,
        0           // core 0
    );
}

// ========== WiFi Task ==========
#include <WiFi.h>
#include "wifi_manager.h"

// TCP server connection parameters (set via start_wifi_task)
static const char* gTcpHost = nullptr;
static uint16_t gTcpPort = 5555;

static void wifi_task(void *pvParameters) {
    (void) pvParameters;
    
    // Buffer for incoming TCP data
    String lineBuf;
    lineBuf.reserve(8192);
    
    for (;;) {
        // Check WiFi connection
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        
        // Try to connect to TCP server
        WiFiClient client;
        
        if (!client.connect(gTcpHost, gTcpPort)) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        
        client.setTimeout(1);
        client.setNoDelay(true);
        lineBuf = "";
        
        while (client.connected() && WiFi.status() == WL_CONNECTED) {
            // === SEND QUEUED COMMANDS (non-blocking) ===
            char cmdBuf[CMD_MAX_LEN];
            while (gCommandQueue && xQueueReceive(gCommandQueue, cmdBuf, 0) == pdTRUE) {
                size_t len = strlen(cmdBuf);
                if (len > 0) {
                    client.print(cmdBuf);
                    if (cmdBuf[len-1] != '\n') {
                        client.print('\n');
                    }
                }
            }
            
            // === READ INCOMING DATA ===
            while (client.available() > 0) {
                char c = (char)client.read();
                
                if (c == '\n') {
                    String line = lineBuf;
                    lineBuf = "";
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
                    lineBuf += c;
                    if (lineBuf.length() > 65535) {
                        lineBuf = "";
                    }
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        
        client.stop();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void start_wifi_task(const char* host, uint16_t port) {
    gTcpHost = host;
    gTcpPort = port;
    
    // Initialize WiFi manager (handles credentials from NVS)
    wifiMgr.begin();
    
    // Try to auto-connect to saved networks
    wifiMgr.autoConnect();
    
    // Start the WiFi task
    xTaskCreatePinnedToCore(
        wifi_task,
        "WiFiTask",
        12288,      // 12KB stack
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
