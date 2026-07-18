// =============================================================================
// ui_renderer.cpp — Rendering der LIST-, DETAIL-, GRAPH- und CHANNELS-Views
// =============================================================================
// Enthält:
//   - geteilter UI-State (g_uiState, g_scrollOffset, g_selectedRow,
//                          g_detailBssid[6], g_detailBssidValid, ...)
//   - Hilfsfunktionen: RSSI-/Auth-Ampelfarben, RSSI-Bars, BSSID-/Alter-
//                       Formatierung, textbreitenbasiertes Kürzen
//   - Header/Footer/Row-Draws für alle vier Screens
//   - Komplett-Render je Screen + renderScreen() als öffentlicher Dispatch
//
// Layout-Grundprinzip: nichts wird geschätzt, alles wird zur Laufzeit
// gemessen (tft.textWidth()/tft.fontHeight()) und auf den tatsächlich
// verfügbaren Platz gerechnet, statt feste Pixel-Konstanten zu pflegen.
// Das macht das Layout robust gegen Font-/Auflösungsänderungen und war
// nötig, weil TFT_eSPI keine API bietet, um Textgrößen vorab "von außen"
// zu kennen, ohne sie tatsächlich zu messen.
//
// Aufgerufen wird ausschließlich renderScreen() aus main.cpp.
// =============================================================================
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <vector>
#include <string.h>   // memcmp

#include "config.h"   // muss vor #if DETAIL_VALUE_USE_GFX stehen

// Optionale GFX-FreeFont-Variante für DETAIL-Values (siehe
// DETAIL_VALUE_USE_GFX in config.h). Der Font-Header wird über
// TFT_eSPI.h -> gfxfont.h (bei aktivem LOAD_GFXFF) automatisch eingebunden;
// ein manueller Include würde zu "redefinition"-Fehlern führen.
#include "wifi/wifi_data.h"
#include "wifi/wifi_scanner.h"
#include "ui/ui_renderer.h"

// -----------------------------------------------------------------------------
// Geteilter UI-State (in ui_renderer.h als extern deklariert)
// -----------------------------------------------------------------------------
UiState   g_uiState          = UI_LIST;
int       g_scrollOffset     = 0;
int       g_selectedRow      = -1;      // -1 = keine Auswahl

uint8_t   g_detailBssid[6]   = {0};
bool      g_detailBssidValid = false;

// GRAPH-State: Top-N BSSIDs werden beim Einstieg in UI_GRAPH einmal aus dem
// aktuellen Scan-Snapshot befüllt und danach stabil gehalten (kein
// Nachjustieren während der Anzeige, damit Linienfarben nicht "springen").
// g_graphStartedMs ist die Referenz für die X-Achse.
uint8_t   g_graphBssids[WIFI_GRAPH_TOP_N][6] = {{0}};
uint8_t   g_graphBssidCount = 0;
uint32_t  g_graphStartedMs  = 0;
UiState   g_graphReturnState = UI_LIST;   // wohin der GRAPH-Zurück-Button führt

// -----------------------------------------------------------------------------
// Ampelfarben: RSSI (Signalstärke) und Auth (Sicherheit)
// -----------------------------------------------------------------------------
static uint16_t rssiColor(int32_t rssi) {
    if (rssi >= -55) return COLOR_ACCENT_GREEN;   // ausgezeichnet
    if (rssi >= -70) return COLOR_ACCENT_AMBER;   // okay
    return COLOR_ACCENT_RED;                       // schwach
}

// 3-stufige Sicherheitsampel für den Auth-Typ: offen/WEP = rot (WEP ist seit
// Jahren in Minuten knackbar, "verschlüsselt" wäre hier eine falsche
// Sicherheit), WPA3-PSK = grün, alles dazwischen = amber. OWE
// ("WPA3-Open") verschlüsselt zwar den Traffic, authentifiziert aber nicht
// (kein Schutz vor Rogue-APs/MITM) und liegt damit naeher am WPA/WPA2-
// Sicherheitsniveau; der WPA2/WPA3-Transition-Mode bleibt amber, weil die
// Downgrade-Angriffsfläche bestehen bleibt, solange WPA2-Clients zugelassen
// werden. WAUTH_UNKNOWN bleibt neutral grau statt einer erzwungenen Farbe.
static uint16_t colorForAuth(WifiAuth a) {
    switch (a) {
        case WAUTH_OPEN:
        case WAUTH_WEP:
            return COLOR_ACCENT_RED;
        case WAUTH_WPA3_PSK:
            return COLOR_ACCENT_GREEN;
        case WAUTH_UNKNOWN:
            return COLOR_DETAIL_LABEL;
        default:   // WPA/WPA2/WPA2-Enterprise/Transition-Mode/WAPI/OWE
            return COLOR_ACCENT_AMBER;
    }
}

static int rssiBars(int32_t rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

// 4 Balken (6, 10, 14, 18 px hoch)
static void drawRssiBars(int32_t rssi, int x, int y) {
    int bars = rssiBars(rssi);
    uint16_t color = rssiColor(rssi);
    constexpr int heights[4] = {6, 10, 14, 18};
    constexpr int barW = 5;
    constexpr int gap = 2;
    constexpr int maxH = 18;
    for (int i = 0; i < 4; ++i) {
        int h = heights[i];
        uint16_t c = (i < bars) ? color : COLOR_LIST_DIVIDER;   // grau für inaktive Balken
        tft.fillRect(x + i * (barW + gap), y + (maxH - h), barW, h, c);
    }
}

#if DETAIL_VALUE_USE_GFX
// Liefert Ascent (Höhe über Baseline) und Descent (Höhe unter Baseline)
// eines GFX-FreeFonts, gemessen über alle Glyphen — TFT_eSPI hält diese
// Werte intern in protected Membern, daher hier eine eigene Berechnung.
// Wird einmalig pro Font berechnet und zwischengespeichert.
//
// Wichtig: tft.setCursor(x, y) + print() interpretiert y bei GFX-FreeFonts
// als BASELINE-Position, nicht als Box-Oberkante wie bei den klassischen
// Bitmap-Fonts. Ascent/Descent ist die tatsächliche Glyphenhöhe; die
// Baseline muss auf Box-Top + Top-Padding + Ascent gesetzt werden, damit
// der Text vertikal zentriert UND komplett innerhalb der Box liegt.
static void gfxFontAscentDescent(const GFXfont* f, int& ascent, int& descent) {
    static const GFXfont* cachedFont = nullptr;
    static int             cachedAscent = 0;
    static int             cachedDescent = 0;
    if (f != cachedFont) {
        int ab = 0, bb = 0;
        uint16_t first = pgm_read_word(&f->first);
        uint16_t last  = pgm_read_word(&f->last);
        GFXglyph* glyphs = (GFXglyph*)pgm_read_dword(&f->glyph);
        for (uint16_t c = 0; c < (uint16_t)(last - first); c++) {
            int a = -(int8_t)pgm_read_byte(&glyphs[c].yOffset);
            if (a > ab) ab = a;
            int b = (int)pgm_read_byte(&glyphs[c].height) - a;
            if (b > bb) bb = b;
        }
        cachedFont    = f;
        cachedAscent  = ab;
        cachedDescent = bb;
    }
    ascent  = cachedAscent;
    descent = cachedDescent;
}
#endif

// -----------------------------------------------------------------------------
// BSSID formatieren (6 Bytes -> "AA:BB:CC:DD:EE:FF")
// -----------------------------------------------------------------------------
static void formatBssid(char* out, const uint8_t* bssid) {
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

// -----------------------------------------------------------------------------
// "vor X s"-String aus millis()-Differenz
// -----------------------------------------------------------------------------
static void formatAge(char* out, size_t n, uint32_t lastSeenMs) {
    uint32_t ageMs = millis() - lastSeenMs;
    if (ageMs < 1000)        snprintf(out, n, "eben");
    else if (ageMs < 60000)  snprintf(out, n, "%lus", (unsigned long)(ageMs / 1000));
    else                     snprintf(out, n, "%lumin", (unsigned long)(ageMs / 60000));
}

// -----------------------------------------------------------------------------
// Kürzt `text` (im aktuell gesetzten Font) auf maximal `maxW` Pixel Breite,
// gemessen per tft.textWidth() statt einer festen Px/Zeichen-Schätzung.
// Hängt bei Bedarf ".." an — ASCII statt UTF-8-Ellipsis "…", weil dieses
// Glyph weder in den TFT_eSPI-Bitmap-Fonts noch in FreeSansBold12pt7b
// enthalten ist (würde sonst unsichtbar verschluckt).
// -----------------------------------------------------------------------------
static String truncateToWidth(const String& text, int maxW) {
    if (tft.textWidth(text) <= maxW) return text;
    String s = text;
    while (s.length() > 1) {
        s.remove(s.length() - 1);
        String candidate = s + "..";
        if (tft.textWidth(candidate) <= maxW) return candidate;
    }
    return s;
}

// -----------------------------------------------------------------------------
// Header: links Titel/SSID, rechts optionaler Zweittext. Beide werden mit
// echter Breitenmessung positioniert/gekürzt, damit längere Texte nie über
// den Bildschirmrand laufen oder sich überlappen.
// -----------------------------------------------------------------------------
static void drawHeader(const char* leftText, const char* rightText) {
    tft.fillRect(0, 0, TFT_WIDTH, TITLE_HEIGHT, COLOR_TITLE_BG);
    tft.setTextColor(COLOR_TITLE_FG, COLOR_TITLE_BG);
    tft.setTextFont(2);
    tft.setTextSize(1);

    int rightW = 0;
    if (rightText != nullptr && rightText[0] != '\0') {
        rightW = tft.textWidth(rightText);
    }
    const int leftX  = PADDING;
    const int rightX = TFT_WIDTH - PADDING - rightW;
    int maxLeftW = rightX - leftX - (rightW > 0 ? PADDING : 0);

    String left = truncateToWidth(String(leftText), maxLeftW);

    int textH = tft.fontHeight(2);
    int y = (TITLE_HEIGHT - textH) / 2;

    tft.setCursor(leftX, y);
    tft.print(left);

    if (rightW > 0) {
        tft.setCursor(rightX, y);
        tft.print(rightText);
    }
}

// Gemeinsamer Footer-Nav-Button. enabled=false zeigt COLOR_BTN_DISABLED
// statt COLOR_BTN_BG — rein visuelles Feedback, die zugehörigen Aktionen
// (pageUp/pageDown/detailSwipeNetwork) sind an ihren Rändern bereits
// no-ops, ein Tap auf einen "disabled" Button ändert also ohnehin nichts.
static void drawFooterButton(int x, int y, int w, int h, const char* label, bool enabled) {
    uint16_t bg = enabled ? COLOR_BTN_BG : COLOR_BTN_DISABLED;
    tft.fillRoundRect(x, y, w, h, 4, bg);
    tft.setTextColor(COLOR_BTN_FG, bg);
    tft.setTextFont(2);
    tft.setTextSize(1);
    int textW = tft.textWidth(label);
    int textH = tft.fontHeight(2);
    tft.setCursor(x + (w - textW) / 2, y + (h - textH) / 2);
    tft.print(label);
}

// -----------------------------------------------------------------------------
// LIST-Header: links "WiFi: N", rechts "A-B/N" (sichtbarer Bereich/Gesamt).
// -----------------------------------------------------------------------------
static void drawListHeader(size_t netCount, size_t maxRows) {
    char rightBuf[24];
    if (netCount == 0) {
        rightBuf[0] = '\0';
    } else {
        int first = (int)g_scrollOffset + 1;
        int last  = (int)((g_scrollOffset + maxRows < netCount)
                           ? (g_scrollOffset + maxRows)
                           : netCount);
        snprintf(rightBuf, sizeof(rightBuf), "%d-%d/%u", first, last, (unsigned)netCount);
    }

    char leftBuf[24];
    snprintf(leftBuf, sizeof(leftBuf), "WiFi: %u", (unsigned)netCount);

    drawHeader(leftBuf, rightBuf);
}

// -----------------------------------------------------------------------------
// DETAIL-Header: zeigt die SSID links, "Detail" rechts.
// -----------------------------------------------------------------------------
static void drawDetailHeader(const WifiNetworkInfo& n) {
    String ssid = n.ssid.isEmpty() ? F("(hidden)") : n.ssid;
    drawHeader(ssid.c_str(), "Detail");
}

// -----------------------------------------------------------------------------
// Eine Listen-Zeile. Zwei-Zeilen-Layout (kein Overlap):
//   Zeile 1 (Font2) : SSID          linksbündig, auf Zeilenbreite gekürzt
//   Zeile 2 (Font1) : "ch=N" grau, AUTH farbig (Sicherheitsampel)
//                     RSSI-dBm      direkt links neben den Bars
//                     RSSI-Bars     rechtsbündig
//   Trennlinie      : 1 px unter der Zeile
// -----------------------------------------------------------------------------
static void drawListRow(int visibleIdx, const WifiNetworkInfo& n, bool selected) {
    int y = LIST_TOP_OFFSET + visibleIdx * ROW_HEIGHT;
    uint16_t bg = selected ? COLOR_LIST_BG_SEL : COLOR_BG;
    tft.fillRect(0, y, TFT_WIDTH, ROW_HEIGHT, bg);

    // --- Zeile 1: SSID (Font2 16px) ------------------------------------------
    tft.setTextFont(2);
    tft.setTextSize(1);
    String ssid = truncateToWidth(n.ssid.isEmpty() ? F("(hidden)") : n.ssid,
                                   TFT_WIDTH - 2 * PADDING);
    tft.setTextColor(COLOR_FG, bg);
    tft.setCursor(PADDING, y + 3);
    tft.print(ssid);

    // --- Zeile 2 (Sub): "ch=N" grau, AUTH farbig, RSSI-dBm + Bars rechts -----
    tft.setTextFont(1);
    tft.setTextSize(1);
    char chBuf[12];
    snprintf(chBuf, sizeof(chBuf), "ch=%u ", (unsigned)n.channel);
    tft.setTextColor(COLOR_DETAIL_LABEL, bg);
    tft.setCursor(PADDING, y + 24);
    tft.print(chBuf);
    // Breite von "ch=N " wird gemessen statt geschätzt, damit der AUTH-Text
    // nahtlos direkt danach beginnt.
    const char* authTxt = authToString(n.auth);
    int chW = tft.textWidth(chBuf);
    tft.setTextColor(colorForAuth(n.auth), bg);
    tft.setCursor(PADDING + chW, y + 24);
    tft.print(authTxt);

    // RSSI-dBm (Font1) — direkt links neben den Bars
    char rssiStr[8];
    snprintf(rssiStr, sizeof(rssiStr), "%d", (int)n.rssi);
    int rssiW = (int)strlen(rssiStr) * 6;       // Font1 ~6 px/Char
    tft.setTextColor(COLOR_FG, bg);
    tft.setCursor(TFT_WIDTH - 30 - rssiW - 4, y + 24);
    tft.print(rssiStr);

    // RSSI-Bars rechts (vertikal mit Sub-Zeile aligned, Bottom = y+32)
    drawRssiBars(n.rssi, TFT_WIDTH - 30, y + 14);

    // --- Trennlinie unter der Zeile ------------------------------------------
    tft.drawFastHLine(0, y + ROW_HEIGHT - 1, TFT_WIDTH, COLOR_LIST_DIVIDER);
}

// -----------------------------------------------------------------------------
// LIST-Footer — Navi-Leiste: Pfeil links | Kanäle | Pfeil rechts. Der reine
// Positions-Text ("A-B/N") steht bereits rechts im Header (drawListHeader())
// und wird hier nicht wiederholt.
// -----------------------------------------------------------------------------
static void drawListFooter(size_t netCount, size_t maxRows) {
    int yBar = TFT_HEIGHT - FOOTER_HEIGHT;

    tft.fillRect(0, yBar, TFT_WIDTH, FOOTER_HEIGHT, COLOR_BG);
    tft.drawFastHLine(0, yBar, TFT_WIDTH, COLOR_LIST_DIVIDER);

    bool canPageUp = false, canPageDown = false;
    if (netCount > maxRows) {
        const int maxOffset = (int)(netCount - maxRows);
        canPageUp   = g_scrollOffset > 0;          // vorherige Seite existiert
        canPageDown = g_scrollOffset < maxOffset;  // nächste Seite existiert
    }
    drawFooterButton(FOOTER_ARROW_LEFT_X, FOOTER_ARROW_Y,
                      FOOTER_ARROW_BTN_W, FOOTER_ARROW_BTN_H, "<", canPageUp);
    drawFooterButton(BTN_BACK_X, BTN_BACK_Y, BTN_BACK_W, BTN_BACK_H,
                      "Kanaele", true);
    drawFooterButton(FOOTER_ARROW_RIGHT_X, FOOTER_ARROW_Y,
                      FOOTER_ARROW_BTN_W, FOOTER_ARROW_BTN_H, ">", canPageDown);
}

// -----------------------------------------------------------------------------
// DETAIL-/GRAPH-/CHANNELS-Footer — Navi-Leiste: Pfeil links | Zurück | Pfeil
// rechts. showArrows=false lässt die Pfeil-Buttons komplett weg (nicht nur
// disabled) für Screens ohne Vor-/Zurück-Konzept (GRAPH, CHANNELS).
//
// Der Footer wird IMMER zuerst komplett geleert (fillRect über die volle
// Breite), bevor irgendein Button gezeichnet wird — wichtig, weil sonst bei
// showArrows=false die linke/rechte Spalte ungeleert bliebe und dort
// Pixel-Reste eines vorher gezeigten Screens (z.B. LIST/DETAIL-Pfeile)
// sichtbar blieben.
// -----------------------------------------------------------------------------
static void drawDetailFooter(bool showArrows, bool hasPrev, bool hasNext) {
    int yBar = TFT_HEIGHT - FOOTER_HEIGHT;
    tft.fillRect(0, yBar, TFT_WIDTH, FOOTER_HEIGHT, COLOR_BG);
    tft.drawFastHLine(0, yBar, TFT_WIDTH, COLOR_LIST_DIVIDER);

    drawFooterButton(BTN_BACK_X, BTN_BACK_Y, BTN_BACK_W, BTN_BACK_H,
                      BTN_BACK_TEXT, true);
    if (showArrows) {
        drawFooterButton(FOOTER_ARROW_LEFT_X, FOOTER_ARROW_Y,
                          FOOTER_ARROW_BTN_W, FOOTER_ARROW_BTN_H, "<", hasPrev);
        drawFooterButton(FOOTER_ARROW_RIGHT_X, FOOTER_ARROW_Y,
                          FOOTER_ARROW_BTN_W, FOOTER_ARROW_BTN_H, ">", hasNext);
    }
}

// -----------------------------------------------------------------------------
// DETAIL-Zeile (Label : Value). y ist die Y-Position der Zeile (Top-Kante),
// rowH die zugewiesene Höhe. Label bleibt klein/grau, der Value-Bereich
// bekommt eine eigene Box-Hintergrundfarbe und wird per Viewport auf seine
// Box geclippt — garantiert kein Überlauf in die Nachbarzeile, unabhängig
// von der tatsächlichen Schriftmetrik.
// -----------------------------------------------------------------------------
static void drawDetailRow(int y, int rowH, const char* label, const char* value, uint16_t valueColor) {
    tft.setTextColor(COLOR_DETAIL_LABEL, COLOR_BG);
    tft.setTextFont(2);
    tft.setTextSize(1);
    int labelH = tft.fontHeight();
    tft.setCursor(DETAIL_LABEL_X, y + (rowH - labelH) / 2);
    tft.print(label);

    int valueW = TFT_WIDTH - DETAIL_VALUE_X - PADDING;
    tft.fillRect(DETAIL_VALUE_X, y, valueW, rowH, COLOR_DETAIL_BOX_BG);
    tft.setTextColor(valueColor, COLOR_DETAIL_BOX_BG);

    const int innerPad = 4;
    int textY;

    #if DETAIL_VALUE_USE_GFX
        tft.setFreeFont(&FreeSansBold12pt7b);
        tft.setTextSize(1);
        // GFX-FreeFonts erwarten die Cursor-Y-Position als BASELINE, nicht
        // als Box-Oberkante (anders als die Bitmap-Fonts im #else-Zweig).
        int ascent, descent;
        gfxFontAscentDescent(&FreeSansBold12pt7b, ascent, descent);
        int valueFontH = ascent + descent;
        int topPad = (rowH - valueFontH) / 2;
        if (topPad < 0) topPad = 0;   // Font höher als rowH: oben anschlagen
        textY = y + topPad + ascent;
    #else
        tft.setTextFont(DETAIL_VALUE_FONT);
        tft.setTextSize(1);
        // Bitmap-Fonts: Cursor-Y ist die Box-Oberkante (kein Baseline-Versatz).
        int valueFontH = tft.fontHeight();
        int topPad = (rowH - valueFontH) / 2;
        if (topPad < 0) topPad = 0;
        textY = y + topPad;
    #endif

    String shown = truncateToWidth(String(value), valueW - 2 * innerPad);

    tft.setViewport(DETAIL_VALUE_X, y, valueW, rowH, false);
    tft.setCursor(DETAIL_VALUE_X + innerPad, textY);
    tft.print(shown);
    tft.resetViewport();

    #if DETAIL_VALUE_USE_GFX
        // Zurück auf Default, damit andere Draw-Routinen (Header/Liste) nicht
        // versehentlich den GFX-Font erben.
        tft.setFreeFont(NULL);
    #endif
}

// -----------------------------------------------------------------------------
// Detailansicht zeichnen
// -----------------------------------------------------------------------------
static void drawDetailScreen(const WifiNetworkInfo& n, bool hasPrev, bool hasNext) {
    drawDetailHeader(n);

    // Body-Bereich zwischen Header und Footer komplett löschen, sonst
    // blieben beim Wechsel LIST -> DETAIL die LIST-Sub-Zeilen sichtbar.
    tft.fillRect(0, TITLE_HEIGHT, TFT_WIDTH,
                 TFT_HEIGHT - TITLE_HEIGHT - FOOTER_HEIGHT, COLOR_BG);

    int sepY = DETAIL_TOP_OFFSET;
    tft.drawFastHLine(0, sepY, TFT_WIDTH, COLOR_LIST_DIVIDER);

    // Zeilenhöhe wird aus dem tatsächlich verfügbaren Platz berechnet
    // (Footer-Top minus erste Zeile, gleichmäßig durch 7 Felder geteilt) —
    // garantiert ein überlappungsfreies Layout unabhängig davon, wie hoch
    // die Value-Schrift tatsächlich ist (drawDetailRow() clippt zusätzlich
    // per Viewport).
    constexpr int kFieldCount = 7;
    int top    = sepY + DETAIL_FIRST_ROW_GAP;
    int bottom = TFT_HEIGHT - FOOTER_HEIGHT;
    int step   = (bottom - top) / kFieldCount;
    int rowH   = step - DETAIL_ROW_GAP;

    int y = top;

    {
        String ssid = n.ssid.isEmpty() ? F("(hidden)") : n.ssid;
        drawDetailRow(y, rowH, "SSID:", ssid.c_str(), COLOR_DETAIL_VALUE);
        y += step;
    }
    {
        char buf[24];
        formatBssid(buf, n.bssid);
        drawDetailRow(y, rowH, "BSSID:", buf, COLOR_DETAIL_VALUE);
        y += step;
    }
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d dBm", (int)n.rssi);
        drawDetailRow(y, rowH, "RSSI:", buf, rssiColor(n.rssi));
        y += step;
    }
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", (unsigned)n.channel);
        drawDetailRow(y, rowH, "Kanal:", buf, COLOR_DETAIL_VALUE);
        y += step;
    }
    {
        drawDetailRow(y, rowH, "Auth:", authToString(n.auth), colorForAuth(n.auth));
        y += step;
    }
    {
        drawDetailRow(y, rowH, "Hidden:", n.hidden ? "ja" : "nein", COLOR_DETAIL_VALUE);
        y += step;
    }
    {
        char buf[16];
        formatAge(buf, sizeof(buf), n.lastSeenMs);
        drawDetailRow(y, rowH, "Letzte:", buf, COLOR_DETAIL_VALUE);
        y += step;
    }

    // Rest unter den Feldern bis Footer-Top clearen
    int yClearEnd = TFT_HEIGHT - FOOTER_HEIGHT;
    if (y < yClearEnd) {
        tft.fillRect(0, y, TFT_WIDTH, yClearEnd - y, COLOR_BG);
    }

    drawDetailFooter(true, hasPrev, hasNext);
}

// -----------------------------------------------------------------------------
// GRAPH-Hilfsfunktionen
// -----------------------------------------------------------------------------

// Mappt dBm-Wert auf Y-Pixel im Plot-Bereich. GRAPH_Y_MIN_DBM = unten (max
// y), GRAPH_Y_MAX_DBM = oben (min y). Werte außerhalb des Bereichs werden
// geclippt.
static int yForDbm(int dbm) {
    if (dbm < GRAPH_Y_MIN_DBM) dbm = GRAPH_Y_MIN_DBM;
    if (dbm > GRAPH_Y_MAX_DBM) dbm = GRAPH_Y_MAX_DBM;
    const int range = GRAPH_Y_MAX_DBM - GRAPH_Y_MIN_DBM;
    const int y = GRAPH_PLOT_Y + GRAPH_PLOT_H
                  - ((dbm - GRAPH_Y_MIN_DBM) * GRAPH_PLOT_H) / range;
    return y;
}

// Mappt Linien-Index (0..WIFI_GRAPH_TOP_N-1) auf Farb-Konstante. Index 0
// ist immer das vom User per Long-Press gewählte Netz — bekommt eine feste
// Hervorhebungsfarbe statt der ersten Rang-Farbe, da es nicht zwangsläufig
// das stärkste Netz im Vergleich ist.
static uint16_t colorForGraphLine(uint8_t idx) {
    switch (idx) {
        case 0:  return COLOR_GRAPH_LINE_OWN; // cyan (gewähltes/eigenes Netz)
        case 1:  return COLOR_GRAPH_LINE_1;   // grün (stärkstes Vergleichsnetz)
        case 2:  return COLOR_GRAPH_LINE_2;   // orange
        case 3:  return COLOR_GRAPH_LINE_3;   // rot
        case 4:  return COLOR_GRAPH_LINE_4;   // blau
        default: return COLOR_GRAPH_LINE_5;   // magenta (Reserve)
    }
}

// -----------------------------------------------------------------------------
// GRAPH-Screen (RSSI-Live-Graph der Top-N Netze)
// -----------------------------------------------------------------------------
static void drawGraphScreen() {
    // 1) Header — zeigt die SSID des gewählten Netzes (Index 0), "Vergleich"
    //    rechts macht den Kontext klar.
    String ownSsid;
    wifiScannerSsidForBssid(g_graphBssids[0], ownSsid);
    if (ownSsid.isEmpty()) ownSsid = "(hidden)";
    char headerBuf[40];
    snprintf(headerBuf, sizeof(headerBuf), "RSSI: %s", ownSsid.c_str());
    drawHeader(headerBuf, "Vergleich");
    tft.drawFastHLine(0, TITLE_HEIGHT, TFT_WIDTH, COLOR_LIST_DIVIDER);

    // 2) Body clearen (volle Breite, von der Header-Trennlinie bis zum
    //    Legenden-Top in einem Rutsch — deckt Plot-Kern, Y-Achsen-Spalte
    //    und den Spalt zur Legende gleichzeitig ab).
    tft.fillRect(0, TITLE_HEIGHT, TFT_WIDTH, GRAPH_LEGEND_Y - TITLE_HEIGHT, COLOR_BG);

    // 3) Y-Achse: Gitternetz + Labels alle 20 dBm
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(COLOR_GRAPH_AXIS, COLOR_BG);
    for (int dbm = GRAPH_Y_MIN_DBM; dbm <= GRAPH_Y_MAX_DBM; dbm += 20) {
        const int y = yForDbm(dbm);
        if (y >= GRAPH_PLOT_Y - 1 && y <= GRAPH_PLOT_Y + GRAPH_PLOT_H + 1) {
            tft.drawFastHLine(GRAPH_PLOT_X, y, GRAPH_PLOT_W, COLOR_GRAPH_GRID);
            char label[8];
            snprintf(label, sizeof(label), "%d", dbm);
            tft.setCursor(2, y - 4);
            tft.print(label);
        }
    }

    // 4) X-Achse: Gitternetz + Labels alle 15 s (0/15/30/45/60)
    for (int s = 0; s <= GRAPH_WINDOW_S; s += 15) {
        const int x = GRAPH_PLOT_X + (GRAPH_PLOT_W * s) / GRAPH_WINDOW_S;
        tft.drawFastVLine(x, GRAPH_PLOT_Y, GRAPH_PLOT_H, COLOR_GRAPH_GRID);
        char label[10];
        if (s == GRAPH_WINDOW_S) snprintf(label, sizeof(label), "jetzt");
        else if (s == 0)         snprintf(label, sizeof(label), "%ds", GRAPH_WINDOW_S);
        else                     snprintf(label, sizeof(label), "%ds", GRAPH_WINDOW_S - s);
        tft.setCursor(x - (s == GRAPH_WINDOW_S ? 14 : 8), GRAPH_PLOT_Y + GRAPH_PLOT_H + 2);
        tft.print(label);
    }

    // 5) Linien pro Top-N BSSID
    static int16_t rssiBuf[WIFI_HISTORY_SIZE];
    static uint32_t tsBuf[WIFI_HISTORY_SIZE];

    const uint32_t nowMs       = millis();
    const int32_t  windowMs    = GRAPH_WINDOW_S * 1000;

    for (uint8_t i = 0; i < g_graphBssidCount && i < WIFI_GRAPH_TOP_N; ++i) {
        const uint16_t color = colorForGraphLine(i);
        const size_t nSamples = wifiScannerCopyHistory(
            g_graphBssids[i], rssiBuf, tsBuf, WIFI_HISTORY_SIZE);
        if (nSamples < 2) continue;

        int prevX = -1, prevY = -1;
        for (size_t j = 0; j < nSamples; ++j) {
            // Sample liegt vor GRAPH-Start? -> ignorieren (X wäre negativ)
            if ((int32_t)(nowMs - tsBuf[j]) > windowMs) continue;

            const int32_t ageMs = (int32_t)(nowMs - tsBuf[j]);  // 0 = gerade, windowMs = linker Rand
            const int x = GRAPH_PLOT_X + GRAPH_PLOT_W
                          - (int)((int64_t)ageMs * GRAPH_PLOT_W / windowMs);
            const int y = yForDbm(rssiBuf[j]);

            if (prevX >= 0) {
                tft.drawLine(prevX, prevY, x, y, color);
            }
            prevX = x;
            prevY = y;
        }
    }

    // 6) Legende (unter Plot, vor Footer). Index 0 (gewähltes/eigenes Netz)
    //    bekommt ein "> "-Präfix, damit es zusätzlich zur Cyan-Farbe klar
    //    als "das ist meins" erkennbar ist.
    tft.fillRect(0, GRAPH_LEGEND_Y, TFT_WIDTH, GRAPH_LEGEND_H, COLOR_BG);
    tft.setTextColor(COLOR_FG, COLOR_BG);
    tft.setTextFont(1);
    tft.setTextSize(1);
    for (uint8_t i = 0; i < g_graphBssidCount && i < WIFI_GRAPH_TOP_N; ++i) {
        const int ly = GRAPH_LEGEND_Y + GRAPH_LEGEND_PAD / 2 + (int)i * GRAPH_LEGEND_ROW_H;
        const uint16_t color = colorForGraphLine(i);
        tft.fillRect(2, ly + 1, 8, 8, color);

        String ssid;
        wifiScannerSsidForBssid(g_graphBssids[i], ssid);
        if (ssid.isEmpty()) ssid = "(hidden)";
        if ((int)ssid.length() > 17) ssid = ssid.substring(0, 16) + "~";

        int16_t rssi = 0;
        wifiScannerCurrentRssi(g_graphBssids[i], rssi);
        char buf[48];
        snprintf(buf, sizeof(buf), "%c%-17s %4d dBm", (i == 0) ? '>' : ' ',
                 ssid.c_str(), (int)rssi);
        tft.setCursor(14, ly);
        tft.print(buf);
    }

    // 7) Footer: nur Zurück-Button, GRAPH hat keine Netz-Navigation.
    drawDetailFooter(false, false, false);
}

// -----------------------------------------------------------------------------
// CHANNELS-Screen: Balkendiagramm, ein Balken pro 2.4GHz-Kanal (1..13) mit
// der Anzahl Netze auf diesem Kanal — hilft, einen wenig belegten Kanal für
// das eigene AP zu finden. Bewusst keine Ampel-Einfärbung, ein einzelner
// Akzentfarbton für alle Balken; die Zahl über dem Balken nennt die exakte
// Anzahl direkt, ein Achsen-Raster wäre hier überflüssig.
// -----------------------------------------------------------------------------
static void drawChannelScreen() {
    std::vector<WifiNetworkInfo> nets;
    wifiScannerCopyNetworks(nets);

    int counts[CHANNEL_COUNT] = {0};
    for (const WifiNetworkInfo& n : nets) {
        if (n.channel >= 1 && n.channel <= CHANNEL_COUNT) {
            counts[n.channel - 1]++;
        }
    }
    int maxCount = 1;   // mindestens 1, sonst Division durch 0 bei leerer Liste
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (counts[i] > maxCount) maxCount = counts[i];
    }

    char rightBuf[16];
    snprintf(rightBuf, sizeof(rightBuf), "%u Netze", (unsigned)nets.size());
    drawHeader("Kanal-Belegung", rightBuf);

    tft.fillRect(0, TITLE_HEIGHT, TFT_WIDTH, TFT_HEIGHT - TITLE_HEIGHT - FOOTER_HEIGHT, COLOR_BG);
    tft.drawFastHLine(0, TITLE_HEIGHT, TFT_WIDTH, COLOR_LIST_DIVIDER);

    // Layout zur Laufzeit berechnet (keine weiteren Pixel-Konstanten nötig).
    const int plotTop      = TITLE_HEIGHT + 4;
    const int footerTop    = TFT_HEIGHT - FOOTER_HEIGHT;
    const int countLabelH  = 10;   // Font1 (8px) + 2px Abstand
    const int chanLabelH   = 10;
    const int barAreaTop   = plotTop + countLabelH;
    const int barAreaBot   = footerTop - chanLabelH;
    const int barAreaH     = barAreaBot - barAreaTop;
    const int slotW        = (TFT_WIDTH - 2 * PADDING) / CHANNEL_COUNT;
    const int barW         = slotW - 4;   // 2px Lücke je Seite zwischen Balken

    tft.setTextFont(1);
    tft.setTextSize(1);

    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        const int barX = PADDING + i * slotW + 2;

        int barH = (counts[i] * barAreaH) / maxCount;
        if (counts[i] > 0 && barH < 2) barH = 2;   // Sichtbarkeits-Mindesthöhe
        const int barY = barAreaBot - barH;

        if (barH > 0) {
            tft.fillRect(barX, barY, barW, barH, COLOR_ACCENT_GREEN);
        }

        // Anzahl über dem Balken (nur wenn > 0, sonst unnötiger "0"-Wirrwarr
        // bei vielen freien Kanälen)
        if (counts[i] > 0) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%d", counts[i]);
            tft.setTextColor(COLOR_FG, COLOR_BG);
            int textW = tft.textWidth(buf);
            tft.setCursor(barX + (barW - textW) / 2, barY - countLabelH + 1);
            tft.print(buf);
        }

        // Kanalnummer unter dem Balken (immer, 1..13)
        char chBuf[4];
        snprintf(chBuf, sizeof(chBuf), "%d", i + 1);
        tft.setTextColor(COLOR_DETAIL_LABEL, COLOR_BG);
        int chTextW = tft.textWidth(chBuf);
        tft.setCursor(PADDING + i * slotW + (slotW - chTextW) / 2, barAreaBot + 2);
        tft.print(chBuf);
    }

    // Footer: nur Zurück (kein Vor-/Zurück-Konzept hier).
    drawDetailFooter(false, false, false);
}

// -----------------------------------------------------------------------------
// Komplette Liste neu zeichnen
// -----------------------------------------------------------------------------
static void drawListScreen() {
    std::vector<WifiNetworkInfo> nets;
    wifiScannerCopyNetworks(nets);

    size_t maxRows = (size_t)((TFT_HEIGHT - LIST_TOP_OFFSET - LIST_BOTTOM_OFFSET) / ROW_HEIGHT);
    size_t total   = nets.size();

    drawListHeader(total, maxRows);

    int maxOffset = (total > maxRows) ? (int)(total - maxRows) : 0;
    if (g_scrollOffset > maxOffset) g_scrollOffset = maxOffset;
    if (g_scrollOffset < 0)         g_scrollOffset = 0;

    if (g_selectedRow >= (int)total) g_selectedRow = -1;

    size_t rows = (total < maxRows) ? total : maxRows;
    for (size_t i = 0; i < rows; ++i) {
        int netIdx = (int)g_scrollOffset + (int)i;
        bool isSel = (netIdx == g_selectedRow);
        drawListRow((int)i, nets[netIdx], isSel);
    }

    // Rest unter den Zeilen clearen (falls weniger Zeilen als maxRows)
    int yClear    = LIST_TOP_OFFSET + (int)rows * ROW_HEIGHT;
    int yClearEnd = TFT_HEIGHT - LIST_BOTTOM_OFFSET;
    if (yClear < yClearEnd) {
        tft.fillRect(0, yClear, TFT_WIDTH, yClearEnd - yClear, COLOR_BG);
    }

    drawListFooter(total, maxRows);
}

// -----------------------------------------------------------------------------
// Komplett-Render je nach UI-State (öffentliche API)
// -----------------------------------------------------------------------------
void renderScreen() {
    if (g_uiState == UI_CHANNELS) {
        drawChannelScreen();
        return;
    }

    // GRAPH-State: wenn g_graphBssidCount == 0 (z.B. Long-Press bei leerer
    // Liste oder Scan noch nicht gelaufen) -> zurück zur Liste.
    if (g_uiState == UI_GRAPH) {
        if (g_graphBssidCount == 0) {
            g_uiState = UI_LIST;
        } else {
            drawGraphScreen();
            return;
        }
    }

    if (g_uiState == UI_DETAIL && g_detailBssidValid) {
        std::vector<WifiNetworkInfo> nets;
        wifiScannerCopyNetworks(nets);

        // Netz anhand der BSSID suchen (nicht per Index) — nach jedem
        // Re-Scan wird die Liste neu nach RSSI sortiert, ein Index wäre
        // dann ungültig. matchIdx wird nur für diesen Render-Durchlauf
        // gebraucht (Prev/Next-Verfügbarkeit für die Footer-Pfeile).
        int matchIdx = -1;
        for (size_t i = 0; i < nets.size(); ++i) {
            if (memcmp(nets[i].bssid, g_detailBssid, 6) == 0) {
                matchIdx = (int)i;
                break;
            }
        }

        if (matchIdx >= 0) {
            const bool hasPrev = matchIdx > 0;
            const bool hasNext = (size_t)matchIdx < nets.size() - 1;
            drawDetailScreen(nets[matchIdx], hasPrev, hasNext);
            return;
        }

        // Netz nicht (mehr) in der Liste (AP ist verschwunden) -> zurück zur Liste.
        g_uiState          = UI_LIST;
        g_detailBssidValid = false;
    }
    drawListScreen();
}
