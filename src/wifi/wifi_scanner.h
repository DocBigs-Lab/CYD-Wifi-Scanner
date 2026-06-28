// =============================================================================
// wifi_scanner.h — Asynchroner WiFi-Scanner (FreeRTOS-Task auf Core 0)
// =============================================================================
// Der Scanner läuft in einem eigenen FreeRTOS-Task auf Core 0, damit die
// UI (loop() auf Core 1) nicht durch WiFi-Scan-Wartezeiten blockiert wird.
//
// Kommunikation mit der UI:
//   - wifiScannerHasNewResult() : true, wenn seit letztem Aufruf ein neuer
//     Scan gelandet ist (UI kann daraufhin neu rendern).
//   - g_networks (in wifi_data.h) : globale, Mutex-geschützte Ergebnisliste.
//
// Threading-Hinweis:
//   Auf g_networks darf nur via den Helper-Funktionen (wifiScannerNetworkCount
//   u. ä.) ODER im kritischen Abschnitt mit portENTER_CRITICAL(&g_networksMux)
//   zugegriffen werden. Die Mutex-Variable ist hier nicht öffentlich; wer
//   einen längeren lesenden Zugriff braucht, soll einen Snapshot machen.
// =============================================================================
#pragma once
#include <Arduino.h>
#include <stddef.h>
#include "wifi_data.h"

// -----------------------------------------------------------------------------
// Startet den Scan-Task auf Core 0. Idempotent (mehrfach aufrufbar).
//
//   intervalMs : Pause zwischen zwei Scans (Default aus config.h: WIFI_SCAN_INTERVAL_MS)
//   taskStack  : Stack-Größe in Bytes. WiFi.scanNetworks() braucht einiges;
//                4096 ist ein sicherer Default, 8192 schadet nie.
// -----------------------------------------------------------------------------
void wifiScannerStart(uint32_t intervalMs = 4000,
                      uint32_t taskStack  = 4096);

// Stoppt den Scan-Task wieder (gibt die Task frei).
void wifiScannerStop();

// Liefert true, wenn seit dem letzten Aufruf ein neuer Scan eingetroffen ist.
// Beim Zurückliefern von true wird der "neu"-Zustand zurückgesetzt (Edge-Detection).
bool wifiScannerHasNewResult();

// millis()-Zeitpunkt, an dem der letzte Scan gelandet ist (0 = noch keiner).
uint32_t wifiScannerLastScanMs();

// Dauer des letzten Scans (vom Trigger bis die Ergebnisse im RAM liegen).
uint32_t wifiScannerLastScanDurationMs();

// Anzahl der aktuell gefundenen Netze (thread-safe Snapshot).
size_t wifiScannerNetworkCount();

// Macht einen thread-safe Snapshot von g_networks in 'out'.
// Vor dem Lesen aus 'out' KEINE Mutex nötig — der Snapshot ist eine Kopie.
void wifiScannerCopyNetworks(std::vector<WifiNetworkInfo>& out);

// -----------------------------------------------------------------------------
// RSSI-History-API (Live-Graph)
// -----------------------------------------------------------------------------
// Liefert die Top-N staerksten Netze sortiert nach RSSI in 'outBssids'.
// outBssids muss Platz fuer maxN * 6 Bytes haben.
// Rueckgabe = Anzahl tatsaechlich gefuellter BSSIDs (<= maxN).
size_t wifiScannerCopyTopBssids(uint8_t outBssids[][6], size_t maxN);

// Liefert Ring-Buffer-Snapshot fuer eine bestimmte BSSID.
// outRssi und outTs muessen je Platz fuer maxSamples Eintraege haben.
// Eintraege werden in CHRONOLOGISCHER Reihenfolge geschrieben
// (aeltestes Sample zuerst, neuestes zuletzt).
// Rueckgabe = Anzahl geschriebener Samples (<= WIFI_HISTORY_SIZE).
// Bei unbekannter BSSID werden outRssi/outTs NICHT angefasst, Rueckgabe = 0.
size_t wifiScannerCopyHistory(const uint8_t bssid[6],
                              int16_t* outRssi, uint32_t* outTs,
                              size_t maxSamples);

// Aktuelle RSSI der BSSID (neuester Sample, ohne Ring-Buffer-Kopie).
// Liefert true bei bekanntem Netz, false sonst.
bool wifiScannerCurrentRssi(const uint8_t bssid[6], int16_t& outRssi);

// Liefert die SSID der BSSID (fuer Legende-Anzeige).
// Bei unbekannter BSSID wird outSsid = "" gesetzt, Rueckgabe = false.
bool wifiScannerSsidForBssid(const uint8_t bssid[6], String& outSsid);