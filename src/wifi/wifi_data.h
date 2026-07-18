// =============================================================================
// wifi_data.h — Datenstrukturen für WiFi-Scan-Ergebnisse
// =============================================================================
// Diese Datei enthält die reinen Datenstrukturen — keine WiFi.h-Abhängigkeit
// (außer dem enum wifi_auth_mode_t im .cpp), damit sie auch in UI-Code
// inkludiert werden kann, der nichts mit dem WiFi-Stack zu tun hat.
// =============================================================================
#pragma once
#include <Arduino.h>
#include <vector>
#include "config.h"   // WIFI_HISTORY_SIZE, WIFI_GRAPH_TOP_N

// -----------------------------------------------------------------------------
// Verschlüsselungstypen, gemappt aus WiFi.encryptionType() / wifi_auth_mode_t
// -----------------------------------------------------------------------------
// ACHTUNG: Die Werte heißen bewusst NICHT WIFI_AUTH_*, weil das mit den
// gleichnamigen Konstanten aus <WiFi.h> (ESP-IDF-Enum wifi_auth_mode_t)
// kollidieren würde. Präfix "WAUTH_" ist eindeutig.
enum WifiAuth {
    WAUTH_OPEN            = 0,
    WAUTH_WEP             = 1,
    WAUTH_WPA_PSK         = 2,
    WAUTH_WPA2_PSK        = 3,
    WAUTH_WPA_WPA2_PSK    = 4,
    WAUTH_WPA3_PSK        = 5,   // ESP32 Arduino Core 2.x+
    WAUTH_WPA2_ENTERPRISE = 6,
    WAUTH_WPA2_WPA3_PSK   = 7,   // Transition-Mode (WPA2 + WPA3)
    WAUTH_WAPI_PSK        = 8,   // chinesischer Standard
    WAUTH_OWE             = 9,   // Opportunistic Wireless Encryption (WPA3-Open)
    WAUTH_UNKNOWN         = 99
};

// -----------------------------------------------------------------------------
// Struktur für ein gefundenes WLAN
// -----------------------------------------------------------------------------
struct WifiNetworkInfo {
    String   ssid;        // kann bei Hidden-Netzen leer sein
    uint8_t  bssid[6];    // MAC-Adresse (6 Byte, network order)
    int32_t  rssi;        // dBm, z.B. -45
    int32_t  channel;     // 1..14
    WifiAuth auth;        // siehe enum oben
    bool     hidden;      // true, wenn SSID leer
    uint32_t lastSeenMs;  // millis() beim letzten Scan
    uint8_t  phyMode;     // 11b/g/n als Bitmask, optional
};

// -----------------------------------------------------------------------------
// RSSI-History-Eintrag mit Ring-Buffer pro BSSID.
//
// Wird im Scan-Task (Core 0) gepflegt: nach jedem WiFi.scanNetworks()-Tick
// wird fuer jedes aktuell sichtbare Netz ein neuer RSSI-Sample in
// rssiSamples[head] geschrieben und head um 1 (mod WIFI_HISTORY_SIZE) erhoeht.
// 'count' zaehlt die gueltigen Samples (<= WIFI_HISTORY_SIZE); bei count <
// WIFI_HISTORY_SIZE ist der Ring-Buffer noch nicht voll (Boot-Phase).
//
// 'active' wird im Scan-Tick auf true gesetzt und nach ein paar verpassten
// Scans (z.B. 3) auf false. Inaktive Eintraege werden bevorzugt evictet
// (siehe updateHistoryFromScanLocked() in wifi_scanner.cpp), bleiben aber
// bis dahin einfach stehen (Linie im Graph endet abrupt).
// -----------------------------------------------------------------------------
struct WifiNetworkHistory {
    uint8_t   bssid[6];
    String    ssid;                 // fuer Legende
    int16_t   rssiSamples[WIFI_HISTORY_SIZE]; // Ring-Buffer (dBm, -128..0)
    uint32_t  timestamps[WIFI_HISTORY_SIZE];  // millis() je Sample
    uint8_t   head;                 // Schreib-Index (0..WIFI_HISTORY_SIZE-1)
    uint8_t   count;                // Anzahl gueltiger Samples
    bool      active;               // in letzten Scans noch gesehen
    uint8_t   missCount;            // aufeinanderfolgende Scans ohne Sichtung
};

// -----------------------------------------------------------------------------
// Globale Liste (von wifi_scanner.cpp definiert)
// -----------------------------------------------------------------------------
extern std::vector<WifiNetworkInfo> g_networks;

// -----------------------------------------------------------------------------
// Vergleichs-Funktion für std::sort: stärkstes Netz zuerst
// -----------------------------------------------------------------------------
inline bool compareByRssi(const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    return a.rssi > b.rssi;
}

// -----------------------------------------------------------------------------
// Auth-Type als kurzen Klartext (für UI / Serial)
// -----------------------------------------------------------------------------
inline const char* authToString(WifiAuth a) {
    switch (a) {
        case WAUTH_OPEN:            return "Open";
        case WAUTH_WEP:             return "WEP";
        case WAUTH_WPA_PSK:         return "WPA";
        case WAUTH_WPA2_PSK:        return "WPA2";
        case WAUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WAUTH_WPA3_PSK:        return "WPA3";
        case WAUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
        case WAUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        case WAUTH_WAPI_PSK:        return "WAPI";
        case WAUTH_OWE:             return "OWE";
        default:                    return "?";
    }
}
