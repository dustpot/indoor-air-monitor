// ELEE4230E CP5 -- integrated sketch
// Extends airquality_CP4.ino with:
//   - 20x4 I2C LCD (backpack at 3.3 V, shared bus GPIO21/22, default addr 0x27)
//   - Menu on the capacitive pad: tap = cycle, hold >=1.5 s = select / back
//   - Fusion indices: Asthma, Sleep, Freshness (0-100 + Good/Average/Poor)
//   - Periodic reload of saved SGP30 outdoor baseline every 60 s (holds the
//     fixed outdoor reference; NO periodic save)
//   - ThingSpeak fields 7 (Asthma) and 8 (Sleep) -- enable on channel.
//     Freshness cannot fit (ThingSpeak max = 8 fields); computed on-device only.

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SGP30.h>
#include <Preferences.h>
#include <LiquidCrystal_I2C.h>
#include "ThingSpeak.h"

// ---------- credentials ----------
char ssid[] = "YOUR_WIFI_SSID";       // redacted for submission
char pass[] = "YOUR_WIFI_PASSWORD";   // redacted for submission
unsigned long myChannelNumber = 3413145;
const char* myWriteAPIKey = "YOUR_THINGSPEAK_WRITE_API_KEY";  // redacted for submission

// ---------- thermistor front-end constants (unchanged from CP3) ----------
#define THERM_PIN 32
const float VEXC = 5.00;
const float R_S  = 1800.0;    // R1
const float R_P  = 7460.0;    // R2
const float R_A  = 4220.0;    // R3
const float R_B  = 9710.0;    // R4
const float R_G  = 9700.0;    // R5
const float R_F  = 84200.0;   // R6
const float R_7  = 15000.0;
const float R_8  = 10000.0;
const float R25  = 10213.0;
const float BETA = 3992.0;
const float T25K = 298.15;
float CAL_OFFSET = 0.0;
const float VM   = VEXC * R_B / (R_A + R_B);
const float GAIN = R_F / R_G;
const float DIV  = R_8 / (R_7 + R_8);

// ---------- capacitive pad ----------
#define PAD_PIN 27
const int  TOUCH_SAMPLES     = 16;
int        touchBaseline     = 76;     // learned at boot (was fixed ~76)
const int  TOUCH_DROP        = 12;     // "touched" = this far below baseline
int        TOUCH_THRESHOLD   = 40;     // computed from baseline at boot
const unsigned long LONG_MS  = 1500;   // hold time for select/back

// ---------- LCD ----------
LiquidCrystal_I2C lcd(0x27, 20, 4);   // run i2c_scanner if blank (0x3F common)

// ---------- objects ----------
WiFiClient client;
Adafruit_BME280 bme;
Adafruit_SGP30 sgp;
Preferences prefs;

// ---------- Kalman (unchanged) ----------
float kx = 25.0, kP = 1.0;
const float kQ = 0.0008, kR = 0.15;
float kalmanStep(float z) {
  kP += kQ; float K = kP / (kP + kR);
  kx += K * (z - kx); kP *= (1.0 - K); return kx;
}

// ---------- thermistor inverse chain (unchanged) ----------
float readThermistorRawC() {
  float vAdc = analogReadMilliVolts(THERM_PIN) / 1000.0;
  float vOut = vAdc / DIV;
  float vA   = VM * (1.0 + GAIN) / GAIN - vOut / GAIN;
  if (vA <= 0.01)      vA = 0.01;
  if (vA >= VEXC-0.01) vA = VEXC - 0.01;
  float rPar = R_S * vA / (VEXC - vA);
  float rNtc = R_P * rPar / (R_P - rPar);
  if (rNtc < 1.0) rNtc = 1.0;
  float tK = 1.0 / ( log(rNtc / R25) / BETA + 1.0 / T25K );
  return tK - 273.15 + CAL_OFFSET;
}

void readBME(float &t, float &h, float &p) {
  t = bme.readTemperature();
  h = bme.readHumidity();
  p = bme.readPressure() / 100.0F;
}

// ---------- pad: averaged read ----------
int readPadAvg() {
  touchRead(PAD_PIN);        // discard: clears stale state left by the ADC read
  delayMicroseconds(200);
  long sum = 0;
  for (int i = 0; i < TOUCH_SAMPLES; i++) { sum += touchRead(PAD_PIN); delay(1); }
  return (int)(sum / TOUCH_SAMPLES);
}

// ---------- SGP30 humidity compensation ----------
uint32_t absHumidity(float tC, float rh) {
  float ah = 216.7f * ((rh / 100.0f) * 6.112f
             * expf((17.62f * tC) / (243.12f + tC)) / (273.15f + tC));   // g/m^3
  return (uint32_t)(1000.0f * ah);                                        // mg/m^3
}

// ============================================================
// FUSION INDICES
// Subscore: 100 inside [lo, hi]; outside, lose points linearly,
// reaching 0 at a distance of 'fall' past the edge.
// ============================================================
float subscore(float x, float lo, float hi, float fall) {
  float d = 0;
  if (x < lo) d = lo - x;
  else if (x > hi) d = x - hi;
  else return 100.0;
  float s = 100.0 * (1.0 - d / fall);
  return (s < 0) ? 0 : s;
}

// index values, recomputed every 1 s in the gas loop
float idxAsthma = 0, idxSleep = 0, idxFresh = 0;

void computeIndices(float t, float h, float co2, float voc) {
  // subscores per index: temp / humidity / eCO2 / TVOC
  float aT = subscore(t,   20, 25,  5);
  float sT = subscore(t,   16, 19,  4);
  float fT = subscore(t,   18, 26,  8);
  float aH = subscore(h,   40, 50, 15);
  float sH = subscore(h,   40, 60, 15);
  float fH = subscore(h,   30, 60, 20);
  float aC = subscore(co2, 400, 800,  800);   // eCO2 floor is 400, no low side
  float sC = subscore(co2, 400, 800,  600);
  float fC = subscore(co2, 400, 800, 1200);
  float aV = subscore(voc, 0, 150, 500);      // TVOC floor is 0, no low side
  float sV = subscore(voc, 0, 150, 850);
  float fV = subscore(voc, 0, 150, 350);

  // locked weights: TVOC / eCO2 / temp / humidity
  idxAsthma = 0.35*aV + 0.25*aC + 0.20*aT + 0.20*aH;
  idxSleep  = 0.10*sV + 0.25*sC + 0.35*sT + 0.30*sH;
  idxFresh  = 0.45*fV + 0.45*fC + 0.05*fT + 0.05*fH;
}

const char* verbal(float s) {
  if (s >= 80) return "Good";
  if (s >= 50) return "Average";
  return "Poor";
}

// ============================================================
// MENU  --  one-button navigation
//   ST_MENU : tap cycles items, hold selects
//   ST_IDX  : viewing an index; tap cycles Asthma->Sleep->Fresh, hold = back
//   ST_MEAS : viewing a measurand; tap cycles the 6 screens, hold = back
// ============================================================
enum UiState { ST_MENU, ST_IDX, ST_MEAS };
UiState ui = ST_MENU;
int menuSel = 0;                       // 0 Asthma, 1 Sleep, 2 Fresh, 3 Measurands
int menuTop = 0;                       // first visible menu row (4-row window)
int idxSel  = 0;                       // 0 Asthma, 1 Sleep, 2 Fresh
int measSel = 0;                       // 0 T-BME, 1 T-Therm, 2 Hum, 3 Pres, 4 eCO2, 5 TVOC
const char* menuItems[4] = {"Asthma Index", "Sleep Index", "Freshness Index", "Measurands"};

// custom LCD arrow glyphs (slots 0 and 1)
byte GLY_UP[8]   = {0b00100,0b01110,0b11111,0b00100,0b00100,0b00100,0b00000,0b00000};
byte GLY_DOWN[8] = {0b00000,0b00000,0b00100,0b00100,0b00100,0b11111,0b01110,0b00100};
bool lcdDirty = true;                  // force immediate redraw on any state change

// latest readings for display
float dT = 0, dH = 0, dP = 0;          // BME280
uint16_t eco2 = 400, tvoc = 0;
float tFilt = 25.0;                    // thermistor (Kalman)

// touch gesture state
bool touchDown = false, longFired = false;
unsigned long pressStart = 0;

void onTap() {
  if (ui == ST_MENU) {
    menuSel = (menuSel + 1) % 4;
    if (menuSel == 0)               menuTop = 0;             // wrapped to top
    else if (menuSel > menuTop + 3) menuTop = menuSel - 3;   // scroll window down
  }
  else if (ui == ST_IDX)  idxSel  = (idxSel  + 1) % 3;
  else                    measSel = (measSel + 1) % 6;
  lcdDirty = true;
}

void onLong() {
  if (ui == ST_MENU) {
    if (menuSel <= 2) { ui = ST_IDX; idxSel = menuSel; }
    else              { ui = ST_MEAS; measSel = 0; }
  } else {
    ui = ST_MENU;                      // hold = back from any view
  }
  lcdDirty = true;
}

void handleTouch(bool touched, unsigned long now) {
  if (touched && !touchDown) {         // press begins
    touchDown = true; longFired = false; pressStart = now;
  } else if (touched && touchDown) {   // still held
    if (!longFired && now - pressStart >= LONG_MS) { longFired = true; onLong(); }
  } else if (!touched && touchDown) {  // release
    touchDown = false;
    if (!longFired) onTap();
  }
}

// menu layout (20x4): col 0 arrows, col 3 cursor, text from col 4
//   up arrow    row 0, col 0 -- only if items above the window
//   down arrow  row 3, col 0 -- only if items below the window
void drawMenuRow(int row, int item) {
  if (item > 3) return;
  lcd.setCursor(3, row);
  lcd.print(item == menuSel ? '>' : ' ');
  lcd.print(menuItems[item]);
}

void drawLcd() {
  lcd.clear();
  if (ui == ST_MENU) {
    for (int r = 0; r < 4; r++) drawMenuRow(r, menuTop + r);
    if (menuTop > 0)     { lcd.setCursor(0, 0); lcd.write(byte(0)); }  // up
    if (menuTop + 4 < 4) { lcd.setCursor(0, 3); lcd.write(byte(1)); }  // down
    return;
  }
  char l1[21], l2[21];
  if (ui == ST_IDX) {
    float v = (idxSel == 0) ? idxAsthma : (idxSel == 1) ? idxSleep : idxFresh;
    snprintf(l1, 21, "%s", menuItems[idxSel]);
    snprintf(l2, 21, "%3d  %s", (int)(v + 0.5), verbal(v));
  } else {
    switch (measSel) {
      case 0: snprintf(l1,21,"Temp (BME280)");    snprintf(l2,21,"%.1f C  %.1f F", dT,    dT*1.8+32);    break;
      case 1: snprintf(l1,21,"Temp (Thermistor)");snprintf(l2,21,"%.1f C  %.1f F", tFilt, tFilt*1.8+32); break;
      case 2: snprintf(l1,21,"Humidity");         snprintf(l2,21,"%.1f %%RH", dH);                       break;
      case 3: snprintf(l1,21,"Pressure");         snprintf(l2,21,"%.1f hPa", dP);                        break;
      case 4: snprintf(l1,21,"eCO2");             snprintf(l2,21,"%u ppm", eco2);                        break;
      case 5: snprintf(l1,21,"TVOC");             snprintf(l2,21,"%u ppb", tvoc);                        break;
    }
  }
  lcd.setCursor(0, 0);  lcd.print(l1);
  lcd.setCursor(0, 1);  lcd.print(l2);
  lcd.setCursor(15, 3); lcd.print(">BACK");   // hold pad to trigger
}

// ---------- scheduling ----------
const unsigned long FAST_PERIOD = 200;      // pad + thermistor + Kalman
const unsigned long GAS_PERIOD  = 1000;     // SGP30 mandatory 1 s cadence
const unsigned long LCD_PERIOD  = 1000;     // live-value refresh in view states
const unsigned long TS_PERIOD   = 20000;    // ThingSpeak
const unsigned long REL_PERIOD  = 60000;    // reload saved outdoor baseline
unsigned long lastFast = 0, lastGas = 0, lastLcd = 0, lastTS = 0, lastRel = 0;
uint16_t savedCO2 = 0, savedTVOC = 0;       // NVS baseline held for reloads

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  if (!bme.begin(0x76)) Serial.println("BME280 not found");
  if (!sgp.begin())     Serial.println("SGP30 not found");

  lcd.init();
  lcd.backlight();
  lcd.createChar(0, GLY_UP);
  lcd.createChar(1, GLY_DOWN);
  lcd.print("Booting...");

  // restore SGP30 baseline from flash; keep values for periodic reload
  prefs.begin("sgp30", false);
  savedCO2  = prefs.getUShort("eco2", 0);
  savedTVOC = prefs.getUShort("tvoc", 0);
  if (savedCO2 != 0 && savedTVOC != 0) {
    sgp.setIAQBaseline(savedCO2, savedTVOC);
    Serial.println("SGP30 baseline restored from flash");
  } else {
    Serial.println("SGP30: no saved baseline (fresh burn-in needed)");
  }

  analogReadResolution(12);
  analogSetPinAttenuation(THERM_PIN, ADC_11db);
  kx = readThermistorRawC();

  // learn the pad baseline as-installed (lid CLOSED, finger OFF at power-up)
  long acc = 0;
  for (int i = 0; i < 32; i++) { acc += readPadAvg(); delay(20); }
  touchBaseline = acc / 32;
  TOUCH_THRESHOLD = touchBaseline - TOUCH_DROP;
  Serial.print("Pad baseline "); Serial.print(touchBaseline);
  Serial.print(" -> threshold "); Serial.println(TOUCH_THRESHOLD);

  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected");
  ThingSpeak.begin(client);

  Serial.println("raw_C,filtered_C,pad,touched,eCO2_ppm,TVOC_ppb,asthma,sleep,fresh");
  lcdDirty = true;
}

void loop() {
  unsigned long now = millis();

  // fast loop: pad FIRST (max gap since last ADC read), then thermistor
  if (now - lastFast >= FAST_PERIOD) {
    lastFast = now;
    int pad = readPadAvg();
    // rolling re-baseline: when clearly untouched, let the baseline follow slow
    // drift (lid, humidity) so the threshold stays TOUCH_DROP below "resting"
    if (pad > touchBaseline - TOUCH_DROP/2) {
      touchBaseline += (pad - touchBaseline) / 8;   // slow track toward resting level
      TOUCH_THRESHOLD = touchBaseline - TOUCH_DROP;
    }
    bool touched = (pad < TOUCH_THRESHOLD);
    handleTouch(touched, now);
    float raw = readThermistorRawC();
    tFilt = kalmanStep(raw);
    Serial.print(raw, 3);   Serial.print(",");
    Serial.print(tFilt, 3); Serial.print(",");
    Serial.print(pad);      Serial.print(",");
    Serial.print(touched ? 1 : 0); Serial.print(",");
    Serial.print(eco2);     Serial.print(",");
    Serial.print(tvoc);     Serial.print(",");
    Serial.print(idxAsthma, 1); Serial.print(",");
    Serial.print(idxSleep, 1);  Serial.print(",");
    Serial.println(idxFresh, 1);
  }

  // gas loop: SGP30 every 1 s + fusion indices
  if (now - lastGas >= GAS_PERIOD) {
    lastGas = now;
    readBME(dT, dH, dP);
    if (!isnan(dT) && !isnan(dH)) sgp.setHumidity(absHumidity(dT, dH));
    if (sgp.IAQmeasure()) { eco2 = sgp.eCO2; tvoc = sgp.TVOC; }
    computeIndices(dT, dH, (float)eco2, (float)tvoc);
  }

  // baseline hold: re-assert the saved outdoor anchor every 60 s (reload ONLY, never save)
  if (savedCO2 != 0 && now - lastRel >= REL_PERIOD) {
    lastRel = now;
    sgp.setIAQBaseline(savedCO2, savedTVOC);
  }

  // LCD: immediate redraw on interaction, else 1 s refresh in view states
  if (lcdDirty || (ui != ST_MENU && now - lastLcd >= LCD_PERIOD)) {
    lastLcd = now; lcdDirty = false;
    drawLcd();
  }

  // cloud loop: 8 fields
  if (now - lastTS >= TS_PERIOD) {
    lastTS = now;
    ThingSpeak.setField(1, dT);
    ThingSpeak.setField(2, dH);
    ThingSpeak.setField(3, dP);
    ThingSpeak.setField(4, tFilt);
    ThingSpeak.setField(5, (long)eco2);
    ThingSpeak.setField(6, (long)tvoc);
    ThingSpeak.setField(7, idxAsthma);
    ThingSpeak.setField(8, idxSleep);
    ThingSpeak.writeFields(myChannelNumber, myWriteAPIKey);
  }
}
