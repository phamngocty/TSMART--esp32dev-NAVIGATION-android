#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <SPI.h>
#include <logo.h>
#include <ChronosESP32.h>
#include <Preferences.h>
#include <WS2812FX.h>
#include "FontMaker.h"

// Định nghĩa các hằng số cho màn hình TFT 240x240
#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 240
#define TFT_CS        10  // GPIO10
#define TFT_DC        5   // GPIO5
#define TFT_RST       4   // GPIO4
#define TFT_SCLK      6   // GPIO6 (SCLK mặc định trên ESP32-C3)
#define TFT_MOSI      7   // GPIO7 (MOSI mặc định trên ESP32-C3)
#define BUTTON_PIN    8   // GPIO8 (tránh GPIO0 vì chức năng boot mode)
#define BUILTINLED    9   // GPIO9 (giả sử LED tích hợp ở GPIO9)
#define LED_PIN       3   // GPIO3
#define LED_COUNT     1   // Số lượng LED

// Định nghĩa màu (RGB565 cho TFT)
#define COLOR_BG        0x0000 // Đen
#define COLOR_TEXT      0x07FF // Cyan
#define COLOR_ARROW     0x07E0 // Xanh lá (không dùng trong displayNavigation)
#define COLOR_SECONDARY 0xC618 // Xám nhạt
#define COLOR_HIGHLIGHT 0xFFE0 // Vàng
#define COLOR_LOGO      0xFFFF // Trắng
#define COLOR_BAR       0x07FF // Cyan
#define COLOR_BAR_BORDER 0xFFFF // Trắng

// Biến toàn cục cho bộ lọc Kalman (đọc điện áp pin)
float kalmanX = 12.0;       // Giá trị ước lượng ban đầu (12V)
float kalmanP = 1.0;        // Độ không chắc chắn ban đầu
float kalmanQ = 0.05;       // Nhiễu quá trình
float kalmanR = 0.1;        // Nhiễu đo lường

// Biến cho điện áp pin
float batteryVoltage = 0.0;
float batteryVoltagetime1s = 0.0;
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 1000;  // Cập nhật mỗi 1 giây

// Biến cho logo intro
unsigned long startTime = 0; // Thời điểm bắt đầu
const unsigned long introDuration = 3000; // 3 giây
bool showIntro = true; // Cờ hiển thị logo

// Cấu trúc trạng thái ứng dụng
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
    String navDuration = "";
    unsigned long lastNavChangeTime = 0;
    bool navChange = false;
    String savedInfoText = "";
    int scrollOffset = 0;
    String lastNavTitle = "";
    int navDisplayMode = 1;
};
AppState app;

// Các biến toàn cục khác
ChronosTimer notifyTimer;
volatile bool buttonPressed = false;
Preferences prefs;
bool needsDisplayUpdate = true; // Cờ kiểm soát vẽ lại
unsigned long lastScrollUpdate = 0;
const unsigned long scrollInterval = 50; // Cuộn văn bản mỗi 50ms
unsigned long lastSlideUpdate = 0;
const unsigned long slideInterval = 5; // Cập nhật hiệu ứng trượt mỗi 5ms

// Chế độ hiển thị
enum DisplayMode {
    TIME_SMALL,
    TIME_LARGE,
    NAVIGATION,
    NOTIFICATION,
    CALL
};
DisplayMode currentMode = TIME_LARGE;

// Khởi tạo các đối tượng
Adafruit_GC9A01A myTFT(TFT_CS, TFT_DC, TFT_RST);
ChronosESP32 watch("TSMART Navigation");
WS2812FX ledStrip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// Thiết lập font cho FontMaker
void setpx(int16_t x, int16_t y, uint16_t color) {
    myTFT.drawPixel(x, y, color);
}
MakeFont myfont(&setpx);

// Callback khi trạng thái kết nối thay đổi
void connectionCallback(bool state) {
    app.isConnected = state;
    Serial.print("Connection state: ");
    Serial.println(state ? "Connected" : "Disconnected");
    needsDisplayUpdate = true;
}

// Callback khi nhận thông báo
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
        currentMode = NOTIFICATION;
    }
    needsDisplayUpdate = true;
}

// Callback khi nhận dữ liệu cấu hình (điều hướng)
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
            app.navDuration = nav.duration;
            app.navChange = true;
            if (nav.title != app.lastNavTitle) {
                app.lastNavChangeTime = millis();
                app.lastNavTitle = nav.title;
            }
        } else {
            currentMode = TIME_LARGE;
            app.navChange = false;
        }
        needsDisplayUpdate = true;
    }
}

// Callback khi có cuộc gọi
void ringerCallback(String caller, bool state) {
    app.isCalling = state;
    if (state) {
        app.callerName = caller;
        currentMode = CALL;
        ledStrip.setMode(FX_MODE_BLINK);
        ledStrip.setColor(0xFF0000); // Đỏ
        ledStrip.start();
    } else {
        currentMode = TIME_LARGE;
        ledStrip.stop();
    }
    needsDisplayUpdate = true;
}

// Chia nhỏ hướng dẫn điều hướng thành 2 dòng
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
    } else if (app.navDisplayMode == 2) {
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

// Chuyển đổi khoảng cách sang mét
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

// Biến để quản lý hiệu ứng trượt
static bool firstSlide = true;
static int slideOffsetY = -240; // Vị trí bắt đầu của hiệu ứng trượt
static bool slideComplete = false;

void resetSlideEffect() {
    firstSlide = true;
    slideOffsetY = -240;
    slideComplete = false;
}

// Hiển thị thời gian và thông tin pin
void displayTime(bool smallFont) {
    String currentTime = watch.getHourZ() + watch.getTime(":%M:%S");
    int centerX = SCREEN_WIDTH / 2; // 120
    int centery = SCREEN_HEIGHT / 2; // 120

    // Hiệu ứng trượt từ trên xuống
    if (!slideComplete) {
        if (firstSlide) {
            slideOffsetY = -240; // Đặt lại vị trí ban đầu
            firstSlide = false;
        }

        unsigned long currentTimeMillis = millis();
        if (currentTimeMillis - lastSlideUpdate >= slideInterval) {
            slideOffsetY += 8; // Tăng tốc độ trượt (8 pixel mỗi bước)
            lastSlideUpdate = currentTimeMillis;

            // Xóa vùng hiển thị chính
            myTFT.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT - 40, COLOR_BG); // [FILLRECT]

            // Vẽ vòng tròn với tọa độ y thay đổi
            myTFT.fillCircle(centerX, -200 + slideOffsetY, 240, 0x0680);

            // Vẽ header "TIME" với tọa độ y thay đổi
            myTFT.setTextSize(2);
            myTFT.setTextColor(COLOR_TEXT);
            myTFT.setCursor(centerX - 20, 10 + slideOffsetY);
            myTFT.print("TIME");

            if (slideOffsetY >= 0) {
                slideComplete = true; // Kết thúc hiệu ứng
            }
        }
    }

    // Vẽ giao diện chính nếu hiệu ứng hoàn tất
    if (slideComplete) {
        // Vẽ thời gian
        //myTFT.fillRect(0, 40, 240, 240, 0x0000);
       // myTFT.fillRect(centerX - 70, 90, 140, 40, COLOR_BG); // [FILLRECT]
        myTFT.setTextSize(smallFont ? 2 : 3);
        myTFT.setTextColor(0x0680, COLOR_BG);
        myTFT.setCursor(centerX - 70, 90);
        myTFT.print(currentTime);

        // Vẽ điện áp
       // myTFT.fillRect(centerX - 80, 140, 160, 20, COLOR_BG); // [FILLRECT]
        myfont.set_font(vietnamtimes10x2);
        myfont.print(centerX - 80, 140, "ĐIỆN ÁP: ", COLOR_TEXT, COLOR_BG);
        myfont.print(centerX + 50, 140, "V", COLOR_TEXT, COLOR_BG);
        myTFT.setTextColor(COLOR_TEXT, COLOR_BG);
        myTFT.setTextSize(2);
        myTFT.setCursor(centerX - 20, 140);
        myTFT.print(String(batteryVoltagetime1s, 2));

        // Vẽ pin điện thoại
       // myTFT.fillRect(centerX + 65, centery - 80, 30, 20, COLOR_BG); // [FILLRECT]
        if (app.isConnected) {
            myTFT.setTextSize(1);
            int x = centerX + 65;
            int y = centery - 80;
            myTFT.fillRect(x, y, 18, 8, COLOR_HIGHLIGHT); // [FILLRECT]
            myTFT.fillRect(x - 2, y + 2, 2, 4, COLOR_HIGHLIGHT); // [FILLRECT]
            myTFT.fillRect(x + 1, y + 1, map(watch.getPhoneBattery(), 0, 100, 16, 0), 6, COLOR_BG); // [FILLRECT]
            myTFT.setCursor(x, y + 10);
            myTFT.print(String(watch.getPhoneBattery()) + "%");
        }

        // Vẽ văn bản cuộn
        if (app.savedInfoText != "") {
            unsigned long currentTimeMillis = millis();
            if (currentTimeMillis - lastScrollUpdate >= scrollInterval) {
                myTFT.fillRect(0, 180, SCREEN_WIDTH, 20, COLOR_BG); // [FILLRECT]
                myfont.set_font(vietnamtimes10x2);
                int textWidth = app.savedInfoText.length() * 10;
                int xPos = SCREEN_WIDTH - app.scrollOffset;
                myfont.print(xPos, 180, app.savedInfoText, COLOR_SECONDARY, COLOR_BG);
                app.scrollOffset += 1;
                if (app.scrollOffset > textWidth + SCREEN_WIDTH) {
                    app.scrollOffset = 0;
                }
                lastScrollUpdate = currentTimeMillis;
            }
        }
    }
}

/// Hàm vẽ thanh tiến trình dạng cung tròn
void drawArcProgress(int16_t centerX, int16_t centerY, int16_t outerRadius, int16_t innerRadius, int16_t startAngle, int16_t endAngle, float progress) {
    static float prevProgress = -1.0; // Lưu giá trị progress trước đó, khởi tạo khác progress hiện tại để vẽ lần đầu

    // Chỉ vẽ lại nếu progress thay đổi
    if (progress != prevProgress) {
        // Bước 1: Xóa toàn bộ vùng cung tròn bằng màu nền
        for (int16_t r = innerRadius + 1; r < outerRadius; r++) {
            for (int16_t angle = startAngle; angle <= endAngle; angle++) {
                float rad = angle * PI / 180.0;
                int16_t x = centerX + r * cos(rad);
                int16_t y = centerY - r * sin(rad);
                myTFT.drawPixel(x, y, COLOR_BG); // Xóa bằng màu nền (đen)
            }
        }

        // Bước 2: Vẽ viền ngoài và trong
        for (int16_t r = innerRadius; r <= outerRadius; r++) {
            float radStart = startAngle * PI / 180.0;
            float radEnd = endAngle * PI / 180.0;
            int16_t xStart = centerX + r * cos(radStart);
            int16_t yStart = centerY - r * sin(radStart);
            int16_t xEnd = centerX + r * cos(radEnd);
            int16_t yEnd = centerY - r * sin(radEnd);
            myTFT.drawPixel(xStart, yStart, COLOR_BAR_BORDER);
            myTFT.drawPixel(xEnd, yEnd, COLOR_BAR_BORDER);
        }

        // Bước 3: Vẽ phần tiến trình (progress)
        int16_t progressAngle = startAngle + (endAngle - startAngle) * progress;
        for (int16_t r = innerRadius + 1; r < outerRadius; r++) {
            for (int16_t angle = startAngle; angle <= progressAngle; angle++) {
                float rad = angle * PI / 180.0;
                int16_t x = centerX + r * cos(rad);
                int16_t y = centerY - r * sin(rad);
                myTFT.drawPixel(x, y, COLOR_BAR);
            }
        }

        // Cập nhật giá trị progress trước đó
        prevProgress = progress;
    }
}


// Hiển thị thông tin điều hướng
void displayNavigation() {
    int centerX = SCREEN_WIDTH / 2; // 120
    int centerY = SCREEN_HEIGHT / 2; // 120

    static String prevDirection = "";
    static String prevDistance = "";
    static String prevETA = "";
    static String prevTitle = "";

    // Hiệu ứng trượt từ trên xuống
    if (!slideComplete) {
        if (firstSlide) {
            slideOffsetY = -240; // Đặt lại vị trí ban đầu
            firstSlide = false;
        }

        unsigned long currentTimeMillis = millis();
        if (currentTimeMillis - lastSlideUpdate >= slideInterval) {
            slideOffsetY += 8; // Tăng tốc độ trượt (8 pixel mỗi bước)
            lastSlideUpdate = currentTimeMillis;

            // Xóa vùng hiển thị chính
            myTFT.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BG); // [FILLRECT]

            // Vẽ vòng tròn với tọa độ y thay đổi
            myTFT.fillCircle(centerX, -80 + slideOffsetY, 120, 0x041F);

            // Vẽ header "Navigation" với tọa độ y thay đổi
            myfont.set_font(vietnamtimes10x2);
            myfont.print(centerX - 30, 10 + slideOffsetY, "Navigation", 0xFFFF, 0x041F);

            if (slideOffsetY >= 0) {
                slideComplete = true; // Kết thúc hiệu ứng
            }
        }
    }

    // Vẽ giao diện chính nếu hiệu ứng hoàn tất
   
    // Vẽ giao diện chính nếu hiệu ứng hoàn tất
    if (slideComplete) {
        // Vẽ tên đường
        String direction = app.navDirection;
        if (app.navDirection != prevDirection) {
            // Xóa vùng cũ bằng cách vẽ chuỗi khoảng trống
            myfont.set_font(vietnamtimes12);
            myfont.print(20, 60, "                                      .", 0x0000, 0x0000);
            myTFT.fillRect(0 ,60,240 ,20,0x0000); // 20 khoảng trống để xóa vùng cũ
            // Vẽ văn bản mới
            myfont.print(20, 60, direction, 0xC618, 0x0000); // "Turn Right" xám nhạt
            prevDirection = app.navDirection;
        }

        // Vẽ "TAKE TIME" và ETA
        myfont.set_font(vietnamtimes10x2);
        myfont.print(10, 110, "TAKE TIME", 0xC618, 0x0000); // Xám nhạt
        if (app.navETA != prevETA) {
            // Xóa vùng cũ
            myfont.set_font(vietnamtimes12);
            myfont.print(-60, 135, "          ", 0x0000, 0x0000); // 10 khoảng trống để xóa vùng cũ
            // Vẽ văn bản mới
            myfont.print(-60, 135, app.navETA, 0xFFFF, 0x0000); // "1.8 Km" trắng
            prevETA = app.navETA;
        }

        // Vẽ "DISTANCE" và khoảng cách
        myfont.set_font(vietnamtimes10x2);
        myfont.print(165, 110, "DISTANCE", 0xC618, 0x0000); // Xám nhạt
        if (app.navDistance != prevDistance) {
            // Xóa vùng cũ
            myfont.set_font(vietnamtimes12);
            myTFT.fillRect(175, 135,50 ,20,0x0000);
            //myfont.print(175, 135, "                 .", 0x0000, 0x0000); // 10 khoảng trống để xóa vùng cũ
            // Vẽ văn bản mới
            myfont.print(175, 135, app.navDistance, 0xFFFF, 0x0000); // "45 mins" trắng
            prevDistance = app.navDistance;
        }

        // Vẽ mũi tên
        if (watch.getNavigation().hasIcon && watch.getNavigation().active) {
            myTFT.drawBitmap(centerX - 24, 100, watch.getNavigation().icon, 48, 48, 0x06FE, 0x0000); // Mũi tên xanh 48,48
        }

        // Vẽ tiêu đề (khoảng cách đến điểm rẽ)
        if (app.navTitle != prevTitle) {
            // Xóa vùng cũ
            myTFT.setTextSize(3);
            myTFT.setTextColor(COLOR_TEXT, COLOR_BG);
            myTFT.setCursor(centerX - 15, 160);
            myTFT.print("        "); // 8 khoảng trống để xóa vùng cũ
            // Vẽ văn bản mới
            myTFT.setCursor(centerX - 15, 160);
            myTFT.print(app.navTitle);
            prevTitle = app.navTitle;
        }

        // Vẽ thanh tiến trình dạng cung tròn
        float meters = convertToMeters(app.navTitle);
        float progress = constrain((500.0 - meters) / 500.0, 0.0, 1.0); // Ánh xạ 0-500m thành 1-0 (0m đầy, 500m trống)
        drawArcProgress(centerX, centerY, 115, 105, 225, 315, progress);
        
        // Vẽ nhãn cho thanh tiến trình
        myfont.set_font(vietnamtimes7x2r);
        //myfont.print(centerX+85, centerY+60, "0m", 0xC618, 0x0000);
        myfont.print(centerX-5, centerY+90, "250", 0xC618, 0x0000);
        myfont.print(centerX-95, centerY+60, "500m", 0xC618, 0x0000);
    }


}

// Hiển thị thông báo
void displayNotification() {
    static bool firstCall = true;
    static String prevMsg[6] = {"", "", "", "", "", ""};

    if (firstCall || needsDisplayUpdate) {
        myTFT.fillRect(20, 30, SCREEN_WIDTH - 40, 180, COLOR_BG); // [FILLRECT]
        myfont.set_font(vietnamtimes8x2);
        for (int i = 0; i < 6; i++) {
            myfont.print(20, 30 + i * 30, app.notificationMsg[i], COLOR_HIGHLIGHT, COLOR_BG);
            prevMsg[i] = app.notificationMsg[i];
        }
        firstCall = false;
        needsDisplayUpdate = false;
    } else {
        for (int i = 0; i < 6; i++) {
            if (app.notificationMsg[i] != prevMsg[i]) {
                myTFT.fillRect(20, 30 + i * 30, SCREEN_WIDTH - 40, 20, COLOR_BG); // [FILLRECT]
                myfont.set_font(vietnamtimes8x2);
                myfont.print(20, 30 + i * 30, app.notificationMsg[i], COLOR_HIGHLIGHT, COLOR_BG);
                prevMsg[i] = app.notificationMsg[i];
            }
        }
    }
}

// Hiển thị thông tin cuộc gọi
void displayCall() {
    static bool firstCall = true;
    static String prevCaller = "";

    if (firstCall || needsDisplayUpdate) {
        myTFT.fillRect(60, 80, SCREEN_WIDTH - 120, 80, COLOR_BG); // [FILLRECT]
        myfont.set_font(MakeFont_Font1);
        myfont.print(60, 80, "Incoming Call", COLOR_HIGHLIGHT, COLOR_BG);
        myfont.print(60, 120, app.callerName, COLOR_TEXT, COLOR_BG);
        prevCaller = app.callerName;
        firstCall = false;
        needsDisplayUpdate = false;
    } else {
        if (app.callerName != prevCaller) {
            myTFT.fillRect(60, 120, SCREEN_WIDTH - 120, 20, COLOR_BG); // [FILLRECT]
            myfont.set_font(MakeFont_Font1);
            myfont.print(60, 120, app.callerName, COLOR_TEXT, COLOR_BG);
            prevCaller = app.callerName;
        }
    }
}

// Hiển thị logo và thanh tải khi khởi động
void drawLogoWithLoadingBar(unsigned long currentTime) {
    static bool firstCall = true;
    if (firstCall) {
        myTFT.fillScreen(COLOR_BG);

        // Vẽ logo
        int logoX = (SCREEN_WIDTH - 64) / 2; // 88
        int logoY = 50;
        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 64; x++) {
                int byteIndex = (y * 64 + x) / 8;
                int bitIndex = 7 - (x % 8);
                uint8_t byteValue = pgm_read_byte(&epd_bitmap_logopnt[byteIndex]);
                bool pixel = (byteValue >> bitIndex) & 0x01;
                myTFT.drawPixel(logoX + x, logoY + y, pixel ? COLOR_LOGO : COLOR_BG);
            }
        }

        // Vẽ viền thanh tải
        int barWidth = 140;
        int barHeight = 10;
        int barX = (SCREEN_WIDTH - barWidth) / 2; // 50
        int barY = 140;
        myTFT.drawRect(barX, barY, barWidth, barHeight, COLOR_BAR_BORDER);
        myfont.set_font(vietnamtimes6x2);
        myfont.print(barX, barY + 15, "loading...", COLOR_TEXT, COLOR_BG);
        firstCall = false;
    }

    // Cập nhật thanh tải
    int barWidth = 140;
    int barHeight = 10;
    int barX = (SCREEN_WIDTH - barWidth) / 2; // 50
    int barY = 140;
    float progress = (float)(currentTime - startTime) / introDuration;
    int fillWidth = progress * barWidth;
    myTFT.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, COLOR_BAR); // [FILLRECT]
    if (fillWidth < barWidth) {
        myTFT.fillRect(barX + fillWidth + 1, barY + 1, barWidth - fillWidth - 1, barHeight - 2, COLOR_BG); // [FILLRECT]
    }
}

// Đọc điện áp pin với bộ lọc Kalman
float readBatteryVoltage() {
    const float alpha = 0.1;
    const float R1 = 100000;
    const float R2 = 10000;
    const float ADC_REF_VOLTAGE = 1;
    static float emaValue = 0;
    int rawADC = analogRead(0); // GPIO0 (ADC1_CH0 trên ESP32-C3)
    emaValue = alpha * rawADC + (1 - alpha) * emaValue;
    float measuredVoltage = (emaValue / 1023.0) * ADC_REF_VOLTAGE * ((R1 + R2) / R2);
    // Bộ lọc Kalman
    float x_pred = kalmanX;
    float P_pred = kalmanP + kalmanQ;
    float K = P_pred / (P_pred + kalmanR);
    kalmanX = x_pred + K * (measuredVoltage - x_pred);
    kalmanP = (1 - K) * P_pred;
    return kalmanX;
}

// Cập nhật giá trị điện áp pin
void updateBatteryVoltage() {
    batteryVoltage = readBatteryVoltage();
}

// Cài đặt ban đầu
void setup() {
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(BUILTINLED, OUTPUT);

    // Khởi tạo SPI
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    SPI.setFrequency(80000000); // 80 MHz (giảm từ 100MHz để đảm bảo ổn định)
    SPI.setDataMode(SPI_MODE0);
    SPI.setBitOrder(MSBFIRST);

    // Khởi tạo TFT
    myTFT.begin();
    myTFT.setSPISpeed(80000000);
    myTFT.setRotation(0);
    myTFT.fillScreen(COLOR_BG);

    // Khởi tạo LED
    ledStrip.init();
    ledStrip.setBrightness(100);
    ledStrip.setSpeed(1000);
    ledStrip.start();

    // Khởi tạo Preferences
    prefs.begin("myApp", false);
    app.savedInfoText = prefs.getString("savedText", "");
    app.navDisplayMode = prefs.getInt("navMode", 1);

    // Khởi tạo ChronosESP32
    watch.setConnectionCallback(connectionCallback);
    watch.setNotificationCallback(notificationCallback);
    watch.setConfigurationCallback(configCallback);
    watch.setRingerCallback(ringerCallback);
    watch.begin();
    watch.set24Hour(true);
    watch.setBattery(80);

    attachInterrupt(BUTTON_PIN, buttonISR, FALLING);
    startTime = millis();
}

// Vòng lặp chính
void loop() {
    unsigned long currentTime = millis();

    watch.loop();
    ledStrip.service();
    updateBatteryVoltage();
    if (currentTime - lastBatteryUpdate >= batteryUpdateInterval) {
        batteryVoltagetime1s = batteryVoltage;
        lastBatteryUpdate = currentTime;
        needsDisplayUpdate = true;
    }

    if (showIntro) {
        if (currentTime - startTime <= introDuration) {
            drawLogoWithLoadingBar(currentTime);
        } else {
            showIntro = false;
            myTFT.fillScreen(COLOR_BG);
            needsDisplayUpdate = true;
        }
        return;
    }

    // Kiểm tra chuyển chế độ
    static DisplayMode prevMode = TIME_LARGE;
    if (app.hasNotification && (currentTime - notifyTimer.time >= 5000)) {
        app.hasNotification = false;
        ledStrip.stop();
        needsDisplayUpdate = true;
    }
    if (app.isCalling) {
        currentMode = CALL;
    } else if (app.hasNotification && (currentTime - notifyTimer.time < 5000)) {
        currentMode = NOTIFICATION;
        ledStrip.setMode(FX_MODE_BLINK);
        ledStrip.setColor(0x00FF00); // Xanh lá
        ledStrip.start();
    } else if (app.isNavigating) {
        currentMode = NAVIGATION;
    } else {
        currentMode = TIME_LARGE;
    }
    if (app.navChange && (currentTime - app.lastNavChangeTime >= 30000)) {
        currentMode = TIME_LARGE;
        needsDisplayUpdate = true;
    }

    // Đặt lại firstCall khi chuyển chế độ
    if (currentMode != prevMode) {
        if (currentMode == TIME_LARGE) {
            static bool firstCall_Time = true;
            firstCall_Time = true;
            resetSlideEffect();
        } else if (currentMode == NAVIGATION) {
            static bool firstCall_Navigation = true;
            firstCall_Navigation = true;
            resetSlideEffect();
        } else if (currentMode == NOTIFICATION) {
            static bool firstCall_Notification = true;
            firstCall_Notification = true;
        } else if (currentMode == CALL) {
            static bool firstCall_Call = true;
            firstCall_Call = true;
        }
        needsDisplayUpdate = true;
        prevMode = currentMode;
    }

    // Vẽ theo chế độ
    switch (currentMode) {
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

// Xử lý ngắt nút nhấn
void IRAM_ATTR buttonISR() {
    buttonPressed = true;
}