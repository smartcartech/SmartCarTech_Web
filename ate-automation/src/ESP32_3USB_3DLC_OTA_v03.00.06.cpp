// ============================================================
// NOTE / LOGIC - WIFI UPDATE
// 1) BOOT: chỉ thử kết nối Wi-Fi đã lưu trong 10 giây.
//    Nếu thất bại -> chạy Offline. KHÔNG tự phát AP cấu hình.
// 2) Chỉ khi user vào Function -> Wifi Setting:
//    ESP32 mới phát AP "ATE_Setup_WiFi", IP 192.168.4.1.
// 3) Wifi Setting KHÔNG timeout; portal chạy non-blocking.
// 4) Trong Wifi Setting, nhấn OK -> hỏi "Exit WiFi Setting?".
//    Dùng DOWN chọn No/Yes, nhấn OK để xác nhận.
//    Mặc định chọn No để tránh thoát nhầm.
// 5) Khi user cấu hình Wi-Fi mới thành công -> hiện "WiFi Updated!"
//    và ESP32 tự reboot.
// 6) Khi phone mở http://192.168.4.1, portal tự chuyển thẳng đến
//    http://192.168.4.1/wifi để hiện list Wi-Fi; bỏ qua trang Home
//    có nút "Configure WiFi" (tránh browser tự đổi link sang HTTPS).
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>        
#include <EEPROM.h>            
#include <Adafruit_MCP23X17.h> 
#include "USB.h" 

// ==========================================
// THƯ VIỆN & CẤU HÌNH WIFI / LOCAL OTA / HTTPS OTA
// ==========================================
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>         // Cho Local OTA
#include <WiFiClientSecure.h>   // Cho HTTPS OTA
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>        // Đọc file version.json
#include <WiFiManager.h>        // THƯ VIỆN WIFIMANAGER

// Thông số cho HTTPS OTA (Kiểm tra phiên bản)
// VERSION_NAME: chỉ dùng để hiển thị cho người dùng.
// VERSION_CODE: dùng để so sánh OTA, tuyệt đối không dùng float.
//
// Quy ước VERSION_NAME -> VERSION_CODE:
// Ví dụ:
//   02.01.01  -> 20101
//   02.01.02  -> 20102
//   10.08.01  -> 100801

const char* CURRENT_VERSION = "03.00.06";
const uint32_t CURRENT_VERSION_CODE = 30006;

const char* version_url = "https://smartcartech.vn/ate-automation/firmware/version.json"; 
const char* base_bin_url = "https://smartcartech.vn/ate-automation/firmware/";

// Tự tay khai báo cổng USB Serial
USBCDC USBSerial;

// Khởi tạo LCD 20x4 (Địa chỉ 0x27)
LiquidCrystal_I2C lcd(0x27, 20, 4); 

// Khởi tạo chip mở rộng MCP23017 
Adafruit_MCP23X17 mcp;

// ==========================================
// CẤU HÌNH ĐA TRẠM (3 CẶP USB - DLC)
// ==========================================
Servo usbServos[3];
const int USB_SERVO_PINS[3] = {47, 21, 14}; 
const int DLC_RELAY_PINS[3] = {11, 12, 13};
int currentPair = 0; 

// ==========================================
// ĐỊNH NGHĨA CHÂN GIAO TIẾP VÀ NÚT NHẤN
// ==========================================
#define I2C_SDA_PIN 4  
#define I2C_SCL_PIN 5  

const int BTN_OK    = 1;  
const int BTN_DOWN  = 2;  
const int BTN_PLUS  = 42; 
const int BTN_MINUS = 41; 
const int BTN_RUN   = 40; 

// ==========================================
// BIẾN CHO CHƯƠNG TRÌNH CHÍNH 
// ==========================================
int usbAngleA[3]; 
int usbAngleB[3]; 
int delayDisconnect; 
int delayConnect; 

const int addrUsbA[3] = {0, 8, 16};
const int addrUsbB[3] = {4, 12, 20};
const int addrTimeD   = 24; 
const int addrTimeC   = 28; 

int currentScreen = 1; 
int cursorIndex = 0; 
bool isRunning = false;
bool isRunManually = false; 
unsigned long previousMillis = 0;
int runStep = 6; // Mặc định chạy ở Step 6 (Chờ PC Command)

bool lastOk = HIGH, lastDown = HIGH, lastPlus = HIGH, lastMinus = HIGH, lastRun = HIGH;

// ==========================================
// BIẾN CHO WIFI SETTING (CONFIG PORTAL KHÔNG BLOCKING)
// ==========================================
// WiFiManager chỉ mở AP cấu hình khi user chủ động vào "Wifi Setting".
// Khi BOOT thất bại, ESP32 KHÔNG tự phát AP cấu hình.
WiFiManager wifiManager;
bool wifiPortalActive = false;
bool wifiExitConfirm = false;
int wifiExitSelection = 0; // 0 = No (mặc định an toàn), 1 = Yes

// JavaScript được WiFiManager chèn vào <head> của portal.
// Chỉ redirect khi đang ở trang root "/"; trang /wifi sẽ không redirect tiếp,
// vì vậy không tạo vòng lặp. Dùng URL tuyệt đối HTTP để tránh browser
// tự nâng link Configure WiFi thành HTTPS.
const char WIFI_PORTAL_DIRECT_WIFI_PAGE[] =
  "<script>"
  "if(window.location.pathname==='/' ){"
  "window.location.replace('http://192.168.4.1/wifi');"
  "}"
  "</script>";

const unsigned long WIFI_BOOT_CONNECT_TIMEOUT_MS = 10000UL; // Chỉ chờ Wi-Fi cũ 10 giây khi BOOT

// Khai báo trước hàm
void printCentered(int row, String text);
void drawScreen();
void handleRunSequence();
void handleSerialCommands();
void trigger11Keys();
void detachServoSafe(int index);
void safeStopAll(); 
void updateFirmwareFromInternet(); // Hàm HTTPS OTA
void setupArduinoOTA();
void startWifiSettingPortal();
void stopWifiSettingPortalAndReturn();
void drawWifiSettingScreen();
void drawWifiExitConfirmScreen();

void setup() {
  USB.VID(0x1720); 
  USB.PID(0xAE01); 
  USB.productName("ATE Test System"); 
  USB.manufacturerName("INNOVA Automation");
  USB.firmwareVersion(0x100); 
  
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  for (int i = 0; i < 3; i++) {
    usbServos[i].setPeriodHertz(50); 
  }

  USBSerial.begin(115200); 
  USB.begin();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();

  if (!mcp.begin_I2C(0x20)) {
    lcd.setCursor(0, 0); lcd.print("Loi Phan Cung!");
    while (1); 
  }
  
  for (int i = 0; i < 16; i++) {
    mcp.pinMode(i, OUTPUT);
    mcp.digitalWrite(i, HIGH); 
  }

  EEPROM.begin(64);
  for (int i = 0; i < 3; i++) {
    EEPROM.get(addrUsbA[i], usbAngleA[i]); 
    EEPROM.get(addrUsbB[i], usbAngleB[i]);
    if(usbAngleA[i] < 0 || usbAngleA[i] > 180) usbAngleA[i] = 20; 
    if(usbAngleB[i] < 0 || usbAngleB[i] > 180) usbAngleB[i] = 40; 
  }

  EEPROM.get(addrTimeD, delayDisconnect);
  EEPROM.get(addrTimeC, delayConnect); 
  if(delayDisconnect < 0 || delayDisconnect > 1000) delayDisconnect = 3; 
  if(delayConnect < 8 || delayConnect > 1000) delayConnect = 8; 

  pinMode(BTN_OK, INPUT_PULLUP); 
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_PLUS, INPUT_PULLUP); 
  pinMode(BTN_MINUS, INPUT_PULLUP);
  pinMode(BTN_RUN, INPUT_PULLUP);

  // ==========================================
  // MÀN HÌNH KHỞI ĐỘNG (CÓ HIỂN THỊ VERSION)
  // ==========================================
  lcd.setCursor(6, 1); lcd.print("INNOVA");
  delay(1000); 
  
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(" INNOVA Automation");
  
  // Hiển thị Firmware Version ra giữa màn hình
  lcd.setCursor(1, 2); 
  lcd.print("Firmware V"); 
  lcd.print(CURRENT_VERSION);

  lcd.setCursor(0, 3); lcd.print("- Design by TuanLe -");
  delay(3000); 

  // ==========================================================
  // WIFI KHI BOOT
  // - Chỉ thử kết nối Wi-Fi đã lưu.
  // - Nếu không kết nối được: chạy Offline.
  // - KHÔNG tự phát AP "ATE_Setup_WiFi" khi BOOT.
  // - Muốn đổi/cấu hình Wi-Fi: user vào Function -> Wifi Setting.
  // ==========================================================
  lcd.clear();
  printCentered(0, "WIFI CONNECTION");
  lcd.setCursor(0, 1); lcd.print("Connecting...");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(); // Dùng SSID/password đã lưu trong NVS của ESP32

  unsigned long wifiStartMillis = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStartMillis < WIFI_BOOT_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    printCentered(0, "WIFI CONNECTED!");
    lcd.setCursor(0, 1); lcd.print("IP:");
    lcd.print(WiFi.localIP());
    delay(1500);

    setupArduinoOTA();
    updateFirmwareFromInternet();

  } else {
    lcd.clear();
    printCentered(1, "WIFI FAILED!");
    lcd.setCursor(0, 2); lcd.print("Running Offline Mode");
    delay(2000);
  }

  // KHỞI ĐỘNG VÀO CHỜ PC MODE
  isRunning = true;         
  isRunManually = false;    
  currentPair = 0;
  runStep = 6;              
  
  lcd.clear();
  printCentered(0, "SYSTEM RUN (PC)");
  lcd.setCursor(0, 1); lcd.print("Tool: 1");
  lcd.setCursor(0, 2); lcd.print("Waiting PC Command..");
  lcd.setCursor(0, 3); lcd.print("Press RUN to Stop");

  currentScreen = 2; 
}

void loop() {
  // WiFiManager portal chạy NON-BLOCKING để vẫn đọc được button OK/DOWN.
  if (wifiPortalActive) {
    wifiManager.process();

    // Vì đã chủ động disconnect Wi-Fi cũ trước khi mở portal,
    // WL_CONNECTED ở đây nghĩa là user đã chọn Wi-Fi và kết nối thành công.
    if (!wifiExitConfirm && WiFi.status() == WL_CONNECTED) {
      wifiPortalActive = false;
      wifiManager.stopConfigPortal();

      lcd.clear();
      printCentered(1, "WiFi Updated!");
      lcd.setCursor(0, 2); lcd.print("Rebooting...");
      delay(2000);
      ESP.restart();
    }
  }

  if (WiFi.status() == WL_CONNECTED && !wifiPortalActive) {
    ArduinoOTA.handle(); 
  }

  handleSerialCommands();

  bool ok    = digitalRead(BTN_OK);
  bool down  = digitalRead(BTN_DOWN);
  bool plus  = digitalRead(BTN_PLUS);
  bool minus = digitalRead(BTN_MINUS);
  bool run   = digitalRead(BTN_RUN);

  if (!wifiPortalActive && run == LOW && lastRun == HIGH) {
    if (isRunning) {
      safeStopAll(); 
      isRunning = false; 
      currentScreen = 2;
      cursorIndex = 0;
      drawScreen(); 
    } else {
      isRunning = true; 
      isRunManually = false; 
      currentPair = 0; 
      runStep = 6; 
      lcd.clear();
      printCentered(0, "SYSTEM RUN (PC)");
      lcd.setCursor(0, 1); lcd.print("Tool: 1");
      lcd.setCursor(0, 2); lcd.print("Waiting PC Command..");
      lcd.setCursor(0, 3); lcd.print("Press RUN to Stop");
    }
    delay(150); 
  }

  if (isRunning) { handleRunSequence(); goto UPDATE_STATE; }

  // THUẬT TOÁN CUỘN TRANG CẬP NHẬT CHO 5 MỤC MENU
  if (down == LOW && lastDown == HIGH) {
    if (wifiPortalActive && wifiExitConfirm) {
      // Ở màn hình xác nhận thoát WiFi Setting:
      // DOWN dùng để đổi giữa No <-> Yes.
      wifiExitSelection = (wifiExitSelection + 1) % 2;
      drawWifiExitConfirmScreen();
    }
    else if (currentScreen == 2) {
      cursorIndex = (cursorIndex + 1) % 5; 
      drawScreen();
    }
    else if (currentScreen >= 4 && currentScreen <= 6) {
      cursorIndex = (cursorIndex + 1) % 2; 
      drawScreen();
    }
    delay(150);
  }

  if (ok == LOW && lastOk == HIGH) {
    // ======================================================
    // WIFI SETTING: OK -> hỏi có muốn thoát hay không
    // ======================================================
    if (wifiPortalActive) {
      if (!wifiExitConfirm) {
        wifiExitConfirm = true;
        wifiExitSelection = 0; // Mặc định = No để tránh thoát nhầm.
        drawWifiExitConfirmScreen();
      } else {
        if (wifiExitSelection == 1) {
          // Yes -> đóng AP cấu hình và quay lại Function.
          stopWifiSettingPortalAndReturn();
        } else {
          // No -> quay lại màn hình WiFi Setting, portal vẫn tiếp tục chạy.
          wifiExitConfirm = false;
          drawWifiSettingScreen();
        }
      }

      delay(150);
      goto UPDATE_STATE;
    }

    if (currentScreen == 2) {
      if (cursorIndex == 0) {
        isRunning = true;
        isRunManually = true; 
        currentPair = 0;
        
        runStep = 2; 
        previousMillis = millis(); 
        
        lcd.clear();
        printCentered(0, "SYSTEM RUN (AUTO)");
        lcd.setCursor(0, 1); lcd.print("Tool: 1");
        lcd.setCursor(0, 2); lcd.print("Wait Disconnect...  ");
        lcd.setCursor(0, 3); lcd.print("Press RUN to Stop");
        delay(150);
        goto UPDATE_STATE; 
      }
      else if (cursorIndex == 1) { currentScreen = 4; cursorIndex = 0; } 
      else if (cursorIndex == 2) { currentScreen = 9; cursorIndex = 0; } 
      else if (cursorIndex == 3) { currentScreen = 8; cursorIndex = 0; } 
      else if (cursorIndex == 4) { 
        // =======================================
        // CHỨC NĂNG ĐỔI WIFI TỪ MENU
        // =======================================
        // Không timeout. Portal chạy non-blocking để button OK/DOWN vẫn hoạt động.
        startWifiSettingPortal();
        delay(150);
        goto UPDATE_STATE;
      }
    } 
    else if (currentScreen == 4) { currentScreen = 5; cursorIndex = 0; }
    else if (currentScreen == 5) { currentScreen = 6; cursorIndex = 0; }
    else if (currentScreen == 6 || currentScreen == 8 || currentScreen == 9) {
       for (int i = 0; i < 3; i++) {
         EEPROM.put(addrUsbA[i], usbAngleA[i]); 
         EEPROM.put(addrUsbB[i], usbAngleB[i]);
       }
       EEPROM.put(addrTimeD, delayDisconnect);
       EEPROM.put(addrTimeC, delayConnect);
       EEPROM.commit(); 
       currentScreen = 2; cursorIndex = 0; 
    }
    drawScreen(); delay(150);
  }

  if (plus == LOW && lastPlus == HIGH) {
    if (currentScreen >= 4 && currentScreen <= 6) {
      int t = currentScreen - 4; 
      if (cursorIndex == 0) usbAngleA[t] = min(180, usbAngleA[t] + 1); 
      else usbAngleB[t] = min(180, usbAngleB[t] + 1); 
      drawScreen();
    } 
    else if (currentScreen == 8) { delayDisconnect++; drawScreen(); }
    else if (currentScreen == 9) { delayConnect++; drawScreen(); }
    delay(100);
  }

  if (minus == LOW && lastMinus == HIGH) {
    if (currentScreen >= 4 && currentScreen <= 6) {
      int t = currentScreen - 4; 
      if (cursorIndex == 0) usbAngleA[t] = max(0, usbAngleA[t] - 1); 
      else usbAngleB[t] = max(0, usbAngleB[t] - 1); 
      drawScreen();
    } 
    else if (currentScreen == 8) { delayDisconnect = max(0, delayDisconnect - 1); drawScreen(); }
    else if (currentScreen == 9) { delayConnect = max(8, delayConnect - 1); drawScreen(); }
    delay(100);
  }

UPDATE_STATE:
  lastOk = ok; lastDown = down; lastPlus = plus; lastMinus = minus; lastRun = run;
}

// ==========================================
// LOCAL OTA SETUP
// ==========================================
void setupArduinoOTA() {
  ArduinoOTA.setHostname("ATE-Tool-System");

  ArduinoOTA.onStart([]() {
    lcd.clear();
    printCentered(1, "LOCAL OTA UPDATING..");
  });

  ArduinoOTA.onEnd([]() {
    lcd.clear();
    printCentered(1, "UPDATE SUCCESS!");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    lcd.setCursor(0, 2);
    lcd.print("Progress: ");
    if (total > 0) {
      lcd.print((progress * 100U) / total);
    } else {
      lcd.print(0);
    }
    lcd.print("%   ");
  });

  ArduinoOTA.begin();
  delay(1500);
}

// ==========================================
// WIFI SETTING - MỞ CONFIG PORTAL KHÔNG TIMEOUT
// ==========================================
void startWifiSettingPortal() {
  isRunning = false;
  isRunManually = false;

  // Không cho Wi-Fi cũ tự reconnect trong lúc user đang cấu hình,
  // nếu không code có thể hiểu nhầm Wi-Fi cũ là "WiFi Updated".
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(200);

  // Config Portal NON-BLOCKING:
  // - Không setConfigPortalTimeout() => không timeout.
  // - loop() gọi wifiManager.process().
  // - Nhờ vậy vẫn đọc được button OK/DOWN trên thiết bị.
  wifiManager.setConfigPortalBlocking(false);

  // Khi user mở 192.168.4.1, tự chuyển thẳng sang trang /wifi.
  // Đây là workaround cho trường hợp browser trên phone đổi nút
  // "Configure WiFi" thành https://192.168.4.1/wifi và gây lỗi.
  wifiManager.setCustomHeadElement(WIFI_PORTAL_DIRECT_WIFI_PAGE);

  wifiManager.startConfigPortal("ATE_Setup_WiFi");

  wifiPortalActive = true;
  wifiExitConfirm = false;
  wifiExitSelection = 0;

  drawWifiSettingScreen();
}

// ==========================================
// WIFI SETTING - USER CHỌN YES ĐỂ THOÁT
// ==========================================
void stopWifiSettingPortalAndReturn() {
  wifiManager.stopConfigPortal();
  wifiPortalActive = false;
  wifiExitConfirm = false;
  wifiExitSelection = 0;

  // Quay về STA mode và thử reconnect Wi-Fi đã lưu.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin();

  currentScreen = 2;
  cursorIndex = 4; // Quay lại đúng mục "Wifi Setting".
  drawScreen();
}

// ==========================================
// MÀN HÌNH WIFI SETTING
// ==========================================
void drawWifiSettingScreen() {
  lcd.clear();
  printCentered(0, "WIFI SETTING");
  lcd.setCursor(0, 1); lcd.print("AP: ATE_Setup_WiFi");
  lcd.setCursor(0, 2); lcd.print("Open: 192.168.4.1");
  lcd.setCursor(0, 3); lcd.print("OK: Exit");
}

// ==========================================
// MÀN HÌNH XÁC NHẬN THOÁT WIFI SETTING
// ==========================================
void drawWifiExitConfirmScreen() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Exit WiFi Setting?");

  lcd.setCursor(0, 1);
  if (wifiExitSelection == 0) lcd.print("> No");
  else                        lcd.print("  No");

  lcd.setCursor(0, 2);
  if (wifiExitSelection == 1) lcd.print("> Yes");
  else                        lcd.print("  Yes");

  lcd.setCursor(0, 3); lcd.print("DOWN Select OK Enter");
}

// ==========================================
// TỪ ĐIỂN COMMAND TỪ SERIAL PC (COM PORT)
// ==========================================
void handleSerialCommands() {
  while (USBSerial.available()) {
    static byte cmdBuffer[4];
    static int cmdIndex = 0;
    byte incomingByte = USBSerial.read();

    if (incomingByte == 0xAE) {
      cmdIndex = 0; 
    }
    cmdBuffer[cmdIndex++] = incomingByte;

    if (cmdIndex == 4) {
      byte ack[] = {0xAA, cmdBuffer[1], cmdBuffer[2], cmdBuffer[3]};
      USBSerial.write(ack, 4); 
      USBSerial.flush(); 

      if (isRunning && runStep == 6 && !isRunManually) {
        
        if (cmdBuffer[1] == 0x00 && cmdBuffer[2] == 0x01) {
          if (cmdBuffer[3] == 0xFF) {
            lcd.setCursor(0, 2); lcd.print("Testing All Keys... ");
            trigger11Keys(); 
            lcd.setCursor(0, 2); lcd.print("Waiting PC Command..");
          } 
          else {
            int pin = -1;
            if (cmdBuffer[3] >= 0x01 && cmdBuffer[3] <= 0x09) { pin = cmdBuffer[3] - 1; } 
            else if (cmdBuffer[3] == 0x10) { pin = 9; }  
            else if (cmdBuffer[3] == 0x11) { pin = 10; } 

            if (pin != -1) {
              int keyNum = pin + 1;
              lcd.setCursor(0, 2); lcd.print("Testing Key "); 
              if (keyNum < 10) lcd.print(" "); lcd.print(keyNum); lcd.print("...   ");
              mcp.digitalWrite(pin, LOW); delay(200);
              mcp.digitalWrite(pin, HIGH); delay(200);
              lcd.setCursor(0, 2); lcd.print("Waiting PC Command..");
            }
          }
        }
        
        else if (cmdBuffer[1] == 0x00 && cmdBuffer[2] == 0x04) {
          if (cmdBuffer[3] == 0x02) {
            runStep = 7; 
            lcd.setCursor(0, 2); lcd.print("Changing Tool...");
          }
          else if (cmdBuffer[3] == 0x00) {
            safeStopAll();
            lcd.clear();
            printCentered(0, "SYSTEM RUN (PC)");
            lcd.setCursor(0, 1); lcd.print("Tool: "); lcd.print(currentPair + 1); 
            lcd.setCursor(0, 2); lcd.print("Waiting PC Command..");
            lcd.setCursor(0, 3); lcd.print("Press RUN to Stop");
            runStep = 6; 
          }
        }

        else if (cmdBuffer[1] >= 0x01 && cmdBuffer[1] <= 0x03) {
          int toolIndex = cmdBuffer[1] - 1; 
          int toolNum = cmdBuffer[1];       
          
          if (cmdBuffer[2] == 0x02) {
            if (cmdBuffer[3] == 0x00) { 
              lcd.setCursor(0, 2); lcd.print("Disconnect USB "); lcd.print(toolNum); lcd.print("... ");
              usbServos[toolIndex].attach(USB_SERVO_PINS[toolIndex]); 
              usbServos[toolIndex].write(usbAngleA[toolIndex]);             
              delay(600); 
              detachServoSafe(toolIndex);
              lcd.setCursor(0, 2); lcd.print("Waiting PC Command..");
            } 
            else if (cmdBuffer[3] == 0x01) { 
              lcd.setCursor(0, 2); lcd.print("Connect USB "); lcd.print(toolNum); lcd.print("...    ");
              usbServos[toolIndex].attach(USB_SERVO_PINS[toolIndex]); 
              usbServos[toolIndex].write(usbAngleB[toolIndex]);             
              delay(600); 
              detachServoSafe(toolIndex);
              lcd.setCursor(0, 2); lcd.print("Waiting PC Command..");
            }
          }
          else if (cmdBuffer[2] == 0x03) {
            if (cmdBuffer[3] == 0x00) { 
              lcd.setCursor(0, 2); lcd.print("Disconnect DLC "); lcd.print(toolNum); lcd.print("... ");
              mcp.digitalWrite(DLC_RELAY_PINS[toolIndex], HIGH); 
              delay(200);
              lcd.setCursor(0, 2); lcd.print("Waiting PC Command..");
            } 
            else if (cmdBuffer[3] == 0x01) { 
              lcd.setCursor(0, 2); lcd.print("Connect DLC "); lcd.print(toolNum); lcd.print("...    ");
              mcp.digitalWrite(DLC_RELAY_PINS[toolIndex], LOW); 
              delay(200);
              lcd.setCursor(0, 2); lcd.print("Waiting PC Command..");
            }
          }
        }
      }
      cmdIndex = 0; 
    }
  }
}

// ==========================================
// HÀM XỬ LÝ HTTPS OTA TỪ INTERNET (TỰ ĐỘNG GỌI LÚC BOOT)
// ==========================================
void updateFirmwareFromInternet() {
  lcd.clear();
  lcd.setCursor(0, 1); 
  lcd.print("Checking OTA...");

  if (WiFi.status() != WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0, 1); 
    lcd.print("No WiFi Connection!");
    delay(1500);
    return;
  }


  WiFiClientSecure client;
  client.setInsecure(); // Bỏ qua kiểm tra chứng chỉ SSL

  HTTPClient http;
  http.begin(client, version_url);

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    lcd.clear();
    lcd.setCursor(0, 1); 
    lcd.print("Server Error: ");
    lcd.print(httpCode);
    delay(1500);
    http.end();
    return;
  }

  String payload = http.getString();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    lcd.clear();
    lcd.setCursor(0, 1); 
    lcd.print("JSON Error!");
    delay(1500);
    http.end();
    return;
  }

  // Đọc version từ JSON:
  // {
  //   "version": "03.00.01",
  //   "version_code": 30001,
  //   "bin_file": "ESP32_3USB_3DLC_OTA_v03.00.01.bin"
  // }
  const char* new_version = doc["version"] | "";
  uint32_t new_version_code = doc["version_code"] | 0;
  const char* bin_file_name = doc["bin_file"] | "";

  // Kiểm tra JSON có đủ dữ liệu bắt buộc hay không
  if (new_version_code == 0 || strlen(new_version) == 0 || strlen(bin_file_name) == 0) {
    lcd.clear();
    lcd.setCursor(0, 0); 
    lcd.print("Invalid version.json");
    lcd.setCursor(0, 1); 
    lcd.print("Missing data!");
    delay(2000);
    http.end();
    return;
  }

  // Debug ra USB Serial để dễ kiểm tra khi cần
  USBSerial.print("Current Version: ");
  USBSerial.println(CURRENT_VERSION);
  USBSerial.print("Current Version Code: ");
  USBSerial.println(CURRENT_VERSION_CODE);
  USBSerial.print("Server Version: ");
  USBSerial.println(new_version);
  USBSerial.print("Server Version Code: ");
  USBSerial.println(new_version_code);

  // Chỉ OTA khi VERSION_CODE trên server lớn hơn VERSION_CODE hiện tại
  if (new_version_code > CURRENT_VERSION_CODE) {
    lcd.clear();
    lcd.setCursor(0, 0); 
    lcd.print("New Version Found!");

    lcd.setCursor(0, 1); 
    lcd.print("Current V: "); 
    lcd.print(CURRENT_VERSION);

    lcd.setCursor(0, 2); 
    lcd.print("New V:     "); 
    lcd.print(new_version);

    lcd.setCursor(0, 3); 
    lcd.print("Downloading FW...");

    String full_bin_url = String(base_bin_url) + String(bin_file_name);

    // Kết thúc HTTP request version.json trước khi download file BIN
    http.end();

    t_httpUpdate_return ret = httpUpdate.update(client, full_bin_url);

    switch (ret) {
      case HTTP_UPDATE_FAILED:
        lcd.clear();
        lcd.setCursor(0, 0); 
        lcd.print("Update Failed!");

        lcd.setCursor(0, 1); 
        lcd.print("Error: ");
        lcd.print(httpUpdate.getLastError());

        lcd.setCursor(0, 2); 
        lcd.print(httpUpdate.getLastErrorString());
        delay(3000);
        break;

      case HTTP_UPDATE_NO_UPDATES:
        lcd.clear();
        lcd.setCursor(0, 1); 
        lcd.print("No Update Needed");
        delay(1500);
        break;

      case HTTP_UPDATE_OK:
        // ESP32 HTTPUpdate thường tự restart sau khi update thành công.
        // Đoạn dưới vẫn giữ để hiển thị trạng thái nếu thư viện trả về.
        lcd.clear();
        lcd.setCursor(0, 1); 
        lcd.print("Update Success!");
        lcd.setCursor(0, 2); 
        lcd.print("Rebooting...");
        delay(1000);
        ESP.restart();
        break;
    }
  } 
  else {
    // VERSION_CODE bằng hoặc nhỏ hơn firmware hiện tại -> KHÔNG download
    lcd.clear();
    lcd.setCursor(0, 0); 
    lcd.print("Already Up To Date!");

    lcd.setCursor(0, 1); 
    lcd.print("Current V: "); 
    lcd.print(CURRENT_VERSION);

    lcd.setCursor(0, 2); 
    lcd.print("Server V:  "); 
    lcd.print(new_version);

    delay(3000);
    http.end();
  }
}


void trigger11Keys() {
  for (int p = 0; p < 11; p++) {
    mcp.digitalWrite(p, LOW);  delay(200);                
    mcp.digitalWrite(p, HIGH); delay(200);                
  }
}

// ==========================================
// MÁY TRẠNG THÁI (HỖ TRỢ CẢ 2 CHẾ ĐỘ AUTO VÀ PC)
// ==========================================
void handleRunSequence() {
  unsigned long currentMillis = millis();
  
  switch(runStep) {
    case 2: 
      if (currentMillis - previousMillis >= delayDisconnect * 1000UL) { runStep = 3; }
      break;
      
    case 3: 
      usbServos[currentPair].attach(USB_SERVO_PINS[currentPair]);  
      usbServos[currentPair].write(usbAngleB[currentPair]);
      
      previousMillis = currentMillis; runStep = 4;
      if(isRunManually) { lcd.setCursor(0, 2); lcd.print("USB Connected..."); }
      break;
      
    case 4: 
      if (currentMillis - previousMillis >= 1000UL) {
        detachServoSafe(currentPair); 
        previousMillis = currentMillis; runStep = 5;
      }
      break;
      
    case 5: 
      if (currentMillis - previousMillis >= 1000UL) {
        mcp.digitalWrite(DLC_RELAY_PINS[currentPair], LOW); 
        runStep = 6; 
        previousMillis = currentMillis; 
        if (isRunManually) { lcd.setCursor(0, 2); lcd.print("DLC Connected..."); } 
        else { lcd.setCursor(0, 2); lcd.print("Waiting PC Command.."); }
      }
      break;
      
    case 6: 
      if (isRunManually) {
          unsigned long waitTime = 0;
          if (delayConnect * 1000UL > 7400UL) { waitTime = delayConnect * 1000UL - 7400UL; }
          
          if (currentMillis - previousMillis >= waitTime) {
              lcd.setCursor(0, 2); lcd.print("Testing 11 Keys...  ");
              trigger11Keys(); 
              
              previousMillis = millis(); runStep = 61; 
          }
      }
      break;

    case 61: 
      if (isRunManually) {
          if (currentMillis - previousMillis >= 3000UL) {
              runStep = 7;
              lcd.setCursor(0, 2); lcd.print("Changing Tool...");
          }
      }
      break;

    case 7: 
      mcp.digitalWrite(DLC_RELAY_PINS[currentPair], HIGH); 
      previousMillis = currentMillis; runStep = 8;
      break;

    case 8:
      if (currentMillis - previousMillis >= 500UL) {
        usbServos[currentPair].attach(USB_SERVO_PINS[currentPair]);  
        usbServos[currentPair].write(usbAngleA[currentPair]); 
        previousMillis = currentMillis; runStep = 9;
      }
      break;

    case 9:
      if (currentMillis - previousMillis >= 1000UL) {
        detachServoSafe(currentPair); 
        currentPair++;
        if (currentPair > 2) currentPair = 0; 
        lcd.setCursor(6, 1); lcd.print(currentPair + 1); 
        
        previousMillis = currentMillis; 
        runStep = 2; 
        lcd.setCursor(0, 2); lcd.print("Change Complete..."); 
      }
      break;
  }
}

void safeStopAll() {
  lcd.clear();
  printCentered(1, "STOPPING SYSTEM...");
  for (int p = 0; p < 16; p++) { mcp.digitalWrite(p, HIGH); }
  for (int i = 0; i < 3; i++) {
    usbServos[i].attach(USB_SERVO_PINS[i]); 
    usbServos[i].write(usbAngleA[i]); 
    delay(500);                             
    detachServoSafe(i);
  }
}

void printCentered(int row, String text) {
  String paddedText = " " + text + " "; 
  int textLen = paddedText.length();
  int padding = (20 - textLen) / 2;
  if (padding <= 0) { lcd.setCursor(0, row); lcd.print(text); return; }
  lcd.setCursor(0, row);
  for (int i = 0; i < padding; i++) lcd.print("-"); 
  lcd.print(paddedText);
  for (int i = 0; i < (20 - textLen - padding); i++) lcd.print("-");
}

void drawScreen() {
  lcd.clear();
  if (currentScreen == 2) {
    printCentered(0, "Function");
    
    // Mảng chứa tên 5 mục Menu
    String menuItems[5] = {"Run Manually", "Servo Calibration", "Delay Connect", "Delay Disconnect", "Wifi Setting"};
    
    // Logic cuộn trang (Hiển thị 3 dòng từ startItem)
    int startItem = 0;
    if (cursorIndex > 2) {
        startItem = cursorIndex - 2;
    }
    
    for (int i = 0; i < 3; i++) {
        if (startItem + i < 5) {
            lcd.setCursor(1, i + 1);
            lcd.print(menuItems[startItem + i]);
        }
    }
    lcd.setCursor(0, cursorIndex - startItem + 1); lcd.print(">"); 
  } 
  else if (currentScreen >= 4 && currentScreen <= 6) {
    int t = currentScreen - 4; 
    String title = "Tool " + String(t + 1) + " Calib";
    printCentered(0, title);
    lcd.setCursor(1, 1); lcd.print("Angle A: "); lcd.print(usbAngleA[t]); lcd.print((char)223);
    lcd.setCursor(1, 2); lcd.print("Angle B: "); lcd.print(usbAngleB[t]); lcd.print((char)223);
    lcd.setCursor(0, cursorIndex + 1); lcd.print(">");
  } 
  else if (currentScreen == 8) {
    printCentered(0, "Delay Disconnect");
    lcd.setCursor(0, 1); lcd.print(delayDisconnect); lcd.print(" seconds");
  }
  else if (currentScreen == 9) {
    printCentered(0, "Delay Connect");
    lcd.setCursor(0, 1); lcd.print(delayConnect); lcd.print(" seconds");
  }
}

void detachServoSafe(int index) {
  usbServos[index].detach();                
  pinMode(USB_SERVO_PINS[index], OUTPUT);   
  digitalWrite(USB_SERVO_PINS[index], LOW); 
}
