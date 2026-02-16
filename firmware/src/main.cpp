/*
 * M5Paper Dashboard
 * Connects to WiFi, syncs time via NTP, fetches dashboard JSON
 * from Pi, displays Trading 212 P&L + moon phase on e-ink.
 *
 * Uses M5.shutdown() for timed wake (RTC alarm).
 * On USB power, shutdown doesn't fully power off — loop() handles restart.
 */

#include <M5EPD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cmath>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include "time.h"

// ---- CONFIG (injected from .env at build time) ----
#ifndef WIFI_SSID
#error "WIFI_SSID not set — check .env file in project root"
#endif
#ifndef WIFI_PASS
#error "WIFI_PASS not set — check .env file in project root"
#endif
#ifndef DASHBOARD_URL
#error "DASHBOARD_URL not set — check .env file in project root"
#endif

const char* wifi_ssid = WIFI_SSID;
const char* wifi_pass = WIFI_PASS;
const char* dashboard_url = DASHBOARD_URL;
const int   REFRESH_MINS  = 30;

// Color map: 0=white, 15=black
#define C_WHITE 0
#define C_BLACK 15
#define C_DARK  12
#define C_MID   8
#define C_LIGHT 3
// ---------------------------------------------------

M5EPD_Canvas canvas(&M5.EPD);

void syncTime();
void drawDashboard(JsonObject& widgets, int battPct);
void drawError(const char* msg);
void drawNoWifi();

void goToSleep() {
    Serial.printf("goToSleep: M5.shutdown for %d min\n", REFRESH_MINS);
    Serial.flush();
    M5.shutdown(REFRESH_MINS * 60);
    // M5.shutdown() won't return on battery (power cuts completely).
    // On USB it returns because USB keeps ESP32 alive — loop() handles that.
}

void setup() {
    M5.begin();
    M5.EPD.SetRotation(0);
    M5.RTC.begin();

    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);

    uint32_t battMv = M5.getBatteryVoltage();
    int battPct = constrain(map(battMv, 3300, 4200, 0, 100), 0, 100);

    // Boot screen
    canvas.createCanvas(960, 540);
    canvas.fillCanvas(C_WHITE);
    canvas.setTextSize(4);
    canvas.setTextColor(C_BLACK);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("BOOTING...", 480, 270);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

    // Connect WiFi
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(wifi_ssid, wifi_pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(250);
        attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        drawNoWifi();
        goToSleep();
        return;
    }

    // Status screens: one message each, size 5, full refresh to avoid ghosting
    auto showStatus = [&](const char* msg, const char* sub = nullptr) {
        canvas.fillCanvas(C_WHITE);
        canvas.setTextSize(5);
        canvas.setTextColor(C_BLACK);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString(msg, 480, sub ? 240 : 270);
        if (sub) {
            canvas.setTextSize(2);
            canvas.setTextColor(C_MID);
            canvas.drawString(sub, 480, 300);
        }
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);  // full refresh for clean display
    };

    showStatus("WiFi connected", WiFi.localIP().toString().c_str());
    esp_task_wdt_reset();
    delay(1500);

    esp_wifi_set_max_tx_power(8);

    showStatus("Preparing fetch...");
    esp_task_wdt_reset();
    delay(5000);
    esp_task_wdt_reset();

    showStatus("Fetching dashboard...");
    esp_task_wdt_reset();

    HTTPClient http;
    http.begin(dashboard_url);
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    int httpCode = http.GET();
    String payload;
    if (httpCode == 200) {
        payload = http.getString();
    }
    http.end();

    if (httpCode != 200) {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "HTTP %d", httpCode);
        drawError(errBuf);
        goToSleep();
        return;
    }

    payload.trim();  // remove BOM/whitespace
    JsonDocument doc;  // ArduinoJson 7 auto-sizes; 2048B enough for ~350B dashboard
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        char errBuf[80];
        snprintf(errBuf, sizeof(errBuf), "Parse: %s (%d)", err.c_str(), payload.length());
        drawError(errBuf);
        goToSleep();
        return;
    }

    JsonObject widgets = doc["widgets"];
    drawDashboard(widgets, battPct);

    syncTime();  // after draw — NTP to internet can hang on battery; show dashboard first
    WiFi.disconnect(true);
    goToSleep();
}

void loop() {
    // On USB, M5.shutdown() doesn't fully power off — the ESP32 stays alive.
    // Re-call shutdown every 30s to keep the RTC alarm fresh,
    // and restart if we've been awake long enough.
    static unsigned long loopStart = millis();
    if (millis() - loopStart > (unsigned long)REFRESH_MINS * 60 * 1000) {
        ESP.restart();  // force a fresh cycle
    }
    M5.shutdown(REFRESH_MINS * 60);  // re-arm RTC alarm
    delay(30000);
}

// ---- NTP ----

void syncTime() {
    configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
        rtc_time_t rtcTime;
        rtcTime.hour = timeinfo.tm_hour;
        rtcTime.min  = timeinfo.tm_min;
        rtcTime.sec  = timeinfo.tm_sec;
        M5.RTC.setTime(&rtcTime);

        rtc_date_t rtcDate;
        rtcDate.year  = timeinfo.tm_year + 1900;
        rtcDate.mon   = timeinfo.tm_mon + 1;
        rtcDate.day   = timeinfo.tm_mday;
        rtcDate.week  = timeinfo.tm_wday;  // 0=Sun .. 6=Sat
        M5.RTC.setDate(&rtcDate);
    }
}

// ---- Drawing ----

// Tile grid: 3 columns x 2 rows
#define TW   320   // tile width  (960 / 3)
#define TH   270   // tile height (540 / 2)

void drawGrid() {
    // Thick grid lines between tiles
    int g = 10;
    // Vertical dividers
    canvas.fillRect(TW - g / 2,      0, g, 540, C_LIGHT);  // col 0|1
    canvas.fillRect(TW * 2 - g / 2,  0, g, 540, C_LIGHT);  // col 1|2
    // Horizontal divider
    canvas.fillRect(0, TH - g / 2, 960, g, C_LIGHT);       // row 0|1
}

void drawLabel(int cx, int y, const char* label) {
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(3);
    canvas.setTextColor(C_MID);
    canvas.drawString(label, cx, y);
}

void drawBigValue(int cx, int y, const char* value) {
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(7);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(value, cx, y);
}

void drawSub(int cx, int y, const char* sub) {
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(3);
    canvas.setTextColor(C_DARK);
    canvas.drawString(sub, cx, y);
}

// Simple tile: label, big value, optional sub
void drawTile(int col, int row, const char* label, const char* value, const char* sub) {
    int x = col * TW;
    int y = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 18, label);
    drawBigValue(cx, y + 80, value);
    if (sub && sub[0]) {
        drawSub(cx, y + 155, sub);
    }
}

// Proper moon phase rendering using scan lines and terminator geometry
void drawMoonDisc(int cx, int cy, int r, float phaseFrac) {
    // phaseFrac: 0 = new moon, 0.5 = full, 1 = new again
    // k: terminator squeeze factor. +1 = all dark, -1 = all lit
    float k = cosf(2.0f * M_PI * phaseFrac);

    for (int dy = -r; dy <= r; dy++) {
        float w = sqrtf((float)(r * r - dy * dy));
        if (w < 1) continue;

        // Terminator x offset from center
        float tx = w * k;

        int darkL, darkR;
        if (phaseFrac <= 0.5f) {
            // Waxing: right side lit, left side dark
            darkL = (int)(cx - w);
            darkR = (int)(cx + tx);
        } else {
            // Waning: left side lit, right side dark
            darkL = (int)(cx - tx);
            darkR = (int)(cx + w);
        }

        int len = darkR - darkL;
        if (len > 0) {
            canvas.drawFastHLine(darkL, cy + dy, len, C_BLACK);
        }
    }

    canvas.drawCircle(cx, cy, r, C_BLACK);
}

void drawMoonTile(int col, int row, const char* name, float illum, float age) {
    int x = col * TW;
    int y = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 10, "MOON");

    // Calculate phase fraction from age (synodic month = 29.53 days)
    float phaseFrac = fmodf(age, 29.53f) / 29.53f;
    drawMoonDisc(cx, y + 115, 55, phaseFrac);

    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(3);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(name, cx, y + 185);

    char infoBuf[32];
    snprintf(infoBuf, sizeof(infoBuf), "%.0f%%  day %.0f", illum, age);
    canvas.setTextSize(2);
    canvas.setTextColor(C_DARK);
    canvas.drawString(infoBuf, cx, y + 220);
}

// Trading 212 combined tile: overall + today, both big
void drawTradingTile(int col, int row, JsonObject& t212) {
    int x = col * TW;
    int y = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 8, "TRADING 212");

    if (t212.containsKey("error")) {
        drawBigValue(cx, y + 80, "ERR");
        drawSub(cx, y + 155, t212["error"].as<const char*>());
        return;
    }

    float pnlPct   = t212["pnl_pct"]   | 0.0f;
    float dailyPct = t212["daily_pct"]  | 0.0f;

    // Overall P&L
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(2);
    canvas.setTextColor(C_MID);
    canvas.drawString("overall", cx, y + 38);

    char pctBuf[16];
    snprintf(pctBuf, sizeof(pctBuf), "%+.2f%%", pnlPct);
    canvas.setTextSize(5);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(pctBuf, cx, y + 60);

    // Divider
    canvas.drawFastHLine(x + 30, y + 130, TW - 60, C_LIGHT);

    // Today (percent only)
    canvas.setTextSize(2);
    canvas.setTextColor(C_MID);
    canvas.drawString("today", cx, y + 140);

    char dailyPctBuf[16];
    snprintf(dailyPctBuf, sizeof(dailyPctBuf), "%+.2f%%", dailyPct);
    canvas.setTextSize(5);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(dailyPctBuf, cx, y + 195);
}

// Sunrise/Sunset tile
void drawSunTile(int col, int row, const char* rise, const char* set) {
    int x = col * TW;
    int y = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 10, "SUN");

    // Sunrise
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(3);
    canvas.setTextColor(C_MID);
    canvas.drawString("rise", cx, y + 55);
    canvas.setTextSize(5);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(rise, cx, y + 85);

    // Sunset
    canvas.setTextSize(3);
    canvas.setTextColor(C_MID);
    canvas.drawString("set", cx, y + 155);
    canvas.setTextSize(5);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(set, cx, y + 185);
}

void drawDashboard(JsonObject& widgets, int battPct) {
    canvas.createCanvas(960, 540);
    canvas.fillCanvas(C_WHITE);
    drawGrid();

    // ---- Row 0: Updated At | Date | Battery ----

    rtc_time_t t;
    rtc_date_t d;
    M5.RTC.getTime(&t);
    M5.RTC.getDate(&d);

    // Time
    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", t.hour, t.min);
    drawTile(0, 0, "UPDATED AT", timeBuf, "");

    // Date (vertical: weekday, day+month, year)
    const char* weekdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char* months[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                            "Jul","Aug","Sep","Oct","Nov","Dec"};
    int w = (d.week >= 0 && d.week <= 6) ? d.week : 0;
    int x = 1 * TW, y = 0 * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 18, "DATE");
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(4);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(weekdays[w], cx, y + 65);
    char dayMon[16];
    snprintf(dayMon, sizeof(dayMon), "%d %s", d.day,
             (d.mon >= 1 && d.mon <= 12) ? months[d.mon] : "???");
    canvas.drawString(dayMon, cx, y + 115);
    char yearBuf[8];
    snprintf(yearBuf, sizeof(yearBuf), "%04d", d.year);
    canvas.setTextSize(3);
    canvas.setTextColor(C_DARK);
    canvas.drawString(yearBuf, cx, y + 165);

    // Battery
    char battBuf[8];
    snprintf(battBuf, sizeof(battBuf), "%d%%", battPct);
    drawTile(2, 0, "BATTERY", battBuf, "");

    // ---- Row 1: Trading 212 | Moon | Sunrise/Sunset ----

    if (widgets.containsKey("trading212")) {
        JsonObject t212 = widgets["trading212"];
        drawTradingTile(0, 1, t212);
    }

    if (widgets.containsKey("moon")) {
        JsonObject moon = widgets["moon"];
        const char* name = moon["name"] | "Unknown";
        float age   = moon["age_days"]          | 0.0f;
        float illum = moon["illumination_pct"]   | 0.0f;
        drawMoonTile(1, 1, name, illum, age);
    }

    if (widgets.containsKey("sun")) {
        JsonObject sun = widgets["sun"];
        const char* rise = sun["sunrise"] | "--:--";
        const char* set  = sun["sunset"]  | "--:--";
        drawSunTile(2, 1, rise, set);
    } else {
        drawTile(2, 1, "SUN", "--:--", "no data");
    }

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawError(const char* msg) {
    canvas.createCanvas(960, 540);
    canvas.fillCanvas(C_WHITE);
    canvas.setTextSize(3);
    canvas.setTextColor(C_BLACK);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("Error", 480, 240);
    canvas.setTextSize(2);
    canvas.drawString(msg, 480, 290);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawNoWifi() {
    canvas.createCanvas(960, 540);
    canvas.fillCanvas(C_WHITE);
    canvas.setTextSize(3);
    canvas.setTextColor(C_BLACK);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("WiFi Failed", 480, 220);
    canvas.setTextSize(2);
    canvas.drawString("Check SSID/password", 480, 280);
    char buf[64];
    snprintf(buf, sizeof(buf), "Retrying in %d min...", REFRESH_MINS);
    canvas.drawString(buf, 480, 320);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}
