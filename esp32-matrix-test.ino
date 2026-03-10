#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include "bitmaps.h"
#include "fonts.h"
#include "credentials.h"

#define LED_PIN    14
#define BOOT_BTN   0
#define NUM_LEDS   64
#define BRIGHTNESS 5
#define MATRIX_W   8
#define MATRIX_H   8

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_RGB + NEO_KHZ800);
WiFiMulti wifiMulti;
WebServer server(80);

// --- State ---
uint8_t mode = 1;          // 0=solid,1=rainbow,2=breathe,3=wave,4=matrixRain,5=bitmap,6=text,7=gameOfLife,8=clock,9=cpuTemp,10=music,11=pacman
uint8_t solidR = 255, solidG = 0, solidB = 100;
bool ledsOn = true;
uint16_t animSpeed = 50;   // ms delay for animations (10-500)

// Notification state
bool notifyActive = false;
uint8_t notifyType = 0;       // 0=input,1=done,2=error
unsigned long notifyStart = 0;
bool alertsEnabled = true;
uint16_t notifyDuration = 5000;

// Per-alert-type config: [input, done, error]
uint8_t alertR[3] = {255, 0, 255};
uint8_t alertG[3] = {140, 200, 0};
uint8_t alertB[3] = {0, 0, 0};
uint8_t alertStyle[3] = {0, 1, 2};  // 0=breathe, 1=sweep, 2=blink, 3=pulse

// Bitmap state
uint32_t customBitmap[64];
bool hasCustomBitmap = false;
uint8_t bitmapSource = 0;     // 0=claude,1=heart,2=smiley,3=custom,4=star,5=music,6=ghost,7=sun,8=moon,9=tree,10=skull,11=diamond

// Scrolling text state
char scrollText[128] = "";
uint8_t textR = 255, textG = 136, textB = 0;
int scrollPos = 0;
int scrollSpeed = 80;
unsigned long lastScroll = 0;

// Matrix Rain state
uint8_t rainDrops[MATRIX_W];
uint8_t rainLen[MATRIX_W];
uint8_t rainSpeed[MATRIX_W];
uint8_t rainTick[MATRIX_W];

// Game of Life state
uint8_t golGrid[64];
uint8_t golNext[64];
uint16_t golGen = 0;
uint32_t golLastHash = 0;
uint8_t golStaleCount = 0;

// Rotation: 0=Normal, 1=90°, 2=180°, 3=270°, 4=Flip-H, 5=Flip-V
uint8_t rotation = 0;

// NTP Clock
bool clockMode = false;
unsigned long ntpLastSync = 0;
long utcOffset = 3 * 3600; // UTC+3 Turkey
time_t currentTime = 0;

// CPU Temperature
unsigned long lastTempRead = 0;
float cpuTemp = 0;

// Music Reactive
uint32_t musicFrame[64];
unsigned long lastMusicFrame = 0;

// Pacman effect (mode 11)
int8_t pacX = -4;           // pacman X position (left edge of sprite)
uint8_t pacMouth = 0;       // 0=closed, 1=half, 2=open, 3=half (cycles)
bool ghostColorAlt = false;
bool ghostWave = false;
bool pacDots[8];            // dots at columns 0-7
unsigned long lastPacFrame = 0;

// --- Base64 decode ---
int b64decode(const char* in, int inLen, uint8_t* out, int outMax) {
  static const int8_t lut[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
  };
  int j = 0;
  for (int i = 0; i + 3 < inLen && j < outMax; i += 4) {
    uint32_t n = ((uint32_t)lut[(uint8_t)in[i]] << 18) | ((uint32_t)lut[(uint8_t)in[i+1]] << 12) |
                 ((uint32_t)lut[(uint8_t)in[i+2]] << 6) | (uint32_t)lut[(uint8_t)in[i+3]];
    if (j < outMax) out[j++] = (n >> 16) & 0xFF;
    if (j < outMax && in[i+2] != '=') out[j++] = (n >> 8) & 0xFF;
    if (j < outMax && in[i+3] != '=') out[j++] = n & 0xFF;
  }
  return j;
}

// --- Helpers ---
int xyToIndex(int x, int y) {
  if (x < 0 || x >= MATRIX_W || y < 0 || y >= MATRIX_H) return -1;
  int newX = x, newY = y;
  switch (rotation) {
    case 1: newX = MATRIX_H - 1 - y; newY = x; break;                     // 90° CW
    case 2: newX = MATRIX_W - 1 - x; newY = MATRIX_H - 1 - y; break;     // 180°
    case 3: newX = y; newY = MATRIX_W - 1 - x; break;                     // 270° CW
    case 4: newX = MATRIX_W - 1 - x; break;                               // flip H
    case 5: newY = MATRIX_H - 1 - y; break;                               // flip V
  }
  return newY * MATRIX_W + newX;
}

// Map flat index through rotation
int rotIndex(int i) {
  int x = i % MATRIX_W;
  int y = i / MATRIX_W;
  return xyToIndex(x, y);
}

void setPixelXY(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  int idx = xyToIndex(x, y);
  if (idx >= 0) strip.setPixelColor(idx, strip.Color(r, g, b));
}

// --- Notification animations ---
void animBreathe(uint8_t r, uint8_t g, uint8_t b) {
  unsigned long elapsed = millis() - notifyStart;
  float phase = elapsed * 0.005;
  float breath = (sin(phase) + 1.0) / 2.0;
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(r * breath, g * breath, b * breath));
  strip.show();
}

void animSweep(uint8_t r, uint8_t g, uint8_t b) {
  unsigned long elapsed = millis() - notifyStart;
  int sweepPos = (elapsed / 60) % (MATRIX_W + 4);
  for (int y = 0; y < MATRIX_H; y++) {
    for (int x = 0; x < MATRIX_W; x++) {
      int dist = abs(x - sweepPos);
      float bright = dist < 3 ? (1.0 - dist * 0.33) : 0;
      setPixelXY(x, y, r * bright, g * bright, b * bright);
    }
  }
  strip.show();
}

void animBlink(uint8_t r, uint8_t g, uint8_t b) {
  unsigned long elapsed = millis() - notifyStart;
  bool on = (elapsed / 200) % 2 == 0;
  uint32_t c = on ? strip.Color(r, g, b) : 0;
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, c);
  strip.show();
}

void animPulse(uint8_t r, uint8_t g, uint8_t b) {
  unsigned long elapsed = millis() - notifyStart;
  float phase = (elapsed % 600) / 600.0;
  float bright = phase < 0.15 ? (phase / 0.15) : (1.0 - (phase - 0.15) / 0.85);
  bright = bright * bright; // ease-out curve
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(r * bright, g * bright, b * bright));
  strip.show();
}

void runNotifyAnim() {
  uint8_t r = alertR[notifyType], g = alertG[notifyType], b = alertB[notifyType];
  switch (alertStyle[notifyType]) {
    case 0: animBreathe(r, g, b); break;
    case 1: animSweep(r, g, b); break;
    case 2: animBlink(r, g, b); break;
    case 3: animPulse(r, g, b); break;
    default: animBreathe(r, g, b); break;
  }
}

// --- Mode functions ---
void modeSolid() {
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(solidR, solidG, solidB));
  strip.show();
}

void modeRainbow() {
  static uint16_t hue = 0;
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame < animSpeed) return;
  lastFrame = millis();
  for (int i = 0; i < NUM_LEDS; i++) {
    int pixelHue = hue + (i * 65536L / NUM_LEDS);
    int ri = rotIndex(i);
    if (ri >= 0) strip.setPixelColor(ri, strip.gamma32(strip.ColorHSV(pixelHue)));
  }
  strip.show();
  hue += 256;
}

void modeBreathe() {
  static uint16_t phase = 0;
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame < animSpeed) return;
  lastFrame = millis();
  float breath = (sin(phase * 0.01) + 1.0) / 2.0;
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(solidR * breath, solidG * breath, solidB * breath));
  strip.show();
  phase++;
}

void modeWave() {
  static uint16_t offset = 0;
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame < animSpeed) return;
  lastFrame = millis();
  for (int i = 0; i < NUM_LEDS; i++) {
    float wave = (sin((i % 8 + i / 8 + offset) * 0.5) + 1.0) / 2.0;
    strip.setPixelColor(i, strip.Color(solidR * wave, solidG * wave, solidB * wave));
  }
  strip.show();
  offset++;
}

void modeMatrixRain() {
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame < animSpeed) return;
  lastFrame = millis();
  // Shift everything down, fade
  for (int y = 0; y < MATRIX_H - 1; y++) {
    for (int x = 0; x < MATRIX_W; x++) {
      uint32_t c = strip.getPixelColor(xyToIndex(x, y + 1));
      uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
      g = g > 40 ? g - 40 : 0;
      r = r > 40 ? r - 40 : 0;
      setPixelXY(x, y, r, g, b);
    }
  }
  // New drops at top row
  for (int x = 0; x < MATRIX_W; x++) {
    rainTick[x]++;
    if (rainTick[x] >= rainSpeed[x]) {
      rainTick[x] = 0;
      if (random(0, 3) == 0) {
        setPixelXY(x, MATRIX_H - 1, 180, 255, 180); // bright head
      } else {
        uint32_t c = strip.getPixelColor(xyToIndex(x, MATRIX_H - 1));
        uint8_t g = ((c >> 8) & 0xFF);
        g = g > 50 ? g - 50 : 0;
        setPixelXY(x, MATRIX_H - 1, 0, g, 0);
      }
    }
  }
  strip.show();
}

void modeBitmap() {
  const uint8_t* src = NULL;
  switch (bitmapSource) {
    case 0: src = bmpClaude; break;
    case 1: src = bmpHeart; break;
    case 2: src = bmpSmiley; break;
    case 4: src = bmpStar; break;
    case 5: src = bmpMusic; break;
    case 6: src = bmpGhost; break;
    case 7: src = bmpSun; break;
    case 8: src = bmpMoon; break;
    case 9: src = bmpTree; break;
    case 10: src = bmpSkull; break;
    case 11: src = bmpDiamond; break;
  }

  if (src != NULL) {
    for (int i = 0; i < 64; i++) {
      uint8_t r = pgm_read_byte(&src[i * 3]);
      uint8_t g = pgm_read_byte(&src[i * 3 + 1]);
      uint8_t b = pgm_read_byte(&src[i * 3 + 2]);
      int ri = rotIndex(i);
      if (ri >= 0) strip.setPixelColor(ri, strip.Color(r, g, b));
    }
  } else if (bitmapSource == 3 && hasCustomBitmap) {
    for (int i = 0; i < 64; i++) {
      uint32_t c = customBitmap[i];
      int ri = rotIndex(i);
      if (ri >= 0) strip.setPixelColor(ri, strip.Color((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
    }
  }
  strip.show();
}

void modeScrollText() {
  if (strlen(scrollText) == 0) return;
  unsigned long now = millis();
  if (now - lastScroll < (unsigned long)scrollSpeed) return;
  lastScroll = now;

  strip.clear();
  int textLen = strlen(scrollText);
  int totalWidth = textLen * (FONT_WIDTH + 1);

  for (int cx = 0; cx < MATRIX_W; cx++) {
    int charCol = cx + scrollPos;
    if (charCol < 0) continue;
    int charIdx = charCol / (FONT_WIDTH + 1);
    int col = charCol % (FONT_WIDTH + 1);
    if (col >= FONT_WIDTH) continue; // gap between chars
    if (charIdx >= textLen) continue;

    char ch = scrollText[charIdx];
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR) continue;
    uint8_t colData = pgm_read_byte(&font5x7[ch - FONT_FIRST_CHAR][col]);

    for (int row = 0; row < 7 && row < MATRIX_H; row++) {
      if (colData & (1 << row)) {
        setPixelXY(cx, row, textR, textG, textB);
      }
    }
  }
  strip.show();
  scrollPos++;
  if (scrollPos >= totalWidth) scrollPos = -MATRIX_W;
}

void modeGameOfLife() {
  static unsigned long lastGol = 0;
  if (millis() - lastGol < (unsigned long)animSpeed * 4) return;
  lastGol = millis();

  // Compute next generation
  for (int y = 0; y < MATRIX_H; y++) {
    for (int x = 0; x < MATRIX_W; x++) {
      int neighbors = 0;
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          if (dx == 0 && dy == 0) continue;
          int nx = (x + dx + MATRIX_W) % MATRIX_W;
          int ny = (y + dy + MATRIX_H) % MATRIX_H;
          neighbors += golGrid[ny * MATRIX_W + nx];
        }
      }
      int idx = y * MATRIX_W + x;
      if (golGrid[idx]) {
        golNext[idx] = (neighbors == 2 || neighbors == 3) ? 1 : 0;
      } else {
        golNext[idx] = (neighbors == 3) ? 1 : 0;
      }
    }
  }

  // Check for stagnation
  uint32_t hash = 0;
  for (int i = 0; i < 64; i++) hash = hash * 31 + golNext[i];
  if (hash == golLastHash) golStaleCount++;
  else golStaleCount = 0;
  golLastHash = hash;

  memcpy(golGrid, golNext, 64);
  golGen++;

  // Reseed if stale
  if (golStaleCount > 5 || golGen > 500) {
    for (int i = 0; i < 64; i++) golGrid[i] = random(0, 2);
    golGen = 0;
    golStaleCount = 0;
  }

  // Render
  for (int i = 0; i < 64; i++) {
    int ri = rotIndex(i);
    if (ri >= 0) {
      if (golGrid[i])
        strip.setPixelColor(ri, strip.Color(0, 200, 100));
      else
        strip.setPixelColor(ri, 0);
    }
  }
  strip.show();
}

void modeClock() {
  // Sync NTP every 10 minutes
  if (millis() - ntpLastSync > 600000 || ntpLastSync == 0) {
    configTime(utcOffset, 0, "pool.ntp.org");
    ntpLastSync = millis();
  }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  char buf[8];
  sprintf(buf, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  if (strcmp(scrollText, buf) != 0) {
    strcpy(scrollText, buf);
    scrollPos = -MATRIX_W;
    textR = 0; textG = 150; textB = 255;
  }
  modeScrollText();
}

void modeMusicReactive() {
  // Show musicFrame contents, fade out if no data for 500ms
  unsigned long elapsed = millis() - lastMusicFrame;
  float fade = 1.0;
  if (elapsed > 500) fade = max(0.0, 1.0 - (elapsed - 500) / 1000.0);

  for (int i = 0; i < 64; i++) {
    uint32_t c = musicFrame[i];
    uint8_t r = ((c >> 16) & 0xFF) * fade;
    uint8_t g = ((c >> 8) & 0xFF) * fade;
    uint8_t b = (c & 0xFF) * fade;
    int ri = rotIndex(i);
    if (ri >= 0) strip.setPixelColor(ri, strip.Color(r, g, b));
  }
  strip.show();
}

void modeCpuTemp() {
  if (millis() - lastTempRead > 2000 || lastTempRead == 0) {
    lastTempRead = millis();
    cpuTemp = temperatureRead();
    char buf[8];
    int t = (int)(cpuTemp + 0.5);
    sprintf(buf, "%dC", t);
    if (strcmp(scrollText, buf) != 0) {
      strcpy(scrollText, buf);
      scrollPos = -MATRIX_W;
    }
    // Color-code by temperature
    if (cpuTemp < 45) { textR = 0; textG = 100; textB = 255; }        // blue (cool)
    else if (cpuTemp < 60) { textR = 0; textG = 220; textB = 80; }    // green (normal)
    else if (cpuTemp < 75) { textR = 255; textG = 200; textB = 0; }   // yellow (warm)
    else { textR = 255; textG = 0; textB = 0; }                        // red (hot)
  }
  modeScrollText();
}

void modePacman() {
  unsigned long now = millis();
  if (now - lastPacFrame < animSpeed * 3) return;
  lastPacFrame = now;

  strip.clear();

  // Advance state
  pacX++;
  pacMouth = (pacMouth + 1) % 4;
  ghostWave = !ghostWave;

  // Mouth: 0→closed, 1→half, 2→open, 3→half (map 3→1)
  uint8_t mf = pacMouth;
  if (mf == 3) mf = 1;

  int8_t gx = pacX - 6; // ghost 6 pixels behind

  // Wrap around
  if (pacX > MATRIX_W + 4) {
    pacX = -4;
    gx = pacX - 6;
    for (int i = 0; i < 8; i++) pacDots[i] = false;
    ghostColorAlt = !ghostColorAlt;
  }

  // Eat dots — pacman eats when its left edge reaches dot column
  for (int i = 0; i < 8; i++) {
    if (!pacDots[i] && pacX >= i) pacDots[i] = true;
  }

  // --- Draw dots (row 4, every column that isn't eaten) ---
  for (int i = 0; i < 8; i++) {
    if (!pacDots[i]) setPixelXY(i, 4, 255, 185, 80);
  }

  // --- Draw maze border hints (top & bottom rows, dark blue) ---
  for (int x = 0; x < 8; x++) {
    setPixelXY(x, 0, 0, 0, 30);
    setPixelXY(x, 7, 0, 0, 30);
  }

  // --- Draw Pacman (4x4 sprite, rows 2-5) ---
  // Closed:  .YY.  Half:   .YY.  Open:   .YY.
  //          YYYY          YYY.          YY..
  //          YYYY          YYY.          YY..
  //          .YY.          .YY.          .YY.
  for (int dy = 0; dy < 4; dy++) {
    for (int dx = 0; dx < 4; dx++) {
      int sx = pacX + dx;
      int sy = 2 + dy;
      if (sx < 0 || sx >= MATRIX_W) continue;

      // Circle: corners empty
      if ((dy == 0 || dy == 3) && (dx == 0 || dx == 3)) continue;

      // Mouth cutout (right side columns)
      if (mf == 2) { // wide open: remove cols 2-3 on middle rows
        if (dx >= 2 && (dy == 1 || dy == 2)) continue;
      } else if (mf == 1) { // half: remove col 3 on middle rows
        if (dx == 3 && (dy == 1 || dy == 2)) continue;
      }
      // mf==0: closed, no cutout

      setPixelXY(sx, sy, 255, 255, 0);
    }
  }
  // Eye at top-left of body (offset 1,0 from pacX)
  if (pacX + 1 >= 0 && pacX + 1 < MATRIX_W) setPixelXY(pacX + 1, 2, 0, 0, 0);

  // --- Draw Ghost (4x4 sprite, rows 2-5) ---
  // Shape:  .GG.
  //         GGGG
  //         GWGW  (W=white eyes at dx 1,3)
  //         G.G.  or .G.G (wavy alternates)
  uint8_t gR, gG, gB;
  if (ghostColorAlt) { gR = 0; gG = 200; gB = 200; }
  else { gR = 255; gG = 0; gB = 0; }

  for (int dy = 0; dy < 4; dy++) {
    for (int dx = 0; dx < 4; dx++) {
      int sx = gx + dx;
      int sy = 2 + dy;
      if (sx < 0 || sx >= MATRIX_W) continue;

      // Top corners empty
      if (dy == 0 && (dx == 0 || dx == 3)) continue;

      // Wavy bottom
      if (dy == 3) {
        if (ghostWave && (dx == 1 || dx == 3)) continue;
        if (!ghostWave && (dx == 0 || dx == 2)) continue;
      }

      // Eyes on row 2 (dy==2), columns 1 and 3
      if (dy == 2 && (dx == 1 || dx == 3)) {
        setPixelXY(sx, sy, 255, 255, 255);
        continue;
      }

      setPixelXY(sx, sy, gR, gG, gB);
    }
  }

  strip.show();
}

// --- Web Handlers ---
void handleColor() {
  String hex = server.arg("hex");
  if (hex.length() == 6) {
    solidR = strtol(hex.substring(0, 2).c_str(), NULL, 16);
    solidG = strtol(hex.substring(2, 4).c_str(), NULL, 16);
    solidB = strtol(hex.substring(4, 6).c_str(), NULL, 16);
    mode = 0;
    ledsOn = true;
  }
  server.send(200, "text/plain", "ok");
}

void handleMode() {
  mode = server.arg("m").toInt();
  ledsOn = true;
  if (mode == 4) { // init matrix rain
    for (int x = 0; x < MATRIX_W; x++) {
      rainSpeed[x] = random(1, 4);
      rainTick[x] = 0;
    }
  }
  if (mode == 7) { // init game of life
    for (int i = 0; i < 64; i++) golGrid[i] = random(0, 2);
    golGen = 0; golStaleCount = 0;
  }
  if (mode == 11) { // init pacman
    pacX = -4;
    pacMouth = 0;
    ghostColorAlt = false;
    ghostWave = false;
    for (int i = 0; i < 8; i++) pacDots[i] = false;
    lastPacFrame = 0;
  }
  server.send(200, "text/plain", "ok");
}

void handleBrightness() {
  int v = constrain(server.arg("v").toInt(), 1, 30);
  strip.setBrightness(v);
  server.send(200, "text/plain", "ok");
}

void handleToggle() {
  ledsOn = !ledsOn;
  if (!ledsOn) { strip.clear(); strip.show(); }
  server.send(200, "text/plain", "ok");
}

void handleNotify() {
  if (!alertsEnabled) { server.send(200, "text/plain", "alerts off"); return; }
  String type = server.arg("type");
  if (type == "clear") {
    notifyActive = false;
    server.send(200, "text/plain", "ok");
    return;
  }
  if (type == "input") notifyType = 0;
  else if (type == "done") notifyType = 1;
  else if (type == "error") notifyType = 2;
  else { server.send(400, "text/plain", "bad type"); return; }

  // Optional per-call overrides
  String color = server.arg("color");
  if (color.length() == 6) {
    alertR[notifyType] = strtol(color.substring(0, 2).c_str(), NULL, 16);
    alertG[notifyType] = strtol(color.substring(2, 4).c_str(), NULL, 16);
    alertB[notifyType] = strtol(color.substring(4, 6).c_str(), NULL, 16);
  }
  String style = server.arg("style");
  if (style == "breathe") alertStyle[notifyType] = 0;
  else if (style == "sweep") alertStyle[notifyType] = 1;
  else if (style == "blink") alertStyle[notifyType] = 2;
  else if (style == "pulse") alertStyle[notifyType] = 3;

  String dur = server.arg("duration");
  if (dur.length() > 0) notifyDuration = constrain(dur.toInt(), 1000, 30000);

  notifyActive = true;
  notifyStart = millis();
  server.send(200, "text/plain", "ok");
}

void handleAlertConfig() {
  String type = server.arg("type");
  int idx = -1;
  if (type == "input") idx = 0;
  else if (type == "done") idx = 1;
  else if (type == "error") idx = 2;
  if (idx < 0) { server.send(400, "text/plain", "bad type"); return; }

  String color = server.arg("color");
  if (color.length() == 6) {
    alertR[idx] = strtol(color.substring(0, 2).c_str(), NULL, 16);
    alertG[idx] = strtol(color.substring(2, 4).c_str(), NULL, 16);
    alertB[idx] = strtol(color.substring(4, 6).c_str(), NULL, 16);
  }
  String style = server.arg("style");
  if (style == "breathe") alertStyle[idx] = 0;
  else if (style == "sweep") alertStyle[idx] = 1;
  else if (style == "blink") alertStyle[idx] = 2;
  else if (style == "pulse") alertStyle[idx] = 3;

  server.send(200, "text/plain", "ok");
}

void handleBitmap() {
  String name = server.arg("name");
  if (name == "claude") bitmapSource = 0;
  else if (name == "heart") bitmapSource = 1;
  else if (name == "smiley") bitmapSource = 2;
  else if (name == "star") bitmapSource = 4;
  else if (name == "music") bitmapSource = 5;
  else if (name == "ghost") bitmapSource = 6;
  else if (name == "sun") bitmapSource = 7;
  else if (name == "moon") bitmapSource = 8;
  else if (name == "tree") bitmapSource = 9;
  else if (name == "skull") bitmapSource = 10;
  else if (name == "diamond") bitmapSource = 11;
  mode = 5;
  ledsOn = true;
  server.send(200, "text/plain", "ok");
}

void handleUploadBitmap() {
  String body = server.arg("plain");
  uint8_t buf[192];
  int decoded = b64decode(body.c_str(), body.length(), buf, 192);
  if (decoded >= 192) {
    for (int i = 0; i < 64; i++) {
      customBitmap[i] = ((uint32_t)buf[i*3] << 16) | ((uint32_t)buf[i*3+1] << 8) | buf[i*3+2];
    }
    hasCustomBitmap = true;
    bitmapSource = 3;
    mode = 5;
    ledsOn = true;
    server.send(200, "text/plain", "ok");
  } else {
    server.send(400, "text/plain", "need 192 bytes b64");
  }
}

void handleText() {
  String msg = server.arg("msg");
  String color = server.arg("color");
  String spd = server.arg("speed");

  msg.toCharArray(scrollText, sizeof(scrollText));
  scrollPos = -MATRIX_W;

  if (color.length() == 6) {
    textR = strtol(color.substring(0, 2).c_str(), NULL, 16);
    textG = strtol(color.substring(2, 4).c_str(), NULL, 16);
    textB = strtol(color.substring(4, 6).c_str(), NULL, 16);
  }
  if (spd.length() > 0) scrollSpeed = constrain(spd.toInt(), 20, 500);

  mode = 6;
  ledsOn = true;
  server.send(200, "text/plain", "ok");
}

void handleAlerts() {
  String v = server.arg("v");
  alertsEnabled = (v == "1");
  server.send(200, "text/plain", alertsEnabled ? "on" : "off");
}

void handleRotation() {
  rotation = constrain(server.arg("v").toInt(), 0, 5);
  server.send(200, "text/plain", "ok");
}

void handleSpeed() {
  animSpeed = constrain(server.arg("v").toInt(), 10, 500);
  scrollSpeed = animSpeed;
  server.send(200, "text/plain", "ok");
}

void handleStatus() {
  const char* modeNames[] = {"Solid","Rainbow","Breathe","Wave","Matrix Rain","Bitmap","Text","Game of Life","Clock","CPU Temp","Music","Pacman"};
  String modeName = (mode <= 11) ? modeNames[mode] : "Unknown";
  String json = "{\"mode\":" + String(mode) + ",\"modeName\":\"" + modeName + "\",\"on\":" + String(ledsOn ? "true" : "false") +
    ",\"alerts\":" + String(alertsEnabled ? "true" : "false") +
    ",\"rotation\":" + String(rotation) +
    ",\"speed\":" + String(animSpeed) +
    ",\"cpuTemp\":" + String(temperatureRead(), 1) +
    ",\"wifi\":\"" + WiFi.SSID() + "\"" +
    ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
    ",\"rssi\":" + String(WiFi.RSSI()) + "}";
  server.send(200, "application/json", json);
}

void handleMusicFrame() {
  String body = server.arg("plain");
  uint8_t buf[192];
  int decoded = b64decode(body.c_str(), body.length(), buf, 192);
  if (decoded >= 192) {
    for (int i = 0; i < 64; i++) {
      musicFrame[i] = ((uint32_t)buf[i*3] << 16) | ((uint32_t)buf[i*3+1] << 8) | buf[i*3+2];
    }
    lastMusicFrame = millis();
    if (mode != 10) { mode = 10; ledsOn = true; }
    server.send(200, "text/plain", "ok");
  } else {
    server.send(400, "text/plain", "need 192 bytes b64");
  }
}

void handleCpuTemp() {
  cpuTemp = temperatureRead();
  String json = "{\"temp\":" + String(cpuTemp, 1) + "}";
  server.send(200, "application/json", json);
}

void handleLedState() {
  String out = "[";
  for (int i = 0; i < 64; i++) {
    uint32_t c = strip.getPixelColor(i);
    if (i > 0) out += ",";
    out += String(c);
  }
  out += "]";
  server.send(200, "application/json", out);
}

// --- HTML UI ---
const char HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Matrix</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#111;color:#fff;display:flex;justify-content:center;padding:16px}
.c{max-width:420px;width:100%}
h1{text-align:center;margin-bottom:20px;font-size:1.4em}
h2{font-size:.9em;color:#888;margin:18px 0 10px;text-transform:uppercase;letter-spacing:1px}
.color-pick{width:100%;height:70px;border:none;border-radius:12px;cursor:pointer;margin-bottom:14px}
.presets{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-bottom:14px}
.preset{width:100%;aspect-ratio:1;border-radius:50%;border:3px solid #333;cursor:pointer;transition:.2s}
.preset:hover{transform:scale(1.15);border-color:#fff}
.modes{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-bottom:14px}
.btn{padding:12px 4px;border:2px solid #333;border-radius:10px;background:#222;color:#fff;font-size:.85em;cursor:pointer;text-align:center;transition:.2s}
.btn.active{border-color:#0af;color:#0af}
.btn:active{transform:scale(.95)}
.slider-group{margin-bottom:14px}
.slider-group label{display:block;margin-bottom:6px;color:#888;font-size:.8em}
input[type=range]{width:100%;accent-color:#0af;height:28px}
.off-btn{width:100%;padding:14px;border-radius:10px;border:2px solid #c00;background:#200;color:#f66;font-size:1em;cursor:pointer;font-weight:600}
.off-btn.is-off{border-color:#0a0;background:#020;color:#0f0}
.status{text-align:center;margin-top:10px;color:#888;font-size:.8em}
.text-row{display:flex;gap:8px;margin-bottom:8px}
.text-row input[type=text]{flex:1;padding:10px;border-radius:8px;border:2px solid #333;background:#222;color:#fff;font-size:.9em}
.text-row input[type=color]{width:44px;height:40px;border:none;border-radius:8px;cursor:pointer}
.text-row button{padding:10px 16px;border-radius:8px;border:2px solid #0af;background:#022;color:#0af;cursor:pointer;font-weight:600}
.toggle-row{display:flex;justify-content:space-between;align-items:center;padding:10px 14px;background:#1a1a1a;border-radius:10px;margin-bottom:8px}
.toggle-row span{font-size:.9em}
.sw{position:relative;width:44px;height:24px}
.sw input{opacity:0;width:0;height:0}
.sw .sl{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#333;border-radius:24px;transition:.3s}
.sw .sl:before{content:"";position:absolute;height:18px;width:18px;left:3px;bottom:3px;background:#888;border-radius:50%;transition:.3s}
.sw input:checked+.sl{background:#0a4}
.sw input:checked+.sl:before{transform:translateX(20px);background:#fff}
.upload-area{border:2px dashed #333;border-radius:10px;padding:20px;text-align:center;cursor:pointer;color:#666;font-size:.85em;margin-bottom:8px;transition:.2s}
.upload-area:hover,.upload-area.drag{border-color:#0af;color:#0af}
.preview-grid{display:grid;grid-template-columns:repeat(8,1fr);gap:1px;width:160px;height:160px;margin:8px auto;background:#000;border-radius:4px;overflow:hidden}
.preview-grid div{background:#000}
.upload-badge{display:none;text-align:center;color:#0f0;font-size:.8em;margin-top:4px}
.pixel-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-bottom:14px}
.speed-labels{display:flex;justify-content:space-between;font-size:.7em;color:#555;margin-bottom:2px}
.dash{position:fixed;top:0;left:0;right:0;z-index:999;background:#111e;backdrop-filter:blur(10px);border-bottom:1px solid #222;padding:6px 16px;display:flex;justify-content:center;gap:16px;font-size:.72em;color:#888}
.dash span{display:flex;align-items:center;gap:4px}
.dash .dot{width:6px;height:6px;border-radius:50%;display:inline-block}
.rot-btns{display:flex;gap:4px;flex-wrap:wrap}
.rot-btns .btn{padding:6px 8px;font-size:.75em}
</style>
</head>
<body>
<div class="dash">
<span><i class="dot" id="dWifi" style="background:#0a0"></i> <span id="dSsid">--</span></span>
<span id="dTemp">--</span>
<span id="dRssi">--</span>
<span id="dMode">--</span>
</div>
<div class="c" style="padding-top:30px">
<h1>LED Matrix</h1>

<input type="color" id="cp" class="color-pick" value="#ff0064">

<div class="presets">
<div class="preset" style="background:#ff0000" onclick="sc('ff0000')"></div>
<div class="preset" style="background:#00ff00" onclick="sc('00ff00')"></div>
<div class="preset" style="background:#0000ff" onclick="sc('0000ff')"></div>
<div class="preset" style="background:#ffffff" onclick="sc('ffffff')"></div>
<div class="preset" style="background:#8000ff" onclick="sc('8000ff')"></div>
<div class="preset" style="background:#ff5000" onclick="sc('ff5000')"></div>
<div class="preset" style="background:#00ffff" onclick="sc('00ffff')"></div>
<div class="preset" style="background:#ff0064" onclick="sc('ff0064')"></div>
</div>

<h2>Effects</h2>
<div class="modes">
<div class="btn" id="m0" onclick="sm(0)">Solid</div>
<div class="btn active" id="m1" onclick="sm(1)">Rainbow</div>
<div class="btn" id="m2" onclick="sm(2)">Breathe</div>
<div class="btn" id="m3" onclick="sm(3)">Wave</div>
<div class="btn" id="m4" onclick="sm(4)">Matrix</div>
<div class="btn" id="m7" onclick="sm(7)">Life</div>
<div class="btn" id="m8" onclick="sm(8)">Clock</div>
<div class="btn" id="m9" onclick="sm(9)">Temp</div>
<div class="btn" id="m10" onclick="startMusic()">Music</div>
<div class="btn" id="m11" onclick="sm(11)">Pacman</div>
</div>
<div id="tempBox" style="display:none;text-align:center;padding:14px;background:#1a1a1a;border-radius:10px;margin-bottom:14px">
<span style="font-size:2.2em;font-weight:700" id="tempVal">--</span><span style="font-size:1em;color:#888">&#176;C</span>
</div>
<div id="musicBox" style="display:none;text-align:center;padding:14px;background:#1a1a1a;border-radius:10px;margin-bottom:14px">
<canvas id="musicVis" width="160" height="160" style="border-radius:6px;background:#000"></canvas>
<div id="musicStyles" style="display:flex;gap:4px;margin-top:8px;justify-content:center">
<div class="btn active" id="vs0" onclick="setVis(0)" style="padding:6px 10px;font-size:.7em">Bars</div>
<div class="btn" id="vs1" onclick="setVis(1)" style="padding:6px 10px;font-size:.7em">Wave</div>
<div class="btn" id="vs2" onclick="setVis(2)" style="padding:6px 10px;font-size:.7em">Rings</div>
</div>
<div style="color:#888;font-size:.75em;margin-top:6px" id="musicStatus">Tap Music to start</div>
</div>

<h2>Pixel Art</h2>
<div class="pixel-grid">
<div class="btn" onclick="bmp('claude')">Claude</div>
<div class="btn" onclick="bmp('heart')">Heart</div>
<div class="btn" onclick="bmp('smiley')">Smiley</div>
<div class="btn" onclick="bmp('star')">Star</div>
<div class="btn" onclick="bmp('ghost')">Ghost</div>
<div class="btn" onclick="bmp('sun')">Sun</div>
<div class="btn" onclick="bmp('moon')">Moon</div>
<div class="btn" onclick="bmp('tree')">Tree</div>
<div class="btn" onclick="bmp('skull')">Skull</div>
<div class="btn" onclick="bmp('diamond')">Diamond</div>
</div>
<div class="upload-area" id="upArea" onclick="document.getElementById('fileIn').click()">
<div style="font-size:1.6em;margin-bottom:6px">&#x1F4F7;</div>
<div>Drop image or tap to upload</div>
<div id="fileName" style="color:#0af;margin-top:4px;font-size:.8em"></div>
<input type="file" id="fileIn" accept="image/*" style="display:none">
</div>
<div class="preview-grid" id="preview" style="display:none"></div>
<div class="upload-badge" id="uploadBadge">&#x2714; Uploaded!</div>

<h2>Scrolling Text</h2>
<div class="text-row">
<input type="text" id="textMsg" placeholder="Type message...">
<input type="color" id="textClr" value="#ff8800">
<button onclick="sendText()">Go</button>
</div>
<div class="slider-group">
<label>Scroll Speed: <span id="sv">80</span>ms</label>
<input type="range" id="textSpd" min="20" max="300" value="80"
  oninput="document.getElementById('sv').textContent=this.value">
</div>

<h2>Settings</h2>
<div class="slider-group">
<label>Brightness: <span id="bv">5</span></label>
<input type="range" id="br" min="1" max="30" value="5"
  oninput="document.getElementById('bv').textContent=this.value"
  onchange="send('/brightness?v='+this.value)">
</div>

<div class="slider-group">
<label>Animation Speed</label>
<div class="speed-labels"><span>Slow</span><span>Fast</span></div>
<input type="range" id="aspd" min="1" max="100" value="92"
  onchange="send('/speed?v='+(510-this.value*5))">
</div>

<div class="toggle-row">
<span>Rotation</span>
<div class="rot-btns">
<button class="btn" onclick="send('/rotation?v=0')">Normal</button>
<button class="btn" onclick="send('/rotation?v=1')">90&deg;</button>
<button class="btn" onclick="send('/rotation?v=2')">180&deg;</button>
<button class="btn" onclick="send('/rotation?v=3')">270&deg;</button>
<button class="btn" onclick="send('/rotation?v=4')">Flip H</button>
<button class="btn" onclick="send('/rotation?v=5')">Flip V</button>
</div>
</div>

<div class="toggle-row">
<span>Claude Alerts</span>
<label class="sw"><input type="checkbox" id="alertTog" checked onchange="send('/alerts?v='+(this.checked?'1':'0'))"><span class="sl"></span></label>
</div>
<div class="toggle-row">
<span>Test Alert</span>
<div>
<button class="btn" style="padding:6px 10px;font-size:.75em" onclick="send('/notify?type=input')">Input</button>
<button class="btn" style="padding:6px 10px;font-size:.75em" onclick="send('/notify?type=done')">Done</button>
<button class="btn" style="padding:6px 10px;font-size:.75em" onclick="send('/notify?type=error')">Error</button>
</div>
</div>
<div class="toggle-row" onclick="document.getElementById('alertCfg').style.display=document.getElementById('alertCfg').style.display=='none'?'block':'none'" style="cursor:pointer">
<span>Alert Settings</span><span style="color:#555;font-size:.75em">tap to expand</span>
</div>
<div id="alertCfg" style="display:none;background:#1a1a1a;border-radius:10px;padding:12px;margin-bottom:8px">
<div style="display:flex;gap:8px;align-items:center;margin-bottom:8px">
<span style="width:50px;font-size:.8em">Input</span>
<input type="color" id="acI" value="#ff8c00" style="width:36px;height:28px;border:none;border-radius:4px;cursor:pointer">
<select id="asI" style="flex:1;padding:6px;border-radius:6px;border:1px solid #333;background:#222;color:#fff;font-size:.8em">
<option value="breathe" selected>Breathe</option><option value="sweep">Sweep</option><option value="blink">Blink</option><option value="pulse">Pulse</option>
</select>
<button class="btn" style="padding:4px 8px;font-size:.7em" onclick="saveAlert('input','acI','asI')">Set</button>
</div>
<div style="display:flex;gap:8px;align-items:center;margin-bottom:8px">
<span style="width:50px;font-size:.8em">Done</span>
<input type="color" id="acD" value="#00c800" style="width:36px;height:28px;border:none;border-radius:4px;cursor:pointer">
<select id="asD" style="flex:1;padding:6px;border-radius:6px;border:1px solid #333;background:#222;color:#fff;font-size:.8em">
<option value="breathe">Breathe</option><option value="sweep" selected>Sweep</option><option value="blink">Blink</option><option value="pulse">Pulse</option>
</select>
<button class="btn" style="padding:4px 8px;font-size:.7em" onclick="saveAlert('done','acD','asD')">Set</button>
</div>
<div style="display:flex;gap:8px;align-items:center;margin-bottom:8px">
<span style="width:50px;font-size:.8em">Error</span>
<input type="color" id="acE" value="#ff0000" style="width:36px;height:28px;border:none;border-radius:4px;cursor:pointer">
<select id="asE" style="flex:1;padding:6px;border-radius:6px;border:1px solid #333;background:#222;color:#fff;font-size:.8em">
<option value="breathe">Breathe</option><option value="sweep">Sweep</option><option value="blink" selected>Blink</option><option value="pulse">Pulse</option>
</select>
<button class="btn" style="padding:4px 8px;font-size:.7em" onclick="saveAlert('error','acE','asE')">Set</button>
</div>
<div class="slider-group" style="margin-bottom:0">
<label style="font-size:.75em">Duration: <span id="durV">5</span>s</label>
<input type="range" id="durS" min="1" max="30" value="5" oninput="document.getElementById('durV').textContent=this.value">
</div>
</div>

<button class="off-btn" id="toggle" onclick="toggleLeds()">Turn Off</button>

<div id="liveBox" style="position:fixed;bottom:16px;right:16px;z-index:1000">
<div id="liveWidget" style="display:none;background:#1a1a1a;border:2px solid #333;border-radius:12px;padding:8px;margin-bottom:8px;box-shadow:0 4px 20px rgba(0,0,0,.6)">
<canvas id="ledPreview" width="120" height="120" style="border-radius:6px;background:#000;display:block"></canvas>
</div>
<div style="text-align:right"><button id="prevBtn" onclick="togglePreview()" style="width:44px;height:44px;border-radius:50%;border:2px solid #333;background:#222;color:#0af;font-size:1.1em;cursor:pointer;box-shadow:0 2px 10px rgba(0,0,0,.4)">&#x25A3;</button></div>
</div>
<div class="status" id="st">Mode: Rainbow</div>
</div>
<script>
let isOn=true;
const cp=document.getElementById('cp');

// Dashboard polling
function pollDash(){
  fetch('/status').then(r=>r.json()).then(d=>{
    let w=document.getElementById('dWifi');
    let connected=d.wifi&&d.wifi.length>0;
    w.style.background=connected?'#0a0':'#a00';
    document.getElementById('dSsid').textContent=connected?d.wifi:'No WiFi';
    let t=d.cpuTemp||0;
    let tc=t<45?'#0af':t<60?'#0d4':t<75?'#fa0':'#f00';
    document.getElementById('dTemp').innerHTML='<span style="color:'+tc+'">'+t.toFixed(0)+'&deg;C</span>';
    let rssi=d.rssi||0;
    let bars=rssi>-50?4:rssi>-60?3:rssi>-70?2:1;
    document.getElementById('dRssi').textContent='Signal: '+'\u2582\u2584\u2586\u2588'.slice(0,bars);
    document.getElementById('dMode').textContent=d.on?d.modeName:'OFF';
  }).catch(()=>{
    document.getElementById('dWifi').style.background='#a00';
    document.getElementById('dSsid').textContent='Offline';
  });
}
pollDash();setInterval(pollDash,3000);
const modeIds=[0,1,2,3,4,7,8,9,10,11];
const modeNames={0:'Solid',1:'Rainbow',2:'Breathe',3:'Wave',4:'Matrix Rain',5:'Bitmap',6:'Text',7:'Game of Life',8:'Clock',9:'CPU Temp',10:'Music',11:'Pacman'};

cp.addEventListener('input',function(){sc(this.value.substring(1))});

function sc(hex){cp.value='#'+hex;sa(0);send('/color?hex='+hex);showMode(0)}
function sm(m){sa(m);send('/mode?m='+m);showMode(m)}
function bmp(name){send('/bitmap?name='+name);showMode(5)}

let tempTimer=null;
function showMode(m){
  document.getElementById('st').textContent='Mode: '+(modeNames[m]||'Bitmap');
  let tb=document.getElementById('tempBox');
  let mb=document.getElementById('musicBox');
  if(m==9){
    tb.style.display='block';
    pollTemp();
    if(tempTimer)clearInterval(tempTimer);
    tempTimer=setInterval(pollTemp,2000);
  }else{
    tb.style.display='none';
    if(tempTimer){clearInterval(tempTimer);tempTimer=null;}
  }
  if(m==10){
    mb.style.display='block';
  }else{
    mb.style.display='none';
    stopMusic();
  }
}
function pollTemp(){
  fetch('/cpu-temp').then(r=>r.json()).then(d=>{
    let t=d.temp;
    let el=document.getElementById('tempVal');
    el.textContent=t.toFixed(1);
    let clr=t<45?'#0064ff':t<60?'#00dc50':t<75?'#ffc800':'#ff0000';
    el.style.color=clr;
  }).catch(()=>{});
}

function sa(m){
  modeIds.forEach(id=>{
    let el=document.getElementById('m'+id);
    if(el)el.classList.toggle('active',id==m);
  });
}

function toggleLeds(){
  isOn=!isOn;
  const b=document.getElementById('toggle');
  b.textContent=isOn?'Turn Off':'Turn On';
  b.classList.toggle('is-off',!isOn);
  send('/toggle');
}

function sendText(){
  let msg=encodeURIComponent(document.getElementById('textMsg').value);
  let clr=document.getElementById('textClr').value.substring(1);
  let spd=document.getElementById('textSpd').value;
  send('/text?msg='+msg+'&color='+clr+'&speed='+spd);
  showMode(6);
}

function saveAlert(type,cid,sid){
  let clr=document.getElementById(cid).value.substring(1);
  let sty=document.getElementById(sid).value;
  let dur=document.getElementById('durS').value;
  send('/alert-config?type='+type+'&color='+clr+'&style='+sty);
  send('/notify?type='+type+'&duration='+dur+'000');
}

function send(path){
  fetch(path).then(()=>{}).catch(()=>{
    document.getElementById('st').textContent='Error';
  });
}

// Music reactive
let musicRunning=false,musicStream=null,musicCtx=null,musicRaf=null;
let visStyle=0;
let waveHist=new Array(8).fill(4);
let ringHue=0;
let barPeaks=new Array(8).fill(0);
let barPeakHold=new Array(8).fill(0);
const binR=[[1,2],[3,5],[6,9],[10,16],[17,26],[27,40],[41,60],[61,90]];

function setVis(s){visStyle=s;for(let i=0;i<3;i++){let e=document.getElementById('vs'+i);if(e)e.classList.toggle('active',i==s)}}
function sPx(b,x,y,r,g,bl){if(x<0||x>7||y<0||y>7)return;let i=(y*8+x)*3;b[i]=r;b[i+1]=g;b[i+2]=bl}
function b2s(bytes){let s='';for(let i=0;i<bytes.length;i++)s+=String.fromCharCode(bytes[i]);return btoa(s)}

function renderBars(fd,bytes){
  for(let x=0;x<8;x++){
    let sum=0,cnt=0;
    for(let b=binR[x][0];b<=binR[x][1]&&b<fd.length;b++){sum+=fd[b];cnt++}
    let val=cnt?sum/cnt:0;
    let barH=Math.min(8,Math.floor(val/28));
    // Peak hold
    if(barH>=barPeaks[x]){barPeaks[x]=barH;barPeakHold[x]=12}
    else{barPeakHold[x]--;if(barPeakHold[x]<=0)barPeaks[x]=Math.max(0,barPeaks[x]-1)}
    for(let y=0;y<8;y++){
      if(y<barH){
        let ratio=y/7.0;
        let r=Math.floor(255*ratio),g=Math.floor(255*(1-ratio)),bl=y<2?Math.floor(60*(1-y/2)):0;
        sPx(bytes,x,7-y,r,g,bl);
      }
    }
    // Draw peak marker
    let pk=barPeaks[x];
    if(pk>0&&pk>barH)sPx(bytes,x,7-pk,255,255,255);
  }
}

function renderWave(fd,bytes){
  // Per-column band energy for real waveform shape
  for(let x=0;x<8;x++){
    let sum=0,cnt=0;
    for(let b=binR[x][0];b<=binR[x][1]&&b<fd.length;b++){sum+=fd[b];cnt++}
    let val=cnt?sum/cnt:0;
    let wy=Math.min(7,Math.floor(val/36));
    waveHist[x]=waveHist[x]*0.5+wy*0.5; // smooth
    let sy=Math.round(waveHist[x]);
    for(let row=0;row<8;row++){
      let ry=7-row;
      if(row<=sy){
        let ratio=row/7;
        let bright=row==sy?1.0:0.3+0.4*(row/sy);
        let r=Math.floor((50+205*ratio)*bright);
        let g=Math.floor((200*(1-ratio))*bright);
        let bl=Math.floor((255*(1-ratio))*bright);
        sPx(bytes,x,ry,r,g,bl);
      }
    }
  }
}

function hsl2rgb(h,s,l){
  h/=360;let r,g,b;
  if(s==0){r=g=b=l}else{
    let hue2rgb=function(p,q,t){if(t<0)t+=1;if(t>1)t-=1;if(t<1/6)return p+(q-p)*6*t;if(t<1/2)return q;if(t<2/3)return p+(q-p)*(2/3-t)*6;return p};
    let q=l<0.5?l*(1+s):l+s-l*s,p=2*l-q;
    r=hue2rgb(p,q,h+1/3);g=hue2rgb(p,q,h);b=hue2rgb(p,q,h-1/3);
  }
  return[Math.round(r*255),Math.round(g*255),Math.round(b*255)];
}

function renderRings(fd,bytes){
  let bass=0;for(let i=1;i<=5&&i<fd.length;i++)bass+=fd[i];bass/=5;
  let radius=(bass/255)*4.5;
  ringHue=(ringHue+3)%360;
  let cx=3.5,cy=3.5;
  for(let y=0;y<8;y++){
    for(let x=0;x<8;x++){
      let dist=Math.sqrt((x-cx)*(x-cx)+(y-cy)*(y-cy));
      let rd=Math.abs(dist-radius);
      if(rd<1.4){
        let bright=1.0-(rd/1.4);
        let c=hsl2rgb(ringHue,1,bright*0.5);
        sPx(bytes,x,y,c[0],c[1],c[2]);
      }
    }
  }
  // inner glow
  let c2=hsl2rgb((ringHue+180)%360,1,bass/600);
  sPx(bytes,3,3,c2[0],c2[1],c2[2]);sPx(bytes,4,3,c2[0],c2[1],c2[2]);
  sPx(bytes,3,4,c2[0],c2[1],c2[2]);sPx(bytes,4,4,c2[0],c2[1],c2[2]);
}

function drawPreview(bytes,vCtx){
  vCtx.fillStyle='#000';vCtx.fillRect(0,0,160,160);
  for(let y=0;y<8;y++){
    for(let x=0;x<8;x++){
      let i=(y*8+x)*3;
      let r=bytes[i],g=bytes[i+1],b=bytes[i+2];
      if(r||g||b){vCtx.fillStyle='rgb('+r+','+g+','+b+')'}else{vCtx.fillStyle='#111'}
      vCtx.fillRect(x*20,y*20,18,18);
    }
  }
}

function initMusicAnalyser(stream){
  musicStream=stream;
  musicCtx=new (window.AudioContext||window.webkitAudioContext)();
  let src=musicCtx.createMediaStreamSource(stream);
  let analyser=musicCtx.createAnalyser();
  analyser.fftSize=256;
  analyser.smoothingTimeConstant=0.7;
  src.connect(analyser);
  let freqData=new Uint8Array(analyser.frequencyBinCount);
  musicRunning=true;
  document.getElementById('musicStatus').textContent='Listening...';
  let vCtx=document.getElementById('musicVis').getContext('2d');
  let lastSend=0;
  function render(){
    if(!musicRunning)return;
    analyser.getByteFrequencyData(freqData);
    let bytes=new Uint8Array(192);
    switch(visStyle){
      case 0:renderBars(freqData,bytes);break;
      case 1:renderWave(freqData,bytes);break;
      case 2:renderRings(freqData,bytes);break;
    }
    drawPreview(bytes,vCtx);
    let now=Date.now();
    if(now-lastSend>50){
      lastSend=now;
      fetch('/music-frame',{method:'POST',headers:{'Content-Type':'text/plain'},body:b2s(bytes)}).catch(function(){});
    }
    musicRaf=requestAnimationFrame(render);
  }
  render();
  send('/mode?m=10');
}
function startMusic(){
  sa(10);showMode(10);
  if(musicRunning)return;
  try{
    if(!navigator.mediaDevices||!navigator.mediaDevices.getUserMedia){
      document.getElementById('musicStatus').innerHTML='Mic needs HTTPS. Open:<br><b>chrome://flags/#unsafely-treat-insecure-origin-as-secure</b><br>Add: <b>http://'+location.host+'</b> and relaunch Chrome';
      return;
    }
    navigator.mediaDevices.getUserMedia({audio:{echoCancellation:false,noiseSuppression:false,autoGainControl:false}}).then(function(stream){
      initMusicAnalyser(stream);
    }).catch(function(e){
      document.getElementById('musicStatus').textContent='Mic error: '+e.message;
    });
  }catch(e){
    document.getElementById('musicStatus').textContent='Error: '+e.message;
  }
}
function stopMusic(){
  musicRunning=false;
  if(musicRaf)cancelAnimationFrame(musicRaf);
  if(musicStream)musicStream.getTracks().forEach(function(t){t.stop()});
  if(musicCtx)musicCtx.close().catch(function(){});
  musicStream=null;musicCtx=null;musicRaf=null;
  let ms=document.getElementById('musicStatus');
  if(ms)ms.textContent='Tap Music to start';
}

// Image upload with drag & drop
const upArea=document.getElementById('upArea');
upArea.addEventListener('dragover',function(e){e.preventDefault();this.classList.add('drag')});
upArea.addEventListener('dragleave',function(){this.classList.remove('drag')});
upArea.addEventListener('drop',function(e){
  e.preventDefault();this.classList.remove('drag');
  if(e.dataTransfer.files.length)processFile(e.dataTransfer.files[0]);
});
document.getElementById('fileIn').addEventListener('change',function(e){
  if(e.target.files[0])processFile(e.target.files[0]);
});

function processFile(file){
  document.getElementById('fileName').textContent=file.name;
  document.getElementById('uploadBadge').style.display='none';
  let img=new Image();
  img.onload=function(){
    let canvas=document.createElement('canvas');
    canvas.width=8;canvas.height=8;
    let ctx=canvas.getContext('2d');
    ctx.imageSmoothingEnabled=true;
    ctx.imageSmoothingQuality='medium';
    ctx.drawImage(img,0,0,8,8);
    let data=ctx.getImageData(0,0,8,8).data;
    let bytes=new Uint8Array(192);
    let grid=document.getElementById('preview');
    grid.innerHTML='';grid.style.display='grid';
    for(let i=0;i<64;i++){
      let r=data[i*4],g=data[i*4+1],b=data[i*4+2];
      bytes[i*3]=r;bytes[i*3+1]=g;bytes[i*3+2]=b;
      let d=document.createElement('div');
      d.style.background='rgb('+r+','+g+','+b+')';
      grid.appendChild(d);
    }
    fetch('/upload-bitmap',{method:'POST',
      headers:{'Content-Type':'text/plain'},
      body:b2s(bytes)
    }).then(()=>{
      document.getElementById('uploadBadge').style.display='block';
      showMode(5);
    }).catch(()=>{
      document.getElementById('st').textContent='Upload error';
    });
  };
  img.src=URL.createObjectURL(file);
}

// Live LED preview
let prevTimer=null,prevOn=false;
function togglePreview(){
  prevOn=!prevOn;
  let w=document.getElementById('liveWidget');
  let b=document.getElementById('prevBtn');
  w.style.display=prevOn?'block':'none';
  b.style.borderColor=prevOn?'#0af':'#333';
  if(prevOn){pollLeds();prevTimer=setInterval(pollLeds,200)}
  else{if(prevTimer){clearInterval(prevTimer);prevTimer=null}}
}
function pollLeds(){
  fetch('/led-state').then(r=>r.json()).then(px=>{
    let cv=document.getElementById('ledPreview');
    let ctx=cv.getContext('2d');
    ctx.fillStyle='#000';ctx.fillRect(0,0,120,120);
    for(let i=0;i<64&&i<px.length;i++){
      let c=px[i];
      let r=(c>>16)&0xFF,g=(c>>8)&0xFF,b=c&0xFF;
      let x=i%8,y=Math.floor(i/8);
      ctx.fillStyle=r||g||b?'rgb('+r+','+g+','+b+')':'#111';
      ctx.fillRect(x*15,y*15,13,13);
    }
  }).catch(()=>{});
}
</script>
</body>
</html>)rawliteral";

void handleRoot() { server.send(200, "text/html", HTML); }

// --- Setup ---
void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  strip.begin();
  strip.setBrightness(BRIGHTNESS);

  // Red = connecting
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(5, 0, 0));
  strip.show();

  wifiMulti.addAP(WIFI_SSID, WIFI_PASS);
  wifiMulti.addAP(WIFI_SSID2, WIFI_PASS2);
  Serial.print("Connecting");
  int attempts = 0;
  while (wifiMulti.run() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    for (int i = 0; i < NUM_LEDS; i++)
      strip.setPixelColor(i, strip.Color(0, 5, 0));
    strip.show();
    delay(800);
    Serial.println();
    Serial.println("=================================");
    Serial.print("  IP: http://");
    Serial.println(WiFi.localIP());
    Serial.println("=================================");

    configTime(utcOffset, 0, "pool.ntp.org");
    ntpLastSync = millis();

    // mDNS — access via http://esp32matrix.local
    if (MDNS.begin("esp32matrix")) {
      Serial.println("  mDNS: http://esp32matrix.local");
    }
  } else {
    for (int i = 0; i < NUM_LEDS; i++)
      strip.setPixelColor(i, strip.Color(0, 0, 5));
    strip.show();
    Serial.println("\nWi-Fi FAILED!");
  }

  server.enableCORS(true);
  server.on("/", handleRoot);
  server.on("/color", handleColor);
  server.on("/mode", handleMode);
  server.on("/brightness", handleBrightness);
  server.on("/toggle", handleToggle);
  server.on("/notify", handleNotify);
  server.on("/bitmap", handleBitmap);
  server.on("/upload-bitmap", HTTP_POST, handleUploadBitmap);
  server.on("/text", handleText);
  server.on("/alerts", handleAlerts);
  server.on("/alert-config", handleAlertConfig);
  server.on("/rotation", handleRotation);
  server.on("/speed", handleSpeed);
  server.on("/status", handleStatus);
  server.on("/cpu-temp", handleCpuTemp);
  server.on("/music-frame", HTTP_POST, handleMusicFrame);
  server.on("/led-state", handleLedState);
  server.begin();

  // Init matrix rain speeds
  for (int x = 0; x < MATRIX_W; x++) {
    rainSpeed[x] = random(1, 4);
    rainTick[x] = 0;
  }
}

// --- Loop ---
void loop() {
  server.handleClient();

  if (digitalRead(BOOT_BTN) == LOW) {
    ledsOn = !ledsOn;
    if (!ledsOn) { strip.clear(); strip.show(); }
    delay(300);
  }

  // Notification overlay
  if (notifyActive) {
    if (millis() - notifyStart > notifyDuration) {
      notifyActive = false;
    } else {
      runNotifyAnim();
      return;
    }
  }

  if (!ledsOn) return;

  switch (mode) {
    case 0: modeSolid(); break;
    case 1: modeRainbow(); break;
    case 2: modeBreathe(); break;
    case 3: modeWave(); break;
    case 4: modeMatrixRain(); break;
    case 5: modeBitmap(); break;
    case 6: modeScrollText(); break;
    case 7: modeGameOfLife(); break;
    case 8: modeClock(); break;
    case 9: modeCpuTemp(); break;
    case 10: modeMusicReactive(); break;
    case 11: modePacman(); break;
  }
}
