#include <WiFi.h>
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
WebServer server(80);

// --- State ---
uint8_t mode = 1;          // 0=solid,1=rainbow,2=breathe,3=wave,4=matrixRain,5=bitmap,6=text,7=gameOfLife,8=clock
uint8_t solidR = 255, solidG = 0, solidB = 100;
bool ledsOn = true;
uint16_t animSpeed = 50;   // ms delay for animations (10-500)

// Notification state
bool notifyActive = false;
uint8_t notifyType = 0;       // 0=input,1=done,2=error
unsigned long notifyStart = 0;
bool alertsEnabled = true;

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
void notifyInputAnim() {
  unsigned long elapsed = millis() - notifyStart;
  float phase = elapsed * 0.005;
  float breath = (sin(phase) + 1.0) / 2.0;
  uint8_t r = 255 * breath, g = 140 * breath, b = 0;
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(r, g, b));
  strip.show();
}

void notifyDoneAnim() {
  unsigned long elapsed = millis() - notifyStart;
  int sweepPos = (elapsed / 60) % (MATRIX_W + 4);
  for (int y = 0; y < MATRIX_H; y++) {
    for (int x = 0; x < MATRIX_W; x++) {
      int dist = abs(x - sweepPos);
      uint8_t g = dist < 3 ? (255 - dist * 80) : 0;
      setPixelXY(x, y, 0, g, 0);
    }
  }
  strip.show();
}

void notifyErrorAnim() {
  unsigned long elapsed = millis() - notifyStart;
  bool on = (elapsed / 200) % 2 == 0;
  uint32_t c = on ? strip.Color(255, 0, 0) : 0;
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, c);
  strip.show();
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
  notifyActive = true;
  notifyStart = millis();
  if (type == "input") notifyType = 0;
  else if (type == "done") notifyType = 1;
  else if (type == "error") notifyType = 2;
  else { notifyActive = false; }
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
  if ((int)body.length() >= 192) {
    for (int i = 0; i < 64; i++) {
      uint8_t r = (uint8_t)body[i * 3];
      uint8_t g = (uint8_t)body[i * 3 + 1];
      uint8_t b = (uint8_t)body[i * 3 + 2];
      customBitmap[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    hasCustomBitmap = true;
    bitmapSource = 3;
    mode = 5;
    ledsOn = true;
    server.send(200, "text/plain", "ok");
  } else {
    server.send(400, "text/plain", "need 192 bytes (64 RGB)");
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
  const char* modeNames[] = {"Solid","Rainbow","Breathe","Wave","Matrix Rain","Bitmap","Text","Game of Life","Clock"};
  String modeName = (mode <= 8) ? modeNames[mode] : "Unknown";
  String json = "{\"mode\":" + String(mode) + ",\"modeName\":\"" + modeName + "\",\"on\":" + String(ledsOn ? "true" : "false") +
    ",\"alerts\":" + String(alertsEnabled ? "true" : "false") +
    ",\"rotation\":" + String(rotation) +
    ",\"speed\":" + String(animSpeed) + "}";
  server.send(200, "application/json", json);
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
.rot-btns{display:flex;gap:4px;flex-wrap:wrap}
.rot-btns .btn{padding:6px 8px;font-size:.75em}
</style>
</head>
<body>
<div class="c">
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
</div>

<h2>Pixel Art</h2>
<div class="pixel-grid">
<div class="btn" onclick="bmp('claude')">Claude</div>
<div class="btn" onclick="bmp('heart')">Heart</div>
<div class="btn" onclick="bmp('smiley')">Smiley</div>
<div class="btn" onclick="bmp('star')">Star</div>
<div class="btn" onclick="bmp('music')">Music</div>
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
<div class="toggle-row" style="margin-bottom:14px">
<span>Test Alert</span>
<div>
<button class="btn" style="padding:6px 10px;font-size:.75em" onclick="send('/notify?type=input')">Input</button>
<button class="btn" style="padding:6px 10px;font-size:.75em" onclick="send('/notify?type=done')">Done</button>
<button class="btn" style="padding:6px 10px;font-size:.75em" onclick="send('/notify?type=error')">Error</button>
</div>
</div>

<button class="off-btn" id="toggle" onclick="toggleLeds()">Turn Off</button>
<div class="status" id="st">Mode: Rainbow</div>
</div>
<script>
let isOn=true;
const cp=document.getElementById('cp');
const modeIds=[0,1,2,3,4,7,8];
const modeNames={0:'Solid',1:'Rainbow',2:'Breathe',3:'Wave',4:'Matrix Rain',5:'Bitmap',6:'Text',7:'Game of Life',8:'Clock'};

cp.addEventListener('input',function(){sc(this.value.substring(1))});

function sc(hex){cp.value='#'+hex;sa(0);send('/color?hex='+hex);showMode(0)}
function sm(m){sa(m);send('/mode?m='+m);showMode(m)}
function bmp(name){send('/bitmap?name='+name);showMode(5)}

function showMode(m){
  document.getElementById('st').textContent='Mode: '+(modeNames[m]||'Bitmap');
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

function send(path){
  fetch(path).then(()=>{}).catch(()=>{
    document.getElementById('st').textContent='Error';
  });
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
      headers:{'Content-Type':'application/octet-stream'},
      body:bytes
    }).then(()=>{
      document.getElementById('uploadBadge').style.display='block';
      showMode(5);
    }).catch(()=>{
      document.getElementById('st').textContent='Upload error';
    });
  };
  img.src=URL.createObjectURL(file);
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

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
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
  server.on("/rotation", handleRotation);
  server.on("/speed", handleSpeed);
  server.on("/status", handleStatus);
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

  // Notification overlay (5 second duration)
  if (notifyActive) {
    if (millis() - notifyStart > 5000) {
      notifyActive = false;
    } else {
      switch (notifyType) {
        case 0: notifyInputAnim(); break;
        case 1: notifyDoneAnim(); break;
        case 2: notifyErrorAnim(); break;
      }
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
  }
}
