#!/usr/bin/env python3
"""ESP32 Matrix LED Controller — local web UI over USB serial."""

import serial
import time
import threading
from flask import Flask, request, jsonify

SERIAL_PORT = "/dev/cu.usbmodem101"
BAUD_RATE = 115200

app = Flask(__name__)
ser = None
lock = threading.Lock()

def get_serial():
    global ser
    if ser is None or not ser.is_open:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1,
                           write_timeout=1, dsrdtr=False, rtscts=False)
        ser.dtr = False
        ser.rts = False
        time.sleep(0.5)
        ser.reset_input_buffer()
    return ser

def send_cmd(cmd):
    with lock:
        try:
            s = get_serial()
            data = f"{cmd}\n".encode("ascii")
            s.write(data)
            s.flush()
            time.sleep(0.1)
            # Read response
            resp = ""
            while s.in_waiting:
                resp += s.read(s.in_waiting).decode("ascii", errors="ignore")
            print(f"  -> {cmd}  |  <- {resp.strip()}")
            return True
        except Exception as e:
            print(f"Serial error: {e}")
            global ser
            ser = None
            return False

HTML = """<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Matrix</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#111;color:#fff;
  display:flex;justify-content:center;padding:20px}
.c{max-width:420px;width:100%}
h1{text-align:center;margin-bottom:24px;font-size:1.5em}
.color-pick{width:100%;height:90px;border:none;border-radius:14px;
  cursor:pointer;margin-bottom:18px}
.presets{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-bottom:18px}
.preset{width:100%;aspect-ratio:1;border-radius:50%;border:3px solid #333;
  cursor:pointer;transition:.2s}
.preset:hover{transform:scale(1.15);border-color:#fff}
.modes{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:18px}
.btn{padding:14px;border:2px solid #333;border-radius:12px;background:#222;
  color:#fff;font-size:1em;cursor:pointer;text-align:center;transition:.2s}
.btn.active{border-color:#0af;color:#0af}
.btn:active{transform:scale(.95)}
.slider-group{margin-bottom:18px}
.slider-group label{display:block;margin-bottom:8px;color:#888;font-size:.85em}
input[type=range]{width:100%;accent-color:#0af;height:32px}
.off-btn{width:100%;padding:16px;border-radius:12px;border:2px solid #c00;
  background:#200;color:#f66;font-size:1.1em;cursor:pointer;font-weight:600}
.off-btn.is-off{border-color:#0a0;background:#020;color:#0f0}
.status{text-align:center;margin-top:12px;color:#555;font-size:.8em}
</style>
</head>
<body>
<div class="c">
<h1>LED Matrix</h1>

<input type="color" id="cp" class="color-pick" value="#ff0064">

<div class="presets">
  <div class="preset" style="background:#ff0000" onclick="c('red')"></div>
  <div class="preset" style="background:#00ff00" onclick="c('green')"></div>
  <div class="preset" style="background:#0000ff" onclick="c('blue')"></div>
  <div class="preset" style="background:#ffffff" onclick="c('white')"></div>
  <div class="preset" style="background:#8000ff" onclick="c('purple')"></div>
  <div class="preset" style="background:#ff5000" onclick="c('orange')"></div>
  <div class="preset" style="background:#00ffff" onclick="c('cyan')"></div>
  <div class="preset" style="background:#ff0064" onclick="c('pink')"></div>
</div>

<div class="modes">
  <div class="btn active" id="m0" onclick="setMode(0)">Solid</div>
  <div class="btn" id="m1" onclick="setMode(1)">Rainbow</div>
  <div class="btn" id="m2" onclick="setMode(2)">Breathe</div>
  <div class="btn" id="m3" onclick="setMode(3)">Wave</div>
</div>

<div class="slider-group">
  <label>Brightness: <span id="bv">5</span></label>
  <input type="range" id="br" min="1" max="30" value="5"
    oninput="document.getElementById('bv').textContent=this.value"
    onchange="c('bright '+this.value)">
</div>

<button class="off-btn" id="toggle" onclick="toggleLeds()">Turn Off</button>
<div class="status" id="st">Connected via USB</div>
</div>
<script>
let isOn=true, curMode=0;
const cp=document.getElementById('cp');
const modes=['color #ff0064','rainbow','breathe','wave'];

cp.addEventListener('input',function(){
  c('color #'+this.value.substring(1));
  setActive(0);
});

function c(cmd){
  document.getElementById('st').textContent='Sending...';
  fetch('/cmd',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({cmd:cmd})})
  .then(r=>r.json())
  .then(d=>{document.getElementById('st').textContent=d.ok?'OK: '+cmd:'Error!'})
  .catch(()=>{document.getElementById('st').textContent='Connection error'});
}

function setMode(m){
  curMode=m;
  setActive(m);
  if(m==0) c('color #'+cp.value.substring(1));
  else if(m==1) c('rainbow');
  else if(m==2) c('breathe');
  else if(m==3) c('wave');
}

function setActive(m){
  for(let i=0;i<4;i++) document.getElementById('m'+i).classList.toggle('active',i==m);
}

function toggleLeds(){
  isOn=!isOn;
  const b=document.getElementById('toggle');
  b.textContent=isOn?'Turn Off':'Turn On';
  b.classList.toggle('is-off',!isOn);
  c(isOn?'on':'off');
}
</script>
</body>
</html>"""

@app.route("/")
def index():
    return HTML

@app.route("/cmd", methods=["POST"])
def cmd():
    data = request.get_json()
    command = data.get("cmd", "")
    ok = send_cmd(command)
    return jsonify({"ok": ok, "cmd": command})

if __name__ == "__main__":
    print(f"Connecting to {SERIAL_PORT}...")
    get_serial()
    print("Serial connected!")
    print()
    print(">>> Open http://localhost:5555")
    print()
    app.run(host="0.0.0.0", port=5555, debug=False)
