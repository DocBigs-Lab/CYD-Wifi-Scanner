// =============================================================================
// config.h — Zentrale Konfiguration für CYD-WiFi-Meter
// =============================================================================
// Diese Datei ist die Single Source of Truth für:
//   - Versionsinfo
//   - Display-Farben (RGB565)
//   - Layout-Konstanten (Header, Listenzeilen, Padding, Detail-View)
//   - Touch-Pins und Kalibrierung
//   - Scan-Intervalle
//
// Änderungen an Pins/Farbe/Textgröße gehören HIER hin, nicht in main.cpp.
// =============================================================================
#pragma once
#include <Arduino.h>

// -----------------------------------------------------------------------------
// 1. Versionsinfo
// -----------------------------------------------------------------------------
#define FW_NAME        "CYD WiFi-Meter"
#define FW_VERSION     "0.5.23-M5.23"
#define FW_BUILD_DATE  __DATE__

// -----------------------------------------------------------------------------
// 2. Display-Farben (RGB565, 16-bit)
// -----------------------------------------------------------------------------
// CYD_COLOR(): manche ST7789-Panel-Chargen (bestätigt per Geräte-Test,
// 2026-07-18) ignorieren das Hardware-Inversions-Kommando (TFT_INVERSION_ON/
// OFF in include/User_Setup.h) vollständig und zeigen JEDEN Pixel fest
// bit-invertiert an (0xFFFF XOR) — unabhängig von Treiber/Inversions-
// Einstellung, an drei unabhängigen Farbpaaren nachgewiesen (Header, RSSI-
// Ampel grün/amber). Da sich das nicht auf Panel-Ebene beheben lässt, wird
// hier auf Werte-Ebene vorab gegen-invertiert, sodass die vom Panel selbst
// nochmals invertierte Ausgabe wieder korrekt ankommt. Betrifft nur
// CYD_ST7789-Builds — die ILI9341-Variante ist davon unberührt.
#ifdef CYD_ST7789
#define CYD_COLOR(x) (0xFFFFu ^ (x))
#else
#define CYD_COLOR(x) (x)
#endif

#define COLOR_BG              CYD_COLOR(0x0000u)   // Schwarz (Hintergrund)
#define COLOR_FG              CYD_COLOR(0xFFFFu)   // Weiß  (Standardtext)
#define COLOR_ACCENT_GREEN    CYD_COLOR(0x07E0u)   // Grün  (Ampel grün)
#define COLOR_ACCENT_AMBER    CYD_COLOR(0xFD20u)   // Orange (Ampel gelb)
#define COLOR_ACCENT_RED      CYD_COLOR(0xF800u)   // Rot   (Ampel rot)
#define COLOR_TITLE_BG        CYD_COLOR(0x001Fu)   // Dunkelblau (Header)
#define COLOR_TITLE_FG        CYD_COLOR(0xFFFFu)   // Weiß (Headertext)
#define COLOR_LIST_DIVIDER    CYD_COLOR(0x4208u)   // Dunkelgrau (dezente Trennlinien)
#define COLOR_LIST_BG_SEL     CYD_COLOR(0x2945u)   // Mittelblau (selektierte Zeile)
#define COLOR_BTN_BG          CYD_COLOR(0x2945u)   // Mittelblau (Footer-Button-Hintergrund)
#define COLOR_BTN_FG          CYD_COLOR(0xFFFFu)   // Weiß (Footer-Button-Text)
#define COLOR_BTN_DISABLED    CYD_COLOR(0x4208u)   // Grau (Footer-Button inaktiv)
#define COLOR_DETAIL_LABEL    CYD_COLOR(0xC618u)   // Hellgrau (Detail-Feld-Label)
#define COLOR_DETAIL_VALUE    CYD_COLOR(0xFFFFu)   // Weiß     (Detail-Feld-Value)
#define COLOR_DETAIL_BOX_BG   CYD_COLOR(0x0841u)   // Sehr dunkles Blau (Detail-Wert-Box)

// RSSI-Live-Graph — distinkte Linien-Farben. Index 0 (das vom User per
// Long-Press gewählte Netz) bekommt eine feste, von den Rang-Farben klar
// unterscheidbare Hervorhebung (Cyan); die Vergleichsnetze (Index 1..N-1)
// behalten die Rang-Palette.
#define COLOR_GRAPH_LINE_OWN  CYD_COLOR(0x07FFu)   // Cyan    (gewähltes/eigenes Netz, immer Index 0)
#define COLOR_GRAPH_LINE_1    CYD_COLOR(0x07E0u)   // Grün    (stärkstes Vergleichsnetz)
#define COLOR_GRAPH_LINE_2    CYD_COLOR(0xFD20u)   // Orange  (2.-stärkstes Vergleichsnetz)
#define COLOR_GRAPH_LINE_3    CYD_COLOR(0xF800u)   // Rot     (3.-stärkstes Vergleichsnetz)
#define COLOR_GRAPH_LINE_4    CYD_COLOR(0x001Fu)   // Blau    (Reserve, falls WIFI_GRAPH_TOP_N erhöht wird)
#define COLOR_GRAPH_LINE_5    CYD_COLOR(0xF81Fu)   // Magenta (Reserve, falls WIFI_GRAPH_TOP_N erhöht wird)
#define COLOR_GRAPH_GRID      CYD_COLOR(0x4208u)   // Dunkelgrau (Gitternetz)
#define COLOR_GRAPH_AXIS      CYD_COLOR(0xC618u)   // Hellgrau  (Achsen-Labels)

// -----------------------------------------------------------------------------
// 3. Layout-Konstanten (Pixel, Hochformat, Rotation 0)
// -----------------------------------------------------------------------------
#define TITLE_HEIGHT          24
#define FOOTER_HEIGHT         36
#define ROW_HEIGHT            40
#define PADDING                6
#define LIST_TOP_OFFSET       (TITLE_HEIGHT + 2)
#define LIST_BOTTOM_OFFSET    FOOTER_HEIGHT

// Detail-View — Layout. Die Zeilenhöhe selbst wird in drawDetailScreen()
// zur Laufzeit aus dem tatsächlich verfügbaren Platz berechnet (Footer-Top
// minus erste Zeile, durch 7 Felder geteilt) statt fest vorgegeben — siehe
// ui_renderer.cpp. DETAIL_ROW_GAP ist der visuelle Abstand zwischen den
// Value-Boxen (wird vom berechneten Zeilen-Step abgezogen).
#define DETAIL_TOP_OFFSET     (TITLE_HEIGHT + 4)   // Trennlinie unter Header
#define DETAIL_FIRST_ROW_GAP   10                  // Abstand Trennlinie -> erste Wertezeile
#define DETAIL_ROW_GAP          4                  // visueller Abstand zwischen Value-Boxen
#define DETAIL_LABEL_X        (PADDING)             // Label-Spalte
#define DETAIL_LABEL_W        78                    // Label-Spalte Breite
#define DETAIL_VALUE_X        (DETAIL_LABEL_X + DETAIL_LABEL_W + 4)

// DETAIL-Value-Schrift: Font2 (16px Bitmap) zeigt BSSID ("AA:BB:CC:DD:EE:FF",
// ~111px) und Auth-Text komplett in der 146px-Value-Box. Eine GFX-FreeFont
// (antialiased, etwas größer/fetter) ist als Alternative vorhanden, aber für
// die volle BSSID strukturell zu breit (~235px) — bleibt per Define
// reaktivierbar, falls die Value-Box künftig verbreitert wird (z.B.
// schmalere Label-Spalte). Voraussetzung dafür: include/User_Setup.h
// definiert SMOOTH_FONT + LOAD_GFXFF (ist bereits der Fall).
#define DETAIL_VALUE_USE_GFX       0     // 0 = Bitmap-Font (Font2), 1 = GFX-Font (FreeSansBold12pt7b)
#define DETAIL_VALUE_FONT          2

// Zurück-Button — zentriert im Footer von DETAIL/GRAPH/CHANNELS, und als
// "Kanäle"-Button in der LIST-Footer-Mitte wiederverwendet.
#define BTN_BACK_W           100
#define BTN_BACK_H            28
#define BTN_BACK_X            ((TFT_WIDTH - BTN_BACK_W) / 2)
#define BTN_BACK_Y            (TFT_HEIGHT - FOOTER_HEIGHT + (FOOTER_HEIGHT - BTN_BACK_H) / 2)
#define BTN_BACK_TEXT        "Zurueck"

// Footer-Pfeil-Buttons (Seiten-/Netzwechsel) — links/rechts außen, gleiche
// Höhe/Y-Position wie der Zurück-Button für eine einheitliche Button-Reihe.
#define FOOTER_ARROW_BTN_W    56
#define FOOTER_ARROW_BTN_H    BTN_BACK_H
#define FOOTER_ARROW_LEFT_X   PADDING
#define FOOTER_ARROW_RIGHT_X  (TFT_WIDTH - PADDING - FOOTER_ARROW_BTN_W)
#define FOOTER_ARROW_Y        BTN_BACK_Y

// Polling-Intervall der Touch-Hauptschleife (Long-Press-Erkennung + Tap-Debounce).
#define TOUCH_POLL_MS         15

// Der resistive XPT2046-Touch liefert vereinzelt kurze "nicht berührt"-
// Aussetzer (Druck sinkt für 1-2 Polls unter die Erkennungsschwelle), z.B.
// während eines längeren Long-Press-Haltens. RELEASE_DEBOUNCE_MS
// überbrückt solche kurzen Aussetzer: ein "nicht berührt"-Status zählt
// erst nach Ablauf dieser Frist seit dem letzten echten Touch-Kontakt als
// tatsächliches Loslassen.
#define RELEASE_DEBOUNCE_MS   60

// Long-Press — Finger muss >= LONG_PRESS_MS auf gleicher Position
// (innerhalb LONG_PRESS_MAX_MOVE) gehalten werden, um den RSSI-Live-Graph
// zu öffnen (aus LIST und DETAIL).
#define LONG_PRESS_MS        600
#define LONG_PRESS_MAX_MOVE    8   // px Toleranz während Long-Press-Haltephase

// -----------------------------------------------------------------------------
// 4. Touch-Pins (XPT2046)
// -----------------------------------------------------------------------------
#define TOUCH_CS_PIN          33
#define TOUCH_IRQ_PIN         36   // Input-only GPIO, aber OK als IRQ
#define TOUCH_SCK_PIN         25   // separater SPI-Bus (HSPI)
#define TOUCH_MISO_PIN        39   // Input-only GPIO
#define TOUCH_MOSI_PIN        32

// -----------------------------------------------------------------------------
// 5. Touch-Kalibrierung
// -----------------------------------------------------------------------------
// Rohwerte vom ADC → Bildschirmkoordinaten. Diese 4 Werte MÜSSEN pro Gerät
// einmal ermittelt werden (Test-Touch auf Serial loggen, dann anpassen).
#define TOUCH_MIN_X           200
#define TOUCH_MAX_X          3800
#define TOUCH_MIN_Y           200
#define TOUCH_MAX_Y          3800

// Display-Rotation: 0=Portrait, 1=Landscape, 2=Portrait-180, 3=Landscape-180
#define DISPLAY_ROTATION       0

// -----------------------------------------------------------------------------
// 6. WiFi-Scan
// -----------------------------------------------------------------------------
#define WIFI_SCAN_INTERVAL_MS     4000   // 4 s Rescan
#define WIFI_SCAN_MIN_RSSI       -100    // Netze schwächer ignorieren
#define WIFI_SCAN_MAX_RESULTS      64    // ESP32 liefert bis zu 64 Netze

// Mindestanzeigedauer des Boot-Screens (main.cpp::setup()) — garantiert lesbar,
// unabhängig davon wie schnell der erste WiFi-Scan durchläuft.
#define BOOT_SCREEN_MIN_MS        4000

// RSSI-History (Ring-Buffer pro BSSID) für den Live-Graph. Jedes Sample ist
// ein RSSI-Wert zu einem millis()-Zeitpunkt; bei 1 Hz Update (passend zu
// WIFI_SCAN_INTERVAL_MS) ergeben 60 Samples ein 60-Sekunden-Fenster.
// WIFI_HISTORY_MAX_ENTRIES begrenzt, wie viele verschiedene Netze gleichzeitig
// eine History bekommen — Eviction erfolgt per LRU (ältester Sample-
// Zeitstempel), siehe wifi_scanner.cpp.
#define WIFI_HISTORY_SIZE          60
#define WIFI_HISTORY_MAX_ENTRIES   24

// Gesamtzahl der Linien im Graph. Index 0 ist immer das per Long-Press
// gewählte (eigene) Netz, Index 1..N-1 sind die stärksten anderen Netze
// zum Vergleich.
#define WIFI_GRAPH_TOP_N            3

// Graph-Y-Achsen-Bereich (RSSI in dBm). Feste Skala — kein Auto-Zoom (sonst
// würden Linien bei kurzen Schwankungen ständig springen).
#define GRAPH_Y_MIN_DBM           -100
#define GRAPH_Y_MAX_DBM            -30

// Zeitfenster in Sekunden. Bei 1 Hz Update = GRAPH_WINDOW_S Samples
// sichtbar. Bei abweichendem Scan-Intervall muss GRAPH_WINDOW_S * 1000 /
// WIFI_SCAN_INTERVAL_MS <= WIFI_HISTORY_SIZE bleiben.
#define GRAPH_WINDOW_S             60

// Graph-Layout (Pixel). Y-Achsen-Labels links, X-Achsen-Labels unten.
// GRAPH_PLOT_H wird aus FOOTER_HEIGHT abgeleitet statt hartcodiert, damit
// die Legende immer exakt am Footer-Top endet — unabhängig von
// FOOTER_HEIGHT-Änderungen. Reihenfolge wichtig: GRAPH_AXIS_LABEL_GAP/
// GRAPH_LEGEND_ROW_H/GRAPH_LEGEND_PAD/GRAPH_LEGEND_H müssen vor
// GRAPH_PLOT_H definiert sein (hängen nicht von der Plot-Höhe ab).
#define GRAPH_PLOT_X               30   // Plot-Bereich links
#define GRAPH_PLOT_Y               50   // Plot-Bereich oben (direkt unter Header + Trennlinie)
#define GRAPH_PLOT_W              200   // Plot-Breite (240 - 30 - 10 rechter Rand)
#define GRAPH_AXIS_LABEL_GAP       14   // Platz für X-Achsen-Sekunden-Labels unter dem Plot
#define GRAPH_LEGEND_ROW_H         14   // Zeilenhöhe je Legenden-Eintrag
#define GRAPH_LEGEND_PAD            8   // Innenabstand oben/unten in der Legende
#define GRAPH_LEGEND_H            (WIFI_GRAPH_TOP_N * GRAPH_LEGEND_ROW_H + GRAPH_LEGEND_PAD)
#define GRAPH_PLOT_H              ((TFT_HEIGHT - FOOTER_HEIGHT) - GRAPH_PLOT_Y \
                                    - GRAPH_AXIS_LABEL_GAP - GRAPH_LEGEND_H)
#define GRAPH_LEGEND_Y            (GRAPH_PLOT_Y + GRAPH_PLOT_H + GRAPH_AXIS_LABEL_GAP)

// Kanalbelegung (Balkendiagramm, Netze pro 2.4GHz-Kanal). Nur diese eine
// Konstante nötig — das restliche Layout wird in drawChannelScreen() zur
// Laufzeit aus TITLE_HEIGHT/FOOTER_HEIGHT/PADDING berechnet.
#define CHANNEL_COUNT               13   // Kanäle 1..13 (Standard-2.4GHz außerhalb Japans)
