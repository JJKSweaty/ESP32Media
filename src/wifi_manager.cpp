#include "wifi_manager.h"

WiFiManager wifiMgr;

WiFiManager::WiFiManager() : scanInProgress(false), lastScanCount(0), savedCount(0) {
    memset(savedNetworks, 0, sizeof(savedNetworks));
}

void WiFiManager::begin() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    loadSavedNetworks();
    Serial.println("[WiFiMgr] Initialized");
}

void WiFiManager::loadSavedNetworks() {
    prefs.begin("wifi", true);  // Read-only
    savedCount = prefs.getInt("count", 0);
    if (savedCount > MAX_SAVED_NETWORKS) savedCount = MAX_SAVED_NETWORKS;
    
    for (int i = 0; i < savedCount; i++) {
        char key_ssid[16], key_pass[16];
        snprintf(key_ssid, sizeof(key_ssid), "ssid%d", i);
        snprintf(key_pass, sizeof(key_pass), "pass%d", i);
        
        prefs.getString(key_ssid, savedNetworks[i].ssid, sizeof(savedNetworks[i].ssid));
        prefs.getString(key_pass, savedNetworks[i].password, sizeof(savedNetworks[i].password));
    }
    prefs.end();
    Serial.printf("[WiFiMgr] Loaded %d saved networks\n", savedCount);
}

void WiFiManager::saveSavedNetworks() {
    prefs.begin("wifi", false);  // Read-write
    prefs.putInt("count", savedCount);
    
    for (int i = 0; i < savedCount; i++) {
        char key_ssid[16], key_pass[16];
        snprintf(key_ssid, sizeof(key_ssid), "ssid%d", i);
        snprintf(key_pass, sizeof(key_pass), "pass%d", i);
        
        prefs.putString(key_ssid, savedNetworks[i].ssid);
        prefs.putString(key_pass, savedNetworks[i].password);
    }
    prefs.end();
    Serial.printf("[WiFiMgr] Saved %d networks\n", savedCount);
}

bool WiFiManager::findSavedPassword(const char* ssid, char* password, size_t maxLen) {
    for (int i = 0; i < savedCount; i++) {
        if (strcmp(savedNetworks[i].ssid, ssid) == 0) {
            strncpy(password, savedNetworks[i].password, maxLen - 1);
            password[maxLen - 1] = '\0';
            return true;
        }
    }
    return false;
}

void WiFiManager::startScan() {
    if (scanInProgress) {
        Serial.println("[WiFiMgr] Scan already in progress");
        return;
    }
    
    Serial.println("[WiFiMgr] Starting fast scan...");
    scanInProgress = true;
    
    // Delete previous scan results
    WiFi.scanDelete();
    
    // Use synchronous scan with faster settings
    // Parameters: async=false, show_hidden=false (faster), passive=false, max_ms_per_chan=120
    int result = WiFi.scanNetworks(false, false, false, 120);  // Much faster scan
    
    if (result >= 0) {
        lastScanCount = result;
        Serial.printf("[WiFiMgr] Scan complete: %d networks found\n", result);
    } else {
        lastScanCount = 0;
        Serial.printf("[WiFiMgr] Scan failed with code: %d\n", result);
    }
    
    scanInProgress = false;
}

bool WiFiManager::isScanComplete() {
    return !scanInProgress;
}

int WiFiManager::getScanResults(NetworkInfo* results, int maxResults) {
    int count = min(lastScanCount, maxResults);
    count = min(count, MAX_SCAN_NETWORKS);
    
    for (int i = 0; i < count; i++) {
        strncpy(results[i].ssid, WiFi.SSID(i).c_str(), 32);
        results[i].ssid[32] = '\0';
        results[i].rssi = WiFi.RSSI(i);
        results[i].encType = WiFi.encryptionType(i);
        results[i].saved = false;
        
        // Check if this is a saved network
        for (int j = 0; j < savedCount; j++) {
            if (strcmp(results[i].ssid, savedNetworks[j].ssid) == 0) {
                results[i].saved = true;
                break;
            }
        }
    }
    return count;
}

bool WiFiManager::connect(const char* ssid, const char* password, bool save) {
    Serial.printf("[WiFiMgr] Connecting to %s...\n", ssid);
    
    // Configure WiFi for faster connection
    WiFi.disconnect(true);  // true = clear stored credentials
    WiFi.setAutoReconnect(false);  // We manage reconnection ourselves
    WiFi.begin(ssid, password);
    
    // Wait for connection (with timeout: 50 attempts * 100ms = 5 seconds)
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 50) {
        delay(100);  // Faster polling
        attempts++;
        // Yield to allow other tasks
        if (attempts % 10 == 0) {
            Serial.printf("[WiFiMgr] Still connecting... (%d/50)\n", attempts);
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFiMgr] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        
        // Save credentials if requested
        if (save) {
            // Check if already saved
            bool found = false;
            for (int i = 0; i < savedCount; i++) {
                if (strcmp(savedNetworks[i].ssid, ssid) == 0) {
                    // Update password
                    strncpy(savedNetworks[i].password, password, 64);
                    savedNetworks[i].password[64] = '\0';
                    found = true;
                    break;
                }
            }
            
            // Add new if not found and have space
            if (!found && savedCount < MAX_SAVED_NETWORKS) {
                strncpy(savedNetworks[savedCount].ssid, ssid, 32);
                savedNetworks[savedCount].ssid[32] = '\0';
                strncpy(savedNetworks[savedCount].password, password, 64);
                savedNetworks[savedCount].password[64] = '\0';
                savedCount++;
            }
            saveSavedNetworks();
        }
        return true;
    }
    
    Serial.println("[WiFiMgr] Connection failed");
    return false;
}

bool WiFiManager::connectSaved(int index) {
    if (index < 0 || index >= savedCount) return false;
    return connect(savedNetworks[index].ssid, savedNetworks[index].password, false);
}

void WiFiManager::disconnect() {
    WiFi.disconnect();
    Serial.println("[WiFiMgr] Disconnected");
}

bool WiFiManager::forgetNetwork(const char* ssid) {
    for (int i = 0; i < savedCount; i++) {
        if (strcmp(savedNetworks[i].ssid, ssid) == 0) {
            // Shift remaining networks down
            for (int j = i; j < savedCount - 1; j++) {
                savedNetworks[j] = savedNetworks[j + 1];
            }
            savedCount--;
            saveSavedNetworks();
            Serial.printf("[WiFiMgr] Forgot network: %s\n", ssid);
            return true;
        }
    }
    return false;
}

bool WiFiManager::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String WiFiManager::getConnectedSSID() {
    if (isConnected()) {
        return WiFi.SSID();
    }
    return "";
}

IPAddress WiFiManager::getIP() {
    return WiFi.localIP();
}

int32_t WiFiManager::getRSSI() {
    if (isConnected()) {
        return WiFi.RSSI();
    }
    return 0;
}

int WiFiManager::getSavedNetworks(NetworkInfo* results, int maxResults) {
    int count = min(savedCount, maxResults);
    for (int i = 0; i < count; i++) {
        strncpy(results[i].ssid, savedNetworks[i].ssid, 32);
        results[i].ssid[32] = '\0';
        results[i].rssi = 0;  // Unknown until we scan
        results[i].encType = WIFI_AUTH_WPA2_PSK;  // Assume WPA2
        results[i].saved = true;
    }
    return count;
}

bool WiFiManager::autoConnect() {
    Serial.println("[WiFiMgr] Auto-connecting to best saved network...");
    
    if (savedCount == 0) {
        Serial.println("[WiFiMgr] No saved networks");
        return false;
    }
    
    // Try each saved network
    for (int i = 0; i < savedCount; i++) {
        if (connect(savedNetworks[i].ssid, savedNetworks[i].password, false)) {
            return true;
        }
    }
    
    return false;
}
