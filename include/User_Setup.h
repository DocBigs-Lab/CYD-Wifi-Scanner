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
//
// Deckt beide unterstützten CYD-Display-Varianten ab, gesteuert über das
// Build-Flag CYD_ST7789 (gesetzt in platformio.ini, env:cyd-st7789):
// Pinout, SPI-Timing, Geometrie und Fonts sind bei beiden Varianten
// identisch, nur Display-Treiber und Farbkonfiguration unterscheiden sich
// (siehe #ifdef-Bloecke unten). Welcher Controller verbaut ist, laesst
// sich von außen NICHT zuverlässig erkennen (auch nicht an der
// USB-Portanzahl) — siehe README.md.
// =============================================================================

// ---- Pflicht: verhindert, dass TFT_eSPI eigene Defaults lädt ----------------
#define USER_SETUP_LOADED

// ---- Display-Treiber (240x320, SPI) -----------------------------------------
#ifdef CYD_ST7789
// ST7789-Variante: bei manchen CYD-Board-Chargen verbaut (u.a. bei einigen
// 2-USB-Boards beobachtet, aber nicht zuverlässig darauf eingrenzbar).
// Hersteller verbauen unterschiedliche Panel-Chargen — RGB-Reihenfolge und
// Inversion ggf. am Geraet pruefen, siehe Defines weiter unten.
#define ST7789_DRIVER
#else
// ILI9341-Variante (ESP32-2432S028R v1/v2/v3): der alternative Treiber
// ILI9341_2_DRIVER hat eine angepasste Init-Sequenz, die die Farb-Inversion
// korrekt behandelt. Der generische ILI9341_DRIVER fuehrt bei CYD-Boards zu
// invertierten Farben (Blau erscheint als Gelb, Gruen als Lila etc.), auch
// wenn TFT_INVERSION_ON gesetzt ist. HINWEIS: ILI9341_2_DRIVER respektiert
// TFT_RGB_ORDER NICHT (MADCTL-Farb-Order ist bei diesem Treiber fest auf
// BGR verdrahtet) — nur der generische ILI9341_DRIVER wertet es aus.
// #define ILI9341_DRIVER      // generisch (V1 only) — fuehrt zu Farb-Inversion
#define ILI9341_2_DRIVER       // CYD v1/v2/v3 — korrekte Init-Sequenz
#endif

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

// ---- Farb-Reihenfolge & Display-Inversion -----------------------------------
// WICHTIG (bestätigt per Geräte-Test, 2026-07-18): auf dem getesteten
// ST7789-CYD-Board hatte WEDER TFT_INVERSION_ON/OFF NOCH TFT_RGB_ORDER
// irgendeinen sichtbaren Effekt — der Controller ignoriert das
// Hardware-Inversions-Kommando vollständig und zeigt jeden Pixel fest
// bit-invertiert an. Die eigentliche Korrektur passiert daher NICHT hier,
// sondern auf Werte-Ebene über CYD_COLOR() in src/config.h. Diese beiden
// Defines bleiben trotzdem gesetzt (dokumentierter, plausibler Ausgangswert
// für andere ST7789-Panel-Chargen, die den Befehl ggf. doch korrekt
// umsetzen) — schaden aber nichts, falls sie wie hier wirkungslos sind.
#ifdef CYD_ST7789
#define TFT_RGB_ORDER TFT_BGR   // Alternative: TFT_RGB, falls Rot/Blau vertauscht sind
#define TFT_INVERSION_ON        // Alternative: TFT_INVERSION_OFF, falls Farben invertiert wirken
#else
// ILI9341-Variante (CYD verwendet üblicherweise RGB, nicht BGR): falls die
// Farben "vertauscht" aussehen (Rot erscheint als Türkis etc.),
// auskommentieren.
//#define TFT_RGB_ORDER TFT_RGB
//
// TFT_INVERSION_ON sendet INVON (0x21) an den Controller -> Hardware-Inversion.
// Bei diesem CYD-Board fuehrt das zu invertierten Farben (Blau->Gelb, Gruen->Lila).
// Daher EXPLIZIT AUS (kein INVON-Kommando senden), das Display laeuft dann
// im Werks-Normalmodus.
//#define TFT_INVERSION_ON
#define TFT_INVERSION_OFF                     // INVOFF (0x20) senden = sicherer Normalmodus
#endif
