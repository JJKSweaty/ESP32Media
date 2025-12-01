#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

// Max networks to store from scan
#define MAX_SCAN_NETWORKS 15
#define MAX_SAVED_NETWORKS 3

// Network info struct
struct NetworkInfo {
    char ssid[33];
    int32_t rssi;
    uint8_t encType;  // WIFI_AUTH_OPEN, WIFI_AUTH_WPA_PSK, etc.
    bool saved;       // Is this in our saved networks?
};

// WiFi Manager class
class WiFiManager {
public:
    WiFiManager();
    
    // Initialize (load saved networks from NVS)
    void begin();
    
    // Scan for networks (non-blocking start)
    void startScan();
    
    // Check if scan is complete
    bool isScanComplete();
    
    // Get scan results (call after isScanComplete returns true)
    int getScanResults(NetworkInfo* results, int maxResults);
    
    // Connect to a network
    bool connect(const char* ssid, const char* password, bool save = true);
    
    // Connect to saved network (by index)
    bool connectSaved(int index);
    
    // Disconnect
    void disconnect();
    
    // Forget a saved network
    bool forgetNetwork(const char* ssid);
    
    // Get current connection status
    bool isConnected();
    String getConnectedSSID();
    IPAddress getIP();
    int32_t getRSSI();
    
    // Get saved networks
    int getSavedNetworks(NetworkInfo* results, int maxResults);
    
    // Auto-connect to best saved network
    bool autoConnect();
    
    // Check if we have saved password for an SSID (public for UI use)
    bool findSavedPassword(const char* ssid, char* password, size_t maxLen);

private:
    Preferences prefs;
    bool scanInProgress;
    int lastScanCount;
    
    // Saved network credentials
    struct SavedNetwork {
        char ssid[33];
        char password[65];
    };
    SavedNetwork savedNetworks[MAX_SAVED_NETWORKS];
    int savedCount;
    
    void loadSavedNetworks();
    void saveSavedNetworks();
};

// Global instance
extern WiFiManager wifiMgr;
