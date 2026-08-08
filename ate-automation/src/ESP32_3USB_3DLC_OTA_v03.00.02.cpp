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

const char* CURRENT_VERSION = "03.00.02";
const uint32_t CURRENT_VERSION_CODE = 30002;

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

// Khai báo trước hàm
void printCentered(int row, String text);
void drawScreen();
void handleRunSequence();
void handleSerialCommands();
void trigger11Keys();
void detachServoSafe(int index);
void safeStopAll(); 
void updateFirmwareFromInternet(); // Hàm HTTPS OTA

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
  // KHỞI TẠO WIFI BẰNG WIFIMANAGER (AUTO CONNECT / AP MODE)
  // ==========================================================
  lcd.clear();
  printCentered(0, "WIFI CONNECTION");
  lcd.setCursor(0, 1); lcd.print("Connecting...");
  lcd.setCursor(0, 2); lcd.print("AP: ATE_Setup_WiFi");

  WiFiManager wm;
  // Cài đặt thời gian chờ 3 phút (180s). Hết giờ tự thoát để chạy Offline.
  wm.setConfigPortalTimeout(180); 

  // Tự động kết nối mạng cũ, nếu không được thì mở trạm phát
  bool connected = wm.autoConnect("ATE_Setup_WiFi");

  if (connected) {
    lcd.clear();
    printCentered(0, "WIFI CONNECTED!");
    lcd.setCursor(0, 1); lcd.print("IP:"); 
    lcd.print(WiFi.localIP());

    // CẤU HÌNH LOCAL OTA
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
      lcd.print((progress / (total / 100)));
      lcd.print("%   ");
    });
    ArduinoOTA.begin();
    delay(1500); 

    // TỰ ĐỘNG CẬP NHẬT TỪ INTERNET KHI KẾT NỐI WIFI THÀNH CÔNG
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
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle(); 
  }

  handleSerialCommands();

  bool ok    = digitalRead(BTN_OK);
  bool down  = digitalRead(BTN_DOWN);
  bool plus  = digitalRead(BTN_PLUS);
  bool minus = digitalRead(BTN_MINUS);
  bool run   = digitalRead(BTN_RUN);

  if (run == LOW && lastRun == HIGH) {
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
    if (currentScreen == 2) {
      cursorIndex = (cursorIndex + 1) % 5; 
    }
    else if (currentScreen >= 4 && currentScreen <= 6) {
      cursorIndex = (cursorIndex + 1) % 2; 
    }
    drawScreen(); delay(150);
  }

  if (ok == LOW && lastOk == HIGH) {
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
        lcd.clear();
        printCentered(0, "WIFI SETTING");
        lcd.setCursor(0, 1); lcd.print("AP: ATE_Setup_WiFi");
        lcd.setCursor(0, 2); lcd.print("IP: 192.168.4.1");
        lcd.setCursor(0, 3); lcd.print("Wait for phone...");
        
        WiFiManager wm;
        wm.setConfigPortalTimeout(180); // 3 phút để cài đặt
        
        // Mở cấu hình mạng ép buộc
        if (wm.startConfigPortal("ATE_Setup_WiFi")) {
            lcd.clear();
            printCentered(1, "WiFi Updated!");
            lcd.setCursor(0, 2); lcd.print("Rebooting...");
            delay(2000);
            ESP.restart(); // Khởi động lại để kết nối mạng mới
        } else {
            lcd.clear();
            printCentered(1, "Timeout / Cancel");
            delay(2000);
            currentScreen = 2; cursorIndex = 4; 
        }
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
    lcd.print("New V: "); 
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
    lcd.print("Already Up To Date");

    lcd.setCursor(0, 1); 
    lcd.print("Current V: "); 
    lcd.print(CURRENT_VERSION);

    lcd.setCursor(0, 2); 
    lcd.print("Server V: "); 
    lcd.print(new_version);

    delay(1500);
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
