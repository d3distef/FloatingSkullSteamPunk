/**********************************************************************
  Floating Skull LEDs (ESP32 + FastLED + Web UI + Concentric Paint + OTA)

  • TWO rings total 122 WS2812B on GPIO21:
      Primary: indices 0..60 with geometry:
        R4 (outer): 24
        R3:         16
        R2:         12
        R1:          8
        R0 (center): 1
      Mirror: indices 61..121 automatically mirror the primary each frame
              (facing ring: 12 o’clock aligned, direction reversed).

  • Button on GPIO9 (momentary to GND, INPUT_PULLUP)
      - Short tap  (< ~1000 ms): Full board reset (ESP.restart)
      - Long hold (>= ~1000 ms): Toggle power (OFF/ON)

  • Web UI: effect select, auto-cycle, brightness, painter.
  • OTA on mDNS name.
***********************************************************************/

#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include "secrets.h"

#define FASTLED_RMT_MAX_CHANNELS 1
#define FASTLED_ESP32_FLASH_LOCK 1

// ─────────── Names / Wi-Fi ───────────
static const char* STA_SSID   = WIFI_SSID;
static const char* STA_PASS   = WIFI_PASS;
static const char* AP_SSID    = "FloatingSkull";
static const char* AP_PASS    = "skullskull";
static const char* MDNS_NAME  = "floating-skull";  // http://floating-skull.local/

// ─────────── LED config ───────────
#define LED_PIN            21
#define LED_TYPE           WS2812B
#define COLOR_ORDER        GRB
#define NUM_LEDS           122        // total (primary + mirror)
#define NUM_PRIMARY        61         // primary geometry length

// Ring layout (center-out) for the PRIMARY only
static const uint8_t RINGS = 5;
static const uint8_t RING_COUNTS[RINGS] = { 1, 8, 12, 16, 24 };  // R0..R4 (R4 outermost)
static uint16_t      RING_OFFSETS[RINGS];                        // computed in setup()

// ─────────── Button (momentary to GND) ───────────
#define BTN_PIN           D9
static const uint16_t HOLD_MS = 1000;   // 1.0 s threshold

// State
static bool     btnLast     = false;
static uint32_t btnDownAt   = 0;
static bool     longFired   = false;

// ─────────── Behavior ───────────
#define EFFECT_DWELL_MS   (3UL * 60UL * 1000UL)   // rotate every 3 minutes
#define STARTUP_BLUE_MS   500UL

// ─────────── Power / Brightness ───────────
#define MASTER_BRIGHTNESS 255
#define VOLTS             5
#define MAX_MA            10000                    // both rings

// ─────────── Globals ───────────
CRGB leds[NUM_LEDS];

// Keep your original global flip, but across the WHOLE strip
static inline uint16_t phys(uint16_t logicalIdx) {
  return (NUM_LEDS - 1) - logicalIdx;
}

// Always write via logical indices
static inline void setL(uint16_t logicalIdx, const CRGB& c) {
  leds[phys(logicalIdx)] = c;
}

uint8_t  gHue = 0;
uint8_t  currentEffect = 0;
uint32_t effectStartMs = 0;
bool     paintMode = false;
bool     hardOff = false;

WebServer server(80);

// Global tempo control (bigger = slower)
uint8_t SPEED_DIV = 3;

static inline bool tickDiv(uint8_t div) {
  static uint32_t f = 0;
  return ((++f % div) == 0);
}

// ─────────── Helpers: ring index & pos (PRIMARY GEOMETRY 0..60) ───────────
static inline uint8_t ringOf(uint16_t idx) {
  for (uint8_t r = 0; r < RINGS; ++r) {
    uint16_t start = RING_OFFSETS[r];
    uint16_t end   = start + RING_COUNTS[r];
    if (idx >= start && idx < end) return r;
  }
  return RINGS - 1;
}
static inline uint16_t posInRing(uint16_t idx) {
  uint8_t r = ringOf(idx);
  return idx - RING_OFFSETS[r];
}
static float angleFor(uint16_t idx) {
  uint8_t r = ringOf(idx);
  uint16_t k = posInRing(idx);
  uint8_t n = RING_COUNTS[r];
  if (n == 1) return 0.0f;
  return (2.0f * PI) * (float)k / (float)n;
}

// ─────────── Mirroring (PRIMARY → SECOND) ───────────
// Facing-ring mirror: keep 12 o’clock aligned, reverse direction per ring.
static inline uint16_t mirrorIndexSecond(uint16_t idx) {
  // idx in 0..60
  uint8_t  r  = ringOf(idx);
  uint16_t k  = posInRing(idx);
  uint8_t  n  = RING_COUNTS[r];
  uint16_t k2 = (n == 1) ? 0 : ((n - k) % n);
  return NUM_PRIMARY + RING_OFFSETS[r] + k2;    // 61..121
}
static inline uint16_t primaryFromSecond(uint16_t idx2) {
  // idx2 in 61..121
  uint16_t base = idx2 - NUM_PRIMARY;  // 0..60 in same ring geometry
  uint8_t  r    = ringOf(base);
  uint16_t k2   = posInRing(base);
  uint8_t  n    = RING_COUNTS[r];
  uint16_t k    = (n == 1) ? 0 : ((n - k2) % n);
  return RING_OFFSETS[r] + k;          // 0..60
}
// Copy primary buffer to second ring with mirroring
static inline void duplicateMirrored() {
  for (uint16_t i = 0; i < NUM_PRIMARY; ++i) {
    setL(mirrorIndexSecond(i), leds[phys(i)]);
  }
}

// ─────────── Effect prototypes (unchanged code paths) ───────────
void fx_centerPulse();
void fx_rippleRings();
void fx_ringChase();
void fx_swirlPolar();
void fx_glowCoreWaves();
void fx_outerComet();

typedef void (*EffectFn)();
EffectFn effects[] = {
  fx_centerPulse,   // 0
  fx_rippleRings,   // 1
  fx_ringChase,     // 2
  fx_swirlPolar,    // 3
  fx_glowCoreWaves, // 4
  fx_outerComet     // 5
};
static const char* effectNames[] = {
  "Center Pulse",
  "Ripple Rings",
  "Ring Chase",
  "Polar Swirl",
  "Core Waves",
  "Outer Comet"
};
static const uint8_t EFFECT_COUNT = sizeof(effects)/sizeof(effects[0]);

// ─────────── Core web pages ───────────
String htmlRoot() {
  String h;
  h.reserve(8000);
  h += F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Floating Skull LEDs</title>"
    "<style>"
      "body{font-family:system-ui,Arial;background:#0b0b0f;color:#e8ecf1;text-align:center;padding:14px}"
      "button{margin:6px;padding:10px 16px;border:0;border-radius:10px;background:#2a2f3a;color:#fff;cursor:pointer}"
      "button:hover{background:#3a4150}"
      ".row{margin:12px 0}"
      ".pill{display:inline-block;padding:6px 10px;border-radius:999px;background:#191c23;margin:2px}"
      "input[type=range]{width:280px}"
      "small{color:#9aa4b2}"
      "a{color:#86c5ff}"
    "</style></head><body>"
    "<h2>Floating Skull LED Controller</h2>"
    "<div class='row'><span class='pill'>Power: <b id='pwr'>...</b></span>"
    "<span class='pill'>Effect: <b id='ename'>...</b></span>"
    "<span class='pill'>Mode: <b id='pmode'>...</b></span></div>"
    "<div class='row'>"
  );
  for (uint8_t i=0;i<EFFECT_COUNT;i++){
    h += "<button onclick=\"go('/set?anim="+String(i)+"')\">"+String(effectNames[i])+"</button>";
    if((i%3)==2) h += "<br>";
  }
  h += F(
    "</div><div class='row'>"
      "<button onclick=\"go('/set?anim=255')\">Auto Cycle</button>"
      "<button onclick=\"go('/set?anim=254')\">All Off</button>"
      "<button onclick=\"location.href='/paint'\">Paint</button>"
    "</div>"
    "<div class='row'>Brightness<br>"
      "<input id='br' type='range' min='5' max='255' value='255' oninput='setB(this.value)'>"
    "</div>"
    "<hr>"
    "<small>Short press: full reset. Long hold (≥1s): power toggle.</small>"
    "<script>"
      "function go(p){fetch(p).then(()=>refresh())}"
      "function setB(v){fetch('/setb?b='+v)}"
      "async function refresh(){"
        "let r=await fetch('/api/status'); let s=await r.json();"
        "document.getElementById('pwr').textContent=s.power?'ON':'OFF';"
        "document.getElementById('ename').textContent=s.effect_name;"
        "document.getElementById('pmode').textContent=s.paint?'PAINT':'NORMAL';"
        "document.getElementById('br').value=s.brightness;"
      "}"
      "setInterval(refresh, 800); refresh();"
    "</script>"
    "</body></html>"
  );
  return h;
}

// Concentric painter UI (shows all 122; handler maps correctly)
// Concentric painter UI (show ONLY the primary 61 pixels)
void handlePaintPage() {
  paintMode = true;
  hardOff   = false; // entering paint turns power ON

  const int radii[RINGS] = { 20, 48, 78, 110, 145 };
  const int W = 340, H = 340, CX = W/2, CY = H/2;
  const int dotR = 8;

  String page;
  page.reserve(16000);
  page += F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Paint Rings — Floating Skull</title>"
    "<style>"
      "body{font-family:system-ui,Arial;background:#0b0b0f;color:#e8ecf1;text-align:center;padding:12px}"
      "#palette div{display:inline-block;width:28px;height:28px;cursor:pointer;border:2px solid #333;margin:3px;border-radius:6px}"
      "button{margin:6px;padding:10px 16px;border:0;border-radius:10px;background:#2a2f3a;color:#fff;cursor:pointer}"
      "button:hover{background:#3a4150}"
      "a{color:#86c5ff}"
      "svg{background:#0e1117;border:1px solid #202733;border-radius:10px}"
    "</style></head><body>"
    "<h2>Paint — Concentric Rings (61 LEDs)</h2>"
    "<div id='palette'></div><br>"
    "<svg id='skull' width='"
  );
  page += String(W);
  page += F("' height='");
  page += String(H);
  page += F("'>");

  // background guides
  for (int r = 0; r < RINGS; ++r) {
    page += "<circle cx='" + String(CX) + "' cy='" + String(CY) + "' r='" + String(radii[r]) + "' fill='none' stroke='#1d2430' stroke-dasharray='4,6'/>";
  }

  // PRIMARY ONLY: indices 0..60
  uint16_t idx = 0;
  for (int r = 0; r < RINGS; ++r) {
    int n = RING_COUNTS[r];
    if (n == 1) {
      page += "<circle class='px' data-idx='"+String(idx)+"' cx='"+String(CX)+"' cy='"+String(CY)+"' r='"+String(dotR)+"' fill='#222' stroke='#444'/>";
      idx++;
      continue;
    }
    for (int k = 0; k < n; ++k, ++idx) {
      float ang = 2.0f * PI * (float)k / (float)n;
      int x = CX + int(cosf(ang) * radii[r]);
      int y = CY + int(sinf(ang) * radii[r]);
      page += "<circle class='px' data-idx='"+String(idx)+"' cx='"+String(x)+"' cy='"+String(y)+"' r='"+String(dotR)+"' fill='#222' stroke='#444'/>";
    }
  }

  page += F(
    "</svg><br><br>"
    "<button onclick=\"location.href='/'\">Back</button>"
    "<script>"
      "const colors=[\"FF0000\",\"00FF00\",\"0000FF\",\"FFFF00\",\"FF00FF\",\"00FFFF\",\"FFFFFF\",\"000000\",\"FFA500\",\"800080\",\"008080\",\"FFC0CB\",\"808000\",\"00FF80\",\"404040\",\"FFD700\",\"00AEEF\",\"39FF14\",\"B0B0B0\"];"
      "let current=\"FF0000\";"
      "function pick(sw,col){document.querySelectorAll('#palette div').forEach(d=>d.style.borderColor='#333'); sw.style.borderColor='#FFF'; current=col;}"
      "const pal=document.getElementById('palette');"
      "colors.forEach(col=>{const sw=document.createElement('div');sw.style.background='#'+col;sw.onclick=()=>pick(sw,col);pal.appendChild(sw);});"
      "let drawing=false;"
      "function paint(el){if(!el||!el.dataset.idx) return; el.setAttribute('fill','#'+current); fetch(`/setpixel?idx=${el.dataset.idx}&hex=${current}`)}"
      "document.querySelectorAll('.px').forEach(e=>{"
        "e.onmousedown=ev=>{drawing=true;paint(e);};"
        "e.onmouseover=ev=>{if(drawing)paint(e);};"
        "e.ontouchstart=ev=>{drawing=true;paint(e);ev.preventDefault();};"
        "e.ontouchmove=ev=>{const t=ev.touches[0]; paint(document.elementFromPoint(t.clientX,t.clientY));};"
      "});"
      "document.body.onmouseup = ()=>drawing=false;"
      "document.body.ontouchend= ()=>drawing=false;"
    "</script></body></html>"
  );

  server.send(200, "text/html", page);
}


// ─────────── Web handlers ───────────
void handleRoot(){ paintMode = false; server.send(200,"text/html",htmlRoot()); }

void handleSet(){
  if(!server.hasArg("anim")){ server.send(400,"text/plain","Missing anim"); return; }
  int v = server.arg("anim").toInt();
  paintMode = false;

  if (v == 254) {                 // HARD OFF
    hardOff = true;
    FastLED.clear(true);
    FastLED.show();
  } else if (v == 255) {          // AUTO
    hardOff = false;
    currentEffect = 0;
    effectStartMs = millis();
  } else if (v >= 0 && v < EFFECT_COUNT) {  // Specific effect
    hardOff = false;
    currentEffect = (uint8_t)v;
    effectStartMs = millis();
  }
  server.sendHeader("Location","/"); server.send(302,"text/plain","");
}

void handleSetB(){
  if(server.hasArg("b")){
    uint16_t b = constrain(server.arg("b").toInt(), 5, 255);
    FastLED.setBrightness(b);
    duplicateMirrored();
    FastLED.show();
    server.send(200,"text/plain","OK");
  } else server.send(400,"text/plain","Missing b");
}

void handleAPIStatus(){
  char buf[256];
  snprintf(buf,sizeof(buf),
    "{\"power\":%s,\"effect\":%u,\"effect_name\":\"%s\",\"paint\":%s,\"brightness\":%u}",
    hardOff?"false":"true",
    currentEffect, effectNames[currentEffect],
    paintMode?"true":"false",
    FastLED.getBrightness()
  );
  server.send(200,"application/json",buf);
}

// Paint handler: allow painting either half; always mirror
void handleSetPixel(){
  if(server.hasArg("idx") && server.hasArg("hex")){
    uint16_t idx = server.arg("idx").toInt();
    uint32_t col = strtoul(server.arg("hex").c_str(), nullptr, 16);
    CRGB c(col);

    if(idx < NUM_LEDS){
      if (idx < NUM_PRIMARY) {
        setL(idx, c);
        setL(mirrorIndexSecond(idx), c);
      } else {
        uint16_t p = primaryFromSecond(idx);
        setL(p, c);
        setL(idx, c);
      }
      if (!hardOff) { duplicateMirrored(); FastLED.show(); }
    }
    server.send(200,"text/plain","OK");
  } else {
    server.send(400,"text/plain","Missing parameters");
  }
}

void handleDiag() {
  static uint16_t idx = 0;
  FastLED.clear();
  setL(idx % NUM_PRIMARY, CRGB::White);   // step through primary only
  duplicateMirrored();
  FastLED.show();
  idx = (idx + 1) % NUM_PRIMARY;
  server.send(200,"text/plain","OK");
}

// ─────────── Wi-Fi / Web / OTA ───────────
void setupWiFi(){
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.begin(STA_SSID, STA_PASS);
  uint32_t t0 = millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<7000) delay(100);
  MDNS.begin(MDNS_NAME);
}

void setupWeb(){
  server.on("/",            handleRoot);
  server.on("/set",         handleSet);
  server.on("/setb",        handleSetB);
  server.on("/api/status",  handleAPIStatus);
  server.on("/paint",       handlePaintPage);
  server.on("/setpixel",    handleSetPixel);
  server.on("/diag/step",   handleDiag);
  server.begin();
}

void setupOTA(){
  ArduinoOTA.setHostname(MDNS_NAME);
  ArduinoOTA.begin();
}

// ─────────── Button handling ───────────
void handleButton() {
  bool pressed = (digitalRead(BTN_PIN) == LOW);  // INPUT_PULLUP assumed
  uint32_t now = millis();

  if (pressed && !btnLast) { btnDownAt = now; longFired = false; }

  if (pressed && !longFired && (now - btnDownAt) >= HOLD_MS) {
    longFired = true;
    if (hardOff) {
      hardOff = false;
      fill_solid(leds, NUM_PRIMARY, CRGB(0, 0, 60));
      duplicateMirrored();
      FastLED.show();
      delay(STARTUP_BLUE_MS);
      effectStartMs = now;
    } else {
      hardOff = true;
      FastLED.clear(true);
      FastLED.show();
    }
  }

  if (!pressed && btnLast) {
    uint32_t dur = now - btnDownAt;
    if (!longFired && dur < HOLD_MS) {
      FastLED.clear(true);
      FastLED.show();
      delay(20);
      ESP.restart();
      return;
    }
  }
  btnLast = pressed;
}

// ─────────── Setup / Loop ───────────
void setup() {
  // Compute ring offsets (PRIMARY geometry)
  uint16_t acc = 0;
  for (uint8_t r=0;r<RINGS;r++){ RING_OFFSETS[r] = acc; acc += RING_COUNTS[r]; }

  pinMode(BTN_PIN, INPUT_PULLUP);

  delay(100);
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setDither(true);
  FastLED.setCorrection(TypicalLEDStrip);
  FastLED.setTemperature(Tungsten40W);
  FastLED.setMaxPowerInVoltsAndMilliamps(VOLTS, MAX_MA);
  FastLED.setBrightness(MASTER_BRIGHTNESS);
  FastLED.clear(true);

  // Startup flash on primary, then mirror
  fill_solid(leds, NUM_PRIMARY, CRGB(0, 0, 60));
  duplicateMirrored();
  FastLED.show();
  delay(STARTUP_BLUE_MS);

  effectStartMs = millis();

  setupWiFi();
  setupWeb();
  setupOTA();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  handleButton();

  if (!hardOff && !paintMode) {
    uint32_t now = millis();
    if (now - effectStartMs > EFFECT_DWELL_MS) {
      currentEffect = (currentEffect + 1) % EFFECT_COUNT;
      effectStartMs = now;
    }
    // Render on PRIMARY ONLY, then mirror and show
    effects[currentEffect]();
    duplicateMirrored();
    FastLED.show();
  }

  EVERY_N_MILLISECONDS(20) { gHue++; }
}

/* ===================== EFFECTS (render PRIMARY ONLY) ===================== */

// Utility
static inline void blackPrimary() { for (uint16_t i=0;i<NUM_PRIMARY;i++) setL(i, CRGB::Black); }

// 0) Center Pulse — slower breathing
void fx_centerPulse() {
  uint8_t breath = beatsin8(6, 40, 255);
  for (uint16_t i=0;i<NUM_PRIMARY;i++){
    uint8_t r = ringOf(i);
    uint8_t atten = 255 - r * 40;
    uint8_t v = scale8(breath, (uint8_t)max(20, (int)atten));
    setL(i, CHSV(160, 220, v));
  }
}

// 1) Ripple Rings — advance slower using SPEED_DIV
void fx_rippleRings() {
  static uint16_t t = 0;
  if (tickDiv(SPEED_DIV)) t += 1;
  blackPrimary();
  float phase = (t & 0x1FF) / 512.0f;
  for (uint8_t r=0;r<RINGS;r++){
    float rr = (float)r / (float)(RINGS-1);
    float d = fabsf(rr - phase);
    uint8_t v = (d < 0.22f) ? (uint8_t)((0.22f - d) * (255.0f/0.22f)) : 0;
    CHSV col = CHSV(128 + r*16 + (gHue>>2), 200, v);
    uint16_t start = RING_OFFSETS[r];
    for (uint16_t k=0;k<RING_COUNTS[r];k++){
      setL(start + k, col);
    }
  }
}

// 2) Ring Chase — slower head movement
void fx_ringChase() {
  static uint16_t step = 0;
  if (tickDiv(SPEED_DIV)) step++;
  blackPrimary();
  for (uint8_t r=0;r<RINGS;r++){
    uint16_t n = RING_COUNTS[r];
    uint16_t start = RING_OFFSETS[r];
    if (n == 1) { setL(start, CHSV(200 + gHue, 200, 200)); continue; }
    uint16_t head = (step / (3 + r)) % n;
    for (uint16_t k=0;k<n;k++){
      int16_t d = (int16_t)k - (int16_t)head; if (d < 0) d += n;
      uint8_t fall = qsub8(220, (uint8_t)(d * (180 / max<uint16_t>(1, n/3))));
      setL(start + k, CHSV((r&1 ? 180 : 20) + gHue, 230, fall));
    }
  }
}

// 3) Polar Swirl — slower hue rotation
void fx_swirlPolar() {
  static uint16_t t = 0;
  if (tickDiv(SPEED_DIV)) t += 1;
  for (uint16_t i=0;i<NUM_PRIMARY;i++){
    uint8_t r = ringOf(i);
    float ang = angleFor(i);
    uint8_t ang8 = (uint8_t)(ang * 255.0f / (2.0f * PI));
    uint8_t hue = ang8 + (t>>1);
    uint8_t val = 140 + (r * 20);
    setL(i, CHSV(hue, 220, val));
  }
}

// 4) Core Waves — soften and slow
void fx_glowCoreWaves() {
  static uint16_t t = 0;
  if (tickDiv(SPEED_DIV)) t += 1;
  for (uint16_t i=0;i<NUM_PRIMARY;i++){
    uint8_t r = ringOf(i);
    uint8_t n = RING_COUNTS[r];
    uint8_t k = posInRing(i);
    uint8_t phase = (r*36) + (uint8_t)((k * (255/n))) + (t>>1);
    uint8_t v = sin8(phase);
    setL(i, CHSV(140 + r*10, 200, v));
  }
}

// 5) Outer Comet — slower sweep, longer tail
void fx_outerComet() {
  static uint16_t s = 0;
  if (tickDiv(SPEED_DIV)) s += 1;
  for (uint16_t i=0;i<NUM_PRIMARY;i++){
    setL(i, CHSV(160, 200, 16));
  }
  uint8_t r = 4;
  uint16_t n = RING_COUNTS[r];
  uint16_t start = RING_OFFSETS[r];
  uint16_t head = (s/3) % n;
  for (uint16_t k=0;k<n;k++){
    int16_t d = (int16_t)k - (int16_t)head; if (d<0) d+=n;
    uint8_t val = qsub8(255, (uint8_t)(d * (140 / max<uint16_t>(1, n/2))));
    setL(start + k, CHSV(20 + (gHue>>1), 240, val));
  }
}
