/*
  CYD Windows PC Dashboard - Futuristic HUD edition
  Board: ESP32-2432S028 (CYD2USB, ILI9341 driver confirmed working)

  HOME page: CPU ring gauge (+ temp), RAM ring gauge, GPU ring gauge
  (+ temp), NET summary. Touch any quadrant to open its detail page:
    - CPU  -> per-core bars, frequency, temperature
    - RAM  -> used/total, swap, top 3 memory-hungry processes
    - GPU  -> utilization gauge, temperature
    - NET  -> up/down speed with mini history graph, local IP

  Rendering: every frame is drawn into a full-screen RAM sprite (TFT_eSprite,
  built into the TFT_eSPI library - no extra install needed) and pushed to
  the physical screen in one blit at ~20fps. This removes the flicker you'd
  get from drawing straight to the display, and lets the gauges run a
  continuous animated "radar sweep" plus a breathing status dot.

  Data comes from the Windows Python agent as one JSON line per second
  over USB serial. See pc_monitor_agent.py for the payload format.

  Libraries required (Arduino IDE > Library Manager):
    - TFT_eSPI              by Bodmer
    - ArduinoJson            by Benoit Blanchon (v6 or v7)
    - XPT2046_Touchscreen    by Paul Stoffregen

  Board settings (Arduino IDE > Tools):
    - Board: "ESP32 Dev Module"
    - Upload speed: 921600 (or 115200 if you get upload errors)
*/

#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>
#include <string.h>

// ---------------- Touch pins (separate SPI bus - VSPI) ----------------
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// ---------------- Touch calibration ----------------
// These are typical raw ADC ranges for this board's touch panel. If taps
// feel off (wrong spot, or axes swapped/inverted), tweak these four values.
// Quick test: uncomment PRINT_TOUCH_RAW below, open Serial Monitor, tap
// the four corners, and read the raw min/max values it prints.
#define TS_MINX 200
#define TS_MAXX 3700
#define TS_MINY 240
#define TS_MAXY 3800
// #define PRINT_TOUCH_RAW

SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite gspr = TFT_eSprite(&tft);  // small reusable buffer, just for the gauge circles
const int GSPR_SIZE = 100;              // must be >= 2*(largest gauge radius + margin)

// ---------------- Screen ----------------
#define SCREEN_W 320
#define SCREEN_H 240

// ---------------- Futuristic HUD palette ----------------
#define COL_BG       0x0000  // pure black
#define COL_PANEL    0x0861  // deep blue-black panel
#define COL_LINE     0x18C4  // dim steel-blue grid lines
#define COL_TEXT     0xEFFB  // crisp near-white
#define COL_DIM      0x528D  // muted steel-blue-grey
#define COL_CPU      0x04FF  // ice blue
#define COL_RAM      0x861F  // violet
#define COL_GPU      0xFC1B  // orchid rose
#define COL_NET      0x07EA  // emerald mint
#define COL_WARN     0xF800  // red
#define COL_TRACK    0x1082  // ring track (unfilled)
#define COL_FRAME    0x2A5D  // steel-blue frame/bracket accent

#define COL_TEMP_GOOD 0x07EA  // emerald - comfortable
#define COL_TEMP_WARN 0xFD20  // amber - getting warm
#define COL_TEMP_HOT  0xF800  // red - too high

// Temp thresholds in °C. Tuned for a 12th-gen Intel i7 (TjMax ~100°C)
// and an RTX 4060 Ti (throttles around ~87°C) - adjust if your CPU/GPU
// runs noticeably hotter or cooler under normal load.
const float CPU_TEMP_WARN_C = 70.0f;
const float CPU_TEMP_HOT_C  = 85.0f;
const float GPU_TEMP_WARN_C = 70.0f;
const float GPU_TEMP_HOT_C  = 80.0f;
// DDR4/DDR5 generally runs cool; these are conservative general defaults
// since RAM temp sensors vary a lot by motherboard (many don't expose one).
const float RAM_TEMP_WARN_C = 45.0f;
const float RAM_TEMP_HOT_C  = 55.0f;

// ---------------- Pages ----------------
enum Page { PAGE_HOME, PAGE_CPU, PAGE_RAM, PAGE_GPU, PAGE_NET };
Page currentPage = PAGE_HOME;
Page lastDrawnPage = (Page)-1;   // forces initial static draw

// ---------------- Data model ----------------
struct ProcInfo {
  String name;
  float memMB = 0;
};

struct Metrics {
  float cpu = 0;
  float cpuFreqMHz = 0;
  float cpuTemp = -1;
  float cores[8] = {0};
  int numCores = 0;
  String cpuName = "";

  float ram = 0;
  float ramUsedGB = 0;
  float ramTotalGB = 0;
  float swapPercent = 0;
  float ramTemp = -1;

  ProcInfo procs[3];
  int numProcs = 0;

  float gpuPercent = 0;
  float gpuTemp = -1;
  String gpuName = "";

  float netUp = 0;
  float netDown = 0;
  String ip = "";

  bool valid = false;
};

Metrics metrics;
unsigned long lastDataMillis = 0;
const unsigned long DATA_TIMEOUT_MS = 5000;
String serialBuffer;

// Network history for the mini graph on the NET page
#define NET_HISTORY_LEN 40
uint16_t netUpHistory[NET_HISTORY_LEN] = {0};
uint16_t netDownHistory[NET_HISTORY_LEN] = {0};
int netHistoryIdx = 0;

// Touch debounce
bool wasTouching = false;
unsigned long lastTapMillis = 0;
const unsigned long TAP_COOLDOWN_MS = 250;

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_BG);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  // Small reusable sprite just for the gauge circles - this is what lets
  // the radar sweep animate smoothly without needing a full-screen buffer.
  gspr.setColorDepth(16);
  gspr.createSprite(GSPR_SIZE, GSPR_SIZE);
  if (!gspr.created()) {
    tft.setTextColor(TFT_RED, COL_BG);
    tft.setTextFont(2);
    tft.setCursor(10, 100);
    tft.print("Gauge sprite alloc failed - out of RAM");
    while (true) delay(1000);
  }

  drawSplash();
}

void drawSplash() {
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_CPU, COL_BG);
  tft.setTextFont(4);
  tft.setCursor(50, 100);
  tft.print("SYSTEM MONITOR");
  tft.setTextFont(2);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(70, 140);
  tft.print("waiting for PC link...");
  delay(800);
}

// ============================================================
// Two independent timers:
//  - the main page (labels, icons, panels, text) redraws at a modest
//    rate, same as before - it doesn't need to be fast since it isn't
//    animated.
//  - the gauge circles redraw much faster, via updateGaugeAnimation(),
//    using the small gspr buffer - that's what makes the radar sweep and
//    progress ring animate smoothly without flicker.
const unsigned long DYNAMIC_INTERVAL_MS = 400;
const unsigned long GAUGE_FRAME_MS = 50;   // ~20 fps for the gauges only

void loop() {
  readSerial();
  handleTouch();

  if (currentPage != lastDrawnPage) {
    drawStaticForPage(currentPage);
    lastDrawnPage = currentPage;
  }

  static unsigned long lastDynamic = 0;
  if (millis() - lastDynamic > DYNAMIC_INTERVAL_MS) {
    lastDynamic = millis();
    drawDynamicForPage(currentPage);
  }

  static unsigned long lastGaugeFrame = 0;
  if (millis() - lastGaugeFrame > GAUGE_FRAME_MS) {
    lastGaugeFrame = millis();
    updateGaugeAnimation();
  }
}

// Redraws just the ring gauge(s) relevant to the current page. Runs much
// faster than the rest of the UI since it's cheap (small sprite) and is
// the only part that needs to look smoothly animated.
void updateGaugeAnimation() {
  switch (currentPage) {
    case PAGE_HOME:
      drawGauge(55, 78, 36, 7, metrics.cpu, COL_CPU);
      drawGauge(215, 78, 36, 7, metrics.ram, COL_RAM);
      drawGauge(55, 185, 36, 7, metrics.gpuPercent, COL_GPU);
      break;
    case PAGE_CPU:
      drawGauge(65, 90, 40, 8, metrics.cpu, COL_CPU);
      break;
    case PAGE_RAM:
      drawGauge(65, 90, 40, 8, metrics.ram, COL_RAM);
      break;
    case PAGE_GPU:
      drawGauge(65, 90, 40, 8, metrics.gpuPercent, COL_GPU);
      break;
    default:
      break; // NET page has no ring gauge
  }
}

// ---------------- Serial parsing ----------------
void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      parseLine(serialBuffer);
      serialBuffer = "";
    } else if (c != '\r') {
      serialBuffer += c;
      if (serialBuffer.length() > 1500) serialBuffer = "";
    }
  }
}

void parseLine(const String &line) {
  if (line.length() < 2) return;

  DynamicJsonDocument doc(3072);
  DeserializationError err = deserializeJson(doc, line);
  if (err) return;

  metrics.cpu        = doc["cpu"]     | metrics.cpu;
  metrics.cpuFreqMHz  = doc["freq"]    | metrics.cpuFreqMHz;
  metrics.cpuTemp     = doc["ctemp"]   | metrics.cpuTemp;
  if (doc.containsKey("cpuname")) metrics.cpuName = doc["cpuname"].as<String>();

  JsonArray cores = doc["cores"];
  if (!cores.isNull()) {
    metrics.numCores = min((int)cores.size(), 8);
    for (int i = 0; i < metrics.numCores; i++) metrics.cores[i] = cores[i];
  }

  metrics.ram         = doc["ram"]     | metrics.ram;
  metrics.ramUsedGB    = doc["ramu"]    | metrics.ramUsedGB;
  metrics.ramTotalGB   = doc["ramt"]    | metrics.ramTotalGB;
  metrics.swapPercent  = doc["swap"]    | metrics.swapPercent;
  metrics.ramTemp      = doc["rtemp"]   | metrics.ramTemp;

  JsonArray procs = doc["procs"];
  if (!procs.isNull()) {
    metrics.numProcs = min((int)procs.size(), 3);
    for (int i = 0; i < metrics.numProcs; i++) {
      metrics.procs[i].name = procs[i]["n"].as<String>();
      metrics.procs[i].memMB = procs[i]["m"];
    }
  }

  metrics.gpuPercent = doc["gpu"]   | metrics.gpuPercent;
  metrics.gpuTemp     = doc["gtemp"] | metrics.gpuTemp;
  if (doc.containsKey("gpuname")) metrics.gpuName = doc["gpuname"].as<String>();

  float newUp   = doc["up"]   | metrics.netUp;
  float newDown = doc["down"] | metrics.netDown;
  metrics.netUp = newUp;
  metrics.netDown = newDown;
  netUpHistory[netHistoryIdx] = (uint16_t)min(newUp, 65000.0f);
  netDownHistory[netHistoryIdx] = (uint16_t)min(newDown, 65000.0f);
  netHistoryIdx = (netHistoryIdx + 1) % NET_HISTORY_LEN;

  if (doc.containsKey("ip")) metrics.ip = doc["ip"].as<String>();

  metrics.valid = true;
  lastDataMillis = millis();
}

// ---------------- Touch handling ----------------
// Returns true if the panel is currently being touched, and writes the
// mapped screen coordinates into outX/outY. Using a bool + reference
// params (instead of returning a custom struct) avoids an Arduino IDE
// quirk where it auto-generates function prototypes at the top of the
// file before custom structs are defined.
bool readTouch(int &outX, int &outY) {
  if (!ts.touched()) return false;

  TS_Point p = ts.getPoint();
#ifdef PRINT_TOUCH_RAW
  Serial.printf("raw x=%d y=%d\n", p.x, p.y);
#endif
  int x = map(p.x, TS_MINX, TS_MAXX, 0, SCREEN_W);
  int y = map(p.y, TS_MINY, TS_MAXY, 0, SCREEN_H);
  outX = constrain(x, 0, SCREEN_W - 1);
  outY = constrain(y, 0, SCREEN_H - 1);
  return true;
}

void handleTouch() {
  int x = 0, y = 0;
  bool touching = readTouch(x, y);

  if (touching && !wasTouching && millis() - lastTapMillis > TAP_COOLDOWN_MS) {
    lastTapMillis = millis();
    onTap(x, y);
  }
  wasTouching = touching;
}

void onTap(int x, int y) {
  if (currentPage == PAGE_HOME) {
    if (y < 20) return; // status bar, ignore
    bool leftHalf = x < SCREEN_W / 2;
    bool topHalf = y < 130;
    if (topHalf && leftHalf)      currentPage = PAGE_CPU;
    else if (topHalf && !leftHalf) currentPage = PAGE_RAM;
    else if (!topHalf && leftHalf) currentPage = PAGE_GPU;
    else                            currentPage = PAGE_NET;
  } else {
    // any detail page: top-left corner = back button
    if (x < 70 && y < 30) currentPage = PAGE_HOME;
  }
}

// ---------------- Shared drawing helpers ----------------

// Dims a 565 color toward black by the given factor (0..1)
uint16_t dimColor(uint16_t c, float factor) {
  uint8_t r = (c >> 11) & 0x1F;
  uint8_t g = (c >> 5) & 0x3F;
  uint8_t b = c & 0x1F;
  r = (uint8_t)(r * factor);
  g = (uint8_t)(g * factor);
  b = (uint8_t)(b * factor);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Green under warnThreshold, orange up to hotThreshold, red above that.
// A negative temp (no reading yet) returns COL_DIM, matching the "--" text.
uint16_t tempColor(float tempC, float warnThreshold, float hotThreshold) {
  if (tempC < 0) return COL_DIM;
  if (tempC < warnThreshold) return COL_TEMP_GOOD;
  if (tempC < hotThreshold) return COL_TEMP_WARN;
  return COL_TEMP_HOT;
}

// The home screen cards are tight on width, so this strips the vendor
// prefix off a hardware name (e.g. "i7-12700KF" -> "12700KF", "RTX 4060
// Ti" -> "4060 Ti") and caps the length, leaving just the distinctive
// model part. Detail pages still show the full, unshortened name.
String shortenForHomeScreen(const String &name, int maxChars) {
  String s = name;
  const char* prefixes[] = {"i3-", "i5-", "i7-", "i9-", "RTX ", "GTX ", "RX "};
  for (const char* p : prefixes) {
    if (s.startsWith(p)) {
      s = s.substring(strlen(p));
      break;
    }
  }
  if ((int)s.length() > maxChars) s = s.substring(0, maxChars);
  return s;
}

// Subtle CRT-style scanline texture across the whole screen background
// Subtle depth gradient - a faint blue glow at the top fading to pure
// black at the bottom, giving the background quiet depth instead of a
// flat fill, without being distracting.
// Small L-shaped accents in each screen corner, sci-fi HUD framing
void drawCornerBrackets(uint16_t color) {
  int len = 14, m = 3;
  // top-left
  tft.drawFastHLine(m, m, len, color);
  tft.drawFastVLine(m, m, len, color);
  // top-right
  tft.drawFastHLine(SCREEN_W - m - len, m, len, color);
  tft.drawFastVLine(SCREEN_W - m - 1, m, len, color);
  // bottom-left
  tft.drawFastHLine(m, SCREEN_H - m - 1, len, color);
  tft.drawFastVLine(m, SCREEN_H - m - len, len, color);
  // bottom-right
  tft.drawFastHLine(SCREEN_W - m - len, SCREEN_H - m - 1, len, color);
  tft.drawFastVLine(SCREEN_W - m - 1, SCREEN_H - m - len, len, color);
}

// A "glass panel" card: a dim full outline, with the top and left edges
// picked out brighter to fake a soft light source (a cheap but effective
// beveled-glass look), plus a small accent dot in the top-right corner
// identifying the card at a glance.
void drawCard(int x, int y, int w, int h, uint16_t accent) {
  uint16_t bright = dimColor(accent, 0.55f);
  uint16_t dim = dimColor(accent, 0.16f);
  tft.drawRoundRect(x, y, w, h, 8, dim);
  tft.drawFastHLine(x + 10, y, w - 20, bright);
  tft.drawFastVLine(x, y + 10, h - 20, bright);
  tft.fillCircle(x + w - 14, y + 14, 3, accent);
}

// Connection dot + ONLINE/OFFLINE readout, refreshed every dynamic tick
void drawStatusIndicator() {
  bool stale = (millis() - lastDataMillis > DATA_TIMEOUT_MS) || !metrics.valid;
  uint16_t base = stale ? COL_WARN : COL_NET;

  // Breathing pulse: brightness oscillates smoothly. Faster pulse when
  // offline, as a subtle "still trying to reconnect" cue.
  float period = stale ? 260.0f : 500.0f;
  float phase = (millis() % (unsigned long)period) / period;
  float pulse = (sinf(phase * 2.0f * PI) + 1.0f) / 2.0f;  // 0..1
  uint16_t c = dimColor(base, 0.55f + 0.45f * pulse);

  tft.fillRect(SCREEN_W - 74, 2, 68, 17, COL_PANEL);
  tft.fillCircle(SCREEN_W - 68, 10, 4, c);
  tft.setTextFont(1);
  tft.setTextColor(base, COL_PANEL);
  tft.setCursor(SCREEN_W - 58, 6);
  tft.print(stale ? "OFFLINE" : "ONLINE");
}

void drawTopBar(const char* title, uint16_t accent) {
  tft.fillRect(0, 0, SCREEN_W, 20, COL_PANEL);
  tft.drawFastHLine(0, 20, SCREEN_W, accent);
  tft.drawFastHLine(0, 21, SCREEN_W, COL_LINE);

  tft.setTextFont(2);
  tft.setTextColor(accent, COL_PANEL);
  tft.setCursor(8, 3);
  tft.print("[ ");
  tft.setTextColor(COL_TEXT, COL_PANEL);
  tft.print(title);
  tft.setTextColor(accent, COL_PANEL);
  tft.print(" ]");

  drawStatusIndicator();
}

void drawBackButton() {
  tft.fillRect(0, 0, 60, 20, COL_PANEL);
  tft.setTextFont(2);
  tft.setTextColor(COL_DIM, COL_PANEL);
  tft.setCursor(6, 3);
  tft.print("< BACK");
}

// Fully self-contained gauge: draws track, tick marks, an animated radar
// sweep, the progress arc (with glow + bright tip dot), and the centered
// percent number - all into the small gspr buffer, then pushes that one
// buffer to the screen at (screenCx, screenCy). Doing everything in one
// atomic push is what keeps this flicker-free even on a RAM-limited board.
void drawGauge(int screenCx, int screenCy, int rOuter, int thickness,
                float percent, uint16_t color) {
  percent = constrain(percent, 0, 100);
  int cx = GSPR_SIZE / 2;
  int cy = GSPR_SIZE / 2;
  int rInner = rOuter - thickness;

  gspr.fillSprite(COL_BG);

  // track
  for (int a = 0; a < 360; a += 3) {
    float rad = a * 0.0174533f;
    int x1 = cx + (int)(rInner * cosf(rad));
    int y1 = cy + (int)(rInner * sinf(rad));
    int x2 = cx + (int)(rOuter * cosf(rad));
    int y2 = cy + (int)(rOuter * sinf(rad));
    gspr.drawLine(x1, y1, x2, y2, COL_TRACK);
  }
  // tick marks at 0/25/50/75%
  for (int a = -90; a < 270; a += 90) {
    float rad = a * 0.0174533f;
    int x1 = cx + (int)((rInner - 3) * cosf(rad));
    int y1 = cy + (int)((rInner - 3) * sinf(rad));
    int x2 = cx + (int)((rOuter + 3) * cosf(rad));
    int y2 = cy + (int)((rOuter + 3) * sinf(rad));
    gspr.drawLine(x1, y1, x2, y2, COL_DIM);
  }

  // rotating radar sweep with a fading trail, drawn under the progress arc
  float baseAngle = fmodf(millis() / 6.0f, 360.0f);
  const int trailCount = 9;
  for (int i = 0; i < trailCount; i++) {
    float a = baseAngle - i * 5.0f;
    float rad = a * 0.0174533f;
    float fade = 1.0f - (float)i / trailCount;
    uint16_t c = dimColor(color, 0.15f + 0.5f * fade);
    int x1 = cx + (int)((rInner - 2) * cosf(rad));
    int y1 = cy + (int)((rInner - 2) * sinf(rad));
    int x2 = cx + (int)((rOuter + 2) * cosf(rad));
    int y2 = cy + (int)((rOuter + 2) * sinf(rad));
    gspr.drawLine(x1, y1, x2, y2, c);
  }

  // progress arc: dim glow pass, then bright core pass
  int sweep = (int)(360.0f * (percent / 100.0f));
  uint16_t glow = dimColor(color, 0.35f);
  for (int a = -90; a < -90 + sweep; a += 3) {
    float rad = a * 0.0174533f;
    int x1 = cx + (int)((rInner - 2) * cosf(rad));
    int y1 = cy + (int)((rInner - 2) * sinf(rad));
    int x2 = cx + (int)((rOuter + 2) * cosf(rad));
    int y2 = cy + (int)((rOuter + 2) * sinf(rad));
    gspr.drawLine(x1, y1, x2, y2, glow);
  }
  for (int a = -90; a < -90 + sweep; a += 3) {
    float rad = a * 0.0174533f;
    int x1 = cx + (int)(rInner * cosf(rad));
    int y1 = cy + (int)(rInner * sinf(rad));
    int x2 = cx + (int)(rOuter * cosf(rad));
    int y2 = cy + (int)(rOuter * sinf(rad));
    gspr.drawLine(x1, y1, x2, y2, color);
  }
  if (percent > 0) {
    float rad = (-90 + sweep) * 0.0174533f;
    int midR = (rInner + rOuter) / 2;
    int tx = cx + (int)(midR * cosf(rad));
    int ty = cy + (int)(midR * sinf(rad));
    gspr.fillCircle(tx, ty, 3, TFT_WHITE);
  }

  // centered percent number
  gspr.setTextFont(4);
  gspr.setTextColor(color, COL_BG);
  gspr.setTextDatum(MC_DATUM);
  gspr.drawString(String((int)roundf(percent)) + "%", cx, cy);
  gspr.setTextDatum(TL_DATUM);

  gspr.pushSprite(screenCx - cx, screenCy - cy);
}

void drawBar(int x, int y, int w, int h, float pct, uint16_t color) {
  pct = constrain(pct, 0, 100);
  tft.drawRect(x, y, w, h, COL_DIM);
  tft.fillRect(x + 1, y + 1, w - 2, h - 2, COL_PANEL);
  int fillW = (int)((w - 2) * (pct / 100.0));
  if (fillW > 0) tft.fillRect(x + 1, y + 1, fillW, h - 2, color);
}

// ---------------- Decorative icons ----------------
// Each icon is drawn top-left anchored at (x, y). Kept as simple outlined
// shapes so they stay crisp and fast on a low-power SPI display.

void drawChipIcon(int x, int y, uint16_t color) {
  int s = 24;                 // die size
  int bx = x + 6, by = y + 6; // die top-left (leaves room for pins)
  tft.drawRect(bx, by, s, s, color);
  tft.drawRect(bx + 5, by + 5, s - 10, s - 10, color);
  for (int i = 0; i < 3; i++) {
    int off = 4 + i * 8;
    tft.drawFastHLine(x, by + off, 6, color);            // left pins
    tft.drawFastHLine(bx + s, by + off, 6, color);        // right pins
    tft.drawFastVLine(bx + off, y, 6, color);             // top pins
    tft.drawFastVLine(bx + off, by + s, 6, color);        // bottom pins
  }
}

void drawRamIcon(int x, int y, uint16_t color) {
  int w = 42, h = 18;
  tft.drawRoundRect(x, y, w, h, 2, color);
  for (int i = 0; i < 6; i++) {
    tft.drawFastVLine(x + 5 + i * 6, y + 3, h - 6, color);
  }
  tft.fillRect(x + 5, y + h, 4, 4, color);
  tft.fillRect(x + w - 9, y + h, 4, 4, color);
}

void drawGpuIcon(int x, int y, uint16_t color) {
  // card body
  tft.drawRoundRect(x, y, 38, 22, 2, color);
  // cooling fins along the top edge
  for (int i = 0; i < 5; i++) {
    tft.drawFastVLine(x + 4 + i * 7, y - 5, 5, color);
  }
  // fan hub
  tft.drawCircle(x + 28, y + 11, 6, color);
  tft.drawFastVLine(x + 28, y + 6, 10, color);
  tft.drawFastHLine(x + 23, y + 11, 10, color);
}

void drawNetIcon(int x, int y, uint16_t color) {
  int baseY = y + 22;
  tft.fillRect(x, baseY - 5, 6, 5, color);
  tft.fillRect(x + 9, baseY - 10, 6, 10, color);
  tft.fillRect(x + 18, baseY - 15, 6, 15, color);
  tft.fillRect(x + 27, baseY - 20, 6, 20, color);
}

// ============================================================
// STATIC: drawn once per page switch. DYNAMIC: redrawn periodically.
// The ring gauges themselves are handled separately by
// updateGaugeAnimation() at a much faster rate - see loop().
// ============================================================
void drawStaticForPage(Page p) {
  tft.fillScreen(COL_BG);
  switch (p) {
    case PAGE_HOME: drawHomeStatic(); break;
    case PAGE_CPU:  drawCpuStatic();  break;
    case PAGE_RAM:  drawRamStatic();  break;
    case PAGE_GPU:  drawGpuStatic();  break;
    case PAGE_NET:  drawNetStatic();  break;
  }
  drawCornerBrackets(COL_FRAME);
}

void drawHomeStatic() {
  drawTopBar("SYSTEM MONITOR", COL_CPU);

  // beveled "glass panel" cards for each quadrant
  drawCard(4, 23, 152, 104, COL_CPU);
  drawCard(164, 23, 152, 104, COL_RAM);
  drawCard(4, 133, 152, 104, COL_GPU);
  drawCard(164, 133, 152, 104, COL_NET);

  tft.setTextFont(2);
  tft.setTextColor(COL_NET, COL_BG);
  tft.setCursor(170, 138); tft.print("NETWORK");

  // decorative icons beside each gauge / row
  // (the gauges themselves are drawn by updateGaugeAnimation(), not here -
  // all icon/name/temp positions are kept clear of each gauge's ~100x100
  // animated redraw zone so the fast ticker doesn't erase them)
  drawChipIcon(112, 55, COL_CPU);
  drawRamIcon(268, 60, COL_RAM);
  drawGpuIcon(112, 167, COL_GPU);
  drawNetIcon(272, 158, COL_NET);
}

void drawCpuStatic() {
  drawTopBar("CPU DETAIL", COL_CPU);
  drawBackButton();
  drawCard(4, 24, 312, 212, COL_CPU);
  drawChipIcon(265, 95, COL_CPU);
  tft.setTextFont(2);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(140, 30);  tft.print("FREQ");
  tft.setCursor(140, 60);  tft.print("TEMP");
  tft.setCursor(140, 80);  tft.print("MODEL");
  tft.setCursor(10, 150);  tft.print("PER-CORE LOAD");
}

void drawRamStatic() {
  drawTopBar("RAM DETAIL", COL_RAM);
  drawBackButton();
  drawCard(4, 24, 312, 212, COL_RAM);
  drawRamIcon(255, 100, COL_RAM);
  tft.setTextFont(2);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(140, 30); tft.print("USED / TOTAL");
  tft.setCursor(140, 70); tft.print("SWAP");
  tft.setCursor(140, 110); tft.print("TEMP");
  tft.setCursor(10, 150); tft.print("TOP PROCESSES");
}

void drawGpuStatic() {
  drawTopBar("GPU DETAIL", COL_GPU);
  drawBackButton();
  drawCard(4, 24, 312, 212, COL_GPU);
  drawGpuIcon(255, 100, COL_GPU);
  tft.setTextFont(2);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(140, 30); tft.print("TEMP");
  tft.setCursor(140, 60); tft.print("MODEL");
}

void drawNetStatic() {
  drawTopBar("NETWORK DETAIL", COL_NET);
  drawBackButton();
  drawCard(4, 24, 312, 212, COL_NET);
  tft.setTextFont(2);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(10, 30);  tft.print("UPLOAD");
  tft.setCursor(170, 30); tft.print("DOWNLOAD");
  tft.setCursor(10, 150); tft.print("HISTORY (last ~40s)");
  tft.drawRect(8, 168, SCREEN_W - 16, 55, COL_DIM);
}

// ============================================================
void drawDynamicForPage(Page p) {
  drawStatusIndicator();
  switch (p) {
    case PAGE_HOME: drawHomeDynamic(); break;
    case PAGE_CPU:  drawCpuDynamic();  break;
    case PAGE_RAM:  drawRamDynamic();  break;
    case PAGE_GPU:  drawGpuDynamic(); break;
    case PAGE_NET:  drawNetDynamic();  break;
  }
}

void drawHomeDynamic() {
  // Note: the CPU/RAM/GPU gauges + their percent numbers are drawn by
  // updateGaugeAnimation() at a faster rate - not here. Everything below
  // sits outside each gauge's ~100x100 animated redraw zone so the fast
  // ticker doesn't erase it between updates.

  // --- CPU: model name, then temp (bigger font, color-coded) ---
  tft.setTextFont(1);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.fillRect(108, 92, 48, 10, COL_BG);
  tft.setCursor(108, 92);
  tft.print(metrics.cpuName.length() ? shortenForHomeScreen(metrics.cpuName, 8) : String("--"));

  tft.setTextFont(2);
  tft.setTextColor(tempColor(metrics.cpuTemp, CPU_TEMP_WARN_C, CPU_TEMP_HOT_C), COL_BG);
  tft.fillRect(108, 102, 48, 16, COL_BG);
  tft.setCursor(108, 102);
  if (metrics.cpuTemp >= 0) tft.printf("%.0fC", metrics.cpuTemp);
  else tft.print("--");

  // --- RAM: total capacity, shown large ---
  tft.setTextFont(2);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.fillRect(268, 90, 48, 16, COL_BG);
  tft.setCursor(268, 90);
  if (metrics.ramTotalGB > 0) tft.printf("%.0fGB", metrics.ramTotalGB);
  else tft.print("--");

  // --- GPU: model name, then temp (bigger font, color-coded) ---
  tft.setTextFont(1);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.fillRect(108, 193, 48, 10, COL_BG);
  tft.setCursor(108, 193);
  tft.print(metrics.gpuName.length() ? shortenForHomeScreen(metrics.gpuName, 8) : String("--"));

  tft.setTextFont(2);
  tft.setTextColor(tempColor(metrics.gpuTemp, GPU_TEMP_WARN_C, GPU_TEMP_HOT_C), COL_BG);
  tft.fillRect(108, 203, 48, 16, COL_BG);
  tft.setCursor(108, 203);
  if (metrics.gpuTemp >= 0) tft.printf("%.0fC", metrics.gpuTemp);
  else tft.print("--");

  // Net summary
  tft.setTextFont(2);
  tft.setTextColor(COL_NET, COL_BG);
  tft.fillRect(170, 160, 100, 16, COL_BG);
  tft.setCursor(170, 160);
  tft.printf("U %.0f KB/s", metrics.netUp);
  tft.fillRect(170, 178, 100, 16, COL_BG);
  tft.setCursor(170, 178);
  tft.printf("D %.0f KB/s", metrics.netDown);
  tft.setTextFont(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.fillRect(170, 197, 100, 12, COL_BG);
  tft.setCursor(170, 197);
  tft.print("tap for details");
}

void drawCpuDynamic() {
  tft.setTextFont(2);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.fillRect(200, 30, 110, 16, COL_BG);
  tft.setCursor(200, 30);
  if (metrics.cpuFreqMHz > 0) tft.printf("%.0f MHz", metrics.cpuFreqMHz);
  else tft.print("n/a");

  tft.fillRect(200, 60, 110, 16, COL_BG);
  tft.setCursor(200, 60);
  tft.setTextColor(tempColor(metrics.cpuTemp, CPU_TEMP_WARN_C, CPU_TEMP_HOT_C), COL_BG);
  if (metrics.cpuTemp >= 0) tft.printf("%.0f C", metrics.cpuTemp);
  else tft.print("n/a");

  tft.setTextFont(1);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.fillRect(140, 97, 100, 12, COL_BG);
  tft.setCursor(140, 97);
  tft.print(metrics.cpuName.length() ? metrics.cpuName : String("n/a"));

  // Per-core bars
  tft.fillRect(10, 170, 300, 60, COL_BG);
  int cols = metrics.numCores > 4 ? 2 : 1;
  int perCol = cols == 2 ? (metrics.numCores + 1) / 2 : metrics.numCores;
  for (int i = 0; i < metrics.numCores; i++) {
    int col = i / perCol;
    int row = i % perCol;
    int x = 10 + col * 160;
    int y = 170 + row * 12;
    tft.setTextFont(1);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(x, y);
    tft.printf("C%d", i);
    drawBar(x + 18, y - 1, 120, 9, metrics.cores[i], COL_CPU);
  }
}

void drawRamDynamic() {
  tft.setTextFont(2);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.fillRect(140, 45, 170, 16, COL_BG);
  tft.setCursor(140, 45);
  tft.printf("%.1f / %.1f GB", metrics.ramUsedGB, metrics.ramTotalGB);

  tft.fillRect(140, 85, 100, 16, COL_BG);
  tft.setCursor(140, 85);
  tft.printf("%.0f%%", metrics.swapPercent);

  tft.fillRect(140, 127, 100, 16, COL_BG);
  tft.setCursor(140, 127);
  tft.setTextColor(tempColor(metrics.ramTemp, RAM_TEMP_WARN_C, RAM_TEMP_HOT_C), COL_BG);
  if (metrics.ramTemp >= 0) tft.printf("%.0f C", metrics.ramTemp);
  else tft.print("n/a");

  tft.fillRect(10, 165, 300, 65, COL_BG);
  tft.setTextFont(2);
  tft.setTextColor(COL_TEXT, COL_BG);
  for (int i = 0; i < metrics.numProcs; i++) {
    tft.setCursor(10, 165 + i * 20);
    tft.printf("%d. %-16s %.0f MB", i + 1,
               metrics.procs[i].name.c_str(), metrics.procs[i].memMB);
  }
}

void drawGpuDynamic() {
  tft.setTextFont(2);
  tft.fillRect(200, 30, 110, 16, COL_BG);
  tft.setCursor(200, 30);
  tft.setTextColor(tempColor(metrics.gpuTemp, GPU_TEMP_WARN_C, GPU_TEMP_HOT_C), COL_BG);
  if (metrics.gpuTemp >= 0) tft.printf("%.0f C", metrics.gpuTemp);
  else tft.print("n/a");

  tft.setTextFont(1);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.fillRect(140, 77, 100, 12, COL_BG);
  tft.setCursor(140, 77);
  tft.print(metrics.gpuName.length() ? metrics.gpuName : String("n/a"));
}

void drawNetDynamic() {
  tft.setTextFont(4);
  tft.setTextColor(COL_NET, COL_BG);
  tft.fillRect(10, 50, 140, 26, COL_BG);
  tft.setCursor(10, 50);
  tft.printf("%.0f", metrics.netUp);
  tft.setTextFont(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(10, 78);
  tft.print("KB/s");

  tft.setTextFont(4);
  tft.setTextColor(COL_NET, COL_BG);
  tft.fillRect(170, 50, 140, 26, COL_BG);
  tft.setCursor(170, 50);
  tft.printf("%.0f", metrics.netDown);
  tft.setTextFont(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(170, 78);
  tft.print("KB/s");

  tft.setTextFont(2);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.fillRect(10, 100, 300, 16, COL_BG);
  tft.setCursor(10, 100);
  tft.print(metrics.ip.length() ? metrics.ip : String("IP: n/a"));

  // Mini history graph inside the box drawn in drawNetStatic()
  int gx = 10, gy = 170, gw = SCREEN_W - 20, gh = 50;
  tft.fillRect(gx, gy, gw, gh, COL_BG);

  uint16_t maxVal = 1;
  for (int i = 0; i < NET_HISTORY_LEN; i++) {
    maxVal = max(maxVal, netUpHistory[i]);
    maxVal = max(maxVal, netDownHistory[i]);
  }

  for (int i = 1; i < NET_HISTORY_LEN; i++) {
    int idxPrev = (netHistoryIdx + i - 1) % NET_HISTORY_LEN;
    int idxCur  = (netHistoryIdx + i) % NET_HISTORY_LEN;
    int x1 = gx + (int)((float)(i - 1) / (NET_HISTORY_LEN - 1) * gw);
    int x2 = gx + (int)((float)i / (NET_HISTORY_LEN - 1) * gw);

    int yUp1 = gy + gh - (int)((float)netUpHistory[idxPrev] / maxVal * gh);
    int yUp2 = gy + gh - (int)((float)netUpHistory[idxCur] / maxVal * gh);
    tft.drawLine(x1, yUp1, x2, yUp2, COL_NET);

    int yDn1 = gy + gh - (int)((float)netDownHistory[idxPrev] / maxVal * gh);
    int yDn2 = gy + gh - (int)((float)netDownHistory[idxCur] / maxVal * gh);
    tft.drawLine(x1, yDn1, x2, yDn2, COL_CPU);
  }
}
