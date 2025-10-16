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
#define TFT_CS        5
#define TFT_DC        16
#define TFT_RST       17
#define TFT_SCLK      18
#define TFT_MOSI      23
#define BUTTON_PIN    0
#define BUILTINLED    2
#define LED_PIN       4  // Chân LED WS2812
#define LED_COUNT     1  // Số lượng LED

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
SPIClass spiTFT(VSPI);
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
    // Cập nhật trạng thái kết nối
    app.isConnected = state;
    Serial.print("Connection state: ");
    Serial.println(state ? "Connected" : "Disconnected");
    needsDisplayUpdate = true; // Yêu cầu vẽ lại màn hình
}

// Callback khi nhận thông báo
void notificationCallback(Notification notification) {
    // Xử lý thông báo từ ChronosESP32
    String message = notification.message;
    Serial.print("Raw notification.message: ");
    Serial.println(message);
    if (message.startsWith("info: ")) {
        // Lưu văn bản thông tin để cuộn
        app.savedInfoText = message.substring(6);
        Serial.print("Extracted savedInfoText: ");
        Serial.println(app.savedInfoText);
        app.scrollOffset = 0;
        prefs.putString("savedText", app.savedInfoText);
    } else if (message.startsWith("main: ")) {
        // Cập nhật chế độ hiển thị điều hướng
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
        // Lưu thông báo vào mảng 6 dòng
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
    needsDisplayUpdate = true; // Yêu cầu vẽ lại
}

// Callback khi nhận dữ liệu cấu hình (điều hướng)
void configCallback(Config config, uint32_t a, uint32_t b) {
    // Xử lý dữ liệu điều hướng
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
        needsDisplayUpdate = true; // Yêu cầu vẽ lại
    }
}

// Callback khi có cuộc gọi
void ringerCallback(String caller, bool state) {
    // Xử lý trạng thái cuộc gọi
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
    needsDisplayUpdate = true; // Yêu cầu vẽ lại
}

// Chia nhỏ hướng dẫn điều hướng thành 2 dòng
void splitNavDirection() {
    // Tách app.navDirection thành 2 dòng dựa trên navDisplayMode
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
    // Chuyển chuỗi khoảng cách (VD: "1.5 km") sang mét
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
static bool firstSlide = true;
void resetSlideEffect() {
    firstSlide = true;
}
// Hiển thị thời gian và thông tin pin
void displayTime(bool smallFont) {
    // Hiển thị giờ, điện áp pin, pin điện thoại và văn bản cuộn
    String currentTime = watch.getHourZ() + watch.getTime(":%M:%S");

    int centerX = SCREEN_WIDTH / 2; // 120
    int centery = SCREEN_HEIGHT / 2; // 120

    // Biến tĩnh để kiểm soát hiệu ứng trượt (chỉ chạy một lần)
   
    // Hiệu ứng trượt từ trên xuống
    if (firstSlide) {
        int startY = -240; // Bắt đầu từ ngoài màn hình (trên cùng)
        int endY = 0;      // Vị trí kết thúc (để vòng tròn và văn bản ở vị trí mong muốn)
        int steps = 60;    // Số bước trượt (càng nhiều càng mượt)
        int stepSize = (endY - startY) / steps; // Bước di chuyển mỗi lần

        for (int i = 0; i <= steps; i++) {
            int offsetY = startY + i * stepSize; // Tính vị trí y hiện tại

            // Xóa màn hình trước khi vẽ lại
           // myTFT.fillScreen(COLOR_BG);

            // Vẽ vòng tròn với tọa độ y thay đổi
            myTFT.fillCircle(centerX, -200 + offsetY, 240, 0x0680);

            // Vẽ header "TIME" với tọa độ y thay đổi
            myTFT.setTextSize(2);
            myTFT.setTextColor(COLOR_TEXT);
            myTFT.setCursor(centerX - 20, 10 + offsetY);
            myTFT.print("TIME");

            delay(10); // Độ trễ để tạo hiệu ứng mượt mà (10ms mỗi bước)
        }

        firstSlide = false; // Đánh dấu đã hoàn thành hiệu ứng
    }

    // Vẽ toàn bộ giao diện sau khi hiệu ứng hoàn tất
    {
       // myTFT.fillCircle(centerX, -200, 240,0x0680);

    
        // Vẽ  heder time
  //  myTFT.setTextSize(2);
   // myTFT.setTextColor(COLOR_TEXT);
   // myTFT.setCursor(centerX - 20, 10);
   // myTFT.print("TIME");

        // Vẽ thời gian
       // myTFT.fillRect(60, 50, 140, 40, COLOR_BG);
        myTFT.setTextSize(smallFont ? 2 : 3);
        myTFT.setTextColor(0x0680, COLOR_BG);
        myTFT.setCursor(centerX - 70, 50 + 40);
        myTFT.print(currentTime);

        // Vẽ điện áp
      //  myTFT.fillRect(40, 100, 180, 20, COLOR_BG);
        myfont.set_font(vietnamtimes10x2);
        myfont.print(centerX - 80, 100 + 40, "ĐIỆN ÁP: ", COLOR_TEXT, COLOR_BG);
        myfont.print(centerX + 50, 100 + 40 , "V", COLOR_TEXT, COLOR_BG);
        myTFT.setTextColor(COLOR_TEXT, COLOR_BG);
        myTFT.setTextSize(2);
        myTFT.setCursor(centerX - 20, 100 + 40);
        myTFT.print(String(batteryVoltagetime1s, 2));

        // Vẽ pin điện thoại
       // myTFT.fillRect(170, 50, 60, 20, COLOR_BG);
        if (app.isConnected) {
            myTFT.setTextSize(1);
            int x = centerX + 65;
            int y = centery - 80 ;
            myTFT.fillRect(x, y, 18, 8, COLOR_HIGHLIGHT);
            myTFT.fillRect(x - 2, y + 2, 2, 4, COLOR_HIGHLIGHT);
            myTFT.fillRect(x + 1, y + 1, map(watch.getPhoneBattery(), 0, 100, 16, 0), 6, COLOR_BG);
            myTFT.setCursor(x, y + 10);
            myTFT.print(String(watch.getPhoneBattery()) + "%");
        }


        // Vẽ văn bản cuộn
        /*
        if (app.savedInfoText != "") {
            myfont.set_font(vietnamtimes10x2);
            int textWidth = app.savedInfoText.length() * 10;
            int xPos = SCREEN_WIDTH - app.scrollOffset;
            myfont.print(xPos, 150, app.savedInfoText, COLOR_SECONDARY, COLOR_BG);
            app.scrollOffset += 1;
            if (app.scrollOffset > textWidth + 128) {
                app.scrollOffset = 0;
            }
        }*/
    }
}

// Hiển thị thông tin điều hướng
void displayNavigation() {
    // Hiển thị giao diện điều hướng với nền cong và tối ưu chống nhấp nháy
   

    int centerX = SCREEN_WIDTH / 2; // 120
    int centerY = SCREEN_HEIGHT / 2; // 120

    /*
    String distance;              // distance to destination
    String duration;              // time to destination
    String eta;                   // estimated time of arrival (time,date)
    String title;                 // distance to next point or title
    String directions;
    */
    

       
    // Hiệu ứng trượt từ trên xuống
    if (firstSlide) {
        int startY = -240; // Bắt đầu từ ngoài màn hình (trên cùng)
        int endY = 0;      // Vị trí kết thúc (để vòng tròn và văn bản ở vị trí mong muốn)
        int steps = 30;    // Số bước trượt (càng nhiều càng mượt)
        int stepSize = (endY - startY) / steps; // Bước di chuyển mỗi lần

        for (int i = 0; i <= steps; i++) {
            int offsetY = startY + i * stepSize; // Tính vị trí y hiện tại

            // Xóa màn hình trước khi vẽ lại
            //myTFT.fillScreen(COLOR_BG);

            // Vẽ vòng tròn với tọa độ y thay đổi
            myTFT.fillCircle(centerX, -80 + offsetY, 120, 0x041F);

            // Vẽ header "Navigation" với tọa độ y thay đổi
            myfont.set_font(vietnamtimes10x2);
            myfont.print(centerX - 30, 10 + offsetY, "Navigation", 0xFFFF, 0x041F);

            delay(10); // Độ trễ để tạo hiệu ứng mượt mà (10ms mỗi bước)
        }

        firstSlide = false; // Đánh dấu đã hoàn thành hiệu ứng
    }

        //vẽ tên đường
        myfont.set_font(vietnamtimes12); // Font nhỏ
        String direction = app.navDirection; // Giả định "Turn Right"
        myfont.print(20, 60, direction, 0xC618, 0x0000); // "Turn Right" xám nhạt

        // Vẽ 
       // myTFT.fillRect(10, 110, 100, 40, 0x0000); // Nền đen
        myfont.set_font(vietnamtimes10x2); // Font nhỏ
        myfont.print(10, 110, "TAKE TIME", 0xC618, 0x0000); // Xám nhạt
        myfont.set_font(vietnamtimes12); // Font nhỏ
        myfont.print(-60, 135, app.navETA , 0xFFFF, 0x0000); // "1.8 Km" trắng

        // Vẽ TAKE TIME
       // myTFT.fillRect(130, 110, 100, 40, 0x0000); // Nền đen
       myfont.set_font(vietnamtimes10x2); // Font nhỏ
        myfont.print(165, 110, "DISTANCE", 0xC618, 0x0000); // Xám nhạt
        myfont.set_font(vietnamtimes12); // Font nhỏ
        myfont.print(175, 135, app.navDistance, 0xFFFF, 0x0000); // "45 mins" trắng

        // Vẽ mũi tên
      // myTFT.fillRect(centerX - 24, 95, 48, 48, 0x0000); // Nền đen
        if (watch.getNavigation().hasIcon && watch.getNavigation().active) {
            myTFT.drawBitmap(centerX - 24, 100, watch.getNavigation().icon, 48, 48, 0x06FE, 0x0000); // Mũi tên xanh 48,48
        }

        // Vẽ hướng dẫn rẽ

        myTFT.setTextSize(3);
        myTFT.setTextColor(COLOR_TEXT);
        myTFT.setCursor(centerX - 15, 160);
        myTFT.print(app.navTitle);


        
      //  myTFT.fillRect(0, 170, SCREEN_WIDTH, 50, 0x0000); // Nền đen
       //// myfont.set_font(vietnamtimes12); // Font vừa
        
       
        //myfont.print(centerX-10, 190, app.navTitle, 0xFFFF, 0x0000); // "200M" trắng
      

        /**/
        // Thanh tiến trình (chờ xử lý vị trí và logic)
        // myTFT.fillRect(180, 80, 50, 100, 0x0000); // Ví dụ vị trí
        // float meters = convertToMeters(app.navTitle);
        // float mappedValue = constrain(map(meters, 0, 500, 0, 80), 0, 80);
        // myTFT.drawRect(180, 80, 8, 81, 0xFFFF); // Viền trắng
        // myTFT.fillRect(181, 80 + mappedValue, 6, 81 - mappedValue, 0x07FF); // Thanh xanh
        /**/

    
}

// Hiển thị thông báo
void displayNotification() {
    // Hiển thị tối đa 6 dòng thông báo
    static bool firstCall = true;
    static String prevMsg[6] = {"", "", "", "", "", ""};

    // Vẽ toàn bộ lần đầu hoặc khi cần cập nhật lớn
    if (firstCall || needsDisplayUpdate) {
        myTFT.fillRect(20, 30, SCREEN_WIDTH - 40, 180, COLOR_BG);
        myfont.set_font(vietnamtimes8x2);
        for (int i = 0; i < 6; i++) {
            myfont.print(20, 30 + i * 30, app.notificationMsg[i], COLOR_HIGHLIGHT, COLOR_BG);
            prevMsg[i] = app.notificationMsg[i];
        }
        firstCall = false;
        needsDisplayUpdate = false;
    } else {
        // Cập nhật từng dòng
        for (int i = 0; i < 6; i++) {
            if (app.notificationMsg[i] != prevMsg[i]) {
                myTFT.fillRect(20, 30 + i * 30, SCREEN_WIDTH - 40, 20, COLOR_BG);
                myfont.set_font(vietnamtimes8x2);
                myfont.print(20, 30 + i * 30, app.notificationMsg[i], COLOR_HIGHLIGHT, COLOR_BG);
                prevMsg[i] = app.notificationMsg[i];
            }
        }
    }
    myTFT.fillScreen(COLOR_BG);
}

// Hiển thị thông tin cuộc gọi
void displayCall() {
    // Hiển thị "Incoming Call" và tên người gọi
    static bool firstCall = true;
    static String prevCaller = "";

    // Vẽ toàn bộ lần đầu hoặc khi cần cập nhật lớn
    if (firstCall || needsDisplayUpdate) {
        myTFT.fillRect(60, 80, SCREEN_WIDTH - 120, 80, COLOR_BG);
        myfont.set_font(MakeFont_Font1);
        myfont.print(60, 80, "Incoming Call", COLOR_HIGHLIGHT, COLOR_BG);
        myfont.print(60, 120, app.callerName, COLOR_TEXT, COLOR_BG);
        prevCaller = app.callerName;
        firstCall = false;
        needsDisplayUpdate = false;
    } else {
        // Cập nhật tên người gọi
        if (app.callerName != prevCaller) {
            myTFT.fillRect(60, 120, SCREEN_WIDTH - 120, 20, COLOR_BG);
            myfont.set_font(MakeFont_Font1);
            myfont.print(60, 120, app.callerName, COLOR_TEXT, COLOR_BG);
            prevCaller = app.callerName;
        }
    }
}

// Hiển thị logo và thanh tải khi khởi động
void drawLogoWithLoadingBar(unsigned long currentTime) {
    // Hiển thị logo 64x64 và thanh tiến trình loading trong 3 giây
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
                // myTFT.drawRGBBitmap(70, 25,epd_bitmap_logopnt100x100mau, 100, 100);
            }
        }
        
      
       // myTFT.drawRGBBitmap(70, 25,epd_bitmap_logopnt100x100mau, 100, 100);
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
    myTFT.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, COLOR_BAR);
    if (fillWidth < barWidth) {
        myTFT.fillRect(barX + fillWidth + 1, barY + 1, barWidth - fillWidth - 1, barHeight - 2, COLOR_BG);
    }
}

// Đọc điện áp pin với bộ lọc Kalman
float readBatteryVoltage() {
    // Đọc giá trị ADC từ chân A0 và áp dụng bộ lọc Kalman
    const float alpha = 0.1;
    const float R1 = 100000;
    const float R2 = 10000;
    const float ADC_REF_VOLTAGE = 1;
    static float emaValue = 0;
    int rawADC = analogRead(A0);
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
    // Gọi readBatteryVoltage để cập nhật batteryVoltage
    batteryVoltage = readBatteryVoltage();
}

// Cài đặt ban đầu
void setup() {
    // Khởi tạo Serial, chân, SPI, TFT, LED và ChronosESP32
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(BUILTINLED, OUTPUT);

    // Khởi tạo SPI
    spiTFT.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    spiTFT.setFrequency(100000000); // 60 MHz
    spiTFT.setDataMode(SPI_MODE0);
    spiTFT.setBitOrder(MSBFIRST);

    // Khởi tạo TFT
    myTFT.begin();
    myTFT.setSPISpeed(100000000);
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
    // Xử lý ChronosESP32, LED, điện áp và hiển thị theo chế độ
    unsigned long currentTime = millis();

    watch.loop();
    ledStrip.service();
    updateBatteryVoltage();
    if (currentTime - lastBatteryUpdate >= batteryUpdateInterval) {
        batteryVoltagetime1s = batteryVoltage;
        lastBatteryUpdate = currentTime;
        needsDisplayUpdate = true; // Yêu cầu vẽ lại khi điện áp thay đổi
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
        if (currentMode == TIME_LARGE ) {
            static bool firstCall_Time = true;
            firstCall_Time = true;
            resetSlideEffect(); // Đặt lại firstSlide để chạy hiệu ứng trượt
            myTFT.fillScreen(COLOR_BG);
        } else if (currentMode == NAVIGATION) {
            static bool firstCall_Navigation = true;
            firstCall_Navigation = true;
            resetSlideEffect(); // Đặt lại firstSlide để chạy hiệu ứng trượt
            myTFT.fillScreen(COLOR_BG);
        } else if (currentMode == NOTIFICATION) {
            static bool firstCall_Notification = true;
            firstCall_Notification = true;
           
            myTFT.fillScreen(COLOR_BG);
        } else if (currentMode == CALL) {
            static bool firstCall_Call = true;
            firstCall_Call = true;
            
            myTFT.fillScreen(COLOR_BG);
        }
        needsDisplayUpdate = true;
        prevMode = currentMode;
    }

    // Vẽ theo chế độ
    switch (currentMode) {
        
        case TIME_LARGE:
        //myTFT.fillScreen(COLOR_BG);
            displayTime(false);
            break;
        case NAVIGATION:
        //myTFT.fillScreen(COLOR_BG);
            displayNavigation();
            break;
        case NOTIFICATION:
       // myTFT.fillScreen(COLOR_BG);
            displayNotification();
            break;
        case CALL:
        //myTFT.fillScreen(COLOR_BG);
            displayCall();
            break;
    }
}

// Xử lý ngắt nút nhấn
void IRAM_ATTR buttonISR() {
    // Đặt cờ khi nút được nhấn
    buttonPressed = true;
}
