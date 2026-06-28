// =============================================================================
// wifi_scanner.cpp — FreeRTOS-Scan-Task + RSSI-History-Ring-Buffer pro BSSID
// =============================================================================
// Ablauf pro Iteration:
//   1. WiFi.scanNetworks(false, true) -> synchroner Scan (blockt ca. 2-4 s)
//   2. Ergebnisse in lokales std::vector<WifiNetworkInfo> übertragen
//   3. RSSI-Filter anwenden, nach RSSI sortieren
//   4. Unter portMUX-Critical-Section globalen Vektor atomar austauschen
//   5. WiFi.scanDelete() -> ESP-IDF-Speicher freigeben
//   6. g_newResult setzen, damit die UI neu rendert
//   7. vTaskDelay(g_intervalMs) -> nächste Iteration
//
// Hinweis: Frühere Versionen nutzten async (scanNetworks(true, true)).
// Bei ESP-IDF 5.x / Arduino-ESP32 3.x lieferte das regelmäßig
// WIFI_SCAN_FAILED (-2), wenn direkt nach mode()+disconnect() gescannt
// wurde. Der synchrone Scan ist im FreeRTOS-Task unkritisch (blockiert
// nur diesen einen Task) und wesentlich robuster.
// =============================================================================
#include "wifi_scanner.h"
#include <WiFi.h>
#include <algorithm>
#include <string.h>       // memcpy, memset
#include "config.h"       // WIFI_SCAN_INTERVAL_MS, WIFI_SCAN_MIN_RSSI

// ---- Globaler Zustand ------------------------------------------------------
std::vector<WifiNetworkInfo> g_networks;
static portMUX_TYPE          g_networksMux = portMUX_INITIALIZER_UNLOCKED;

// RSSI-History pro BSSID (Ring-Buffer). Wird im selben kritischen Abschnitt
// wie g_networks gepflegt (scanTask ist single-threaded, kein paralleler
// Zugriff). Read-API nutzt aber den gleichen Mutex fuer den Snapshot.
static std::vector<WifiNetworkHistory> g_history;

static TaskHandle_t     g_taskHandle  = nullptr;
static volatile bool    g_newResult   = false;
static volatile uint32_t g_lastScanMs = 0;
static volatile uint32_t g_lastDur    = 0;
static uint32_t         g_intervalMs  = WIFI_SCAN_INTERVAL_MS;

// ---- Hilfsfunktionen -------------------------------------------------------

// Mappt den ESP-IDF-wifi_auth_mode_t (aus <WiFi.h>) auf unser WifiAuth-Enum.
// linke Seite (case-Label) = ESP-IDF-Konstante WIFI_AUTH_*
// rechte Seite (return)    = unsere Konstante WAUTH_*
static WifiAuth mapAuth(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:            return WAUTH_OPEN;
        case WIFI_AUTH_WEP:             return WAUTH_WEP;
        case WIFI_AUTH_WPA_PSK:         return WAUTH_WPA_PSK;
        case WIFI_AUTH_WPA2_PSK:        return WAUTH_WPA2_PSK;
        case WIFI_AUTH_WPA_WPA2_PSK:    return WAUTH_WPA_WPA2_PSK;
        case WIFI_AUTH_WPA3_PSK:        return WAUTH_WPA3_PSK;
        case WIFI_AUTH_WPA2_ENTERPRISE: return WAUTH_WPA2_ENTERPRISE;
        case WIFI_AUTH_WPA2_WPA3_PSK:   return WAUTH_WPA2_WPA3_PSK;
        case WIFI_AUTH_WAPI_PSK:        return WAUTH_WAPI_PSK;
        default:                        return WAUTH_UNKNOWN;
    }
}

// Führt einen einzelnen Scanversuch durch und liefert true bei Erfolg
// (n >= 0). Bei Fehler (WIFI_SCAN_FAILED) wird ein Retry mit kurzem
// Backoff versucht. Maximal 3 Versuche pro Aufruf.
static bool tryScanOnce(int16_t& outN, uint32_t& outDurMs) {
    const uint32_t t0 = millis();

    // Sicherheitscheck: falls noch ein Scan laeuft, erst abschliessen
    // oder hart abbrechen.
    int16_t prev = WiFi.scanComplete();
    if (prev == WIFI_SCAN_RUNNING) {
        Serial.println("[scan] WARN: alter Scan haengt, breche ab (scanDelete)");
        WiFi.scanDelete();
    }

    // SYNCHRONER Scan. false = blockiert bis Ergebnis vorliegt (2-4 s),
    // true  = zeigt auch Hidden-SSIDs an.
    int16_t n = WiFi.scanNetworks(false /* async=false -> blockierend */,
                                  true  /* show_hidden */);
    const uint32_t dur = millis() - t0;

    if (n == WIFI_SCAN_FAILED) {
        Serial.printf("[scan] scanNetworks() FEHLGESCHLAGEN (status=%d, dauer=%lu ms)\n",
                      (int)WiFi.status(), (unsigned long)dur);
        outN      = WIFI_SCAN_FAILED;
        outDurMs  = dur;
        return false;
    }

    Serial.printf("[scan] scanNetworks() -> %d Netze (status=%d, dauer=%lu ms)\n",
                  (int)n, (int)WiFi.status(), (unsigned long)dur);

    outN     = n;
    outDurMs = dur;
    return true;
}

// -----------------------------------------------------------------------------
// History-Pflege
// -----------------------------------------------------------------------------
// Wird innerhalb der g_networksMux-Critical-Section aufgerufen (also
// single-threaded relativ zu loop()/Read-APIs). Sucht fuer jede BSSID in
// 'current' den passenden History-Eintrag und schiebt einen neuen RSSI-
// Sample in dessen Ring-Buffer. Neue BSSIDs bekommen einen neuen Eintrag
// (bis WIFI_HISTORY_MAX_ENTRIES), alte bekommen einen weiteren Sample.
// BSSIDs, die in 'current' fehlen, kriegen missCount++ (nach 3 verpassten
// Scans wird active=false gesetzt; Eintrag bleibt aber stehen).
static void updateHistoryFromScanLocked(const std::vector<WifiNetworkInfo>& current) {
    const uint32_t now = millis();

    // 1) Aktualisieren / Anlegen fuer jede sichtbare BSSID
    for (const auto& n : current) {
        WifiNetworkHistory* entry = nullptr;

        // Existierenden Eintrag suchen
        for (auto& h : g_history) {
            if (memcmp(h.bssid, n.bssid, 6) == 0) {
                entry = &h;
                break;
            }
        }

        // Neu anlegen, falls Platz ist
        if (entry == nullptr) {
            if (g_history.size() >= WIFI_HISTORY_MAX_ENTRIES) {
                // Eviction per echtem LRU: zuerst ein inaktiver Eintrag,
                // sonst der AKTIVE Eintrag mit dem AELTESTEN letzten
                // Sample-Zeitstempel. Ein Netz, das in DIESEM Scan vorkommt,
                // wurde bereits (oder wird gleich) mit Zeitstempel=now
                // aktualisiert — hat also nie den aeltesten Zeitstempel und
                // wird dadurch nie versehentlich evictet, selbst wenn es
                // gerade im Live-Graph angezeigt wird.
                size_t replaceIdx   = 0;
                bool   foundInactive = false;
                for (size_t i = 0; i < g_history.size(); ++i) {
                    if (!g_history[i].active) {
                        replaceIdx    = i;
                        foundInactive = true;
                        break;
                    }
                }
                if (!foundInactive) {
                    uint32_t oldestTs = g_history[0].timestamps[g_history[0].head];
                    for (size_t i = 1; i < g_history.size(); ++i) {
                        const uint32_t ts = g_history[i].timestamps[g_history[i].head];
                        if (ts < oldestTs) {
                            oldestTs   = ts;
                            replaceIdx = i;
                        }
                    }
                }
                g_history[replaceIdx] = WifiNetworkHistory{};
                entry = &g_history[replaceIdx];
            } else {
                g_history.push_back(WifiNetworkHistory{});
                entry = &g_history.back();
            }
            memcpy(entry->bssid, n.bssid, 6);
            entry->ssid        = n.ssid;
            entry->active      = true;
            entry->missCount   = 0;
            entry->head        = 0;
            entry->count       = 0;
        }

        // Neuen Sample in den Ring-Buffer schieben
        entry->head = (entry->head + 1) % WIFI_HISTORY_SIZE;
        entry->rssiSamples[entry->head]  = (int16_t)n.rssi;
        entry->timestamps[entry->head]   = now;
        if (entry->count < WIFI_HISTORY_SIZE) entry->count++;
        entry->ssid     = n.ssid;   // aktualisieren (Hidden-Netz kann SSID bekommen)
        entry->active   = true;
        entry->missCount = 0;
    }

    // 2) Fehlende BSSIDs markieren (missCount++), nach 3 Scans inaktiv
    for (auto& h : g_history) {
        bool seen = false;
        for (const auto& n : current) {
            if (memcmp(h.bssid, n.bssid, 6) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            if (h.missCount < 255) h.missCount++;
            if (h.missCount > 3) h.active = false;
        }
    }
}

static void performOneScan() {
    const uint32_t t0 = millis();
    int16_t n = WIFI_SCAN_FAILED;
    uint32_t dur = 0;

    // Bis zu 3 Versuche, falls das Funkmodul beim ersten Mal noch nicht
    // bereit ist (typisches Symptom direkt nach mode()+disconnect()).
    constexpr int MAX_TRIES = 3;
    for (int attempt = 1; attempt <= MAX_TRIES; ++attempt) {
        if (tryScanOnce(n, dur)) {
            break;   // Erfolg
        }
        Serial.printf("[scan] Retry %d/%d in 250 ms ...\n", attempt, MAX_TRIES);
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    g_lastDur    = millis() - t0;
    g_lastScanMs = millis();

    if (n <= 0) {
        // n == 0  = Scan fertig, aber keine Netze gefunden
        // n <  0  = Fehler (z.B. WIFI_SCAN_FAILED = -2)
        return;
    }

    // Ergebnisse in unsere Struktur übertragen
    std::vector<WifiNetworkInfo> local;
    local.reserve(static_cast<size_t>(n));

    for (int16_t i = 0; i < n; i++) {
        WifiNetworkInfo info;
        info.ssid       = WiFi.SSID(i);
        info.rssi       = WiFi.RSSI(i);
        info.channel    = WiFi.channel(i);
        info.auth       = mapAuth(static_cast<wifi_auth_mode_t>(WiFi.encryptionType(i)));
        info.hidden     = (info.ssid.length() == 0);
        info.lastSeenMs = millis();
        info.phyMode    = 0;   // (ESP-IDF-Lieferung von phy-Mode optional; erstmal 0)

        // BSSID (MAC) als 6 Bytes
        const uint8_t* b = WiFi.BSSID(i);
        if (b) {
            memcpy(info.bssid, b, 6);
        } else {
            memset(info.bssid, 0, 6);
        }

        // RSSI-Filter: zu schwache Netze ignorieren
        if (info.rssi >= WIFI_SCAN_MIN_RSSI) {
            local.push_back(std::move(info));
        }
    }

    // Nach RSSI absteigend sortieren (stärkstes oben)
    std::sort(local.begin(), local.end(), compareByRssi);

    // Globalen Vektor atomar austauschen
    portENTER_CRITICAL(&g_networksMux);
    g_networks = std::move(local);
    // History mit-pflegen (gleicher Mutex, damit Read-Snapshots konsistent
    // sind zwischen g_networks und g_history).
    updateHistoryFromScanLocked(g_networks);
    portEXIT_CRITICAL(&g_networksMux);

    // ESP-IDF-Scan-Speicher freigeben
    WiFi.scanDelete();

    g_newResult = true;
}

// ---- FreeRTOS-Task (läuft auf Core 0) --------------------------------------

static void scanTask(void* /*arg*/) {
    Serial.printf("[scan] Task gestartet auf Core %d (intervall=%lu ms)\n",
                  xPortGetCoreID(), static_cast<unsigned long>(g_intervalMs));

    // ---- WiFi-Initialisierung (in der richtigen Reihenfolge!) -----------
    //   1. persistent(false) verhindert NVS-Schreibvorgaenge waehrend Init
    //      und rueckstandsbedingte Probleme mit alten Credentials.
    //   2. mode(WIFI_STA) braucht beim ersten Aufruf deutlich laenger als
    //      100 ms, bis der Mode-Switch stabil ist (interner WiFi-Task +
    //      PHY-Init). 500 ms ist ein sicherer Wert.
    //   3. disconnect() (ohne eraseCredentials) reicht aus, um den Auto-
    //      Reconnect zu unterbinden. eraseCredentials schreibt zusaetzlich
    //      in NVS, was in seltenen Faellen zu Timing-Problemen fuehrt.
    //   4. setSleep(false) verhindert Power-Save waehrend der Scans, sonst
    //      kann das Funkmodul Pakete bzw. Beacons verpassen.
    //   5. setTxPower(WIFI_POWER_19_5dBm) maximiert die Reichweite fuer
    //      schwache APs (vor allem bei CYD ohne externe Antenne).
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    vTaskDelay(pdMS_TO_TICKS(500));                // Mode-Switch stabilisieren
    WiFi.disconnect();                              // kein eraseCredentials noetig
    vTaskDelay(pdMS_TO_TICKS(200));
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);            // max. Reichweite

    Serial.printf("[scan] WiFi init: mode=%d, status=%d, txpower=%d\n",
                  (int)WiFi.getMode(), (int)WiFi.status(),
                  (int)WiFi.getTxPower());

    // Ersten Scan bewusst NICHT sofort ausfuehren — das Funkmodul ist
    // nach mode()+disconnect() oft noch nicht scan-faehig. Stattdessen
    // ein zusaetzlicher Stabilisierungs-Delay und dann der erste Scan
    // innerhalb der regulaeren Schleife (mit Retry-Logik).
    vTaskDelay(pdMS_TO_TICKS(500));

    while (true) {
        performOneScan();
        vTaskDelay(pdMS_TO_TICKS(g_intervalMs));
    }
}

// ---- Public API ------------------------------------------------------------

void wifiScannerStart(uint32_t intervalMs, uint32_t taskStack) {
    if (g_taskHandle != nullptr) {
        Serial.println("[scan] bereits aktiv");
        return;
    }
    if (intervalMs > 0) g_intervalMs = intervalMs;

    BaseType_t ok = xTaskCreatePinnedToCore(
        scanTask, "wifi-scan", taskStack, nullptr, /*prio=*/1,
        &g_taskHandle, /*core=*/0);

    if (ok != pdPASS) {
        Serial.println("[scan] FEHLER: Task konnte nicht erstellt werden");
        g_taskHandle = nullptr;
    }
}

void wifiScannerStop() {
    if (g_taskHandle == nullptr) return;
    vTaskDelete(g_taskHandle);
    g_taskHandle = nullptr;
    Serial.println("[scan] Task gestoppt");
}

bool wifiScannerHasNewResult() {
    if (g_newResult) {
        g_newResult = false;
        return true;
    }
    return false;
}

uint32_t wifiScannerLastScanMs()         { return g_lastScanMs; }
uint32_t wifiScannerLastScanDurationMs() { return g_lastDur; }

size_t wifiScannerNetworkCount() {
    portENTER_CRITICAL(&g_networksMux);
    size_t n = g_networks.size();
    portEXIT_CRITICAL(&g_networksMux);
    return n;
}

void wifiScannerCopyNetworks(std::vector<WifiNetworkInfo>& out) {
    out.clear();
    portENTER_CRITICAL(&g_networksMux);
    out = g_networks;   // Wertkopie (Std-Vektor kopiert die Elemente)
    portEXIT_CRITICAL(&g_networksMux);
}

// -----------------------------------------------------------------------------
// RSSI-History Public API
// -----------------------------------------------------------------------------

size_t wifiScannerCopyTopBssids(uint8_t outBssids[][6], size_t maxN) {
    portENTER_CRITICAL(&g_networksMux);
    const size_t n = (g_networks.size() < maxN) ? g_networks.size() : maxN;
    for (size_t i = 0; i < n; ++i) {
        memcpy(outBssids[i], g_networks[i].bssid, 6);
    }
    portEXIT_CRITICAL(&g_networksMux);
    return n;
}

size_t wifiScannerCopyHistory(const uint8_t bssid[6],
                              int16_t* outRssi, uint32_t* outTs,
                              size_t maxSamples) {
    if (maxSamples == 0) return 0;

    portENTER_CRITICAL(&g_networksMux);

    // History-Eintrag suchen
    const WifiNetworkHistory* entry = nullptr;
    for (const auto& h : g_history) {
        if (memcmp(h.bssid, bssid, 6) == 0) {
            entry = &h;
            break;
        }
    }

    if (entry == nullptr || entry->count == 0) {
        portEXIT_CRITICAL(&g_networksMux);
        return 0;
    }

    // Chronologisch (aeltestes zuerst) in outRssi/outTs schreiben.
    // Ring-Buffer: head zeigt auf den NEUESTEN Sample. count Samples
    // zurueck ist der aelteste. Wenn count < WIFI_HISTORY_SIZE, ist der
    // Buffer noch nicht voll — dann ist Index 0 der aelteste und head
    // der neueste.
    const uint8_t total   = entry->count;
    const uint8_t oldest  = (entry->count < WIFI_HISTORY_SIZE)
                              ? 0
                              : (uint8_t)((entry->head + 1) % WIFI_HISTORY_SIZE);

    const size_t toWrite = (total < maxSamples) ? total : maxSamples;
    for (size_t i = 0; i < toWrite; ++i) {
        const uint8_t idx = (uint8_t)((oldest + i) % WIFI_HISTORY_SIZE);
        outRssi[i] = entry->rssiSamples[idx];
        outTs[i]   = entry->timestamps[idx];
    }

    portEXIT_CRITICAL(&g_networksMux);
    return toWrite;
}

bool wifiScannerCurrentRssi(const uint8_t bssid[6], int16_t& outRssi) {
    portENTER_CRITICAL(&g_networksMux);
    bool found = false;
    for (const auto& h : g_history) {
        if (memcmp(h.bssid, bssid, 6) == 0 && h.count > 0) {
            outRssi = h.rssiSamples[h.head];
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&g_networksMux);
    return found;
}

bool wifiScannerSsidForBssid(const uint8_t bssid[6], String& outSsid) {
    outSsid = "";
    portENTER_CRITICAL(&g_networksMux);
    bool found = false;
    // History hat den letzten gesehenen SSID-String (auch Hidden-Netz)
    for (const auto& h : g_history) {
        if (memcmp(h.bssid, bssid, 6) == 0) {
            outSsid = h.ssid;
            found = true;
            break;
        }
    }
    // Fallback: aktuelle Liste
    if (!found) {
        for (const auto& n : g_networks) {
            if (memcmp(n.bssid, bssid, 6) == 0) {
                outSsid = n.ssid;
                found = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&g_networksMux);
    return found;
}
