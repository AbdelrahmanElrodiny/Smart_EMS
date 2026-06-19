/*
 * ================================================================
 *  Smart Environmental Monitoring System  (Smart-EMS)  v3.0
 * ================================================================
 *  MCU         : ESP32
 *  Sensors     : BMP280 (temperature + pressure)
 *                MQ-135 (air quality — AQI & PPM estimate)
 *                Microphone module (noise level)
 *  Display     : DWIN DGUS II  DMG80480C050_03WTR  (800 × 480)
 *  Indicators  : 3 × LEDs (Green/Orange/Red)  +  Active Buzzer
 *  Time        : DS1307 RTC  |  millis()-based fallback if absent
 *  Connectivity: WiFi SoftAP  +  AJAX web dashboard (1 s refresh)
 *
 *  Required libraries (install via Arduino Library Manager):
 *    - Adafruit BMP280 Library
 *    - Adafruit Unified Sensor
 *    - RTClib  (by Adafruit)
 * ================================================================
 */

#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_Sensor.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

// ================================================================
//  Pin Definitions
// ================================================================
#define I2C_SDA    21     // BMP280 + DS1307 SDA
#define I2C_SCL    22     // BMP280 + DS1307 SCL
#define MQ135_PIN  34     // MQ-135 AO — 0..3.3 V via 10k/20k voltage divider
#define MIC_PIN    35     // Microphone AO
#define LED_GREEN  25     // Level 1 — Good
#define LED_ORANGE 26     // Level 2 — Moderate
#define LED_RED    27     // Level 3 — Danger
#define BUZZER_PIN 32
#define BTN_HOUR   18     // INPUT_PULLUP, active-LOW
#define BTN_MIN    19
#define DWIN_TX    17     // ESP32 TX2 → DWIN RX2
#define DWIN_RX    16     // ESP32 RX2 ← DWIN TX2

// ================================================================
//  System Thresholds  (tune to your environment)
// ================================================================
#define TEMP_GOOD       28.0f   // °C: below → Good
#define TEMP_MODERATE   35.0f   // °C: above → Danger

#define AQ_GOOD         150     // MQ-135 raw ADC after burn-in; clean air ≈ 80-150
#define AQ_MODERATE     250

#define NOISE_GOOD      50      // dB (approximate)
#define NOISE_MODERATE  70

// MQ-135 startup warm-up.  The heater takes ~35 s to stabilise after
// cold-start; suppressing AQ during this window prevents false alarms.
#define MQ_WARMUP_MS    35000UL

// ================================================================
//  WiFi SoftAP
// ================================================================
const char* AP_SSID = "Smart-EMS";
const char* AP_PASS = "12345678";

// ================================================================
//  DWIN VP Addresses
//
//  Page 1 — Clock
//    0x1000  Data Variable  Hours   (2 digits, "Display Invalid Zero" ON)
//    0x1001  Data Variable  Minutes
//    0x1002  Data Variable  Seconds
//    0x1003  VAR Icon       AM=0 / PM=1
//
//  Page 2 — Sensors
//    0x1030  Data Display   Temperature (°C integer)
//    0x1040  Data Display   Humidity    (% simulated)
//    0x1050  Data Display   Pressure    (hPa integer)
//    0x1060  Data Display   AQI raw ADC
//    0x1070  Data Display   Noise (dB)
//
//  Page 3 — MQ-135 Detail
//    0x3000  Data Display   AQI Value
//    0x3010  Data Display   PPM estimate
//    0x3020  VAR Icon       Good=0 / Moderate=1 / Danger=2
// ================================================================
#define VP_HOUR        0x1000
#define VP_MIN         0x1001
#define VP_SEC         0x1002
#define VP_AMPM        0x1003
#define VP_TEMP        0x1030
#define VP_HUM         0x1040
#define VP_PRES        0x1050
#define VP_AQ          0x1060
#define VP_NOISE       0x1070
#define VP_P3_AQI      0x3000
#define VP_P3_PPM      0x3010
#define VP_P3_STATUS   0x3020

// ================================================================
//  Library Objects
// ================================================================
Adafruit_BMP280 bme;
RTC_DS1307      rtc;
WebServer       server(80);

// ================================================================
//  Sensor Globals
// ================================================================
float g_temp      = 0.0f;
float g_pressure  = 0.0f;
float g_humidity  = 55.0f;   // simulated — no real humidity sensor
int   g_airQuality = 0;      // MQ-135 raw ADC  (0..4095)
int   g_noiseDB    = 30;     // approximate dB
int   g_systemLevel = 1;     // 1=Good | 2=Moderate | 3=Danger
bool  g_mqReady    = false;  // false during warm-up
bool  g_bmpOK      = false;
bool  g_rtcOK      = false;

// ================================================================
//  Software Clock  (fallback when DS1307 is absent)
// ================================================================
int  g_swHour = 0, g_swMin = 0, g_swSec = 0;
unsigned long g_swLastTick = 0;
String        g_timeStr    = "--:--:-- --";

// ================================================================
//  Non-blocking Timers
// ================================================================
unsigned long t_sensor  = 0;
unsigned long t_display = 0;
unsigned long t_time    = 0;
unsigned long t_buzzer  = 0;
unsigned long t_btnH    = 0;
unsigned long t_btnM    = 0;
bool          g_buzzerOn = false;

#define SENSOR_MS   1000UL
#define DISPLAY_MS  1000UL
#define TIME_MS      500UL
#define DEBOUNCE_MS  250UL

// ================================================================
//  DWIN Protocol — Write a 16-bit word to a VP address
//  Frame: 5A A5 05 82 [AH] [AL] [VH] [VL]
// ================================================================
void dwinWord(uint16_t vp, uint16_t val) {
  Serial2.write(0x5A); Serial2.write(0xA5);
  Serial2.write(0x05); Serial2.write(0x82);
  Serial2.write((uint8_t)(vp  >> 8)); Serial2.write((uint8_t)(vp  & 0xFF));
  Serial2.write((uint8_t)(val >> 8)); Serial2.write((uint8_t)(val & 0xFF));
}

// Switch display page
void dwinPage(uint8_t page) {
  Serial2.write(0x5A); Serial2.write(0xA5);
  Serial2.write(0x07); Serial2.write(0x82);
  Serial2.write(0x00); Serial2.write(0x84);
  Serial2.write(0x5A); Serial2.write(0x01);
  Serial2.write(0x00); Serial2.write(page);
  delay(200);
}

// ================================================================
//  PPM Estimate from MQ-135 ADC
//  Empirical power-law:  PPM = 400 × (ADC / BASE_ADC)^0.8
//  BASE_ADC = your sensor's clean-air reading (read from Serial).
//  Formula gives ~400 ppm at baseline, rises with pollution.
// ================================================================
float calcPPM(int adc) {
  const float BASE_ADC = 150.0f;  // ← set to your clean-air ADC reading
  const float BASE_PPM = 400.0f;
  if (adc <= 0) return BASE_PPM;
  float r = (float)adc / BASE_ADC;
  return constrain(BASE_PPM * powf(r > 1.0f ? r : 1.0f, 0.8f),
                   BASE_PPM, 9999.0f);
}

// ================================================================
//  Sensor Reading
// ================================================================
void readSensors() {
  if (millis() - t_sensor < SENSOR_MS) return;
  t_sensor = millis();

  // --- BMP280 ---
  if (g_bmpOK) {
    g_temp     = bme.readTemperature();
    g_pressure = bme.readPressure() / 100.0f;
  }

  // --- MQ-135: average 8 samples to reduce ADC noise ---
  {
    long sum = 0;
    for (int i = 0; i < 8; i++) { sum += analogRead(MQ135_PIN); delayMicroseconds(500); }
    g_airQuality = (int)(sum / 8);
    g_mqReady    = (millis() >= MQ_WARMUP_MS);
  }

  // --- Microphone: peak-to-peak over 200 ms ---
  // Peak-to-peak captures the AC swing of the audio signal regardless
  // of the DC bias (~VCC/2).  Mapped logarithmically to approximate dB.
  {
    int hi = 0, lo = 4095;
    unsigned long t0 = millis();
    while (millis() - t0 < 200) {
      int v = analogRead(MIC_PIN);
      if (v > hi) hi = v;
      if (v < lo) lo = v;
    }
    int pp = hi - lo;                          // 0..4095 peak-to-peak

    // Log scale mapping:  p-p=0→30 dB | p-p=10→45 | p-p=100→60 | p-p=1000→75
    g_noiseDB = (int)constrain(
      30.0f + 15.0f * log10f((float)(pp > 0 ? pp : 1)),
      30.0f, 100.0f);

    // Debug — print raw p-p so you can verify the sensor is responding
    Serial.printf("[MIC] p-p=%d  →  %d dB\n", pp, g_noiseDB);
  }

  // --- Simulated humidity (temperature-based slow drift) ---
  {
    float target = constrain(58.0f + (g_temp - 25.0f) * (-0.6f)
                             + sinf(millis() / 60000.0f) * 2.5f, 30.0f, 80.0f);
    g_humidity += (target - g_humidity) * 0.05f;
    g_humidity  = constrain(g_humidity, 30.0f, 80.0f);
  }

  Serial.printf("[Sensors] T:%.1f°C H:%.0f%% P:%.1fhPa AQ:%d%s Noise:%ddB\n",
    g_temp, g_humidity, g_pressure,
    g_airQuality, g_mqReady ? "" : "(warmup)", g_noiseDB);
}

// ================================================================
//  System Level
// ================================================================
int calcLevel() {
  int aq = g_mqReady ? g_airQuality : 0;   // ignore AQ during warm-up
  bool danger = (g_temp    > TEMP_MODERATE)
             || (aq        > AQ_MODERATE)
             || (g_noiseDB > NOISE_MODERATE);
  bool mod    = !danger
             && ((g_temp    > TEMP_GOOD    && g_temp    <= TEMP_MODERATE)
             ||  (aq        > AQ_GOOD      && aq        <= AQ_MODERATE)
             ||  (g_noiseDB > NOISE_GOOD   && g_noiseDB <= NOISE_MODERATE));
  return danger ? 3 : mod ? 2 : 1;
}

// ================================================================
//  LED + Buzzer
// ================================================================
void updateOutputs() {
  digitalWrite(LED_GREEN,  g_systemLevel == 1 ? HIGH : LOW);
  digitalWrite(LED_ORANGE, g_systemLevel == 2 ? HIGH : LOW);
  digitalWrite(LED_RED,    g_systemLevel == 3 ? HIGH : LOW);

  if (g_systemLevel == 3) {
    if (millis() - t_buzzer >= 400UL) {
      t_buzzer  = millis();
      g_buzzerOn = !g_buzzerOn;
      digitalWrite(BUZZER_PIN, g_buzzerOn ? HIGH : LOW);
    }
  } else {
    g_buzzerOn = false;
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// ================================================================
//  Time Management
// ================================================================
void updateTime() {
  if (millis() - t_time < TIME_MS) return;
  t_time = millis();

  if (g_rtcOK) {
    DateTime n = rtc.now();
    g_swHour = n.hour(); g_swMin = n.minute(); g_swSec = n.second();
    g_swLastTick = millis();
  } else {
    unsigned long e = (millis() - g_swLastTick) / 1000UL;
    if (e > 0) {
      g_swLastTick += e * 1000UL;
      g_swSec += (int)e;
      if (g_swSec >= 60) { g_swSec -= 60; g_swMin++; }
      if (g_swMin >= 60) { g_swMin = 0;   g_swHour++; }
      if (g_swHour >= 24) g_swHour = 0;
    }
  }
  int h = g_swHour % 12; if (!h) h = 12;
  char buf[14];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s",
           h, g_swMin, g_swSec, g_swHour >= 12 ? "PM" : "AM");
  g_timeStr = buf;
}

// ================================================================
//  Button Handling
// ================================================================
void checkButtons() {
  if (digitalRead(BTN_HOUR) == LOW && millis() - t_btnH > DEBOUNCE_MS) {
    t_btnH = millis();
    g_swHour = (g_swHour + 1) % 24;
    if (g_rtcOK) {
      DateTime n = rtc.now();
      rtc.adjust(DateTime(n.year(), n.month(), n.day(), g_swHour, n.minute(), n.second()));
    }
    t_time = 0;
  }
  if (digitalRead(BTN_MIN) == LOW && millis() - t_btnM > DEBOUNCE_MS) {
    t_btnM = millis();
    g_swMin = (g_swMin + 1) % 60; g_swSec = 0;
    if (g_rtcOK) {
      DateTime n = rtc.now();
      rtc.adjust(DateTime(n.year(), n.month(), n.day(), n.hour(), g_swMin, 0));
    }
    t_time = 0;
  }
}

// ================================================================
//  DWIN Display Update  (VP memory is global — all pages updated)
// ================================================================
void updateDisplay() {
  if (millis() - t_display < DISPLAY_MS) return;
  t_display = millis();

  // Page 1 — Clock
  int h = g_swHour % 12; if (!h) h = 12;
  dwinWord(VP_HOUR, (uint16_t)h);
  dwinWord(VP_MIN,  (uint16_t)g_swMin);
  dwinWord(VP_SEC,  (uint16_t)g_swSec);
  dwinWord(VP_AMPM, g_swHour >= 12 ? 1 : 0);  // 0=AM icon | 1=PM icon

  // Page 2 — Sensor values
  dwinWord(VP_TEMP,  g_bmpOK   ? (uint16_t)g_temp       : 0);
  dwinWord(VP_HUM,   (uint16_t)g_humidity);
  dwinWord(VP_PRES,  g_bmpOK   ? (uint16_t)g_pressure    : 0);
  dwinWord(VP_AQ,    g_mqReady ? (uint16_t)g_airQuality  : 0);
  dwinWord(VP_NOISE, (uint16_t)g_noiseDB);

  // Page 3 — MQ-135 detail
  uint16_t aqv = g_mqReady ? (uint16_t)g_airQuality : 0;
  dwinWord(VP_P3_AQI, aqv);
  dwinWord(VP_P3_PPM, g_mqReady ? (uint16_t)calcPPM(g_airQuality) : 0);

  // Status icon:  0=Good | 1=Moderate | 2=Danger
  dwinWord(VP_P3_STATUS, (uint16_t)(g_systemLevel - 1));
}

// ================================================================
//  Web Dashboard — static HTML shell; data filled by AJAX
// ================================================================
String buildHTML() {
  String html;
  html.reserve(5000);
  html += F("<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Smart-EMS</title><style>"
            "*{box-sizing:border-box;margin:0;padding:0}"
            "body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#c9d1d9}"
            ".hdr{background:linear-gradient(135deg,#161b22,#21262d);"
            "padding:15px 20px;border-bottom:2px solid #238636;text-align:center}"
            ".hdr h1{font-size:1.55em;color:#58a6ff}"
            ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;"
            "background:#28a745;margin-left:6px;animation:blink 1s infinite}"
            "@keyframes blink{0%,100%{opacity:1}50%{opacity:.15}}"
            ".wrap{max-width:780px;margin:14px auto;padding:0 12px}"
            ".clk-box{background:#161b22;border-radius:10px;padding:14px;"
            "text-align:center;margin-bottom:14px;border:1px solid #30363d}"
            ".clk{font-size:2.5em;font-weight:700;color:#58a6ff;letter-spacing:3px}"
            ".badge{text-align:center;padding:11px;border-radius:10px;"
            "margin-bottom:14px;font-size:1.2em;font-weight:700;"
            "transition:background .4s,color .4s}"
            ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(185px,1fr));gap:12px}"
            ".card{background:#161b22;border-radius:10px;padding:14px;"
            "border:1px solid #30363d;border-top:3px solid #238636;"
            "transition:border-top-color .4s}"
            ".lbl{font-size:.76em;color:#8b949e;margin-bottom:4px}"
            ".val{font-size:1.8em;font-weight:700;color:#f0f6fc}"
            ".unit{font-size:.78em;color:#58a6ff}"
            ".bar-bg{background:#21262d;border-radius:20px;height:6px;"
            "margin-top:8px;overflow:hidden}"
            ".bar{height:100%;border-radius:20px;transition:width .6s,background .4s}"
            ".warm{font-size:.95em!important;color:#ffc107!important}"
            ".sub{font-size:.75em;color:#ffc107;margin-top:3px}"
            ".foot{text-align:center;padding:14px;color:#484f58;font-size:.74em}"
            "</style></head><body>");

  html += F("<div class='hdr'><h1>🌿 Smart-EMS<span class='dot'></span></h1></div>"
            "<div class='wrap'>"
            "<div class='clk-box'><div class='clk' id='clk'>--:--:-- --</div></div>"
            "<div class='badge' id='badge'>--</div>"
            "<div class='grid'>"

            "<div class='card' id='ct'><div class='lbl'>🌡️ Temperature</div>"
            "<div class='val' id='vt'>--<span class='unit'> °C</span></div>"
            "<div class='bar-bg'><div class='bar' id='bt' style='width:0'></div></div></div>"

            "<div class='card' id='ch' style='border-top-color:#17a2b8'>"
            "<div class='lbl'>💧 Humidity (est.)</div>"
            "<div class='val' id='vh'>--<span class='unit'> %</span></div>"
            "<div class='bar-bg'><div class='bar' id='bh'"
            " style='width:0;background:#17a2b8'></div></div></div>"

            "<div class='card' id='cp' style='border-top-color:#6f42c1'>"
            "<div class='lbl'>🔵 Pressure</div>"
            "<div class='val' id='vp'>--<span class='unit'> hPa</span></div>"
            "<div class='bar-bg'><div class='bar' id='bp'"
            " style='width:55%;background:#6f42c1'></div></div></div>"

            "<div class='card' id='cq'><div class='lbl'>🌬️ Air Quality (MQ-135)</div>"
            "<div class='val' id='vq'>--</div>"
            "<div class='sub' id='sq' style='display:none'></div>"
            "<div class='bar-bg'><div class='bar' id='bq' style='width:0'></div></div></div>"

            "<div class='card' id='cn'><div class='lbl'>🔊 Noise</div>"
            "<div class='val' id='vn'>--<span class='unit'> dB</span></div>"
            "<div class='bar-bg'><div class='bar' id='bn' style='width:0'></div></div></div>"
            "</div>");

  html += "<div class='foot'>📡 " + String(AP_SSID) +
          " &nbsp;|&nbsp; " + WiFi.softAPIP().toString() +
          F(" &nbsp;|&nbsp; Smart-EMS v3.0</div></div>");

  // AJAX — polls /api/data every 1 second and updates DOM without page reload
  html += F("<script>"
  "const TG=28,TM=35,AQG=150,AQM=250,NG=50,NM=70;"
  "function cl(v,g,m){return v<g?'#28a745':v<m?'#ffc107':'#dc3545';}"
  "function pc(v,a,b){return Math.min(100,Math.max(0,Math.round((v-a)/(b-a)*100)));}"
  "function css(el,s){document.getElementById(el).style.cssText=s;}"
  "function tx(el,h){document.getElementById(el).innerHTML=h;}"
  "async function poll(){"
    "try{"
      "const d=await(await fetch('/api/data')).json();"
      // Clock
      "tx('clk',d.time);"
      // Badge
      "const lv=[,{t:'✅ GOOD',bg:'#d4edda',cl:'#155724'},"
                  "{t:'⚠️ MODERATE',bg:'#fff3cd',cl:'#856404'},"
                  "{t:'🚨 DANGER',bg:'#f8d7da',cl:'#721c24'}][d.level]||{};"
      "const b=document.getElementById('badge');"
      "b.textContent=lv.t;b.style.background=lv.bg;b.style.color=lv.cl;"
      // Temperature
      "const ct=cl(d.temperature,TG,TM);"
      "tx('vt',d.temperature.toFixed(1)+'<span class=\"unit\"> °C</span>');"
      "css('bt','width:'+pc(d.temperature,0,60)+'%;background:'+ct);"
      "document.getElementById('ct').style.borderTopColor=ct;"
      // Humidity
      "tx('vh',d.humidity.toFixed(0)+'<span class=\"unit\"> %</span>');"
      "css('bh','width:'+Math.min(d.humidity,100)+'%;background:#17a2b8');"
      // Pressure
      "tx('vp',d.pressure.toFixed(1)+'<span class=\"unit\"> hPa</span>');"
      // Air quality
      "const cq=cl(d.airQuality,AQG,AQM);"
      "if(d.mqReady){"
        "tx('vq',d.airQuality+'<span class=\"unit\"> ADC</span>');"
        "document.getElementById('sq').style.display='none';"
        "css('bq','width:'+pc(d.airQuality,0,4095)+'%;background:'+cq);"
        "document.getElementById('cq').style.borderTopColor=cq;"
      "}else{"
        "document.getElementById('vq').className='val warm';"
        "tx('vq','⏳ Warming up');"
        "const sq=document.getElementById('sq');"
        "sq.style.display='block';sq.textContent='~'+d.aqRemaining+'s remaining';"
      "}"
      // Noise
      "const cn=cl(d.noiseLevel,NG,NM);"
      "tx('vn',d.noiseLevel+'<span class=\"unit\"> dB</span>');"
      "css('bn','width:'+pc(d.noiseLevel,30,100)+'%;background:'+cn);"
      "document.getElementById('cn').style.borderTopColor=cn;"
    "}catch(e){}finally{setTimeout(poll,1000);}"  // 1-second refresh
  "}"
  "poll();"   // start immediately on page load
  "</script></body></html>");

  return html;
}

void handleRoot()    { server.send(200, "text/html; charset=UTF-8", buildHTML()); }

void handleApiData() {
  unsigned long rem = g_mqReady ? 0 : (MQ_WARMUP_MS - millis()) / 1000;
  String j = "{";
  j += "\"temperature\":"  + String(g_temp,     1) + ",";
  j += "\"humidity\":"     + String(g_humidity,  1) + ",";
  j += "\"pressure\":"     + String(g_pressure,  1) + ",";
  j += "\"airQuality\":"   + String(g_airQuality)   + ",";
  j += "\"noiseLevel\":"   + String(g_noiseDB)      + ",";
  j += "\"level\":"        + String(g_systemLevel)  + ",";
  j += "\"mqReady\":"      + String(g_mqReady ? "true" : "false") + ",";
  j += "\"aqRemaining\":"  + String(rem)             + ",";
  j += "\"time\":\""       + g_timeStr               + "\"";
  j += "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

// ================================================================
//  Startup self-test: flash each LED once, two short buzzer beeps
// ================================================================
void selfTest() {
  const uint8_t leds[] = {LED_GREEN, LED_ORANGE, LED_RED};
  for (uint8_t pin : leds) { digitalWrite(pin, HIGH); delay(200); digitalWrite(pin, LOW); }
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW); delay(80);
  }
}

// ================================================================
//  setup()
// ================================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[Smart-EMS] Booting v3.0 ..."));

  Serial2.begin(115200, SERIAL_8N1, DWIN_RX, DWIN_TX);
  delay(400);

  Wire.begin(I2C_SDA, I2C_SCL);

  // --- BMP280 ---
  g_bmpOK = bme.begin(0x76);
  if (!g_bmpOK) g_bmpOK = bme.begin(0x77);
  if (!g_bmpOK) {
    Serial.println(F("[ERROR] BMP280 not found — check SDA=21, SCL=22, VCC=3.3V"));
  } else {
    bme.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X4,
                    Adafruit_BMP280::SAMPLING_X4,
                    Adafruit_BMP280::FILTER_X4,
                    Adafruit_BMP280::STANDBY_MS_250);
    delay(300);
    g_temp     = bme.readTemperature();
    g_pressure = bme.readPressure() / 100.0f;
    Serial.printf("[OK]  BMP280  T:%.1f°C  P:%.1fhPa\n", g_temp, g_pressure);
  }

  // --- DS1307 ---
  g_rtcOK = rtc.begin();
  if (!g_rtcOK) {
    Serial.println(F("[WARN] DS1307 absent — using millis() clock"));
    g_swLastTick = millis();
  } else {
    if (!rtc.isrunning()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    DateTime n = rtc.now();
    g_swHour = n.hour(); g_swMin = n.minute(); g_swSec = n.second();
    g_swLastTick = millis();
    Serial.println(F("[OK]  DS1307 RTC"));
  }

  // --- GPIO ---
  for (uint8_t p : {(uint8_t)LED_GREEN,(uint8_t)LED_ORANGE,(uint8_t)LED_RED,(uint8_t)BUZZER_PIN})
    pinMode(p, OUTPUT);
  pinMode(BTN_HOUR, INPUT_PULLUP);
  pinMode(BTN_MIN,  INPUT_PULLUP);

  selfTest();

  // --- WiFi SoftAP ---
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);
  Serial.printf("[WiFi] AP ready  SSID:%s  IP:%s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());

  // --- Web server ---
  server.on("/",         HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleApiData);
  server.onNotFound([]() { server.send(404, "text/plain", "Not Found"); });
  server.begin();
  Serial.println(F("[HTTP] Server started on port 80"));

  // --- DWIN: go to clock page ---
  dwinPage(1);

  // Reset button timers after all startup delays to avoid phantom press
  t_btnH = t_btnM = millis();

  Serial.println(F("[Smart-EMS] Ready ✅"));
}

// ================================================================
//  loop()
// ================================================================
void loop() {
  server.handleClient();
  readSensors();
  updateTime();
  g_systemLevel = calcLevel();
  updateOutputs();
  checkButtons();
  updateDisplay();
  delay(5);
}
