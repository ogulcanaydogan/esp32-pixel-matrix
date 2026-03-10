# ESP32 Pixel Matrix

Single-file ESP32 firmware for an 8x8 WS2812B LED matrix with a mobile-friendly web UI.

Comes with built-in pixel art icons, image upload, scrolling text, visual effects, music visualizer, Pacman animation, NTP clock, CPU temperature monitoring, and a notification API you can hook into your dev tools.

![Web UI](docs/webui.png)

## Features

- **Web UI** with color picker, presets, and live controls
- **12 visual effects**: solid, rainbow, breathe, wave, matrix rain, Game of Life, NTP clock, CPU temp, music reactive, Pacman
- **Music visualizer**: 3 styles (bars with peak hold, wave, rings) using microphone input
- **Pacman animation**: classic Pacman moving across the matrix, eating dots, ghost chasing
- **11 pixel art icons**: heart, smiley, star, ghost, music, skull, etc.
- **Image upload**: drag & drop any image, auto-resized to 8x8
- **Scrolling text** with custom color and speed
- **Status dashboard**: fixed top bar showing WiFi SSID, signal strength, CPU temp, active mode
- **Live LED preview**: floating widget showing real-time matrix state
- **Multi-WiFi**: auto-connects to strongest available network
- **Notification API**: trigger alerts (input/done/error) with customizable colors and animations
- **mDNS**: access via `http://esp32matrix.local`
- **CORS enabled** for easy API integration
- **Rotation**: 0/90/180/270 degrees + horizontal/vertical flip

## Hardware

| Component | Spec |
|-----------|------|
| MCU | ESP32-S3 (or any ESP32 with WiFi) |
| LED Matrix | WS2812B 8x8 (64 LEDs) |
| Data Pin | GPIO 14 |
| Power | 5V, ~2A recommended |

### Wiring

```
ESP32 GPIO14 --> DIN (WS2812B)
ESP32 GND    --> GND (WS2812B)
ESP32 5V     --> VCC (WS2812B)
```

Tip: use a 3.3V to 5V level shifter on the data line, or keep the wire short (<10cm).

## Quick Start

### 1. Clone and configure

```bash
git clone https://github.com/ogulcanaydogan/esp32-pixel-matrix.git
cd esp32-pixel-matrix
cp credentials.example.h credentials.h
```

Edit `credentials.h` with your WiFi credentials. You can add multiple networks:

```cpp
const char* WIFI_SSID = "HomeWiFi";
const char* WIFI_PASS = "password1";
const char* WIFI_SSID2 = "WorkWiFi";
const char* WIFI_PASS2 = "password2";
```

The ESP32 will automatically connect to whichever network is available.

### 2. Upload (Arduino IDE)

1. Install [Arduino IDE](https://www.arduino.cc/en/software)
2. Add ESP32 board support: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Install library: **Adafruit NeoPixel**
4. Select your board and port, then upload `esp32-matrix-test.ino`

### 3. Upload (arduino-cli)

```bash
arduino-cli lib install "Adafruit NeoPixel"
arduino-cli compile --fqbn esp32:esp32:esp32s3 esp32-matrix-test.ino
arduino-cli upload --fqbn esp32:esp32:esp32s3 -p /dev/cu.usbmodem101 esp32-matrix-test.ino
```

### 4. Connect

Open `http://esp32matrix.local` or check Serial Monitor for the IP address.

## API Reference

Base URL: `http://esp32matrix.local`

| Endpoint | Method | Params | What it does |
|----------|--------|--------|--------------|
| `/` | GET | | Web UI |
| `/color` | GET | `hex` (e.g. `ff0000`) | Set solid color |
| `/mode` | GET | `m` (0-11) | Switch mode |
| `/brightness` | GET | `v` (1-30) | Set brightness |
| `/speed` | GET | `v` (10-500) | Animation speed in ms |
| `/toggle` | GET | | Toggle on/off |
| `/rotation` | GET | `v` (0-5) | 0=normal, 1=90, 2=180, 3=270, 4=flipH, 5=flipV |
| `/bitmap` | GET | `name` | Show a built-in icon |
| `/upload-bitmap` | POST | 192 bytes raw RGB | Upload custom 8x8 image |
| `/text` | GET | `msg`, `color`, `speed` | Scroll text |
| `/notify` | GET | `type`, `color`, `style`, `duration` | Trigger alert animation |
| `/alert-config` | GET | `type`, `color`, `style` | Save alert defaults |
| `/alerts` | GET | `v` (0 or 1) | Enable/disable alerts |
| `/cpu-temp` | GET | | Get CPU temp as JSON |
| `/status` | GET | | Current state as JSON (mode, WiFi, temp, RSSI) |
| `/music-frame` | POST | 192 bytes base64 RGB | Send music visualization frame |
| `/led-state` | GET | | Current LED colors as JSON array |

### Modes

| ID | Name | Description |
|----|------|-------------|
| 0 | Solid | Single color |
| 1 | Rainbow | Rotating rainbow cycle |
| 2 | Breathe | Pulsing solid color |
| 3 | Wave | Diagonal wave pattern |
| 4 | Matrix Rain | Green rain drops |
| 5 | Bitmap | Pixel art display |
| 6 | Scrolling Text | Horizontal scroll |
| 7 | Game of Life | Conway's simulation |
| 8 | NTP Clock | Real-time clock display |
| 9 | CPU Temp | Temperature readout |
| 10 | Music | Microphone-reactive visualizer |
| 11 | Pacman | Pacman animation |

### Built-in Icons

`claude`, `heart`, `smiley`, `star`, `music`, `ghost`, `sun`, `moon`, `tree`, `skull`, `diamond`

### Upload Image (Python example)

```python
from PIL import Image
import requests

img = Image.open("myimage.png").resize((8, 8)).convert("RGB")
requests.post("http://esp32matrix.local/upload-bitmap", data=img.tobytes())
```

## Music Visualizer

The Music mode uses the browser's microphone to analyze audio and send visualization frames to the matrix in real-time.

**3 visualization styles:**
- **Bars**: frequency spectrum with peak hold indicators
- **Wave**: per-band energy waveform
- **Rings**: expanding rings reacting to bass

Note: Microphone access requires HTTPS or a localhost exception in Chrome.

## Notification API

The `/notify` endpoint triggers visual alerts on the matrix. You can customize the color, animation style, and duration per alert type.

**Parameters:**
- `type`: `input`, `done`, `error`, or `clear`
- `color` (optional): hex color like `ff8800`
- `style` (optional): `breathe`, `sweep`, `blink`, or `pulse`
- `duration` (optional): milliseconds (1000-30000)

```bash
# orange breathe (waiting for input)
curl "http://esp32matrix.local/notify?type=input"

# green sweep (task done)
curl "http://esp32matrix.local/notify?type=done"

# red blink (error)
curl "http://esp32matrix.local/notify?type=error"

# custom: purple pulse for 10 seconds
curl "http://esp32matrix.local/notify?type=done&color=8000ff&style=pulse&duration=10000"

# clear, go back to previous mode
curl "http://esp32matrix.local/notify?type=clear"
```

To save defaults per alert type (persists until reboot):

```bash
curl "http://esp32matrix.local/alert-config?type=error&color=ff4400&style=pulse"
```

### Hook Example

You can wire this into your dev tools. For example, with Claude Code hooks (`.claude/hooks.json`):

```json
{
  "hooks": [
    {
      "event": "notification",
      "command": "curl -s 'http://esp32matrix.local/notify?type=$NOTIFICATION_TYPE'"
    }
  ]
}
```

## USB Serial Controller (optional)

`controller.py` is a simple Flask app that talks to the ESP32 over USB serial. Useful if you want to control it without WiFi.

```bash
pip install flask pyserial
python controller.py
# opens on http://localhost:5555
```

## Project Structure

```
esp32-pixel-matrix/
├── esp32-matrix-test.ino    # main firmware (single file, ~1400 lines)
├── bitmaps.h                # 11 pixel art icons
├── fonts.h                  # 5x7 bitmap font
├── credentials.h            # your wifi creds (gitignored)
├── credentials.example.h    # template
├── controller.py            # usb serial controller
├── docs/
│   └── webui.png            # screenshot
├── LICENSE
└── README.md
```

## License

MIT
