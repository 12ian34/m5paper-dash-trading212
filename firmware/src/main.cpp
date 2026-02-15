/*
 * M5Paper Dashboard
 * Connects to WiFi, syncs time via NTP, fetches dashboard JSON
 * from Pi, displays Trading 212 P&L + moon phase on e-ink.
 *
 * Power button wakes from shutdown and triggers a full refresh.
 * Side toggle (left/right/click) reserved for future use.
 */

#include <M5EPD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cmath>
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

const char* wifi_ssid     = WIFI_SSID;
const char* wifi_pass     = WIFI_PASS;
const char* dashboard_url = DASHBOARD_URL;
const int   REFRESH_MINS  = 15;
// ---------------------------------------------------

M5EPD_Canvas canvas(&M5.EPD);

void syncTime();
void drawDashboard(JsonObject& widgets, int battPct);
void drawError(const char* msg);
void drawNoWifi();

void setup() {
    M5.begin();
    M5.EPD.SetRotation(0);  // landscape, USB+rocker on top: 960 x 540
    // Don't call M5.EPD.Clear(true) here — it blanks the screen immediately
    // and if power is lost before pushCanvas, you get a blank display.
    // UPDATE_MODE_GC16 in pushCanvas does a full refresh anyway.
    M5.RTC.begin();

    uint32_t battMv = M5.getBatteryVoltage();
    int battPct = constrain(map(battMv, 3300, 4200, 0, 100), 0, 100);

    // Connect WiFi
    WiFi.begin(wifi_ssid, wifi_pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(250);
        attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        drawNoWifi();
        M5.shutdown(REFRESH_MINS * 60);
        return;
    }

    syncTime();

    // Fetch dashboard JSON
    HTTPClient http;
    http.begin(dashboard_url);
    http.setTimeout(10000);
    int httpCode = http.GET();

    if (httpCode != 200) {
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "HTTP %d", httpCode);
        drawError(errBuf);
        http.end();
        WiFi.disconnect(true);
        M5.shutdown(REFRESH_MINS * 60);
        return;
    }

    String payload = http.getString();
    http.end();
    WiFi.disconnect(true);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        drawError("JSON parse failed");
        M5.shutdown(REFRESH_MINS * 60);
        return;
    }

    JsonObject widgets = doc["widgets"];
    drawDashboard(widgets, battPct);

    M5.shutdown(REFRESH_MINS * 60);
}

void loop() {
    delay(REFRESH_MINS * 60 * 1000UL);
    ESP.restart();
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
        rtcDate.year = timeinfo.tm_year + 1900;
        rtcDate.mon  = timeinfo.tm_mon + 1;
        rtcDate.day  = timeinfo.tm_mday;
        M5.RTC.setDate(&rtcDate);
    }
}

// ---- Drawing ----

// Color map: 0=white, 15=black
#define C_WHITE 0
#define C_BLACK 15
#define C_DARK  12
#define C_MID   8
#define C_LIGHT 3

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
    float dailyPnl = t212["daily_pnl"]  | 0.0f;
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

    // Today
    canvas.setTextSize(2);
    canvas.setTextColor(C_MID);
    canvas.drawString("today", cx, y + 140);

    char dailyBuf[16];
    snprintf(dailyBuf, sizeof(dailyBuf), "%+.2f", dailyPnl);
    canvas.setTextSize(5);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(dailyBuf, cx, y + 162);

    char dailyPctBuf[16];
    snprintf(dailyPctBuf, sizeof(dailyPctBuf), "%+.2f%%", dailyPct);
    canvas.setTextSize(3);
    canvas.setTextColor(C_DARK);
    canvas.drawString(dailyPctBuf, cx, y + 230);
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

    // Date
    const char* months[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                            "Jul","Aug","Sep","Oct","Nov","Dec"};
    char dateBuf[24];
    snprintf(dateBuf, sizeof(dateBuf), "%d %s", d.day,
             (d.mon >= 1 && d.mon <= 12) ? months[d.mon] : "???");
    char yearBuf[8];
    snprintf(yearBuf, sizeof(yearBuf), "%04d", d.year);
    drawTile(1, 0, "DATE", dateBuf, yearBuf);

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
