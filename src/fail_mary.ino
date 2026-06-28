/*
 * ============================================================
 *  FAIL MARY — ESP32 WiFi Robot Car Controller
 *  v1.1 — reliability fixes only
 * ============================================================
 *  FIXES APPLIED:
 *    1. Dead-man watchdog: motors stop if no command in 500ms
 *    2. turnLeft/turnRight corrected (were identical to spin)
 *    3. Speed slider updates motors immediately mid-movement
 *    4. JS hold() timer skips if previous fetch still in-flight
 *    5. Stop button ontouchend void(0) bug fixed
 *    6. motorSpeed read at execution time, not captured at press
 *
 *  Hardware:
 *    - ESP32 Dev Module
 *    - 2x BTS7960 motor drivers
 *    - 4x Johnson 12V 200RPM motors (left side on Driver 1,
 *      right side on Driver 2, right side polarity flipped)
 *
 *  BTS7960 Pin Connections:
 *    Driver 1 (LEFT side):
 *      RPWM  <- GPIO 25   (left forward)
 *      LPWM  <- GPIO 26   (left reverse)
 *      R_EN  <- 3.3V
 *      L_EN  <- 3.3V
 *      VCC   <- 5V (from LM2596)
 *      GND   <- Common GND
 *      B+    <- 11.1V (from LiPo)
 *      B-    <- Common GND
 *
 *    Driver 2 (RIGHT side):
 *      RPWM  <- GPIO 27   (right forward)
 *      LPWM  <- GPIO 14   (right reverse)
 *      R_EN  <- 3.3V
 *      L_EN  <- 3.3V
 *      VCC   <- 5V (from LM2596)
 *      GND   <- Common GND
 *      B+    <- 11.1V (from LiPo)
 *      B-    <- Common GND
 *
 *  WiFi:
 *    SSID     : Fail mary
 *    Password : signofthetimes
 *    IP       : 192.168.4.2
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>

// ── WiFi credentials ──────────────────────────────────────
const char* SSID     = "Fail mary";
const char* PASSWORD = "signofthetimes";

// ── GPIO pin assignments ───────────────────────────────────
#define LEFT_RPWM   25
#define LEFT_LPWM   26
#define RIGHT_RPWM  27
#define RIGHT_LPWM  14

// ── PWM settings ───────────────────────────────────────────
#define PWM_FREQ  20000
#define PWM_RES   8

// ── Default speed (0–255) ──────────────────────────────────
int motorSpeed = 200;

// ── FIX 1: Dead-man watchdog ──────────────────────────────
// Tracks the last time any motion command was received.
// loop() checks this every iteration — if more than
// WATCHDOG_MS have elapsed without a command, motors stop.
// This means a dropped WiFi connection, crashed browser tab,
// or phone going to sleep can never leave the robot running.
#define WATCHDOG_MS 500
unsigned long lastCmdTime = 0;
bool motorsStopped = true;   // avoid spamming stopAll() every loop tick

// ── Current motion command ─────────────────────────────────
// FIX 6: store the command name so we can re-apply it when
// speed changes mid-movement (instead of capturing the old
// motorSpeed value at button-press time).
String currentCmd = "";

// ── Web server on port 80 ──────────────────────────────────
WebServer server(80);

// ══════════════════════════════════════════════════════════
//  Motor control helpers
// ══════════════════════════════════════════════════════════

void stopAll() {
  ledcWrite(LEFT_RPWM,  0);
  ledcWrite(LEFT_LPWM,  0);
  ledcWrite(RIGHT_RPWM, 0);
  ledcWrite(RIGHT_LPWM, 0);
}

void setLeft(int spd) {
  if (spd >= 0) {
    ledcWrite(LEFT_RPWM, spd);
    ledcWrite(LEFT_LPWM, 0);
  } else {
    ledcWrite(LEFT_RPWM, 0);
    ledcWrite(LEFT_LPWM, -spd);
  }
}

void setRight(int spd) {
  if (spd >= 0) {
    ledcWrite(RIGHT_RPWM, spd);
    ledcWrite(RIGHT_LPWM, 0);
  } else {
    ledcWrite(RIGHT_RPWM, 0);
    ledcWrite(RIGHT_LPWM, -spd);
  }
}

void moveForward()  { setLeft(motorSpeed);  setRight(motorSpeed);  }
void moveBackward() { setLeft(-motorSpeed); setRight(-motorSpeed); }

// FIX 2: turnLeft/turnRight corrected.
// Previously these were identical to spinLeft/spinRight
// (both sides opposite directions = spin in place).
// A proper tank turn drives one side at full speed and
// brakes the other — giving a wide arc instead of spinning.
// The turning side drives at motorSpeed; the inside wheel
// stops. For a tighter arc, you could drive the inside wheel
// backward at a reduced speed — but stopped is the safe
// baseline that's clearly different from spin.
void turnLeft()  { setLeft(0);           setRight(motorSpeed);  }
void turnRight() { setLeft(motorSpeed);  setRight(0);           }

// Spin in place — left and right at equal speed, opposite directions
void spinLeft()  { setLeft(-motorSpeed); setRight(motorSpeed);  }
void spinRight() { setLeft(motorSpeed);  setRight(-motorSpeed); }

// FIX 6 helper: re-apply the current command using the
// latest motorSpeed value. Called from handleSpeed() so
// a slider change takes effect immediately.
void applyCurrentCmd() {
  if      (currentCmd == "forward")   moveForward();
  else if (currentCmd == "backward")  moveBackward();
  else if (currentCmd == "left")      turnLeft();
  else if (currentCmd == "right")     turnRight();
  else if (currentCmd == "spinleft")  spinLeft();
  else if (currentCmd == "spinright") spinRight();
  // "stop" or empty → do nothing; motors are already stopped
}

// ══════════════════════════════════════════════════════════
//  HTML page
// ══════════════════════════════════════════════════════════

const char PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>Fail Mary</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Bebas+Neue&family=Share+Tech+Mono&display=swap');

  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  :root {
    --bg:       #0a0a0f;
    --panel:    #111118;
    --accent:   #e8ff00;
    --red:      #ff2d2d;
    --dim:      #2a2a35;
    --text:     #c8c8d8;
    --btn-h:    80px;
    --btn-w:    80px;
    --gap:      12px;
  }

  html, body {
    width: 100%; height: 100%;
    background: var(--bg);
    color: var(--text);
    font-family: 'Share Tech Mono', monospace;
    overflow: hidden;
    touch-action: none;
  }

  body::after {
    content: '';
    position: fixed; inset: 0;
    background: repeating-linear-gradient(
      0deg,
      transparent,
      transparent 2px,
      rgba(0,0,0,0.08) 2px,
      rgba(0,0,0,0.08) 4px
    );
    pointer-events: none;
    z-index: 100;
  }

  #app {
    display: flex;
    flex-direction: row;
    align-items: center;
    justify-content: space-around;
    width: 100vw;
    height: 100vh;
    padding: 16px 24px;
    gap: 16px;
  }

  #title {
    position: fixed;
    top: 0; left: 0; right: 0;
    text-align: center;
    font-family: 'Bebas Neue', sans-serif;
    font-size: 13px;
    letter-spacing: 6px;
    color: var(--accent);
    padding: 5px 0;
    background: rgba(0,0,0,0.6);
    z-index: 50;
  }

  #status {
    position: fixed;
    top: 6px; right: 16px;
    font-size: 10px;
    letter-spacing: 2px;
    color: var(--accent);
    display: flex; align-items: center; gap: 6px;
    z-index: 51;
  }
  #dot {
    width: 7px; height: 7px;
    border-radius: 50%;
    background: var(--accent);
    box-shadow: 0 0 8px var(--accent);
    animation: pulse 1.4s ease-in-out infinite;
  }
  /* FIX 1: watchdog timeout turns dot red */
  #dot.dead {
    background: var(--red);
    box-shadow: 0 0 8px var(--red);
    animation: none;
  }
  @keyframes pulse {
    0%,100% { opacity: 1; }
    50%      { opacity: 0.3; }
  }

  #dpad {
    display: grid;
    grid-template-columns: var(--btn-w) var(--btn-w) var(--btn-w);
    grid-template-rows:    var(--btn-h) var(--btn-h) var(--btn-h);
    gap: var(--gap);
    flex-shrink: 0;
  }

  #right-panel {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 24px;
    flex-shrink: 0;
  }

  #spin-row { display: flex; gap: var(--gap); }

  .btn {
    width: var(--btn-w);
    height: var(--btn-h);
    border: 2px solid var(--dim);
    border-radius: 10px;
    background: var(--panel);
    color: var(--text);
    font-family: 'Share Tech Mono', monospace;
    font-size: 11px;
    letter-spacing: 1px;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 4px;
    cursor: pointer;
    user-select: none;
    -webkit-user-select: none;
    transition: border-color 0.1s, background 0.1s, box-shadow 0.1s;
    -webkit-tap-highlight-color: transparent;
  }
  .btn svg { width: 28px; height: 28px; fill: var(--text); transition: fill 0.1s; }
  .btn span { font-size: 9px; letter-spacing: 2px; opacity: 0.6; }

  .btn.active, .btn:active {
    background: rgba(232, 255, 0, 0.12);
    border-color: var(--accent);
    box-shadow: 0 0 18px rgba(232, 255, 0, 0.3), inset 0 0 8px rgba(232, 255, 0, 0.08);
  }
  .btn.active svg, .btn:active svg { fill: var(--accent); }

  .btn.spin.active, .btn.spin:active {
    background: rgba(255, 45, 45, 0.12);
    border-color: var(--red);
    box-shadow: 0 0 18px rgba(255, 45, 45, 0.3), inset 0 0 8px rgba(255, 45, 45, 0.08);
  }
  .btn.spin.active svg, .btn.spin:active svg { fill: var(--red); }
  .btn.spin svg { fill: var(--text); }

  #btn-fwd  { grid-column: 2; grid-row: 1; }
  #btn-left { grid-column: 1; grid-row: 2; }
  #btn-stop { grid-column: 2; grid-row: 2; }
  #btn-right{ grid-column: 3; grid-row: 2; }
  #btn-bwd  { grid-column: 2; grid-row: 3; }

  #btn-stop {
    border-color: #3a1a1a;
    background: #180a0a;
  }
  #btn-stop svg { fill: #ff5555; }
  #btn-stop:active, #btn-stop.active {
    border-color: var(--red);
    box-shadow: 0 0 18px rgba(255,45,45,0.4);
    background: rgba(255,45,45,0.15);
  }

  #slider-wrap {
    width: 200px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  #slider-label {
    display: flex;
    justify-content: space-between;
    font-size: 10px;
    letter-spacing: 3px;
    color: var(--accent);
  }
  #speed-slider {
    -webkit-appearance: none;
    appearance: none;
    width: 100%;
    height: 6px;
    border-radius: 3px;
    background: linear-gradient(to right, var(--accent) 0%, var(--accent) var(--pct, 78%), var(--dim) var(--pct, 78%));
    outline: none;
    cursor: pointer;
  }
  #speed-slider::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 22px; height: 22px;
    border-radius: 50%;
    background: var(--accent);
    box-shadow: 0 0 12px rgba(232,255,0,0.6);
    border: 3px solid var(--bg);
    cursor: pointer;
  }
  #speed-slider::-moz-range-thumb {
    width: 22px; height: 22px;
    border-radius: 50%;
    background: var(--accent);
    box-shadow: 0 0 12px rgba(232,255,0,0.6);
    border: 3px solid var(--bg);
    cursor: pointer;
  }

  #spin-label {
    font-size: 10px;
    letter-spacing: 3px;
    color: var(--red);
    text-align: center;
  }

  .divider {
    width: 200px;
    height: 1px;
    background: linear-gradient(to right, transparent, var(--dim), transparent);
  }
</style>
</head>
<body>

<div id="title">FAIL&nbsp;&nbsp;MARY&nbsp;&nbsp;·&nbsp;&nbsp;RC&nbsp;&nbsp;CONTROLLER</div>
<div id="status"><div id="dot"></div><span id="status-text">LIVE</span></div>

<div id="app">

  <div id="dpad">
    <button class="btn" id="btn-fwd"
      ontouchstart="hold('forward')" ontouchend="release()"
      onmousedown="hold('forward')" onmouseup="release()">
      <svg viewBox="0 0 24 24"><path d="M12 4l8 10H4z"/></svg>
      <span>FWD</span>
    </button>

    <button class="btn" id="btn-left"
      ontouchstart="hold('left')" ontouchend="release()"
      onmousedown="hold('left')" onmouseup="release()">
      <svg viewBox="0 0 24 24"><path d="M4 12l10-8v16z"/></svg>
      <span>LEFT</span>
    </button>

    <!-- FIX 5: removed void(0) from ontouchend; stop is a one-shot, no release needed -->
    <button class="btn" id="btn-stop"
      ontouchstart="send('stop')" ontouchend="return false"
      onmousedown="send('stop')">
      <svg viewBox="0 0 24 24"><rect x="5" y="5" width="14" height="14" rx="2"/></svg>
      <span>STOP</span>
    </button>

    <button class="btn" id="btn-right"
      ontouchstart="hold('right')" ontouchend="release()"
      onmousedown="hold('right')" onmouseup="release()">
      <svg viewBox="0 0 24 24"><path d="M20 12L10 4v16z"/></svg>
      <span>RIGHT</span>
    </button>

    <button class="btn" id="btn-bwd"
      ontouchstart="hold('backward')" ontouchend="release()"
      onmousedown="hold('backward')" onmouseup="release()">
      <svg viewBox="0 0 24 24"><path d="M12 20l8-10H4z"/></svg>
      <span>BWD</span>
    </button>
  </div>

  <div id="right-panel">
    <div id="spin-label">⟳&nbsp;&nbsp;SPIN IN PLACE&nbsp;&nbsp;⟳</div>

    <div id="spin-row">
      <button class="btn spin" id="btn-spinL"
        ontouchstart="hold('spinleft')" ontouchend="release()"
        onmousedown="hold('spinleft')" onmouseup="release()">
        <svg viewBox="0 0 24 24">
          <path d="M12 4a8 8 0 1 0 7.4 5h-2.2A6 6 0 1 1 12 6V4z"/>
          <path d="M9 1l3 4-4 2z"/>
        </svg>
        <span>↺ CCW</span>
      </button>

      <button class="btn spin" id="btn-spinR"
        ontouchstart="hold('spinright')" ontouchend="release()"
        onmousedown="hold('spinright')" onmouseup="release()">
        <svg viewBox="0 0 24 24">
          <path d="M12 4a8 8 0 1 1-7.4 5h2.2A6 6 0 1 0 12 6V4z"/>
          <path d="M15 1l-3 4 4 2z"/>
        </svg>
        <span>↻ CW</span>
      </button>
    </div>

    <div class="divider"></div>

    <div id="slider-wrap">
      <div id="slider-label">
        <span>SPEED</span>
        <span id="spd-val">78%</span>
      </div>
      <input type="range" id="speed-slider" min="0" max="255" value="200"
        oninput="updateSpeed(this.value)">
    </div>
  </div>

</div>

<script>
  var holdTimer  = null;
  var activeBtn  = null;

  // FIX 4: track in-flight fetch to avoid command backlog.
  // If the previous request hasn't returned yet we skip
  // this tick rather than queuing another fetch behind it.
  var fetchInFlight = false;

  // FIX 1: watchdog — ping the server; if fetch fails
  // the server-side watchdog will stop the motors on its own,
  // but we also update the UI dot to show loss of connection.
  var lastAckTime = Date.now();
  setInterval(function() {
    if (Date.now() - lastAckTime > 1000) {
      document.getElementById('dot').classList.add('dead');
      document.getElementById('status-text').textContent = 'TIMEOUT';
    }
  }, 500);

  function send(cmd) {
    // FIX 4: skip this send if a fetch is already in-flight
    if (fetchInFlight) return;
    fetchInFlight = true;
    fetch('/cmd?action=' + cmd)
      .then(function() {
        lastAckTime = Date.now();
        document.getElementById('dot').classList.remove('dead');
        document.getElementById('status-text').textContent = 'LIVE';
      })
      .catch(function() {})
      .finally(function() { fetchInFlight = false; });
  }

  function hold(cmd) {
    if (activeBtn) clearBtn(activeBtn);
    var id = 'btn-' + cmdToId(cmd);
    activeBtn = document.getElementById(id);
    if (activeBtn) activeBtn.classList.add('active');

    send(cmd);
    holdTimer = setInterval(function() { send(cmd); }, 100);
  }

  function release() {
    if (holdTimer) { clearInterval(holdTimer); holdTimer = null; }
    if (activeBtn) { activeBtn.classList.remove('active'); activeBtn = null; }
    send('stop');
  }

  function cmdToId(cmd) {
    var map = {
      forward: 'fwd', backward: 'bwd', left: 'left', right: 'right',
      spinleft: 'spinL', spinright: 'spinR'
    };
    return map[cmd] || cmd;
  }

  function clearBtn(el) {
    if (el) el.classList.remove('active');
  }

  // FIX 3 & 6: speed change takes effect immediately.
  // We send /speed (which re-applies the current command
  // server-side at the new speed) rather than just updating
  // a local variable that only matters on the next press.
  function updateSpeed(val) {
    var pct = Math.round(val / 255 * 100);
    document.getElementById('spd-val').textContent = pct + '%';
    document.getElementById('speed-slider').style.setProperty('--pct', pct + '%');
    fetch('/speed?val=' + val).catch(function(){});
  }

  updateSpeed(200);

  document.addEventListener('contextmenu', function(e){ e.preventDefault(); });
</script>
</body>
</html>
)rawhtml";

// ══════════════════════════════════════════════════════════
//  HTTP route handlers
// ══════════════════════════════════════════════════════════

void handleRoot() {
  server.send_P(200, "text/html", PAGE);
}

void handleCmd() {
  if (!server.hasArg("action")) { server.send(400, "text/plain", "missing action"); return; }
  String action = server.arg("action");

  // FIX 1: any command (including "stop") resets the watchdog
  lastCmdTime = millis();
  motorsStopped = false;

  // FIX 6: record which command is active so speed changes
  // can re-apply it immediately
  currentCmd = action;

  if      (action == "forward")   moveForward();
  else if (action == "backward")  moveBackward();
  else if (action == "left")      turnLeft();
  else if (action == "right")     turnRight();
  else if (action == "spinleft")  spinLeft();
  else if (action == "spinright") spinRight();
  else {
    // "stop" or unknown
    currentCmd = "";
    stopAll();
    motorsStopped = true;
  }

  server.send(200, "text/plain", "OK");
}

void handleSpeed() {
  if (!server.hasArg("val")) { server.send(400, "text/plain", "missing val"); return; }
  motorSpeed = constrain(server.arg("val").toInt(), 0, 255);

  // FIX 3: re-apply the active command at the new speed so
  // the change is felt immediately without releasing the button
  applyCurrentCmd();

  server.send(200, "text/plain", "OK");
}

// ══════════════════════════════════════════════════════════
//  setup()
// ══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  ledcAttach(LEFT_RPWM,  PWM_FREQ, PWM_RES);
  ledcAttach(LEFT_LPWM,  PWM_FREQ, PWM_RES);
  ledcAttach(RIGHT_RPWM, PWM_FREQ, PWM_RES);
  ledcAttach(RIGHT_LPWM, PWM_FREQ, PWM_RES);

  stopAll();

  // FIX 1: initialise watchdog timestamp so we don't
  // trigger a false stop during the boot window
  lastCmdTime = millis();

  WiFi.mode(WIFI_AP);
  IPAddress localIP(192, 168, 4, 2);
  IPAddress gateway(192, 168, 4, 2);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(localIP, gateway, subnet);
  WiFi.softAP(SSID, PASSWORD);
  Serial.println("AP started");
  Serial.print("SSID: ");     Serial.println(SSID);
  Serial.print("IP:   ");     Serial.println(WiFi.softAPIP());

  server.on("/",      handleRoot);
  server.on("/cmd",   handleCmd);
  server.on("/speed", handleSpeed);
  server.begin();

  Serial.println("HTTP server started");
}

// ══════════════════════════════════════════════════════════
//  loop()
// ══════════════════════════════════════════════════════════
void loop() {
  server.handleClient();

  // FIX 1: dead-man watchdog
  // If no command has arrived within WATCHDOG_MS, stop motors.
  // This catches: dropped WiFi, closed browser, phone sleep,
  // app crash — anything that cuts off the command stream.
  if (!motorsStopped && (millis() - lastCmdTime > WATCHDOG_MS)) {
    stopAll();
    currentCmd = "";
    motorsStopped = true;
    Serial.println("WATCHDOG: no command received — motors stopped");
  }
}
