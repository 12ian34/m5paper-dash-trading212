/*
 * M5Paper Trading Dashboard
 * Connects to WiFi, fetches dashboard JSON from Pi,
 * displays Trading 212 P&L + top winners/losers on e-ink.
 *
 * Uses M5.shutdown() for timed wake (RTC alarm).
 * On USB power, shutdown doesn't fully power off — loop() handles restart.
 */

#include <M5EPD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
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
void drawDashboard(JsonObject& data, int battPct);
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
    JsonDocument doc;  // ArduinoJson 7 auto-sizes
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

// Variable-width 3x2 grid: narrow P&L column, wider list columns
#define COL0_W 200   // P&L tiles
#define COL1_W 380   // winners / best overall
#define COL2_W 380   // losers / worst overall
#define COL1_X COL0_W
#define COL2_X (COL0_W + COL1_W)
#define TH     270   // tile height (540 / 2)

void drawGrid() {
    int g = 10;
    canvas.fillRect(COL1_X - g / 2, 0, g, 540, C_LIGHT);   // col 0|1
    canvas.fillRect(COL2_X - g / 2, 0, g, 540, C_LIGHT);   // col 1|2
    canvas.fillRect(0, TH - g / 2, 960, g, C_LIGHT);        // row 0|1
}

void drawLabel(int cx, int y, const char* label) {
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(3);
    canvas.setTextColor(C_MID);
    canvas.drawString(label, cx, y);
}

// Small corner inlays: updated time (bottom-left) + battery (bottom-right)
void drawInlays(int battPct) {
    rtc_time_t t;
    M5.RTC.getTime(&t);

    // Bottom-left: updated time
    char timeBuf[24];
    snprintf(timeBuf, sizeof(timeBuf), "updated: %02d:%02d", t.hour, t.min);
    canvas.setTextDatum(BL_DATUM);
    canvas.setTextSize(2);
    canvas.setTextColor(C_MID);
    canvas.drawString(timeBuf, 8, 534);

    // Bottom-right: battery
    char battBuf[16];
    snprintf(battBuf, sizeof(battBuf), "%d%%", battPct);
    canvas.setTextDatum(BR_DATUM);
    canvas.setTextSize(2);
    canvas.setTextColor(C_MID);
    canvas.drawString(battBuf, 952, 534);
}

// Big percentage tile (used for overall P&L and 24h P&L)
void drawPctTile(int x, int w, int row, const char* label, float pct) {
    int y = row * TH;
    int cx = x + w / 2;
    drawLabel(cx, y + 18, label);

    char pctBuf[16];
    snprintf(pctBuf, sizeof(pctBuf), "%+.1f%%", pct);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(5);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(pctBuf, cx, y + TH / 2 + 15);
}

String truncateToWidth(const char* text, int textSize, int maxWidth) {
    String out = (text && text[0]) ? String(text) : String("???");
    canvas.setTextSize(textSize);
    if (canvas.textWidth(out.c_str()) <= maxWidth) return out;

    const String ellipsis = ".";
    while (out.length() > 0) {
        out.remove(out.length() - 1);
        String candidate = out + ellipsis;
        if (canvas.textWidth(candidate.c_str()) <= maxWidth) {
            return candidate;
        }
    }
    return ellipsis;
}

// List tile showing up to 5 stocks with ticker + % change
void drawListTile(int x, int w, int row, const char* title, JsonArray items) {
    int y = row * TH;
    int cx = x + w / 2;
    drawLabel(cx, y + 10, title);

    int rowY = y + 45;
    int spacing = 43;
    int count = items.size() < 5 ? (int)items.size() : 5;

    for (int i = 0; i < count; i++) {
        const char* name = items[i]["ticker"] | "???";
        float pct = items[i]["pct"] | 0.0f;

        char pctBuf[16];
        snprintf(pctBuf, sizeof(pctBuf), "%+.1f%%", pct);

        const int nameLeft = x + 15;
        const int pctRight = x + w - 15;
        const int colGap = 10;

        canvas.setTextSize(3);
        int pctWidth = canvas.textWidth(pctBuf);
        int nameMaxWidth = pctRight - pctWidth - colGap - nameLeft;
        if (nameMaxWidth < 20) nameMaxWidth = 20;

        const int nameSize = 2;
        String nameText = truncateToWidth(name, nameSize, nameMaxWidth);

        canvas.setTextDatum(TL_DATUM);
        canvas.setTextSize(nameSize);
        canvas.setTextColor(C_BLACK);
        canvas.drawString(nameText.c_str(), nameLeft, rowY);

        canvas.setTextDatum(TR_DATUM);
        canvas.setTextSize(3);
        canvas.setTextColor(C_DARK);
        canvas.drawString(pctBuf, pctRight, rowY);

        rowY += spacing;
    }
}

void drawDashboard(JsonObject& widgets, int battPct) {
    canvas.createCanvas(960, 540);
    canvas.fillCanvas(C_WHITE);
    drawGrid();

    JsonObject t212 = widgets["trading212"];
    if (t212) {
        if (t212["error"].is<const char*>()) {
            // Error state — show message across full screen
            canvas.setTextDatum(MC_DATUM);
            canvas.setTextSize(4);
            canvas.setTextColor(C_BLACK);
            canvas.drawString("T212 Error", 480, 240);
            canvas.setTextSize(2);
            canvas.setTextColor(C_DARK);
            canvas.drawString(t212["error"].as<const char*>(), 480, 290);
        } else {
            float dailyPct = t212["daily_pct"] | 0.0f;
            float pnlPct   = t212["pnl_pct"]  | 0.0f;

            // Row 0: 24h P&L | Winners | Losers
            drawPctTile(0, COL0_W, 0, "24H", dailyPct);

            JsonArray winners = t212["winners"];
            if (winners) drawListTile(COL1_X, COL1_W, 0, "WINNERS", winners);

            JsonArray losers = t212["losers"];
            if (losers) drawListTile(COL2_X, COL2_W, 0, "LOSERS", losers);

            // Row 1: Overall P&L | Best Overall | Worst Overall
            drawPctTile(0, COL0_W, 1, "OVERALL", pnlPct);

            JsonArray best = t212["best_overall"];
            if (best) drawListTile(COL1_X, COL1_W, 1, "BEST OVERALL", best);

            JsonArray worst = t212["worst_overall"];
            if (worst) drawListTile(COL2_X, COL2_W, 1, "WORST OVERALL", worst);
        }
    }

    drawInlays(battPct);

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
