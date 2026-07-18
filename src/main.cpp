// =============================================================================
// main.cpp — CYD WiFi-Meter
// =============================================================================
// Setup + Loop fuer den ESP32-2432S028R ("Cheap Yellow Display"). Rendering
// liegt komplett in src/ui/ui_renderer.cpp; main.cpp ist zustaendig fuer:
//   - Setup (Display + Touch init, Scanner-Task start, erstes Render)
//   - Loop (Touch-Polling, Long-Press-Erkennung, Render-Triggers)
//   - Input-Handling: handleTouch() (Footer-Nav-Buttons), pageUp/pageDown,
//     detailSwipeNetwork(), listMaxRows()
// =============================================================================
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <string.h>   // memcpy
#include <vector>

#include "config.h"
#include "wifi/wifi_data.h"
#include "wifi/wifi_scanner.h"
#include "ui/ui_renderer.h"

// Welcher Display-Controller-Codepath aktiv ist (siehe include/User_Setup.h,
// CYD_ST7789-Build-Flag) — genutzt für Boot-Log (Serial) und Boot-Screen, da
// sich die Variante von außen nicht zuverlässig am Board ablesen lässt.
#ifdef CYD_ST7789
#define CYD_VARIANT_NAME "ST7789"
#else
#define CYD_VARIANT_NAME "ILI9341"
#endif

// ---- Globale Objekte (Display + Touch) ------------------------------------
TFT_eSPI                tft = TFT_eSPI();
SPIClass                touchSPI(HSPI);
XPT2046_Touchscreen     ts(TOUCH_CS_PIN, TOUCH_IRQ_PIN);

// ---- Loop-State ------------------------------------------------------------
// State, der nur in main.cpp (loop + Input) gebraucht wird.
static uint32_t g_lastFallbackDraw = 0;   // fuer "vor Xs"-Anzeige (NUR LIST)
static uint32_t g_bootScreenStartMs = 0;  // Boot-Screen soll mindestens BOOT_SCREEN_MIN_MS stehen bleiben
static bool     g_firstRenderDone   = false;

// Touch-Tracking: dient der Long-Press-Erkennung und dem Tap-Debounce.
static bool             g_touchActive     = false;  // Finger liegt auf dem Panel
static int              g_touchStartX     = 0;      // X bei Touch-Down
static int              g_touchStartY     = 0;      // Y bei Touch-Down
static int              g_touchLastX      = 0;      // letzte X waehrend Touch
static int              g_touchLastY      = 0;      // letzte Y waehrend Touch
static uint32_t         g_touchStartMs    = 0;      // millis() bei Touch-Down
static uint32_t         g_touchLastSeenMs = 0;       // letzter echter Touch-Kontakt

// Long-Press-State. Wird im Touch-Hold-Block gesetzt, sobald der Finger
// LONG_PRESS_MS ohne nennenswerte Bewegung (LONG_PRESS_MAX_MOVE) auf gleicher
// Position verharrt. Beim Touch-Up wird der Tap-Handler dann uebersprungen,
// weil das Long-Press-Event bereits gefeuert hat.
static bool             g_longPressFired = false;

// ---- Scroll-Helfer (von den LIST-Footer-Pfeil-Buttons aufgerufen) ----------
// Liefert true, wenn der Offset sich tatsaechlich geaendert hat (render noetig).
static bool pageUp(size_t maxRows) {
    if (g_scrollOffset > 0) {
        g_scrollOffset -= (int)maxRows;
        if (g_scrollOffset < 0) g_scrollOffset = 0;
        Serial.printf("[nav] PGUP -> offset=%d\n", g_scrollOffset);
        return true;
    }
    return false;
}

static bool pageDown(size_t maxRows) {
    size_t total = wifiScannerNetworkCount();
    int maxOffset = (total > maxRows) ? (int)(total - maxRows) : 0;
    if (g_scrollOffset < maxOffset) {
        g_scrollOffset += (int)maxRows;
        if (g_scrollOffset > maxOffset) g_scrollOffset = maxOffset;
        Serial.printf("[nav] PGDN -> offset=%d\n", g_scrollOffset);
        return true;
    }
    return false;
}

// ---- maxRows-Helfer (Anzahl sichtbarer Zeilen) -----------------------------
static size_t listMaxRows() {
    return (size_t)((TFT_HEIGHT - LIST_TOP_OFFSET - LIST_BOTTOM_OFFSET) / ROW_HEIGHT);
}

// ---- Netzwechsel zum naechsten/vorherigen Netz in der DETAIL-Ansicht ------
// direction: -1 = vorheriges (staerkeres) Netz, +1 = naechstes (schwaecheres),
// in der aktuellen RSSI-Sortierung. Liefert true, wenn ein Wechsel
// stattgefunden hat (Render noetig).
//
// Der aktuelle Index wird bei JEDEM Aufruf frisch per BSSID-Suche im
// Snapshot ermittelt (nicht zwischengespeichert) — nach jedem Re-Scan kann
// sich die Sortierung aendern, ein gemerkter Index waere dann falsch.
// Am Rand der Liste wird einfach angehalten (kein Wrap-Around), das ist
// weniger verwirrend als ein Sprung vom letzten zum ersten Netz.
static bool detailSwipeNetwork(int direction) {
    if (!g_detailBssidValid) return false;

    std::vector<WifiNetworkInfo> snapshot;
    wifiScannerCopyNetworks(snapshot);

    int curIdx = -1;
    for (size_t i = 0; i < snapshot.size(); ++i) {
        if (memcmp(snapshot[i].bssid, g_detailBssid, 6) == 0) {
            curIdx = (int)i;
            break;
        }
    }
    if (curIdx < 0) return false;   // eigenes Netz nicht (mehr) in der Liste

    int newIdx = curIdx + direction;
    if (newIdx < 0 || newIdx >= (int)snapshot.size()) return false;   // am Rand

    memcpy(g_detailBssid, snapshot[newIdx].bssid, 6);
    Serial.printf("[touch] DETAIL-Swipe: idx %d -> %d (bssid=%02X:%02X:%02X:%02X:%02X:%02X)\n",
                  curIdx, newIdx,
                  snapshot[newIdx].bssid[0], snapshot[newIdx].bssid[1], snapshot[newIdx].bssid[2],
                  snapshot[newIdx].bssid[3], snapshot[newIdx].bssid[4], snapshot[newIdx].bssid[5]);
    return true;
}

// Gemeinsamer Rechteck-Hit-Test fuer alle Footer-Buttons. Die vertikale
// Pruefung nutzt grosszuegig die GESAMTE Footer-Hoehe (nicht nur die
// schmalere, sichtbar gezeichnete Button-Box) — groessere, leichter zu
// treffende Tap-Flaeche als das, was man sieht. Nur die horizontale Spalte
// (x..x+w) unterscheidet die drei Footer-Buttons.
static bool inFooterButton(int tx, int ty, int x, int w) {
    return (ty >= TFT_HEIGHT - FOOTER_HEIGHT) && (tx >= x && tx <= x + w);
}

// ---- Touch-Hit-Test --------------------------------------------------------
// Liefert true, wenn der Tap eine sichtbare Aenderung bewirkt hat
// (Selection, State-Wechsel, ...).
static bool handleTouch(int tx, int ty) {
    static uint32_t lastTouchMs = 0;
    uint32_t now = millis();
    if (now - lastTouchMs < 80) return false;   // debounce
    lastTouchMs = now;

    // --- CHANNELS-State: nur Zurueck-Button ---------------------------------
    // Die Kanalbelegung hat nur einen Einsprungpunkt (LIST-Footer), Zurueck
    // fuehrt deshalb immer fix zur Liste.
    if (g_uiState == UI_CHANNELS) {
        if (inFooterButton(tx, ty, BTN_BACK_X, BTN_BACK_W)) {
            Serial.println("[touch] CHANNELS -> BACK");
            g_uiState = UI_LIST;
            return true;
        }
        return false;
    }

    // --- GRAPH-State: nur Zurueck-Button ------------------------------------
    // Tap auf "Zurueck" fuehrt zum Einsprung-Screen zurueck
    // (g_graphReturnState: UI_LIST oder UI_DETAIL, je nachdem von wo der
    // Long-Press kam), statt immer fix in die Liste zu wechseln. GRAPH hat
    // keine Pfeil-Buttons (kein Vor-/Zurueck-Konzept).
    if (g_uiState == UI_GRAPH) {
        if (inFooterButton(tx, ty, BTN_BACK_X, BTN_BACK_W)) {
            Serial.printf("[touch] GRAPH -> BACK (zu %s)\n",
                          (g_graphReturnState == UI_DETAIL) ? "DETAIL" : "LIST");
            g_uiState = g_graphReturnState;
            return true;
        }
        return false;
    }

    // --- DETAIL-State: Pfeil links | Zurueck | Pfeil rechts -----------------
    // detailSwipeNetwork() ist an den Raendern bereits ein no-op (gibt false
    // zurueck) — kein zusaetzlicher Enabled-Check noetig.
    if (g_uiState == UI_DETAIL) {
        if (inFooterButton(tx, ty, BTN_BACK_X, BTN_BACK_W)) {
            Serial.printf("[touch] DETAIL -> BACK\n");
            g_uiState          = UI_LIST;
            g_detailBssidValid = false;
            return true;
        }
        if (inFooterButton(tx, ty, FOOTER_ARROW_LEFT_X, FOOTER_ARROW_BTN_W)) {
            Serial.println("[touch] DETAIL -> Pfeil links (vorheriges Netz)");
            return detailSwipeNetwork(-1);
        }
        if (inFooterButton(tx, ty, FOOTER_ARROW_RIGHT_X, FOOTER_ARROW_BTN_W)) {
            Serial.println("[touch] DETAIL -> Pfeil rechts (naechstes Netz)");
            return detailSwipeNetwork(+1);
        }
        // Tip irgendwo sonst in der Detailflaeche: kein State-Wechsel
        return false;
    }

    // --- LIST-State ---------------------------------------------------------
    // Thread-safe Snapshot der Liste, daraus lesen wir BSSID + Index.
    // Direkter Zugriff auf g_networks waere ein Race mit dem Scan-Task.
    std::vector<WifiNetworkInfo> snapshot;
    wifiScannerCopyNetworks(snapshot);
    size_t total = snapshot.size();

    // --- Listen-Bereich (Tap auf Zeile) -------------------------------------
    // Tap auf eine Zeile oeffnet sofort die Detailansicht. g_selectedRow wird
    // zusaetzlich gesetzt, damit die Zeile beim Zurueck-Sprung in die Liste
    // weiterhin blau markiert ist.
    if (ty >= LIST_TOP_OFFSET && ty < TFT_HEIGHT - LIST_BOTTOM_OFFSET) {
        int visibleRow = (ty - LIST_TOP_OFFSET) / ROW_HEIGHT;
        int netIdx     = (int)g_scrollOffset + visibleRow;
        if (netIdx >= 0 && netIdx < (int)total) {
            // BSSID aus dem Snapshot kopieren statt Index merken — der Index
            // wuerde sich nach jedem Re-Scan auf ein anderes Netz verschieben,
            // weil nach RSSI neu sortiert wird. BSSID ist stabil.
            const WifiNetworkInfo& n = snapshot[netIdx];
            memcpy(g_detailBssid, n.bssid, 6);
            g_detailBssidValid = true;
            g_selectedRow      = netIdx;
            g_uiState          = UI_DETAIL;
            Serial.printf("[touch] row=%d net=%d -> DETAIL (bssid=%02X:%02X:%02X:%02X:%02X:%02X)\n",
                          visibleRow, netIdx,
                          n.bssid[0], n.bssid[1], n.bssid[2],
                          n.bssid[3], n.bssid[4], n.bssid[5]);
            return true;
        }
    }

    // --- LIST-Footer: Pfeil links | Kanaele | Pfeil rechts ------------------
    if (inFooterButton(tx, ty, FOOTER_ARROW_LEFT_X, FOOTER_ARROW_BTN_W)) {
        Serial.println("[touch] LIST -> Pfeil links (vorherige Seite)");
        return pageUp(listMaxRows());
    }
    if (inFooterButton(tx, ty, BTN_BACK_X, BTN_BACK_W)) {
        Serial.println("[touch] LIST -> Kanaele");
        g_uiState = UI_CHANNELS;
        return true;
    }
    if (inFooterButton(tx, ty, FOOTER_ARROW_RIGHT_X, FOOTER_ARROW_BTN_W)) {
        Serial.println("[touch] LIST -> Pfeil rechts (naechste Seite)");
        return pageDown(listMaxRows());
    }

    // --- Header-Bereich -----------------------------------------------------
    if (ty < LIST_TOP_OFFSET) {
        Serial.printf("[touch] header tap (ignored), tx=%d ty=%d\n", tx, ty);
    }
    return false;
}

// ---- Long-Press-Handler -----------------------------------------------------
// Wird aus loop() aufgerufen, sobald der Finger >= LONG_PRESS_MS auf gleicher
// Position (innerhalb LONG_PRESS_MAX_MOVE) verharrt. Oeffnet den RSSI-Live-
// Graph fuer das Netz unter dem Finger. In UI_GRAPH selbst ist Long-Press
// no-op (kein Sinn, den Graph aus dem Graph heraus neu zu oeffnen).
//
// Das long-gepresste Netz ist immer die erste Linie (g_graphBssids[0]), die
// uebrigen WIFI_GRAPH_TOP_N-1 Plaetze werden mit den global staerksten
// Netzen gefuellt (Dedupe, falls das eigene Netz dort ohnehin vorkommt).
//
// Funktioniert sowohl aus UI_LIST (BSSID per Zeilen-Lookup im Snapshot) als
// auch aus UI_DETAIL (BSSID direkt aus g_detailBssid, kein Lookup noetig) —
// beide Zweige ermitteln nur `ownBssid` unterschiedlich, die Dedupe+Auffuell-
// Logik danach ist fuer beide Einsprungpunkte identisch.
static void handleLongPress(int tx, int ty) {
    Serial.printf("[touch] LONG-PRESS detected at (%d, %d), state=%d\n",
                  tx, ty, (int)g_uiState);

    uint8_t ownBssid[6];

    if (g_uiState == UI_LIST) {
        // Nur Long-Press innerhalb der Listen-Area oeffnet den Graph.
        if (ty < LIST_TOP_OFFSET || ty >= TFT_HEIGHT - LIST_BOTTOM_OFFSET) return;

        // Thread-safe Snapshot, da g_networks nur ueber den Scan-Task
        // (Core 0) veraendert wird.
        std::vector<WifiNetworkInfo> snapshot;
        wifiScannerCopyNetworks(snapshot);
        int visibleRow = (ty - LIST_TOP_OFFSET) / ROW_HEIGHT;
        int netIdx     = (int)g_scrollOffset + visibleRow;
        if (netIdx < 0 || netIdx >= (int)snapshot.size()) {
            Serial.println("[graph] Long-Press auf ungueltige Zeile, ignoriert");
            return;
        }
        memcpy(ownBssid, snapshot[netIdx].bssid, 6);
    } else if (g_uiState == UI_DETAIL) {
        // Long-Press auf dem Zurueck-Button ignorieren (sonst koennte ein
        // etwas zu lang gehaltener Tap auf "Zurueck" versehentlich den
        // Graph oeffnen statt zur Liste zurueckzukehren).
        if (!g_detailBssidValid) return;
        if (ty >= BTN_BACK_Y && ty <= BTN_BACK_Y + BTN_BACK_H &&
            tx >= BTN_BACK_X && tx <= BTN_BACK_X + BTN_BACK_W) return;
        memcpy(ownBssid, g_detailBssid, 6);
    } else {
        return;   // UI_GRAPH/UI_CHANNELS: Long-Press no-op
    }

    // Globale Top-N als Kandidaten fuer die Vergleichs-Linien holen, dann
    // das eigene Netz herausfiltern (Dedupe) und den Rest auffuellen.
    uint8_t candidates[WIFI_GRAPH_TOP_N][6];
    size_t  candidateCount = wifiScannerCopyTopBssids(candidates, WIFI_GRAPH_TOP_N);

    g_graphBssidCount = 0;
    memcpy(g_graphBssids[g_graphBssidCount++], ownBssid, 6);
    for (size_t i = 0; i < candidateCount && g_graphBssidCount < WIFI_GRAPH_TOP_N; ++i) {
        if (memcmp(candidates[i], ownBssid, 6) == 0) continue;   // eigenes Netz nicht doppelt
        memcpy(g_graphBssids[g_graphBssidCount++], candidates[i], 6);
    }

    const char* entrySource = (g_uiState == UI_DETAIL) ? "DETAIL" : "LIST";
    g_graphReturnState = g_uiState;   // Zurueck-Button fuehrt hierher zurueck
    g_graphStartedMs   = millis();
    g_uiState          = UI_GRAPH;
    Serial.printf("[graph] GRAPH-State (Einsprung via %s): eigenes Netz + %u Vergleichsnetze\n",
                  entrySource, (unsigned)(g_graphBssidCount - 1));
    renderScreen();
}

// ---- Setup -----------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[setup] CYD-WIFI-Meter " FW_VERSION " starten...");
    Serial.println("[setup] Display-Variante: " CYD_VARIANT_NAME);

    // Display init
    tft.init();
    tft.setRotation(DISPLAY_ROTATION);
    // Default-Textwrap aus: TFT_eSPI bricht zu breiten Text sonst an
    // SPALTE 0 um (nicht an der Box-Kante!). Mit wrap=false wird stattdessen
    // geclippt, was zusammen mit den gemessenen Truncation-Helfern
    // (truncateToWidth() in ui_renderer.cpp) ein vorhersagbares Layout ergibt.
    tft.setTextWrap(false);
    tft.fillScreen(COLOR_BG);
    tft.setTextColor(COLOR_FG, COLOR_BG);
    tft.setTextFont(2);   // 16px, bereits fuer Header/Liste geladen — besser lesbar als Font1
    tft.setTextSize(1);
    int bootLineH = tft.fontHeight() + 6;   // gemessen statt geschaetzt (siehe Lessons Learned)
    int bootY = 10;
    tft.setCursor(10, bootY);
    tft.print("Booting...");
    bootY += bootLineH;
    tft.setCursor(10, bootY);
    tft.print("Display: " CYD_VARIANT_NAME);
    bootY += bootLineH;
    tft.setCursor(10, bootY);
    tft.print(FW_VERSION);
    g_bootScreenStartMs = millis();

    // Touch init (separater HSPI-Bus)
    touchSPI.begin(TOUCH_SCK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
    ts.begin(touchSPI);
    ts.setRotation(DISPLAY_ROTATION);

    // Scanner start
    wifiScannerStart();

    // Kein sofortiges Render hier: der Boot-Screen (oben) bleibt stehen, bis
    // loop() den ersten echten Scan-Ergebnis-Render auslöst (wifiScannerHasNewResult())
    // UND mindestens BOOT_SCREEN_MIN_MS vergangen sind (siehe loop()) — sonst waere
    // der Boot-Screen bei einem schnellen ersten Scan kaum lesbar.
    Serial.println("[setup] ready");
}

// ---- Loop ------------------------------------------------------------------
void loop() {
    // ---- Touch (non-blocking) ----------------------------------------------
    if (ts.touched()) {
        TS_Point p = ts.getPoint();
        int tx = map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, TFT_WIDTH);
        int ty = map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, TFT_HEIGHT);

        g_touchLastSeenMs = millis();   // realer Touch-Kontakt fuer Release-Debounce

        if (!g_touchActive) {
            // ---- Touch-Down: Start erfassen ---------------------------------
            g_touchActive  = true;
            g_touchStartX  = tx;
            g_touchStartY  = ty;
            g_touchLastX   = tx;
            g_touchLastY   = ty;
            g_touchStartMs = millis();
        } else {
            // ---- Finger hält: nur letzte Position mitfuehren ----------------
            g_touchLastX = tx;
            g_touchLastY = ty;

            // ---- Long-Press-Erkennung --------------------------------------
            // Wenn der Finger >= LONG_PRESS_MS auf gleicher Position (innerhalb
            // LONG_PRESS_MAX_MOVE) verharrt, feuert ein Long-Press-Event.
            if (!g_longPressFired) {
                const uint32_t heldMs = millis() - g_touchStartMs;
                const int moveX = g_touchLastX - g_touchStartX;
                const int moveY = g_touchLastY - g_touchStartY;
                const int totalMove = abs(moveX) + abs(moveY);
                if (heldMs >= LONG_PRESS_MS && totalMove <= LONG_PRESS_MAX_MOVE) {
                    g_longPressFired = true;
                    handleLongPress(g_touchStartX, g_touchStartY);
                    // Touch-Tracking bleibt aktiv bis Touch-Up (fuer Cleanup),
                    // aber der Tap wird dann ignoriert (siehe g_longPressFired).
                }
            }
        }
    } else if (g_touchActive && (millis() - g_touchLastSeenMs >= RELEASE_DEBOUNCE_MS)) {
        // ---- Touch-Up: Tap auswerten ----------------------------------------
        // ts.touched()==false zaehlt erst als echtes Loslassen, wenn es seit
        // RELEASE_DEBOUNCE_MS ohne Unterbrechung anhaelt — ueberbrueckt kurze
        // Touch-Aussetzer waehrend eines laengeren Haltens (resistiver Touch).
        g_touchActive = false;

        if (g_longPressFired) {
            g_longPressFired = false;   // Long-Press bereits gefeuert -> Tap ueberspringen
        } else {
            if (handleTouch(g_touchStartX, g_touchStartY)) renderScreen();
        }
    }

    // ---- Redraw bei neuen Scan-Ergebnissen ---------------------------------
    bool hasNewResult = wifiScannerHasNewResult();   // konsumiert das Flag — nur einmal pro Loop abfragen

    if (!g_firstRenderDone) {
        // Boot-Screen (main.cpp::setup()) bleibt mindestens BOOT_SCREEN_MIN_MS
        // UND bis der erste Scan durch ist stehen. wifiScannerLastScanMs() ist
        // (anders als wifiScannerHasNewResult()) ein reiner Getter ohne
        // Konsum-Semantik — verhindert, dass ein zu frueh eintreffendes erstes
        // Scan-Ergebnis "verloren geht", waehrend die Mindestzeit noch laeuft.
        if (wifiScannerLastScanMs() > 0 && millis() - g_bootScreenStartMs >= BOOT_SCREEN_MIN_MS) {
            g_scrollOffset = 0;
            renderScreen();
            g_lastFallbackDraw = millis();
            g_firstRenderDone  = true;
        }
        // Kein periodisches Fallback-Redraw vor dem ersten echten Render —
        // sonst wuerde der 1s-Timer den Boot-Screen mit einer noch leeren
        // Liste ueberschreiben, bevor ueberhaupt ein Scan fertig ist.
    } else if (hasNewResult) {
        if (g_uiState == UI_LIST) {
            // Im LIST-State: oben bleiben (staerkste Netze sichtbar)
            g_scrollOffset = 0;
            renderScreen();
        } else if (g_uiState == UI_GRAPH) {
            renderScreen();
        } else if (g_uiState == UI_CHANNELS) {
            renderScreen();
        }
        // Im DETAIL-State: NICHT neu rendern, sonst wuerden Werte "springen"
        // (RSSI/letzte Sichtung wuerden staendig ueberschrieben). Das
        // angezeigte Netz wird beim naechsten State-Wechsel (Tap/Back)
        // ohnehin wieder aus den Live-Daten per BSSID geholt.
    } else {
        // Keine neuen Scan-Ergebnisse: periodischer 1s-Render fuer LIST/GRAPH/
        // CHANNELS ("vor Xs"-Anzeige bzw. scrollende Linien/Balken). DETAIL hat
        // keinen periodischen Render (waere Flacker-Quelle, da drawDetailScreen()
        // den ganzen Bildschirm neu aufbaut).
        if (g_uiState == UI_LIST || g_uiState == UI_GRAPH || g_uiState == UI_CHANNELS) {
            if (millis() - g_lastFallbackDraw > 1000) {
                renderScreen();
                g_lastFallbackDraw = millis();
            }
        }
    }

    delay(TOUCH_POLL_MS);
}
