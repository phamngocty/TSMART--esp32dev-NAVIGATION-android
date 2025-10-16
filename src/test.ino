#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_SSD1306.h>
#include <ChronosESP32.h>
#include <WS2812FX.h>
#include "FontMaker.h"
#include <logo.h>
#include <Preferences.h>

#define LED_COUNT 1
#define LED_PIN 8         // Thay đổi từ 27 sang GPIO8 cho ESP32-C3
#define BUILTINLED 9      // Thay đổi từ 2 sang GPIO9 cho ESP32-C3
#define BUTTON_PIN 0      // Giữ nguyên GPIO0
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 6        // Thay đổi từ 25 sang GPIO6 cho ESP32-C3
#define OLED_SCL 7        // Thay đổi từ 26 sang GPIO7 cho ESP32-C3

// Dynamic display selection
enum DisplayType { DT_SSD1306 = 0, DT_SH1106G = 1 };
Adafruit_SSD1306* d1306 = nullptr;
Adafruit_SH1106G* d1106 = nullptr;
Adafruit_GFX* gfx = nullptr;
DisplayType currentDisplayType = DT_SSD1306;

ChronosESP32 watch("TSMART Navigation");
WS2812FX ws2812fx = WS2812FX(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// Định nghĩa hàm setpx cho MakeFont (vẽ qua gfx)
void setpx(int16_t x, int16_t y, uint16_t color) {
    if (gfx) {
        gfx->drawPixel(x, y, color ? 1 : 0);
    }
}
MakeFont myfont(&setpx);

// Biến cho logo intro và thanh loading
unsigned long startTime = 0; // Thời điểm bắt đầu
const unsigned long introDuration = 3000; // 3 giây
bool showIntro = true; // Cờ hiển thị logo

// Nhóm biến toàn cục vào struct
struct AppState {
    bool isConnected = false;
    bool isNavigating = false;
    bool hasNotification = false;
    bool isCalling = false;
    String navDirection = "";
    String navDirectionLines[2];
    String navDistance = "";
    String navETA = "";
    String navTitle = "";
    String notificationMsg[6];
    String callerName = "";
    unsigned long lastNavChangeTime = 0;
    bool navChange = false;
    String savedInfoText = "";
    int scrollOffset = 0;
    String lastNavTitle = "";
    int navDisplayMode = 1;
};
AppState app;

// Biến hỗ trợ
ChronosTimer notifyTimer;
volatile bool buttonPressed = false;
Preferences prefs;

// Enum cho trạng thái hiển thị
enum DisplayMode {
    TIME_SMALL,
    TIME_LARGE,
    NAVIGATION,
    NOTIFICATION,
    CALL
};
DisplayMode currentMode = TIME_LARGE;

// Wrappers for device-specific calls
void OLED_clear() {
    if (d1306) d1306->clearDisplay();
    else if (d1106) d1106->clearDisplay();
}

void OLED_display() {
    if (d1306) d1306->display();
    else if (d1106) d1106->display();
}

bool setDisplay(DisplayType newType) {
    // Delete old instances
    if (d1306) { delete d1306; d1306 = nullptr; }
    if (d1106) { delete d1106; d1106 = nullptr; }
    gfx = nullptr;

    bool ok = false;
    if (newType == DT_SSD1306) {
        d1306 = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
        ok = d1306->begin(SSD1306_SWITCHCAPVCC, 0x3C);
        if (ok) {
            gfx = (Adafruit_GFX*)d1306;
        }
    } else {
        d1106 = new Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
        ok = d1106->begin(0x3C, true);
        if (ok) {
            gfx = (Adafruit_GFX*)d1106;
        }
    }
    if (ok) {
        currentDisplayType = newType;
        // Basic defaults
        OLED_clear();
        if (gfx) {
            gfx->setTextColor(1);
            gfx->setTextSize(1);
            gfx->setCursor(0, 0);
        }
        OLED_display();
        prefs.putInt("displayType", (int)newType);
    }
    return ok;
}

bool initDisplayFromPrefs() {
    int stored = prefs.getInt("displayType", (int)DT_SSD1306);
    DisplayType t = (stored == (int)DT_SH1106G) ? DT_SH1106G : DT_SSD1306;
    if (!setDisplay(t)) {
        // Fallback to SSD1306
        return setDisplay(DT_SSD1306);
    }
    return true;
}

// **Callback functions**
void connectionCallback(bool state) {
    app.isConnected = state;
    Serial.print("Connection state: ");
    Serial.println(state ? "Connected" : "Disconnected");
}

void notificationCallback(Notification notification) {
    String message = notification.message;
    Serial.print("Raw notification.message: ");
    Serial.println(message);

    if (message.startsWith("info; ")) {
        app.savedInfoText = message.substring(6);
        Serial.print("Extracted savedInfoText: ");
        Serial.println(app.savedInfoText);
        app.scrollOffset = 0;
        prefs.putString("savedText", app.savedInfoText);
    } else if (message.startsWith("main; ")) {
        String modeStr = message.substring(6);
        modeStr.trim();
        if (modeStr == "0.96") {
            setDisplay(DT_SSD1306);
            if (setDisplay(DT_SSD1306)) {
                Serial.println("Switched to SSD1306 (0.96\")");
            } else {
                Serial.println("Failed to init SSD1306");
            }
        } else if (modeStr == "1.3") {
            setDisplay(DT_SH1106G);
            if (setDisplay(DT_SH1106G)) {
                Serial.println("Switched to SH1106G (1.3\")");
            } else {
                Serial.println("Failed to init SH1106G");
            }
        } else if (modeStr == "1") {
            app.navDisplayMode = 1;
            Serial.println("Set navigation display mode to map1");
        } else if (modeStr == "2") {
            app.navDisplayMode = 2;
            Serial.println("Set navigation display mode to map2");
        } else if (modeStr == "3") {
            app.navDisplayMode = 3;
            Serial.println("Set navigation display mode to map3");
        }
        prefs.putInt("navMode", app.navDisplayMode);
    } else {
        app.notificationMsg[0] = message.substring(0, 22);
        app.notificationMsg[1] = message.substring(22, 44);
        app.notificationMsg[2] = message.substring(44, 66);
        app.notificationMsg[3] = message.substring(66, 88);
        app.notificationMsg[4] = message.substring(88, 110);
        app.notificationMsg[5] = message.substring(110, 132);
        app.hasNotification = true;
        notifyTimer.time = millis();
        notifyTimer.active = true;
        ws2812fx.setMode(FX_MODE_BLINK);
        ws2812fx.setBrightness(100);
        ws2812fx.setSpeed(500);
        currentMode = NOTIFICATION;
    }
}

void configCallback(Config config, uint32_t a, uint32_t b) {
    if (config == CF_NAV_DATA) {
        app.isNavigating = a;
        if (a && app.isConnected) {
            Navigation nav = watch.getNavigation();
            app.navDirection = nav.directions;
            splitNavDirection();
            app.navDistance = nav.distance;
            app.navETA = nav.eta;
            app.navTitle = nav.title;
            app.navChange = true;
            if (nav.title != app.lastNavTitle) {
                app.lastNavChangeTime = millis();
                app.lastNavTitle = nav.title;
            }
        } else {
            currentMode = TIME_LARGE;
            app.navChange = false;
        }
    }
}

void splitNavDirection() {
    String direction = app.navDirection;
    String lines[2];
    if (app.navDisplayMode == 1) {
        if (direction.length() <= 10) {
            lines[0] = direction;
            lines[1] = "";
        } else {
            lines[0] = direction.substring(0, 12);
            lines[1] = direction.substring(12, 24);
        }
    } else if (app.navDisplayMode == 2 || app.navDisplayMode == 3) {
        if (direction.length() <= 10) {
            lines[0] = direction;
            lines[1] = "";
        } else {
            lines[0] = direction.substring(0, 22);
            lines[1] = direction.substring(22, 44);
        }
    }
    app.navDirectionLines[0] = lines[0];
    app.navDirectionLines[1] = lines[1];
}

void ringerCallback(String caller, bool state) {
    app.isCalling = state;
    if (state) {
        app.callerName = caller;
        ws2812fx.setMode(FX_MODE_TWINKLE_FADE_RANDOM);
        ws2812fx.setBrightness(200);
        ws2812fx.setSpeed(200);
        currentMode = CALL;
    } else {
        ws2812fx.setBrightness(0);
        currentMode = TIME_LARGE;
    }
}

// **Hàm hỗ trợ**
float convertToMeters(String title) {
    title.trim();
    String numberPart = "";
    int i = 0;
    while (i < title.length() && (isDigit(title[i]) || title[i] == '.')) {
        numberPart += title[i];
        i++;
    }
    if (numberPart.length() == 0) return -1;
    float value = numberPart.toFloat();
    String unit = title.substring(i);
    unit.toLowerCase();
    if (unit.indexOf("km") != -1) value *= 1000;
    return value;
}

// **Hàm hiển thị**
void displayTime(bool smallFont) {
    OLED_clear();
    if (gfx) {
        gfx->setTextSize(2);
        gfx->setTextColor(1);
        gfx->setCursor(0, 0);
        gfx->print(watch.getHourZ() + watch.getTime(":%M:%S"));
    }

    float batteryVoltage = readBatteryVoltage();
    myfont.set_font(vietnamtimes10x2);
    if (gfx) gfx->fillRect(0, 21, 128, 16, 0);
    myfont.print(0, 25, "ĐIỆN ÁP: ", 1, 0);
    myfont.print(112, 25, "V", 1, 0);
    if (gfx) {
        gfx->setTextSize(2);
        gfx->setCursor(53, 25);
        gfx->print(String(batteryVoltage, 2));
    }

    if (app.savedInfoText != "") {
        myfont.set_font(vietnamtimes10x2);
        int textWidth = app.savedInfoText.length() * 10;
        int xPos = 128 - app.scrollOffset;
        myfont.print(xPos, 45, app.savedInfoText, 1, 0);
        app.scrollOffset += 1;
        if (app.scrollOffset > textWidth + 128) {
            app.scrollOffset = 0;
        }
    }

    if (app.isConnected) {
        if (gfx) {
            gfx->setTextSize(1);
            int x = 110;
            int y = 0;
            gfx->fillRect(x, y, 18, 8, 1);
            gfx->fillRect(x - 2, y + 2, 2, 4, 1);
            gfx->fillRect(x + 1, y + 1, map(watch.getPhoneBattery(), 0, 100, 16, 0), 6, 0);
            gfx->setCursor(110, 11);
            gfx->print(String(watch.getPhoneBattery()) + "%");
        }
    }
    OLED_display();
}

void displayNavigation() {
    if (app.navDisplayMode == 1) {
        OLED_clear();
        if (watch.getNavigation().hasIcon && watch.getNavigation().active && gfx) {
            gfx->drawBitmap(50, 0, watch.getNavigation().icon, 48, 48, 1);
        }
        myfont.set_font(vietnamtimes8x2);
        myfont.print(0, 25, ": " + app.navTitle, 1, 0);
        myfont.print(0, 40, "KC: " + app.navDistance, 1, 0);
        myfont.print(0, 53, "TG: " + app.navETA, 1, 0);
        myfont.set_font(vietnamtimes7x2r);
        myfont.print(0, 0, app.navDirectionLines[0], 1, 0);
        if (app.navDirectionLines[1] != "") {
            myfont.print(0, 12, app.navDirectionLines[1], 1, 0);
        }
        float meters = convertToMeters(app.navTitle);
        float mappedValue = constrain(map(meters, 0, 500, 0, 50), 0, 50);
        if (gfx) {
            gfx->drawRect(100, 0, 8, 51, 1);
            gfx->fillRect(101, 1, 6, 50, 0);
            gfx->fillRect(100, mappedValue, 8, 51 - mappedValue, 1);
        }
        myfont.print(108, 0, "0m", 1, 0);
        myfont.print(108, 25, "250", 1, 0);
        myfont.print(108, 45, "500", 1, 0);
        OLED_display();
    } else if (app.navDisplayMode == 2) {
        OLED_clear();
        if (watch.getNavigation().hasIcon && watch.getNavigation().active && gfx) {
            gfx->drawBitmap(70, 0, watch.getNavigation().icon, 48, 48, 1);
        }
        myfont.set_font(vietnamtimes8x2);
        myfont.print(0, 25, ": " + app.navTitle, 1, 0);
        myfont.print(0, 40, "KC: " + app.navDistance, 1, 0);
        myfont.print(0, 53, "TG: " + app.navETA, 1, 0);
        myfont.set_font(vietnamtimes7x2r);
        myfont.print(0, 0, app.navDirectionLines[0], 1, 0);
        if (app.navDirectionLines[1] != "") {
            myfont.print(0, 12, app.navDirectionLines[1], 1, 0);
        }
        float meters = convertToMeters(app.navTitle);
        float mappedValue = constrain(map(meters, 0, 500, 0, 50), 0, 50);
        if (gfx) {
            gfx->drawRect(120, 0, 8, 51, 1);
            gfx->fillRect(121, 1, 6, 50, 0);
            gfx->fillRect(120, mappedValue, 8, 51 - mappedValue, 1);
        }
        OLED_display();
    } else if (app.navDisplayMode == 3) {
        OLED_clear();
        if (watch.getNavigation().hasIcon && watch.getNavigation().active && gfx) {
            gfx->drawBitmap(50, 0, watch.getNavigation().icon, 48, 48, 1);
        }
        myfont.set_font(vietnamtimes8x2);
        myfont.print(0, 25, ": " + app.navTitle, 1, 0);
        myfont.print(0, 40, "KC: " + app.navDistance, 1, 0);
        myfont.print(0, 53, "TG: " + app.navETA, 1, 0);
        if (gfx) {
            gfx->setTextColor(1);
            gfx->setCursor(0, 0);
            gfx->print(watch.getHourZ() + watch.getTime(":%M:%S"));
            gfx->setTextSize(1);
            int x = 2;
            int y = 15;
            gfx->fillRect(x, y, 18, 8, 1);
            gfx->fillRect(x - 2, y + 2, 2, 4, 1);
            gfx->fillRect(x + 1, y + 1, map(watch.getPhoneBattery(), 0, 100, 16, 0), 6, 0);
            gfx->setCursor(22, 15);
            gfx->print(String(watch.getPhoneBattery()) + "%");
        }
        float meters = convertToMeters(app.navTitle);
        float mappedValue = constrain(map(meters, 0, 500, 0, 50), 0, 50);
        if (gfx) {
            gfx->drawRect(100, 0, 8, 51, 1);
            gfx->fillRect(101, 1, 6, 50, 0);
            gfx->fillRect(100, mappedValue, 8, 51 - mappedValue, 1);
        }
        myfont.print(108, 0, "0m", 1, 0);
        myfont.print(108, 25, "250", 1, 0);
        myfont.print(108, 45, "500", 1, 0);
        OLED_display();
    }
}

void displayNotification() {
    OLED_clear();
    myfont.set_font(vietnamtimes8x2);
    for (int i = 0; i < 6; i++) {
        myfont.print(0, 1 + i * 11, app.notificationMsg[i], 1, 0);
    }
    OLED_display();
}

void displayCall() {
    OLED_clear();
    myfont.set_font(MakeFont_Font1);
    myfont.print(32, 12, "Incoming Call", 1, 0);
    myfont.print(32, 34, app.callerName, 1, 0);
    OLED_display();
}

void drawLogoWithLoadingBar(unsigned long currentTime) {
    OLED_clear();
    
    // Vẽ logo 64x64 tại giữa (32, 0)
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            int byteIndex = (y * 64 + x) / 8;
            int bitIndex = 7 - (x % 8);
            uint8_t byteValue = pgm_read_byte(&epd_bitmap_logopnt[byteIndex]);
            bool pixel = (byteValue >> bitIndex) & 0x01;
            if (gfx) gfx->drawPixel(32 + x, y, pixel ? 1 : 0);
        }
    }

    // Tính tiến trình thanh loading
    float progress = (float)(currentTime - startTime) / introDuration;
    int barWidth = progress * SCREEN_WIDTH; // 0 đến 128 pixel

    // Vẽ thanh loading tại (0, 60)
    myfont.set_font(vietnamtimes6x2);
    myfont.print(0, 50, "loading...", 1, 0);
    if (gfx) gfx->fillRect(0, 60, barWidth, 4, 1);
    OLED_display();
}
// **Hàm chính**
void setup() {
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(BUILTINLED, OUTPUT);
    Wire.begin(OLED_SDA, OLED_SCL);
    prefs.begin("myApp", false);
    initDisplayFromPrefs();
    app.savedInfoText = prefs.getString("savedText", "");
    app.navDisplayMode = prefs.getInt("navMode", 1);
    watch.setConnectionCallback(connectionCallback);
    watch.setNotificationCallback(notificationCallback);
    watch.setConfigurationCallback(configCallback);
    watch.setRingerCallback(ringerCallback);
    watch.begin();
    watch.set24Hour(true);
    watch.setBattery(80);
    ws2812fx.init();
    ws2812fx.setBrightness(0);
    ws2812fx.setSpeed(500);
    ws2812fx.setColor(0x007BFF);
    ws2812fx.setMode(FX_MODE_STATIC);
    ws2812fx.start();
    attachInterrupt(BUTTON_PIN, buttonISR, FALLING);
}

void loop() {
    watch.loop();
    ws2812fx.service();
    unsigned long currentTime = millis();
    
    if (app.hasNotification && (millis() - notifyTimer.time >= 5000)) {
        app.hasNotification = false;
        ws2812fx.setBrightness(0);
    }
     if (showIntro) {
        if (currentTime - startTime <= introDuration) {
            drawLogoWithLoadingBar(currentTime);
        } else {
            showIntro = false;
            OLED_clear();
            OLED_display();
        }
        return;
    }
    if (app.isCalling) {
        currentMode = CALL;
    } else if (app.hasNotification && (millis() - notifyTimer.time < 5000)) {
        currentMode = NOTIFICATION;
    } else if (app.isNavigating) {
        currentMode = NAVIGATION;
    } else {
        currentMode = TIME_LARGE;
    }
    if (app.navChange && (millis() - app.lastNavChangeTime >= 30000)) {
        currentMode = TIME_LARGE;
    }
    switch (currentMode) {
        case TIME_SMALL:
            displayTime(true);
            break;
        case TIME_LARGE:
            displayTime(false);
            break;
        case NAVIGATION:
            displayNavigation();
            break;
        case NOTIFICATION:
            displayNotification();
            break;
        case CALL:
            displayCall();
            break;
    }
}

// **Kalman Filter và đọc điện áp**
float kalmanX = 12.0;
float kalmanP = 1.0;
float kalmanQ = 0.05;
float kalmanR = 0.1;

float readBatteryVoltage() {
    const float alpha = 0.1;
    const float R1 = 100000;
    const float R2 = 10000;
    const float ADC_REF_VOLTAGE = 3.3;
    const float correctionFactor = 0.944;   // 12.00 / 12.70 ≈ 0.944 mặt định =1 chỉ tính khi sai số
    static float emaValue = 0;
    
    int rawADC = analogRead(A0); // Thay A0 bằng A4 (GPIO4) cho ESP32-C3
    emaValue = alpha * rawADC + (1 - alpha) * emaValue;
    float measuredVoltage = (emaValue / 4095.0) * ADC_REF_VOLTAGE * ((R1 + R2) / R2);
    float x_pred = kalmanX;
    float P_pred = kalmanP + kalmanQ;
    float K = P_pred / (P_pred + kalmanR);
    kalmanX = x_pred + K * (measuredVoltage - x_pred);
    kalmanP = (1 - K) * P_pred;
  
     return kalmanX * correctionFactor;  // 👈 Áp dụng hệ số tinh chỉnh
}

void IRAM_ATTR buttonISR() {
    buttonPressed = true;
}