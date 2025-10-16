#include <Wire.h>
#include <Adafruit_GFX.h>
//#include <Adafruit_SH110X.h>
#include <Adafruit_SH110X.h>
#include <ChronosESP32.h>
#include <WS2812FX.h>

#include "FontMaker.h"
#include <Preferences.h>

#define LED_COUNT -1
#define LED_PIN -1//27
#define BUILTINLED -1//2
#define BUTTON_PIN -1//0
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
//esp32
#define OLED_SDA 25
#define OLED_SCL 26

//c3
//#define OLED_SDA 4
//#define OLED_SCL 5




//Adafruit_SH110X myOLED(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
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
    String navDirectionLines[2]; // Thêm dòng này
    String navDistance = "";
    String navETA = "";
    String navTitle = "";
    String notificationMsg[6];
    String callerName = "";
    
    unsigned long lastNavChangeTime = 0;
    bool navChange = false;
    String savedInfoText = ""; // Lưu văn bản từ notification
    int scrollOffset = 0;      // Vị trí cuộn của văn bản
    String lastNavTitle = ""; // Chỉ lưu tiêu đề làm mốc
    int navDisplayMode = 1; // Thêm biến để lưu kiểu hiển thị (1 = map1, 2 = map2)
    
};
AppState app;

// Biến hỗ trợ
ChronosTimer notifyTimer;
volatile bool buttonPressed = false;
Preferences prefs; // Đối tượng để lưu trữ dữ liệu

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

    if (message.startsWith("info; ")) {
        app.savedInfoText = message.substring(6);
        Serial.print("Extracted savedInfoText: ");
        Serial.println(app.savedInfoText);
        app.scrollOffset = 0;
        prefs.putString("savedText", app.savedInfoText);
    } else if (message.startsWith("main; ")) {
        String modeStr = message.substring(6); // Lấy phần sau "mani: "
        if (modeStr == "1") {
            app.navDisplayMode = 1; // Kiểu hiển thị map1
            Serial.println("Set navigation display mode to map1");
        } else if (modeStr == "2") {
            app.navDisplayMode = 2; // Kiểu hiển thị map2
            Serial.println("Set navigation display mode to map2");
        } else if (modeStr == "3") {
            app.navDisplayMode = 3; // Kiểu hiển thị map2
            Serial.println("Set navigation display mode to map2");
        }
        // Lưu vào Preferences để giữ qua các lần khởi động nếu cần
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
        if (a && app.isConnected) { // Kiểm tra kết nối
            Navigation nav = watch.getNavigation();


            
               
                app.navDirection = nav.directions;
                splitNavDirection();
                app.navDistance = nav.distance;
                app.navETA = nav.eta;
                app.navTitle = nav.title;
                app.navChange = true;
                 // Kiểm tra xem tiêu đề có thay đổi không và không rỗng
             if (nav.title != app.lastNavTitle ) {
                
                app.lastNavChangeTime = millis();
                app.lastNavTitle = nav.title;
                //currentMode = NAVIGATION;
            }
                
              
           
        } else {
            currentMode = TIME_LARGE; // Khi không còn điều hướng hoặc không kết nối
            app.navChange = false;
        }
    }
}

void splitNavDirection() {
    String direction = app.navDirection;
    String lines[2]; // Mảng lưu tối đa 2 dòng cho h=24
     if (app.navDisplayMode == 1) {
    // Chia thành 2 dòng, mỗi dòng tối đa 6 ký tự=
    if (direction.length() <= 10) {
        lines[0] = direction; // Nếu chuỗi ngắn, chỉ cần 1 dòng
        lines[1] = "";
    } else {
        lines[0] = direction.substring(0, 12); // 6 ký tự đầu
        lines[1] = direction.substring(12, 24); // 6 ký tự tiếp theo (hoặc ít hơn)
        // Nếu chuỗi dài hơn 12, phần còn lại sẽ bị cắt bỏ để vừa khung
    }
    } else if (app.navDisplayMode == 2) {

      
            // Chia thành 2 dòng, mỗi dòng tối đa 6 ký tự=
            if (direction.length() <= 10) {
                lines[0] = direction; // Nếu chuỗi ngắn, chỉ cần 1 dòng
                lines[1] = "";
            }  else {
                lines[0] = direction.substring(0, 22); // 6 ký tự đầu
                lines[1] = direction.substring(22, 44); // 6 ký tự tiếp theo (hoặc ít hơn)
                // Nếu chuỗi dài hơn 12, phần còn lại sẽ bị cắt bỏ để vừa khung
                }
        }
    
    // Lưu vào biến toàn cục hoặc sử dụng trực tiếp trong hàm hiển thị
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
    //myOLED.setTextSize(smallFont ? 1 : 2);
    myOLED.setTextSize(2);
    myOLED.setTextColor(SH110X_WHITE);
    myOLED.setCursor(0, 0);
    myOLED.print(watch.getHourZ() + watch.getTime(":%M:%S"));

   // if (!smallFont) {
        float batteryVoltage = readBatteryVoltage();
        myfont.set_font(vietnamtimes10x2);
        myOLED.fillRect(0, 21, 128, 16, SH110X_BLACK);
        myfont.print(0, 25, "ĐIỆN ÁP: ", SH110X_WHITE, SH110X_BLACK);
        myfont.print(112, 25, "V", SH110X_WHITE, SH110X_BLACK);
        myOLED.setTextSize(2);
        myOLED.setCursor(53, 25);
        myOLED.print(String(batteryVoltage, 2));

        // Hiển thị văn bản cuộn bên dưới "ĐIỆN ÁP: "
        if (app.savedInfoText != "") {
            myfont.set_font(vietnamtimes10x2); // Font nhỏ hơn để cuộn
            int textWidth = app.savedInfoText.length() * 10; // Ước tính chiều rộng (8px/ký tự)
            int xPos = 128 - app.scrollOffset; // Bắt đầu từ phải, cuộn sang phải
            myfont.print(xPos, 45, app.savedInfoText, SH110X_WHITE, SH110X_BLACK);
            
            // Cập nhật vị trí cuộn
            app.scrollOffset += 1; // Tốc độ cuộn (2 pixel mỗi lần)
            if (app.scrollOffset > textWidth + 128) {
                app.scrollOffset = 0; // Reset khi cuộn hết
            }
        }
   // }
    if (app.isConnected) {
       /*
        myOLED.setTextSize(1);
        int x = smallFont ? 3 : 110;
        int y = smallFont ? 11 : 0;
        myOLED.fillRect(x, y, 18, 8, SH110X_WHITE);
        myOLED.fillRect(x - 2, y + 2, 2, 4, SH110X_WHITE);
        myOLED.fillRect(x + 1, y + 1, map(watch.getPhoneBattery(), 0, 100, 16, 0), 6, SH110X_BLACK);
        myOLED.setCursor(smallFont ? 22 : 110, smallFont ? 11 : 11);
        myOLED.print(String(watch.getPhoneBattery()) + "%");

        */

        myOLED.setTextSize(1);
        int x = 110;
        int y = 0;
        myOLED.fillRect(x, y, 18, 8, SH110X_WHITE);
        myOLED.fillRect(x - 2, y + 2, 2, 4, SH110X_WHITE);
        myOLED.fillRect(x + 1, y + 1, map(watch.getPhoneBattery(), 0, 100, 16, 0), 6, SH110X_BLACK);
        myOLED.setCursor(110,11);
        myOLED.print(String(watch.getPhoneBattery()) + "%");
        
    }
    myOLED.display();
}

void displayNavigation() {
    if (app.navDisplayMode == 1) {
        // Kiểu hiển thị "map1"
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
        // Kiểu hiển thị "map2"
         
         myOLED.clearDisplay();

         if (watch.getNavigation().hasIcon && watch.getNavigation().active) {
             myOLED.drawBitmap(50+20, 0, watch.getNavigation().icon, 48, 48, SH110X_WHITE);
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
         myOLED.drawRect(100+20, 0, 8, 51, SH110X_WHITE);
         myOLED.fillRect(101+20, 1, 6, 50, SH110X_BLACK);
         myOLED.fillRect(100+20, mappedValue, 8, 51 - mappedValue, SH110X_WHITE);
         
 
         myOLED.display();

    } else if (app.navDisplayMode == 3) {
         // Kiểu hiển thị "map3"

         myOLED.clearDisplay();

         if (watch.getNavigation().hasIcon && watch.getNavigation().active) {
             myOLED.drawBitmap(50, 0, watch.getNavigation().icon, 48, 48, SH110X_WHITE);
         }
 
         myfont.set_font(vietnamtimes8x2);
         myfont.print(0, 25, ": " + app.navTitle, SH110X_WHITE, SH110X_BLACK);
         myfont.print(0, 40, "KC: " + app.navDistance, SH110X_WHITE, SH110X_BLACK);
         myfont.print(0, 53, "TG: " + app.navETA, SH110X_WHITE, SH110X_BLACK);
 
         //myfont.set_font(vietnamtimes7x2r);
         //myOLED.setTextSize(1);
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
        myOLED.setCursor(22,15);
        myOLED.print(String(watch.getPhoneBattery()) + "%");

         /*myfont.print(0, 0, app.navDirectionLines[0], SH110X_WHITE, SH110X_BLACK);
         if (app.navDirectionLines[1] != "") {
             myfont.print(0, 12, app.navDirectionLines[1], SH110X_WHITE, SH110X_BLACK);
         }*/
 
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
   // if (!myOLED.begin(SH110X_SWITCHCAPVCC, 0x3C)) {
     //   Serial.println(F("SH110X allocation failed"));
     //   while (1);
   // }
    if (!myOLED.begin(0x3C, true)) { // Có thể là 0x3C hoặc 0x78
        Serial.println(F("SH1106 not found!"));
        while (1);
    }
    
    // Khởi tạo Preferences
    prefs.begin("myApp", false); // "myApp" là tên namespace
    app.savedInfoText = prefs.getString("savedText", ""); // Đọc văn bản đã lưu, mặc định rỗng
    app.navDisplayMode = prefs.getInt("navMode", 1); // Mặc định là 1 (map1) nếu chưa lưu
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
  // Kiểm tra hết thời gian hiển thị thông báo
  if (app.hasNotification && (millis() - notifyTimer.time >= 5000)) {
    app.hasNotification = false;
    ws2812fx.setBrightness(0);
}








    // Xử lý trạng thái
    if (app.isCalling) {
        currentMode = CALL;
    } else if (app.hasNotification && (millis() - notifyTimer.time < 5000)) {
        currentMode = NOTIFICATION;
    } else if (app.isNavigating) {
        currentMode = NAVIGATION;
    } 
     /*else {
        currentMode = app.navChange ?  TIME_SMALL  : TIME_LARGE ;
    }*/
    else {
        currentMode = TIME_LARGE ;
    }
// Kiểm tra hết thời gian thay đổi điều hướng
if (app.navChange && (millis() - app.lastNavChangeTime >= 30000)) {
    
    currentMode = TIME_LARGE; // chuyển sang TIME_SMALL sau 30 giây không có dữ liệu mới

    } 


    // Hiển thị theo trạng thái
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
// Định nghĩa các biến toàn cục cho Kalman Filter
float kalmanX = 12.0;       // Giá trị ước lượng ban đầu (12V)
float kalmanP = 1.0;        // Độ không chắc chắn ban đầu
float kalmanQ = 0.05;       // Process noise (tăng để phản ứng nhanh hơn)
float kalmanR = 0.1;        // Measurement noise (giảm để tin tưởng hơn vào phép đo)

// Hàm đọc điện áp pin kết hợp EMA và Kalman
float readBatteryVoltage() {
    // Các hằng số
    const float alpha = 0.1;         // Hằng số EMA
    const float R1 = 100000;         // Điện trở R1 trong mạch phân áp
    const float R2 = 10000;          // Điện trở R2 trong mạch phân áp
    const float ADC_REF_VOLTAGE = 1; // Điện áp tham chiếu của ADC
    
    // EMA Filter
    static float emaValue = 0;       // Giá trị EMA tĩnh
    int rawADC = analogRead(A0);     // Đọc giá trị ADC thô
    emaValue = alpha * rawADC + (1 - alpha) * emaValue; // Lọc EMA
    float measuredVoltage = (emaValue / 1023.0) * ADC_REF_VOLTAGE * ((R1 + R2) / R2); // Chuyển đổi sang điện áp
    
    // Kalman Filter
    // Bước 1: Dự đoán
    float x_pred = kalmanX;          // Giá trị dự đoán bằng giá trị trước đó
    float P_pred = kalmanP + kalmanQ; // Độ không chắc chắn dự đoán
    
    // Bước 2: Cập nhật
    float K = P_pred / (P_pred + kalmanR);    // Kalman Gain
    kalmanX = x_pred + K * (measuredVoltage - x_pred); // Cập nhật giá trị ước lượng
    kalmanP = (1 - K) * P_pred;          // Cập nhật độ không chắc chắn
    
    // Giới hạn trong 10V - 14V (dựa trên acquy xe máy của bạn)
    
    
    return kalmanX;
}

void IRAM_ATTR buttonISR() {
    buttonPressed = true;
}