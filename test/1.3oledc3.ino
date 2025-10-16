#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ChronosESP32.h>
#include <WS2812FX.h>
#include "FontMaker.h"
#include <Preferences.h>

#define LED_COUNT 1
#define LED_PIN 8         // Thay đổi từ 27 sang GPIO8 cho ESP32-C3
#define BUILTINLED 9      // Thay đổi từ 2 sang GPIO9 cho ESP32-C3
#define BUTTON_PIN 0      // Giữ nguyên GPIO0
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 6        // Thay đổi từ 25 sang GPIO6 cho ESP32-C3
#define OLED_SCL 7        // Thay đổi từ 26 sang GPIO7 cho ESP32-C3

Adafruit_SH1106G myOLED(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

ChronosESP32 watch("TSMART Navigation");
WS2812FX ws2812fx = WS2812FX(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// Định nghĩa hàm setpx cho MakeFont
void setpx(int16_t x, int16_t y, uint16_t color) {
    myOLED.drawPixel(x, y, color ? SH110X_WHITE : SH110X_BLACK);
}
MakeFont myfont(&setpx);

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

    if (message.startsWith("info: ")) {
        app.savedInfoText = message.substring(6);
        Serial.print("Extracted savedInfoText: ");
        Serial.println(app.savedInfoText);
        app.scrollOffset = 0;
        prefs.putString("savedText", app.savedInfoText);
    } else if (message.startsWith("main: ")) {
        String modeStr = message.substring(6);
        if (modeStr == "1") {
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
    myOLED.clearDisplay();
    myOLED.setTextSize(2);
    myOLED.setTextColor(SH110X_WHITE);
    myOLED.setCursor(0, 0);
    myOLED.print(watch.getHourZ() + watch.getTime(":%M:%S"));

    float batteryVoltage = readBatteryVoltage();
    myfont.set_font(vietnamtimes10x2);
    myOLED.fillRect(0, 21, 128, 16, SH110X_BLACK);
    myfont.print(0, 25, "ĐIỆN ÁP: ", SH110X_WHITE, SH110X_BLACK);
    myfont.print(112, 25, "V", SH110X_WHITE, SH110X_BLACK);
    myOLED.setTextSize(2);
    myOLED.setCursor(53, 25);
    myOLED.print(String(batteryVoltage, 2));

    if (app.savedInfoText != "") {
        myfont.set_font(vietnamtimes10x2);
        int textWidth = app.savedInfoText.length() * 10;
        int xPos = 128 - app.scrollOffset;
        myfont.print(xPos, 45, app.savedInfoText, SH110X_WHITE, SH110X_BLACK);
        app.scrollOffset += 1;
        if (app.scrollOffset > textWidth + 128) {
            app.scrollOffset = 0;
        }
    }

    if (app.isConnected) {
        myOLED.setTextSize(1);
        int x = 110;
        int y = 0;
        myOLED.fillRect(x, y, 18, 8, SH110X_WHITE);
        myOLED.fillRect(x - 2, y + 2, 2, 4, SH110X_WHITE);
        myOLED.fillRect(x + 1, y + 1, map(watch.getPhoneBattery(), 0, 100, 16, 0), 6, SH110X_BLACK);
        myOLED.setCursor(110, 11);
        myOLED.print(String(watch.getPhoneBattery()) + "%");
    }
    myOLED.display();
}

void displayNavigation() {
    if (app.navDisplayMode == 1) {
        myOLED.clearDisplay();
        if (watch.getNavigation().hasIcon && watch.getNavigation().active) {
            myOLED.drawBitmap(50, 0, watch.getNavigation().icon, 48, 48, SH110X_WHITE);
        }
        myfont.set_font(vietnamtimes8x2);
        myfont.print(0, 25, ": " + app.navTitle, SH110X_WHITE, SH110X_BLACK);
        myfont.print(0, 40, "KC: " + app.navDistance, SH110X_WHITE, SH110X_BLACK);
        myfont.print(0, 53, "TG: " + app.navETA, SH110X_WHITE, SH110X_BLACK);
        myfont.set_font(vietnamtimes7x2r);
        myfont.print(0, 0, app.navDirectionLines[0], SH110X_WHITE, SH110X_BLACK);
        if (app.navDirectionLines[1] != "") {
            myfont.print(0, 12, app.navDirectionLines[1], SH110X_WHITE, SH110X_BLACK);
        }
        float meters = convertToMeters(app.navTitle);
        float mappedValue = constrain(map(meters, 0, 500, 0, 50), 0, 50);
        myOLED.drawRect(100, 0, 8, 51, SH110X_WHITE);
        myOLED.fillRect(101, 1, 6, 50, SH110X_BLACK);
        myOLED.fillRect(100, mappedValue, 8, 51 - mappedValue, SH110X_WHITE);
        myfont.print(108, 0, "0m", SH110X_WHITE, SH110X_BLACK);
        myfont.print(108, 25, "250", SH110X_WHITE, SH110X_BLACK);
        myfont.print(108, 45, "500", SH110X_WHITE, SH110X_BLACK);
        myOLED.display();
    } else if (app.navDisplayMode == 2) {
        myOLED.clearDisplay();
        if (watch.getNavigation().hasIcon && watch.getNavigation().active) {
            myOLED.drawBitmap(70, 0, watch.getNavigation().icon, 48, 48, SH110X_WHITE);
        }
        myfont.set_font(vietnamtimes8x2);
        myfont.print(0, 25, ": " + app.navTitle, SH110X_WHITE, SH110X_BLACK);
        myfont.print(0, 40, "KC: " + app.navDistance, SH110X_WHITE, SH110X_BLACK);
        myfont.print(0, 53, "TG: " + app.navETA, SH110X_WHITE, SH110X_BLACK);
        myfont.set_font(vietnamtimes7x2r);
        myfont.print(0, 0, app.navDirectionLines[0], SH110X_WHITE, SH110X_BLACK);
        if (app.navDirectionLines[1] != "") {
            myfont.print(0, 12, app.navDirectionLines[1], SH110X_WHITE, SH110X_BLACK);
        }
        float meters = convertToMeters(app.navTitle);
        float mappedValue = constrain(map(meters, 0, 500, 0, 50), 0, 50);
        myOLED.drawRect(120, 0, 8, 51, SH110X_WHITE);
        myOLED.fillRect(121, 1, 6, 50, SH110X_BLACK);
        myOLED.fillRect(120, mappedValue, 8, 51 - mappedValue, SH110X_WHITE);
        myOLED.display();
    } else if (app.navDisplayMode == 3) {
        myOLED.clearDisplay();
        if (watch.getNavigation().hasIcon && watch.getNavigation().active) {
            myOLED.drawBitmap(50, 0, watch.getNavigation().icon, 48, 48, SH110X_WHITE);
        }
        myfont.set_font(vietnamtimes8x2);
        myfont.print(0, 25, ": " + app.navTitle, SH110X_WHITE, SH110X_BLACK);
        myfont.print(0, 40, "KC: " + app.navDistance, SH110X_WHITE, SH110X_BLACK);
        myfont.print(0, 53, "TG: " + app.navETA, SH110X_WHITE, SH110X_BLACK);
        myfont.set_font(vietnamtimes8x2);
        myOLED.setTextColor(SH110X_WHITE);
        myOLED.setCursor(0, 0);
        myOLED.print(watch.getHourZ() + watch.getTime(":%M:%S"));
        myOLED.setTextSize(1);
        int x = 2;
        int y = 15;
        myOLED.fillRect(x, y, 18, 8, SH110X_WHITE);
        myOLED.fillRect(x - 2, y + 2, 2, 4, SH110X_WHITE);
        myOLED.fillRect(x + 1, y + 1, map(watch.getPhoneBattery(), 0, 100, 16, 0), 6, SH110X_BLACK);
        myOLED.setCursor(22, 15);
        myOLED.print(String(watch.getPhoneBattery()) + "%");
        float meters = convertToMeters(app.navTitle);
        float mappedValue = constrain(map(meters, 0, 500, 0, 50), 0, 50);
        myOLED.drawRect(100, 0, 8, 51, SH110X_WHITE);
        myOLED.fillRect(101, 1, 6, 50, SH110X_BLACK);
        myOLED.fillRect(100, mappedValue, 8, 51 - mappedValue, SH110X_WHITE);
        myfont.print(108, 0, "0m", SH110X_WHITE, SH110X_BLACK);
        myfont.print(108, 25, "250", SH110X_WHITE, SH110X_BLACK);
        myfont.print(108, 45, "500", SH110X_WHITE, SH110X_BLACK);
        myOLED.display();
    }
}

void displayNotification() {
    myOLED.clearDisplay();
    myfont.set_font(vietnamtimes8x2);
    for (int i = 0; i < 6; i++) {
        myfont.print(0, 1 + i * 11, app.notificationMsg[i], SH110X_WHITE, SH110X_BLACK);
    }
    myOLED.display();
}

void displayCall() {
    myOLED.clearDisplay();
    myfont.set_font(MakeFont_Font1);
    myfont.print(32, 12, "Incoming Call", SH110X_WHITE, SH110X_BLACK);
    myfont.print(32, 34, app.callerName, SH110X_WHITE, SH110X_BLACK);
    myOLED.display();
}

// **Hàm chính**
void setup() {
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(BUILTINLED, OUTPUT);
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!myOLED.begin(0x3C, true)) {
        Serial.println(F("SH1106 not found!"));
        while (1);
    }
    prefs.begin("myApp", false);
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
    if (app.hasNotification && (millis() - notifyTimer.time >= 5000)) {
        app.hasNotification = false;
        ws2812fx.setBrightness(0);
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
    const float ADC_REF_VOLTAGE = 1;
    static float emaValue = 0;
    int rawADC = analogRead(A0); // Thay A0 bằng A4 (GPIO4) cho ESP32-C3
    emaValue = alpha * rawADC + (1 - alpha) * emaValue;
    float measuredVoltage = (emaValue / 1023.0) * ADC_REF_VOLTAGE * ((R1 + R2) / R2);
    float x_pred = kalmanX;
    float P_pred = kalmanP + kalmanQ;
    float K = P_pred / (P_pred + kalmanR);
    kalmanX = x_pred + K * (measuredVoltage - x_pred);
    kalmanP = (1 - K) * P_pred;
    return kalmanX;
}

void IRAM_ATTR buttonISR() {
    buttonPressed = true;
}