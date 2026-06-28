// =============================================================================
// ui_renderer.h — Public API des UI-Renderers
// =============================================================================
// Diese Datei kapselt sämtliche Zeichenoperationen (LIST/DETAIL/GRAPH/
// CHANNELS). main.cpp ruft ausschließlich renderScreen() auf; alle internen
// draw*()-Funktionen sind Translation-Unit-lokal in ui_renderer.cpp.
//
// Geteilter UI-State (von main.cpp gelesen + beschrieben):
//   - g_uiState          : UI_LIST | UI_DETAIL | UI_GRAPH | UI_CHANNELS
//   - g_scrollOffset     : erste sichtbare Zeile in der Netzliste
//   - g_selectedRow      : Index der selektierten Zeile, -1 = nichts selektiert
//   - g_detailBssid[]    : BSSID (6 Bytes) des in der Detailansicht gezeigten Netzes
//   - g_detailBssidValid : true, solange ein Netz im Detail gezeigt wird
//
// Netze werden ueber ihre BSSID identifiziert, nicht ueber einen Listen-Index:
// nach jedem Re-Scan wird die Liste neu nach RSSI sortiert, ein gemerkter
// Index wuerde dann auf ein anderes Netz zeigen. Die BSSID ist die stabile
// Identitaet eines Access Points ueber alle Sortiervorgaenge hinweg.
// =============================================================================
#pragma once
#include <Arduino.h>
#include "config.h"
#include "wifi/wifi_data.h"

// -----------------------------------------------------------------------------
// UI-State-Machine
// -----------------------------------------------------------------------------
enum UiState {
    UI_LIST,      // gescrollte Liste aller Netze
    UI_DETAIL,    // Detailansicht eines einzelnen Netzes
    UI_GRAPH,     // RSSI-Live-Graph der Top-N staerksten Netze
    UI_CHANNELS   // Kanalbelegung (Balkendiagramm, Netze pro Kanal 1-13)
};

// -----------------------------------------------------------------------------
// Geteilter UI-State (Definition in ui_renderer.cpp)
// -----------------------------------------------------------------------------
extern UiState   g_uiState;
extern int       g_scrollOffset;
extern int       g_selectedRow;
extern uint8_t   g_detailBssid[6];
extern bool      g_detailBssidValid;

// BSSIDs, die im GRAPH-State geplottet werden. Wird beim Einstieg in
// UI_GRAPH einmal aus dem aktuellen Netz-Snapshot gefuellt und danach NICHT
// mehr dynamisch nachjustiert (deterministisches Verhalten — Linien-Farben
// bleiben stabil). Index 0 ist immer das per Long-Press gewaehlte (eigene)
// Netz, Index 1..N-1 sind die staerksten anderen Netze zum Vergleich.
extern uint8_t   g_graphBssids[WIFI_GRAPH_TOP_N][6];
extern uint8_t   g_graphBssidCount;     // <= WIFI_GRAPH_TOP_N
extern uint32_t  g_graphStartedMs;      // millis() bei GRAPH-Start

// Welcher Screen hat den GRAPH-State gestartet (UI_LIST oder UI_DETAIL) —
// der Zurueck-Button im GRAPH kehrt dorthin zurueck, statt immer fix in die
// Liste zu wechseln. Wird in main.cpp::handleLongPress() gesetzt, in
// main.cpp::handleTouch() (GRAPH-Zweig) gelesen.
extern UiState   g_graphReturnState;

// -----------------------------------------------------------------------------
// Geteiltes TFT-Objekt (Definition in main.cpp)
// -----------------------------------------------------------------------------
extern TFT_eSPI  tft;

// -----------------------------------------------------------------------------
// Öffentliche API
// -----------------------------------------------------------------------------
// Zeichnet den aktuell aktiven Screen (LIST/DETAIL/GRAPH/CHANNELS) komplett
// neu. Wird von loop() bei Touch-Events und neuen Scan-Ergebnissen aufgerufen.
// Kein periodischer Aufruf im DETAIL-State (waere Flacker-Quelle, da
// drawDetailScreen() den ganzen Bildschirm neu aufbaut).
void renderScreen();
