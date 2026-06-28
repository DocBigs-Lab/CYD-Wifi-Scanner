// =============================================================================
// User_Setup.h — TFT_eSPI Konfiguration für CYD (ESP32-2432S028R)
// =============================================================================
// Diese Datei wird über die build_flags (-include ...) in platformio.ini
// in TFT_eSPI eingebunden. Sie ersetzt die User_Setup.h, die mit der
// TFT_eSPI-Bibliothek ausgeliefert wird.
//
// WICHTIG: Nicht die Datei in der TFT_eSPI-Lib bearbeiten — diese hier ist
// die Quelle der Wahrheit.
//
// Pinout-Quelle: Standard-CYD (v1/v2). Siehe KI-Instructions.md Sektion 4.
// =============================================================================

// ---- Pflicht: verhindert, dass TFT_eSPI eigene Defaults lädt ----------------
#define USER_SETUP_LOADED

// ---- Display-Treiber (ILI9341, 240x320, SPI) -------------------------------
// CYD (ESP32-2432S028R v1/v2/v3): der alternative Treiber ILI9341_2_DRIVER
// hat eine angepasste Init-Sequenz, die die Farb-Inversion korrekt behandelt.
// Der generische ILI9341_DRIVER fuehrt bei CYD-Boards zu invertierten Farben
// (Blau erscheint als Gelb, Gruen als Lila etc.), auch wenn TFT_INVERSION_ON
// gesetzt ist.
// #define ILI9341_DRIVER      // generisch (V1 only) — fuehrt zu Farb-Inversion
#define ILI9341_2_DRIVER       // CYD v1/v2/v3 — korrekte Init-Sequenz

// ---- Pinout Display --------------------------------------------------------
#define TFT_MOSI   13   // SPI-Daten  (auch SDA genannt)
#define TFT_SCLK   14   // SPI-Takt   (auch SCL genannt)
#define TFT_CS     15   // Chip-Select
#define TFT_DC      2   // Data/Command (RS)
#define TFT_RST     4   // Reset (kann oft offen bleiben, hier explizit gesetzt)
#define TFT_BL     21   // Backlight (PWM-fähig)

// Logikpegel zum Einschalten der Hintergrundbeleuchtung
#ifndef TFT_BACKLIGHT_ON
#define TFT_BACKLIGHT_ON HIGH
#endif

// ---- SPI-Timing ------------------------------------------------------------
#define SPI_FREQUENCY          40000000   // 40 MHz Write (Display verträgt mehr)
#define SPI_READ_FREQUENCY     20000000   // 20 MHz Read
#define SPI_TOUCH_FREQUENCY    2500000    // 2.5 MHz (Touch braucht weniger)

// ---- Display-Geometrie -----------------------------------------------------
#define TFT_WIDTH   240
#define TFT_HEIGHT  320

// ---- Schriftarten ----------------------------------------------------------
#define SMOOTH_FONT                          // Anti-Aliasing für eingebettete Schriften
#define LOAD_GLCD                            // Font 1 (Adafruit 8px) – fallback
#define LOAD_FONT2                           // Font 2 (16px)
#define LOAD_FONT4                           // Font 4 (26px) – gut für Headlines
#define LOAD_FONT6                           // Font 6 (Groß, nummerisch)
#define LOAD_FONT7                           // Font 7 (7-Segment-Look)
#define LOAD_FONT8                           // Font 8 (große Ziffern)
#define LOAD_GFXFF                           // FreeFonts aus GFX-Lib

// ---- Erweiterte Buffer-Optionen -------------------------------------------
// Bei nur 320 KB RAM und einem 240x320-Display reicht ein voller Framebuffer
// (~150 KB) theoretisch, kostet aber zu viel Heap. Wir lassen es bei
// "zeichnen direkt ins Display" (kein Sprite-Buffer hier aktiv).
//#define TFT_BUFFER_SIZE  4096   // kleiner SPI-Buffer

// ---- Farb-Reihenfolge (CYD verwendet üblicherweise RGB, nicht BGR) -------
// Falls die Farben "vertauscht" aussehen (Rot erscheint als Türkis etc.):
// auskommentieren.
//#define TFT_RGB_ORDER TFT_RGB
//
// ---- Display-Inversion ----
// TFT_INVERSION_ON sendet INVON (0x21) an den Controller -> Hardware-Inversion.
// Bei diesem CYD-Board fuehrt das zu invertierten Farben (Blau->Gelb, Gruen->Lila).
// Daher EXPLIZIT AUS (kein INVON-Kommando senden), das Display laeuft dann
// im Werks-Normalmodus.
//#define TFT_INVERSION_ON
#define TFT_INVERSION_OFF                     // INVOFF (0x20) senden = sicherer Normalmodus
