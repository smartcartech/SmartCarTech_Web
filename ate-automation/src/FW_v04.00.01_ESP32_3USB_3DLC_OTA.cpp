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
// 7) LOCAL LITTLEFS OTA SAFE:
//    - PlatformIO uploadfs qua espota được nhận diện riêng với firmware OTA.
//    - Audio task dừng và đóng WAV trước khi LittleFS.end().
//    - OTA thành công -> ArduinoOTA tự reboot; setupAudio() mount lại LittleFS.
//    - OTA filesystem lỗi -> thử remount LittleFS không format và resume audio.
// 8) HTTPS OTA FW + LITTLEFS:
//    - version.json quản lý độc lập firmware và filesystem.
//    - Firmware mới được update trước, chưa reboot ngay.
//    - LittleFS mới: dừng audio -> đóng WAV -> LittleFS.end() -> update image.
//    - Chỉ reboot một lần sau khi các update cần thiết hoàn tất.
//    - Version filesystem đã cài được lưu trong EEPROM để không download lại mỗi boot.
// 9) WEB UI V1 (LOCAL):
//    - Web chạy trực tiếp trên ESP32-S3, truy cập qua http://<ATE-IP>/.
//    - Static files nằm trong LittleFS: /web/index.html, /web/style.css, /web/app.js.
//    - Dashboard realtime bằng REST polling, không dùng thư viện web ngoài.
//    - Control web bị chặn khi Automation đang chạy đối với thao tác hardware manual.
//    - Khi LittleFS OTA: WebServer dừng trước LittleFS.end(), reboot/remount an toàn.
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
#include <Update.h>             // U_LITTLEFS / filesystem OTA target
#include <ArduinoJson.h>        // Đọc file version.json
#include <WiFiManager.h>        // THƯ VIỆN WIFIMANAGER
#include <WebServer.h>           // Web UI local - built-in Arduino-ESP32 2.0.17

// ==========================================
// AUDIO - MAX98357A / I2S / LITTLEFS
// ==========================================
#include <driver/i2s.h>  // Arduino-ESP32 2.0.17 / ESP-IDF 4.4 legacy I2S driver
#include <LittleFS.h>
#include <FS.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Thông số cho HTTPS OTA (Kiểm tra phiên bản)
// VERSION_NAME: chỉ dùng để hiển thị cho người dùng.
// VERSION_CODE: dùng để so sánh OTA, tuyệt đối không dùng float.
//
// Quy ước VERSION_NAME -> VERSION_CODE:
// Ví dụ:
//   02.01.01  -> 20101
//   02.01.02  -> 20102
//   10.08.01  -> 100801

const char* CURRENT_VERSION = "04.00.01";
const uint32_t CURRENT_VERSION_CODE = 40001;

// Filesystem version KHÔNG thể chỉ dùng const trong firmware, vì LittleFS có thể
// được update độc lập với firmware. Giá trị thực tế đã cài sẽ được lưu EEPROM.
// 0 = chưa biết/chưa từng sync HTTPS; lần đầu có Wi-Fi sẽ sync theo version.json.
const uint32_t FACTORY_FS_VERSION_CODE = 0;
uint32_t installedFsVersionCode = FACTORY_FS_VERSION_CODE;

const char* version_url = "https://smartcartech.vn/ate-automation/firmware/version.json"; 
const char* base_bin_url = "https://smartcartech.vn/ate-automation/firmware/";


// ==========================================
// WEB UI V1 - LOCAL CONTROL / DASHBOARD
// ==========================================
// Web UI chạy trên ESP32 trong LAN. smartcartech.vn chỉ dùng làm OTA server.
// Truy cập: http://<IP-hiển-thị-trên-LCD>/ hoặc http://ate-tool-system.local/
const char* ATE_STATION_NAME = "ATE-01";
const uint16_t ATE_WEB_PORT = 80;

WebServer ateWeb(ATE_WEB_PORT);
bool webRoutesConfigured = false;
bool webServerStarted = false;

enum WebPendingAction : uint8_t {
  WEB_ACTION_NONE = 0,
  WEB_ACTION_START_WIFI_PORTAL,
  WEB_ACTION_HTTPS_OTA
};

volatile WebPendingAction pendingWebAction = WEB_ACTION_NONE;
unsigned long pendingWebActionMillis = 0;
const uint32_t WEB_DEFERRED_ACTION_DELAY_MS = 400UL;

const char WEB_FALLBACK_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ATE Automation</title><style>body{font-family:Arial,sans-serif;background:#111827;color:#f9fafb;margin:0;padding:28px}main{max-width:720px;margin:auto;background:#1f2937;padding:24px;border-radius:16px}code{background:#111827;padding:2px 6px;border-radius:6px}.ok{color:#34d399}</style></head>
<body><main><h2>ATE Automation Web</h2><p>ESP32 WebServer is running, but the LittleFS web assets are not available.</p><p>Put the files in <code>data/web/</code>, build the complete LittleFS image (including <code>data/voice/</code>), then upload/update the filesystem.</p><p class="ok">API status is still available at <code>/api/status</code>.</p></main></body></html>
)HTML";

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
// CẤU HÌNH AUDIO - MAX98357A + LOA 3W
// ==========================================
// MAX98357A:
//   BCLK -> GPIO15
//   LRC/WS -> GPIO16
//   DIN -> GPIO17
//   VIN -> 5V
//   GND -> GND
//   SPK+ / SPK- -> 2 chân loa (KHÔNG nối SPK- xuống GND)
const int I2S_BCLK_PIN = 15;
const int I2S_LRC_PIN  = 16;
const int I2S_DOUT_PIN = 17;

const uint32_t BUTTON_BEEP_SAMPLE_RATE = 16000;
const uint16_t BUTTON_BEEP_FREQ_HZ     = 2200;
const uint16_t BUTTON_BEEP_DURATION_MS = 60;
const int16_t  BUTTON_BEEP_AMPLITUDE   = 5200; // ~16% full scale, đủ nghe nhưng không quá gắt
const uint8_t  VOICE_VOLUME_PERCENT    = 75;   // chỉnh 0..100 nếu muốn nhỏ/lớn hơn

// Voice files trong LittleFS.
// Yêu cầu file: WAV PCM, 16-bit, MONO. Sample rate 8..96 kHz; I2S tự đổi clock theo WAV.
const char* VOICE_SYSTEM_READY       = "/voice/system_ready.wav";
const char* VOICE_AUTO_WITH_KEY      = "/voice/auto_with_key.wav";
const char* VOICE_AUTO_SKIP_KEY      = "/voice/auto_skip_key.wav";
const char* VOICE_TOOL_1             = "/voice/tool_1.wav";
const char* VOICE_TOOL_2             = "/voice/tool_2.wav";
const char* VOICE_TOOL_3             = "/voice/tool_3.wav";
const char* VOICE_WIFI_FAILED        = "/voice/wifi_failed.wav";
const char* VOICE_OTA_FAILED         = "/voice/ota_failed.wav";
const char* VOICE_HARDWARE_ERROR     = "/voice/hardware_error.wav";
const char* VOICE_AUTOMATION_STOPPED = "/voice/automation_stopped.wav";

const i2s_port_t AUDIO_I2S_PORT = I2S_NUM_0;
QueueHandle_t audioBeepQueue = nullptr;
QueueHandle_t audioVoiceQueue = nullptr;
TaskHandle_t audioTaskHandle = nullptr;
bool audioI2SReady = false;
bool audioFSReady = false;

// ==========================================
// TRẠNG THÁI AUDIO / LITTLEFS KHI LOCAL OTA
// ==========================================
// Khi upload filesystem qua PlatformIO (pio run -t uploadfs), ArduinoOTA
// sẽ báo command khác U_FLASH. Ta yêu cầu audio task dừng cooperative,
// đóng file WAV đang đọc rồi mới LittleFS.end() để tránh ghi đè filesystem
// trong lúc audio vẫn còn giữ file mở.
volatile bool audioFsOtaPauseRequested = false;
volatile bool audioFsOtaPaused = false;
volatile bool localOtaInProgress = false;
volatile bool localOtaFilesystemUpdate = false;
const uint32_t AUDIO_FS_OTA_PAUSE_TIMEOUT_MS = 2000UL;

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

// EEPROM metadata cho version LittleFS HTTPS OTA.
// Các địa chỉ cũ 0..31 giữ nguyên hoàn toàn.
const int addrFsVersionCode  = 32;
const int addrFsVersionMagic = 36;
const uint32_t FS_VERSION_MAGIC = 0x46535631UL; // ASCII gần tương đương "FSV1"

int currentScreen = 1; 
int cursorIndex = 0; 
bool isRunning = false;
bool isRunManually = false; 
bool skipManualKeyTest = false; // false = AUTO (test 11 keys), true = AUTO2 (skip key test)
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
void updateFirmwareFromInternet(); // HTTPS OTA: Firmware + LittleFS
void setupArduinoOTA();
void startWifiSettingPortal();
void stopWifiSettingPortalAndReturn();
void drawWifiSettingScreen();
void drawWifiExitConfirmScreen();

// Web UI V1
void setupAteWebServer();
void stopAteWebServer();
void configureAteWebRoutes();
void processPendingWebAction();
void serveAteWebFile(const char* path, const char* contentType);
void sendWebJson(int httpCode, JsonDocument& doc);
void sendWebMessage(int httpCode, const char* status, const char* message);
bool parseWebJson(JsonDocument& doc);
bool webManualControlAllowed();
String webModeName();
String webStepName();
bool fetchOtaManifestForWeb(String& fwVersion, uint32_t& fwCode, String& fsVersion, uint32_t& fsCode, String& errorText);
void webStartPcMode();
void webStartManualMode(bool skipKeyTest);
void webStopAutomation();

// Audio
void setupAudio();
void audioTask(void* parameter);
void requestButtonBeep();
void queueVoice(const char* filePath);
void queueToolVoice(int toolIndex);
void playButtonBeepInternal();
bool playVoiceWavInternal(const char* filePath);
bool parseWavHeader(File& file, uint32_t& sampleRate, uint16_t& channels, uint16_t& bitsPerSample, uint32_t& dataSize);
uint16_t readLE16(const uint8_t* p);
uint32_t readLE32(const uint8_t* p);
bool pauseAudioAndUnmountLittleFSForOta();
void resumeAudioAfterFilesystemOtaError();
void writeAudioSilence();

// HTTPS OTA / filesystem version metadata
void loadInstalledFsVersion();
void saveInstalledFsVersion(uint32_t versionCode);
String versionCodeToName(uint32_t versionCode);

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

  // Khởi tạo I2S + LittleFS + audio task trước MCP23017 để nếu MCP lỗi
  // vẫn có thể phát voice "Hardware initialization error".
  setupAudio();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();

  if (!mcp.begin_I2C(0x20)) {
    lcd.setCursor(0, 0); lcd.print("Loi Phan Cung!");
    queueVoice(VOICE_HARDWARE_ERROR);
    while (1) { delay(1000); } // Halt hệ thống nhưng vẫn nhường CPU cho audio task.
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

  // Đọc version LittleFS đã cài từ EEPROM. Không dùng compile-time constant
  // để tránh download lại filesystem sau mỗi lần reboot.
  loadInstalledFsVersion();

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
    setupAteWebServer();

  } else {
    lcd.clear();
    printCentered(1, "WIFI FAILED!");
    lcd.setCursor(0, 2); lcd.print("Running Offline Mode");
    queueVoice(VOICE_WIFI_FAILED);
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

  // Voice quan trọng khi hệ thống đã hoàn tất boot và sẵn sàng.
  queueVoice(VOICE_SYSTEM_READY);
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

  // WebServer tự start/restart khi STA Wi-Fi sẵn sàng. Điều này cũng cover
  // trường hợp user thoát WiFi Setting mà không reboot, hoặc FS OTA lỗi rồi remount.
  if (!webServerStarted && WiFi.status() == WL_CONNECTED && !wifiPortalActive && !localOtaInProgress) {
    setupAteWebServer();
  }

  if (webServerStarted && WiFi.status() == WL_CONNECTED && !wifiPortalActive && !localOtaInProgress) {
    ateWeb.handleClient();
  }

  // Các action làm mất connection/reboot được defer để HTTP response kịp gửi về browser.
  processPendingWebAction();

  if (WiFi.status() == WL_CONNECTED && !wifiPortalActive) {
    ArduinoOTA.handle(); 
  }

  handleSerialCommands();

  bool ok    = digitalRead(BTN_OK);
  bool down  = digitalRead(BTN_DOWN);
  bool plus  = digitalRead(BTN_PLUS);
  bool minus = digitalRead(BTN_MINUS);
  bool run   = digitalRead(BTN_RUN);

  // ======================================================
  // BUTTON SOUND
  // Chỉ beep khi có cạnh nhấn mới của 5 nút vật lý.
  // Không dùng beep cho USB/DLC/Tool change/Warning/Error.
  // ======================================================
  if (ok    == LOW && lastOk    == HIGH) requestButtonBeep();
  if (down  == LOW && lastDown  == HIGH) requestButtonBeep();
  if (plus  == LOW && lastPlus  == HIGH) requestButtonBeep();
  if (minus == LOW && lastMinus == HIGH) requestButtonBeep();
  if (run   == LOW && lastRun   == HIGH) requestButtonBeep();

  if (!wifiPortalActive && run == LOW && lastRun == HIGH) {
    if (isRunning) {
      safeStopAll(); 
      queueVoice(VOICE_AUTOMATION_STOPPED);
      isRunning = false; 
      currentScreen = 2;
      cursorIndex = 0;
      drawScreen(); 
    } else {
      isRunning = true; 
      isRunManually = false; 
      skipManualKeyTest = false;
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
    else if (currentScreen == 10) {
      // Run Manually submenu: Include Key Test / Skip Key Test
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
        // Run Manually -> chọn có test 11 keys hay bỏ qua key test.
        currentScreen = 10;
        cursorIndex = 0;
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
    else if (currentScreen == 10) {
      isRunning = true;
      isRunManually = true;
      skipManualKeyTest = (cursorIndex == 1);
      currentPair = 0;

      runStep = 2;
      previousMillis = millis();

      lcd.clear();
      if (skipManualKeyTest) printCentered(0, "SYSTEM RUN (AUTO2)");
      else                   printCentered(0, "SYSTEM RUN (AUTO)");
      lcd.setCursor(0, 1); lcd.print("Tool: 1");
      lcd.setCursor(0, 2); lcd.print("Wait Disconnect...  ");
      lcd.setCursor(0, 3); lcd.print("Press RUN to Stop");

      // Voice mode + Tool 1. Không có voice "Automatic test completed"
      // vì AUTO/AUTO2 hiện chạy vòng liên tục 1 -> 2 -> 3 -> 1.
      if (skipManualKeyTest) queueVoice(VOICE_AUTO_SKIP_KEY);
      else                   queueVoice(VOICE_AUTO_WITH_KEY);
      queueToolVoice(0);

      delay(150);
      goto UPDATE_STATE;
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
// WEB UI V1 - LOCAL DASHBOARD / CONTROL
// ==========================================
String webModeName() {
  if (!isRunning) return "STOPPED";
  if (!isRunManually) return "PC";
  return skipManualKeyTest ? "AUTO2" : "AUTO";
}

String webStepName() {
  if (wifiPortalActive) return "WiFi Setting";
  if (localOtaInProgress) return localOtaFilesystemUpdate ? "Local Filesystem OTA" : "Local Firmware OTA";
  if (!isRunning) return "Stopped";

  if (!isRunManually) {
    if (runStep == 6) return "Waiting PC Command";
    if (runStep >= 7 && runStep <= 9) return "Changing Tool";
    return "PC Command";
  }

  switch (runStep) {
    case 2:  return "Wait Disconnect";
    case 3:  return "USB Connecting";
    case 4:  return "USB Connected";
    case 5:  return "DLC Connecting";
    case 6:  return skipManualKeyTest ? "Delay Connect" : "Waiting Key Test";
    case 61: return "Key Test Complete";
    case 7:
    case 8:
    case 9:  return "Changing Tool";
    default: return "Running";
  }
}

void sendWebJson(int httpCode, JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  ateWeb.sendHeader("Cache-Control", "no-store");
  ateWeb.send(httpCode, "application/json", body);
}

void sendWebMessage(int httpCode, const char* status, const char* message) {
  JsonDocument doc;
  doc["status"] = status;
  doc["message"] = message;
  sendWebJson(httpCode, doc);
}

bool parseWebJson(JsonDocument& doc) {
  if (!ateWeb.hasArg("plain")) return false;
  DeserializationError err = deserializeJson(doc, ateWeb.arg("plain"));
  return !err;
}

bool webManualControlAllowed() {
  return !isRunning && !wifiPortalActive && !localOtaInProgress && pendingWebAction == WEB_ACTION_NONE;
}

void serveAteWebFile(const char* path, const char* contentType) {
  if (!audioFSReady) {
    if (strcmp(path, "/web/index.html") == 0) {
      ateWeb.send_P(200, "text/html", WEB_FALLBACK_HTML);
    } else {
      ateWeb.send(404, "text/plain", "LittleFS is not mounted");
    }
    return;
  }

  File f = LittleFS.open(path, "r");
  if (!f) {
    if (strcmp(path, "/web/index.html") == 0) {
      ateWeb.send_P(200, "text/html", WEB_FALLBACK_HTML);
    } else {
      ateWeb.send(404, "text/plain", "Web asset not found");
    }
    return;
  }

  ateWeb.sendHeader("Cache-Control", "no-cache");
  ateWeb.streamFile(f, contentType);
  f.close();
}

void webStartPcMode() {
  isRunning = true;
  isRunManually = false;
  skipManualKeyTest = false;
  currentPair = 0;
  runStep = 6;

  lcd.clear();
  printCentered(0, "SYSTEM RUN (PC)");
  lcd.setCursor(0, 1); lcd.print("Tool: 1");
  lcd.setCursor(0, 2); lcd.print("Waiting PC Command..");
  lcd.setCursor(0, 3); lcd.print("Press RUN to Stop");
}

void webStartManualMode(bool skipKeyTest) {
  isRunning = true;
  isRunManually = true;
  skipManualKeyTest = skipKeyTest;
  currentPair = 0;
  runStep = 2;
  previousMillis = millis();

  lcd.clear();
  if (skipManualKeyTest) printCentered(0, "SYSTEM RUN (AUTO2)");
  else                   printCentered(0, "SYSTEM RUN (AUTO)");
  lcd.setCursor(0, 1); lcd.print("Tool: 1");
  lcd.setCursor(0, 2); lcd.print("Wait Disconnect...  ");
  lcd.setCursor(0, 3); lcd.print("Press RUN to Stop");

  if (skipManualKeyTest) queueVoice(VOICE_AUTO_SKIP_KEY);
  else                   queueVoice(VOICE_AUTO_WITH_KEY);
  queueToolVoice(0);
}

void webStopAutomation() {
  if (isRunning) {
    safeStopAll();
    queueVoice(VOICE_AUTOMATION_STOPPED);
  }
  isRunning = false;
  isRunManually = false;
  skipManualKeyTest = false;
  currentScreen = 2;
  cursorIndex = 0;
  drawScreen();
}

bool fetchOtaManifestForWeb(String& fwVersion, uint32_t& fwCode, String& fsVersion, uint32_t& fsCode, String& errorText) {
  fwVersion = "";
  fsVersion = "";
  fwCode = 0;
  fsCode = 0;
  errorText = "";

  if (WiFi.status() != WL_CONNECTED) {
    errorText = "WiFi not connected";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(12000);

  HTTPClient http;
  if (!http.begin(client, version_url)) {
    errorText = "Cannot open version URL";
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    errorText = "HTTP " + String(code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    errorText = "Invalid version.json";
    return false;
  }

  fwVersion = String((const char*)(doc["firmware"]["version"] | ""));
  fwCode = doc["firmware"]["version_code"] | 0;
  fsVersion = String((const char*)(doc["filesystem"]["version"] | ""));
  fsCode = doc["filesystem"]["version_code"] | 0;

  if (fwVersion.length() == 0 || fwCode == 0 || fsVersion.length() == 0 || fsCode == 0) {
    errorText = "Manifest missing firmware/filesystem fields";
    return false;
  }

  return true;
}

void configureAteWebRoutes() {
  if (webRoutesConfigured) return;

  ateWeb.on("/", HTTP_GET, []() {
    serveAteWebFile("/web/index.html", "text/html");
  });
  ateWeb.on("/style.css", HTTP_GET, []() {
    serveAteWebFile("/web/style.css", "text/css");
  });
  ateWeb.on("/app.js", HTTP_GET, []() {
    serveAteWebFile("/web/app.js", "application/javascript");
  });
  ateWeb.on("/favicon.ico", HTTP_GET, []() {
    ateWeb.send(204, "text/plain", "");
  });

  // -------------------- STATUS --------------------
  ateWeb.on("/api/status", HTTP_GET, []() {
    JsonDocument doc;
    doc["station"] = ATE_STATION_NAME;
    doc["running"] = isRunning;
    doc["mode"] = webModeName();
    doc["tool"] = currentPair + 1;
    doc["step"] = webStepName();
    doc["run_step"] = runStep;
    doc["fw_version"] = CURRENT_VERSION;
    doc["fw_version_code"] = CURRENT_VERSION_CODE;
    doc["fs_version"] = versionCodeToName(installedFsVersionCode);
    doc["fs_version_code"] = installedFsVersionCode;
    doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
    doc["ssid"] = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "";
    doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    doc["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "0.0.0.0";
    doc["uptime_s"] = millis() / 1000UL;
    doc["littlefs_ready"] = audioFSReady;
    doc["audio_ready"] = audioI2SReady;
    doc["wifi_portal_active"] = wifiPortalActive;
    doc["local_ota"] = localOtaInProgress;
    sendWebJson(200, doc);
  });

  // -------------------- RUN CONTROL --------------------
  ateWeb.on("/api/run", HTTP_POST, []() {
    if (wifiPortalActive || localOtaInProgress || pendingWebAction != WEB_ACTION_NONE) {
      sendWebMessage(409, "busy", "ATE is busy with WiFi/OTA operation");
      return;
    }
    if (isRunning) {
      sendWebMessage(409, "busy", "Stop the current mode before starting another mode");
      return;
    }

    JsonDocument req;
    if (!parseWebJson(req)) {
      sendWebMessage(400, "error", "Invalid JSON body");
      return;
    }

    String mode = String((const char*)(req["mode"] | ""));
    mode.toLowerCase();

    if (mode == "pc") webStartPcMode();
    else if (mode == "auto") webStartManualMode(false);
    else if (mode == "auto2") webStartManualMode(true);
    else {
      sendWebMessage(400, "error", "mode must be pc, auto or auto2");
      return;
    }

    sendWebMessage(200, "ok", "Automation started");
  });

  ateWeb.on("/api/stop", HTTP_POST, []() {
    if (wifiPortalActive || localOtaInProgress) {
      sendWebMessage(409, "busy", "ATE is busy with WiFi/OTA operation");
      return;
    }
    webStopAutomation();
    sendWebMessage(200, "ok", "Automation stopped");
  });

  // -------------------- MANUAL HARDWARE --------------------
  ateWeb.on("/api/manual/usb", HTTP_POST, []() {
    if (!webManualControlAllowed()) {
      sendWebMessage(409, "busy", "Stop Automation before manual hardware control");
      return;
    }

    JsonDocument req;
    if (!parseWebJson(req)) {
      sendWebMessage(400, "error", "Invalid JSON body");
      return;
    }

    int tool = req["tool"] | 0;
    String action = String((const char*)(req["action"] | ""));
    action.toLowerCase();
    if (tool < 1 || tool > 3 || (action != "connect" && action != "disconnect")) {
      sendWebMessage(400, "error", "Invalid tool/action");
      return;
    }

    int idx = tool - 1;
    usbServos[idx].attach(USB_SERVO_PINS[idx]);
    usbServos[idx].write(action == "connect" ? usbAngleB[idx] : usbAngleA[idx]);
    delay(600);
    detachServoSafe(idx);
    sendWebMessage(200, "ok", "USB actuator command completed");
  });

  ateWeb.on("/api/manual/dlc", HTTP_POST, []() {
    if (!webManualControlAllowed()) {
      sendWebMessage(409, "busy", "Stop Automation before manual hardware control");
      return;
    }

    JsonDocument req;
    if (!parseWebJson(req)) {
      sendWebMessage(400, "error", "Invalid JSON body");
      return;
    }

    int tool = req["tool"] | 0;
    String action = String((const char*)(req["action"] | ""));
    action.toLowerCase();
    if (tool < 1 || tool > 3 || (action != "connect" && action != "disconnect")) {
      sendWebMessage(400, "error", "Invalid tool/action");
      return;
    }

    int idx = tool - 1;
    mcp.digitalWrite(DLC_RELAY_PINS[idx], action == "connect" ? LOW : HIGH);
    sendWebMessage(200, "ok", "DLC relay command completed");
  });

  ateWeb.on("/api/manual/key", HTTP_POST, []() {
    if (!webManualControlAllowed()) {
      sendWebMessage(409, "busy", "Stop Automation before manual key control");
      return;
    }

    JsonDocument req;
    if (!parseWebJson(req)) {
      sendWebMessage(400, "error", "Invalid JSON body");
      return;
    }

    bool allKeys = req["all"] | false;
    if (allKeys) {
      trigger11Keys();
      sendWebMessage(200, "ok", "All 11 keys tested");
      return;
    }

    int key = req["key"] | 0;
    if (key < 1 || key > 11) {
      sendWebMessage(400, "error", "key must be 1..11");
      return;
    }

    int pin = key - 1;
    mcp.digitalWrite(pin, LOW); delay(200);
    mcp.digitalWrite(pin, HIGH); delay(200);
    sendWebMessage(200, "ok", "Key tested");
  });

  ateWeb.on("/api/audio/test", HTTP_POST, []() {
    if (!audioFSReady) {
      sendWebMessage(503, "error", "LittleFS/voice is not ready");
      return;
    }
    queueVoice(VOICE_SYSTEM_READY);
    sendWebMessage(200, "ok", "Voice test queued");
  });

  // -------------------- CONFIG / CALIBRATION --------------------
  ateWeb.on("/api/config", HTTP_GET, []() {
    JsonDocument doc;
    JsonArray a = doc["usbAngleA"].to<JsonArray>();
    JsonArray b = doc["usbAngleB"].to<JsonArray>();
    for (int i = 0; i < 3; i++) {
      a.add(usbAngleA[i]);
      b.add(usbAngleB[i]);
    }
    doc["delayDisconnect"] = delayDisconnect;
    doc["delayConnect"] = delayConnect;
    sendWebJson(200, doc);
  });

  ateWeb.on("/api/config", HTTP_POST, []() {
    if (isRunning || wifiPortalActive || localOtaInProgress) {
      sendWebMessage(409, "busy", "Stop Automation before changing configuration");
      return;
    }

    JsonDocument req;
    if (!parseWebJson(req)) {
      sendWebMessage(400, "error", "Invalid JSON body");
      return;
    }

    JsonArray a = req["usbAngleA"].as<JsonArray>();
    JsonArray b = req["usbAngleB"].as<JsonArray>();
    int newDelayD = req["delayDisconnect"] | -1;
    int newDelayC = req["delayConnect"] | -1;

    if (a.size() != 3 || b.size() != 3 || newDelayD < 0 || newDelayD > 1000 || newDelayC < 8 || newDelayC > 1000) {
      sendWebMessage(400, "error", "Invalid calibration/delay values");
      return;
    }

    for (int i = 0; i < 3; i++) {
      int va = a[i] | -1;
      int vb = b[i] | -1;
      if (va < 0 || va > 180 || vb < 0 || vb > 180) {
        sendWebMessage(400, "error", "Servo angle must be 0..180");
        return;
      }
      usbAngleA[i] = va;
      usbAngleB[i] = vb;
    }

    delayDisconnect = newDelayD;
    delayConnect = newDelayC;

    for (int i = 0; i < 3; i++) {
      EEPROM.put(addrUsbA[i], usbAngleA[i]);
      EEPROM.put(addrUsbB[i], usbAngleB[i]);
    }
    EEPROM.put(addrTimeD, delayDisconnect);
    EEPROM.put(addrTimeC, delayConnect);
    EEPROM.commit();

    sendWebMessage(200, "ok", "Configuration saved to EEPROM");
  });

  ateWeb.on("/api/calibration/move", HTTP_POST, []() {
    if (!webManualControlAllowed()) {
      sendWebMessage(409, "busy", "Stop Automation before calibration move");
      return;
    }

    JsonDocument req;
    if (!parseWebJson(req)) {
      sendWebMessage(400, "error", "Invalid JSON body");
      return;
    }

    int tool = req["tool"] | 0;
    int angle = req["angle"] | -1;
    if (tool < 1 || tool > 3 || angle < 0 || angle > 180) {
      sendWebMessage(400, "error", "Invalid tool/angle");
      return;
    }

    int idx = tool - 1;
    usbServos[idx].attach(USB_SERVO_PINS[idx]);
    usbServos[idx].write(angle);
    delay(600);
    detachServoSafe(idx);
    sendWebMessage(200, "ok", "Servo moved");
  });

  // -------------------- WIFI --------------------
  ateWeb.on("/api/wifi/setup", HTTP_POST, []() {
    if (isRunning || localOtaInProgress || pendingWebAction != WEB_ACTION_NONE) {
      sendWebMessage(409, "busy", "Stop Automation before starting WiFi Setup");
      return;
    }
    pendingWebAction = WEB_ACTION_START_WIFI_PORTAL;
    pendingWebActionMillis = millis();
    sendWebMessage(202, "accepted", "WiFi Setup AP will start. Connect phone to ATE_Setup_WiFi.");
  });

  // -------------------- HTTPS OTA --------------------
  ateWeb.on("/api/update/check", HTTP_GET, []() {
    String fwVersion, fsVersion, errorText;
    uint32_t fwCode = 0, fsCode = 0;
    if (!fetchOtaManifestForWeb(fwVersion, fwCode, fsVersion, fsCode, errorText)) {
      JsonDocument doc;
      doc["status"] = "error";
      doc["message"] = errorText;
      sendWebJson(502, doc);
      return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["current_fw"] = CURRENT_VERSION;
    doc["current_fw_code"] = CURRENT_VERSION_CODE;
    doc["server_fw"] = fwVersion;
    doc["server_fw_code"] = fwCode;
    doc["fw_update_available"] = (fwCode > CURRENT_VERSION_CODE);
    doc["current_fs"] = versionCodeToName(installedFsVersionCode);
    doc["current_fs_code"] = installedFsVersionCode;
    doc["server_fs"] = fsVersion;
    doc["server_fs_code"] = fsCode;
    doc["fs_update_available"] = (fsCode > installedFsVersionCode);
    sendWebJson(200, doc);
  });

  ateWeb.on("/api/update/start", HTTP_POST, []() {
    if (isRunning || wifiPortalActive || localOtaInProgress || pendingWebAction != WEB_ACTION_NONE) {
      sendWebMessage(409, "busy", "Stop Automation before HTTPS OTA");
      return;
    }
    if (WiFi.status() != WL_CONNECTED) {
      sendWebMessage(503, "error", "WiFi not connected");
      return;
    }
    pendingWebAction = WEB_ACTION_HTTPS_OTA;
    pendingWebActionMillis = millis();
    sendWebMessage(202, "accepted", "HTTPS OTA check/update will start. Device may reboot.");
  });

  ateWeb.onNotFound([]() {
    if (ateWeb.uri().startsWith("/api/")) {
      sendWebMessage(404, "error", "API endpoint not found");
    } else {
      serveAteWebFile("/web/index.html", "text/html");
    }
  });

  webRoutesConfigured = true;
}

void setupAteWebServer() {
  if (webServerStarted || WiFi.status() != WL_CONNECTED || wifiPortalActive || localOtaInProgress) return;

  configureAteWebRoutes();
  ateWeb.begin();
  webServerStarted = true;

  // ArduinoOTA đã khởi tạo mDNS với hostname ATE-Tool-System.
  // Add HTTP service để browser có thể thử http://ate-tool-system.local/.
  MDNS.addService("http", "tcp", ATE_WEB_PORT);

  USBSerial.print("[WEB] ATE UI started: http://");
  USBSerial.print(WiFi.localIP());
  USBSerial.println("/");
}

void stopAteWebServer() {
  if (!webServerStarted) return;
  ateWeb.stop();
  webServerStarted = false;
  USBSerial.println("[WEB] WebServer stopped");
}

void processPendingWebAction() {
  if (pendingWebAction == WEB_ACTION_NONE) return;
  if (millis() - pendingWebActionMillis < WEB_DEFERRED_ACTION_DELAY_MS) return;

  WebPendingAction action = pendingWebAction;
  pendingWebAction = WEB_ACTION_NONE;

  if (action == WEB_ACTION_START_WIFI_PORTAL) {
    stopAteWebServer();
    startWifiSettingPortal();
    return;
  }

  if (action == WEB_ACTION_HTTPS_OTA) {
    updateFirmwareFromInternet();
    // Nếu không update hoặc update fail nhưng không reboot, đảm bảo web trở lại.
    if (WiFi.status() == WL_CONNECTED && !wifiPortalActive && !localOtaInProgress) {
      setupAteWebServer();
    }
  }
}

// ==========================================
// LOCAL OTA SETUP
// ==========================================
void setupArduinoOTA() {
  ArduinoOTA.setHostname("ATE-Tool-System");

  // ArduinoOTA mặc định reboot sau update thành công. Set explicit để
  // đảm bảo cả firmware OTA và filesystem OTA đều reboot sau khi hoàn tất.
  // Sau reboot, setupAudio() sẽ mount lại LittleFS và các voice mới sẽ có hiệu lực.
  ArduinoOTA.setRebootOnSuccess(true);

  ArduinoOTA.onStart([]() {
    localOtaInProgress = true;
    localOtaFilesystemUpdate = (ArduinoOTA.getCommand() != U_FLASH);

    lcd.clear();

    if (localOtaFilesystemUpdate) {
      printCentered(0, "LITTLEFS OTA");
      lcd.setCursor(0, 1); lcd.print("Stopping Audio...");

      // QUAN TRỌNG:
      // 1) Chặn enqueue beep/voice mới.
      // 2) Đợi audio task đóng WAV đang phát.
      // 3) Sau đó mới unmount LittleFS.
      bool fsSafe = pauseAudioAndUnmountLittleFSForOta();

      lcd.setCursor(0, 1);
      if (fsSafe) {
        lcd.print("Uploading Voice...  ");
      } else {
        // Không cho OTA tiếp tục nếu audio chưa xác nhận đã đóng file.
        // ArduinoOTA gọi onStart() trước khi nhận/ghi data, vì vậy restart
        // tại đây sẽ hủy phiên OTA trước khi filesystem bị ghi không an toàn.
        lcd.print("Audio Stop Timeout! ");
        lcd.setCursor(0, 2); lcd.print("OTA Aborted - Reboot");
        delay(500);
        ESP.restart();
      }
    } else {
      printCentered(1, "LOCAL OTA UPDATING..");
    }
  });

  ArduinoOTA.onEnd([]() {
    lcd.clear();

    if (localOtaFilesystemUpdate) {
      printCentered(0, "LITTLEFS UPDATED!");
      lcd.setCursor(0, 1); lcd.print("Voice Files Updated");
      lcd.setCursor(0, 2); lcd.print("Rebooting...");

      // Không remount ở đây. ArduinoOTA sẽ reboot ngay sau callback này
      // (setRebootOnSuccess(true)); setupAudio() ở boot kế tiếp sẽ mount
      // LittleFS mới. Đây là cách an toàn nhất sau khi partition vừa được ghi.
    } else {
      printCentered(1, "UPDATE SUCCESS!");
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    lcd.clear();
    printCentered(0, "LOCAL OTA FAILED!");
    lcd.setCursor(0, 1); lcd.print("Error: "); lcd.print((int)error);

    // Dùng cả getCommand() để cover trường hợp OTA_BEGIN_ERROR xảy ra
    // trước khi onStart() được gọi. ArduinoOTA dùng U_FLASHFS cho uploadfs.
    bool fsUpdate = localOtaFilesystemUpdate || (ArduinoOTA.getCommand() != U_FLASH);

    if (fsUpdate) {
      // OTA filesystem lỗi thì ArduinoOTA không reboot tự động.
      // Thử mount lại filesystem hiện tại KHÔNG format. Nếu mount được,
      // audio được resume và có thể phát voice OTA failed. Nếu filesystem
      // đã bị ghi dở và mount không được, voice vẫn bị disable an toàn.
      resumeAudioAfterFilesystemOtaError();
    } else {
      queueVoice(VOICE_OTA_FAILED);
    }

    localOtaInProgress = false;
    localOtaFilesystemUpdate = false;
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
  skipManualKeyTest = false;

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
            lcd.setCursor(0, 2); lcd.print("Changing Tool...    ");
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
// HTTPS OTA TỪ INTERNET - FIRMWARE + LITTLEFS
// version.json dạng:
// {
//   "firmware": {
//     "version": "03.00.10",
//     "version_code": 30010,
//     "bin_file": "ATE_FW_v03.00.10.bin"
//   },
//   "filesystem": {
//     "version": "01.00.01",
//     "version_code": 10001,
//     "bin_file": "ATE_LittleFS_v01.00.01.bin"
//   }
// }
//
// Flow an toàn:
// 1) Check manifest 1 lần.
// 2) Nếu FW mới: update FW trước, KHÔNG reboot.
// 3) Nếu FS mới: stop audio -> close WAV -> LittleFS.end() -> update LittleFS.
// 4) Nếu có update thành công: reboot đúng 1 lần.
// 5) Nếu FW cần update nhưng FW fail: KHÔNG update FS để tránh mismatch.
// ==========================================
void updateFirmwareFromInternet() {
  lcd.clear();
  printCentered(0, "HTTPS OTA");
  lcd.setCursor(0, 1);
  lcd.print("Checking versions...");

  if (WiFi.status() != WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("No WiFi Connection!");
    queueVoice(VOICE_OTA_FAILED);
    delay(1500);
    return;
  }

  // ========================================================
  // 1) DOWNLOAD + PARSE version.json
  // ========================================================
  WiFiClientSecure manifestClient;
  manifestClient.setInsecure(); // Giữ nguyên policy HTTPS hiện tại của project.
  manifestClient.setTimeout(12000);

  HTTPClient http;
  if (!http.begin(manifestClient, version_url)) {
    lcd.clear();
    lcd.setCursor(0, 1); lcd.print("OTA URL Error!");
    queueVoice(VOICE_OTA_FAILED);
    delay(1500);
    return;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("Server Error: ");
    lcd.print(httpCode);
    queueVoice(VOICE_OTA_FAILED);
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
    queueVoice(VOICE_OTA_FAILED);
    delay(1500);
    http.end();
    return;
  }

  JsonVariant firmwareNode = doc["firmware"];
  JsonVariant filesystemNode = doc["filesystem"];

  if (!firmwareNode.is<JsonObject>() || !filesystemNode.is<JsonObject>()) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Invalid version.json");
    lcd.setCursor(0, 1); lcd.print("Missing FW/FS node");
    queueVoice(VOICE_OTA_FAILED);
    delay(2000);
    http.end();
    return;
  }

  JsonObject firmware = firmwareNode.as<JsonObject>();
  JsonObject filesystem = filesystemNode.as<JsonObject>();

  String newFwVersion = firmware["version"] | "";
  uint32_t newFwVersionCode = firmware["version_code"] | 0;
  String fwBinFile = firmware["bin_file"] | "";

  String newFsVersion = filesystem["version"] | "";
  uint32_t newFsVersionCode = filesystem["version_code"] | 0;
  String fsBinFile = filesystem["bin_file"] | "";

  if (newFwVersionCode == 0 || newFwVersion.length() == 0 || fwBinFile.length() == 0 ||
      newFsVersionCode == 0 || newFsVersion.length() == 0 || fsBinFile.length() == 0) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Invalid version.json");
    lcd.setCursor(0, 1); lcd.print("Missing OTA data!");
    queueVoice(VOICE_OTA_FAILED);
    delay(2000);
    http.end();
    return;
  }

  // Không cần giữ HTTP connection của manifest trong lúc flash binary.
  http.end();

  const bool needFwUpdate = (newFwVersionCode > CURRENT_VERSION_CODE);
  const bool needFsUpdate = (newFsVersionCode > installedFsVersionCode);

  USBSerial.println("[HTTPS-OTA] ===== VERSION CHECK =====");
  USBSerial.print("[HTTPS-OTA] FW current: ");
  USBSerial.print(CURRENT_VERSION);
  USBSerial.print(" ("); USBSerial.print(CURRENT_VERSION_CODE); USBSerial.println(")");
  USBSerial.print("[HTTPS-OTA] FW server : ");
  USBSerial.print(newFwVersion);
  USBSerial.print(" ("); USBSerial.print(newFwVersionCode); USBSerial.println(")");
  USBSerial.print("[HTTPS-OTA] FS current: ");
  USBSerial.print(versionCodeToName(installedFsVersionCode));
  USBSerial.print(" ("); USBSerial.print(installedFsVersionCode); USBSerial.println(")");
  USBSerial.print("[HTTPS-OTA] FS server : ");
  USBSerial.print(newFsVersion);
  USBSerial.print(" ("); USBSerial.print(newFsVersionCode); USBSerial.println(")");

  if (!needFwUpdate && !needFsUpdate) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Already Up To Date!");
    lcd.setCursor(0, 1); lcd.print("FW: "); lcd.print(CURRENT_VERSION);
    lcd.setCursor(0, 2); lcd.print("Voice: "); lcd.print(newFsVersion);
    delay(2500);
    return;
  }

  // QUAN TRỌNG: tắt auto reboot của HTTPUpdate để nếu cần update cả FW + FS,
  // ta chỉ reboot đúng một lần ở cuối.
  httpUpdate.rebootOnUpdate(false);

  bool fwUpdated = false;
  bool fsUpdated = false;
  bool fwUpdateFailed = false;
  bool fsUpdateFailed = false;

  // ========================================================
  // 2) FIRMWARE UPDATE - LÀM TRƯỚC
  // ========================================================
  if (needFwUpdate) {
    lcd.clear();
    printCentered(0, "UPDATE FIRMWARE");
    lcd.setCursor(0, 1); lcd.print(CURRENT_VERSION);
    lcd.print(" -> "); lcd.print(newFwVersion);
    lcd.setCursor(0, 2); lcd.print("Downloading FW...");

    String fwUrl = String(base_bin_url) + fwBinFile;

    WiFiClientSecure fwClient;
    fwClient.setInsecure();
    fwClient.setTimeout(12000);

    t_httpUpdate_return fwRet = httpUpdate.update(fwClient, fwUrl);

    if (fwRet == HTTP_UPDATE_OK) {
      fwUpdated = true;
      USBSerial.println("[HTTPS-OTA] Firmware update staged successfully");
    } else {
      fwUpdateFailed = true;

      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("FW Update Failed!");
      lcd.setCursor(0, 1); lcd.print("Error: "); lcd.print(httpUpdate.getLastError());
      lcd.setCursor(0, 2); lcd.print(httpUpdate.getLastErrorString());

      USBSerial.print("[HTTPS-OTA] Firmware update failed: ");
      USBSerial.println(httpUpdate.getLastErrorString());

      queueVoice(VOICE_OTA_FAILED);
      delay(3000);
    }
  }

  // Nếu firmware BẮT BUỘC phải update nhưng lại fail thì không ghi filesystem mới.
  // Điều này tránh trường hợp filesystem mới yêu cầu firmware mới nhưng FW cũ vẫn chạy.
  bool safeToUpdateFs = (!needFwUpdate || fwUpdated);

  // ========================================================
  // 3) LITTLEFS / VOICE UPDATE
  // ========================================================
  if (needFsUpdate && safeToUpdateFs) {
    lcd.clear();
    printCentered(0, "UPDATE VOICE");
    lcd.setCursor(0, 1);
    lcd.print(versionCodeToName(installedFsVersionCode));
    lcd.print(" -> "); lcd.print(newFsVersion);
    lcd.setCursor(0, 2); lcd.print("Stopping Audio...");

    // Dùng chung cơ chế pause an toàn với Local LittleFS OTA:
    // - khóa request beep/voice mới
    // - dừng voice đang phát
    // - đóng WAV
    // - xác nhận audio task paused
    // - LittleFS.end()
    bool fsSafe = pauseAudioAndUnmountLittleFSForOta();

    if (!fsSafe) {
      fsUpdateFailed = true;
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Voice OTA Failed!");
      lcd.setCursor(0, 1); lcd.print("Audio Stop Timeout");

      // Khôi phục audio state vì filesystem chưa bị ghi.
      resumeAudioAfterFilesystemOtaError();
      delay(2000);
    } else {
      lcd.setCursor(0, 2); lcd.print("Downloading Voice... ");

      String fsUrl = String(base_bin_url) + fsBinFile;

      WiFiClientSecure fsClient;
      fsClient.setInsecure();
      fsClient.setTimeout(12000);

      t_httpUpdate_return fsRet;

      // Arduino-ESP32 Core 2.0.17 chưa có updateLittlefs().
      // API legacy updateSpiffs() ghi raw filesystem image vào data partition
      // subtype SPIFFS; LittleFS trên core 2.x dùng cùng data partition subtype.
      fsRet = httpUpdate.updateSpiffs(fsClient, fsUrl);

      if (fsRet == HTTP_UPDATE_OK) {
        fsUpdated = true;

        // Ghi version vào EEPROM CHỈ SAU KHI filesystem image update thành công.
        // Nhờ vậy nếu download/flash lỗi, lần boot sau vẫn tự retry.
        saveInstalledFsVersion(newFsVersionCode);

        USBSerial.println("[HTTPS-OTA] LittleFS update successful");

        // Không remount LittleFS ở đây. Partition vừa được ghi xong và nếu có
        // update thành công ta sẽ reboot một lần ở cuối. setupAudio() ở boot mới
        // sẽ mount filesystem mới sạch sẽ.
      } else {
        fsUpdateFailed = true;

        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("Voice Update Failed!");
        lcd.setCursor(0, 1); lcd.print("Error: "); lcd.print(httpUpdate.getLastError());
        lcd.setCursor(0, 2); lcd.print(httpUpdate.getLastErrorString());

        USBSerial.print("[HTTPS-OTA] LittleFS update failed: ");
        USBSerial.println(httpUpdate.getLastErrorString());

        // Nếu update FS lỗi, thử mount lại KHÔNG FORMAT để hệ thống cũ vẫn có
        // thể tiếp tục dùng voice nếu filesystem còn nguyên vẹn.
        resumeAudioAfterFilesystemOtaError();
        delay(3000);
      }
    }
  }

  // Nếu FW fail, FS đã được chủ động skip.
  if (needFsUpdate && !safeToUpdateFs) {
    USBSerial.println("[HTTPS-OTA] Filesystem update skipped because firmware update failed");
  }

  // Khôi phục default của HTTPUpdate cho các lần gọi khác trong tương lai.
  httpUpdate.rebootOnUpdate(true);

  // ========================================================
  // 4) FINAL RESULT / REBOOT MỘT LẦN
  // ========================================================
  if (fwUpdated || fsUpdated) {
    lcd.clear();
    printCentered(0, "UPDATE DONE");

    lcd.setCursor(0, 1);
    lcd.print("FW: ");
    if (fwUpdated) lcd.print("Updated");
    else           lcd.print("No Change");

    lcd.setCursor(0, 2);
    lcd.print("Voice: ");
    if (fsUpdated) lcd.print("Updated");
    else if (fsUpdateFailed) lcd.print("Failed");
    else lcd.print("No Change");

    lcd.setCursor(0, 3); lcd.print("Rebooting...");

    // Nếu FW đã update nhưng FS fail, vẫn reboot vào FW mới. Vì FS version
    // chưa được save, boot kế tiếp sẽ tự retry filesystem từ server.
    delay(1500);
    ESP.restart();
    return;
  }

  // Không có update nào thành công.
  if (fwUpdateFailed || fsUpdateFailed) {
    lcd.clear();
    printCentered(0, "OTA FAILED");
    if (fwUpdateFailed) {
      lcd.setCursor(0, 1); lcd.print("Firmware: Failed");
    }
    if (fsUpdateFailed) {
      lcd.setCursor(0, 2); lcd.print("Voice: Failed");
    }
    lcd.setCursor(0, 3); lcd.print("Running old version");
    delay(2500);
    return;
  }
}

// ==========================================
// HTTPS OTA - FILESYSTEM VERSION METADATA
// ==========================================
String versionCodeToName(uint32_t versionCode) {
  if (versionCode == 0) return "00.00.00";

  uint32_t patch = versionCode % 100UL;
  uint32_t minor = (versionCode / 100UL) % 100UL;
  uint32_t major = versionCode / 10000UL;

  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%02lu.%02lu.%02lu",
           (unsigned long)major,
           (unsigned long)minor,
           (unsigned long)patch);
  return String(buffer);
}

void loadInstalledFsVersion() {
  uint32_t magic = 0;
  uint32_t savedVersion = 0;

  EEPROM.get(addrFsVersionMagic, magic);
  EEPROM.get(addrFsVersionCode, savedVersion);

  if (magic == FS_VERSION_MAGIC) {
    installedFsVersionCode = savedVersion;
  } else {
    installedFsVersionCode = FACTORY_FS_VERSION_CODE;
  }

  USBSerial.print("[HTTPS-OTA] Installed FS version code: ");
  USBSerial.println(installedFsVersionCode);
}

void saveInstalledFsVersion(uint32_t versionCode) {
  EEPROM.put(addrFsVersionCode, versionCode);
  EEPROM.put(addrFsVersionMagic, FS_VERSION_MAGIC);
  EEPROM.commit();
  installedFsVersionCode = versionCode;

  USBSerial.print("[HTTPS-OTA] Saved FS version code: ");
  USBSerial.println(installedFsVersionCode);
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
          // AUTO: giữ nguyên logic cũ. Tổng thời gian Delay Connect bao gồm
          // 4.4s test 11 keys + 3s chờ ở step 61.
          // AUTO2: không test key, vì vậy chờ đủ delayConnect rồi đổi Tool.
          unsigned long waitTime = 0;

          if (skipManualKeyTest) {
              waitTime = delayConnect * 1000UL;
          } else {
              if (delayConnect * 1000UL > 7400UL) {
                  waitTime = delayConnect * 1000UL - 7400UL;
              }
          }
          
          if (currentMillis - previousMillis >= waitTime) {
              if (skipManualKeyTest) {
                  runStep = 7;
                  lcd.setCursor(0, 2); lcd.print("Changing Tool...    ");
              } else {
                  lcd.setCursor(0, 2); lcd.print("Testing 11 Keys...  ");
                  trigger11Keys();
                  previousMillis = millis();
                  runStep = 61;
              }
          }
      }
      break;

    case 61: 
      if (isRunManually) {
          if (currentMillis - previousMillis >= 3000UL) {
              runStep = 7;
              lcd.setCursor(0, 2); lcd.print("Changing Tool...    ");
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
        lcd.setCursor(0, 2); lcd.print("Change Complete...  ");
        if (isRunManually) queueToolVoice(currentPair);
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
  else if (currentScreen == 10) {
    printCentered(0, "Run Manually");
    lcd.setCursor(1, 1); lcd.print("Include Key Test");
    lcd.setCursor(1, 2); lcd.print("Skip Key Test");
    lcd.setCursor(0, cursorIndex + 1); lcd.print(">");
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


// ==========================================
// AUDIO - MAX98357A
// - Beep: chỉ cho DOWN / OK / + / - / RUN
// - Voice: sự kiện quan trọng / warning / error
// - Audio chạy trên FreeRTOS task riêng, không block ATE logic
// ==========================================
uint16_t readLE16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t readLE32(const uint8_t* p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

void setupAudio() {
  // Arduino-ESP32 2.0.17 dùng legacy I2S driver (driver/i2s.h).
  // MAX98357A chỉ cần BCLK + WS/LRC + DOUT từ ESP32-S3.
  i2s_config_t i2sConfig = {};
  i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sConfig.sample_rate = BUTTON_BEEP_SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 8;
  i2sConfig.dma_buf_len = 128;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;
  i2sConfig.fixed_mclk = 0;
  i2sConfig.mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT;
  i2sConfig.bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT;

  esp_err_t i2sErr = i2s_driver_install(AUDIO_I2S_PORT, &i2sConfig, 0, nullptr);
  if (i2sErr != ESP_OK) {
    USBSerial.print("[AUDIO] i2s_driver_install failed: ");
    USBSerial.println((int)i2sErr);
    audioI2SReady = false;
    return;
  }

  i2s_pin_config_t pinConfig = {};
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
  pinConfig.bck_io_num = I2S_BCLK_PIN;
  pinConfig.ws_io_num = I2S_LRC_PIN;
  pinConfig.data_out_num = I2S_DOUT_PIN;
  pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

  i2sErr = i2s_set_pin(AUDIO_I2S_PORT, &pinConfig);
  if (i2sErr != ESP_OK) {
    USBSerial.print("[AUDIO] i2s_set_pin failed: ");
    USBSerial.println((int)i2sErr);
    i2s_driver_uninstall(AUDIO_I2S_PORT);
    audioI2SReady = false;
    return;
  }

  i2sErr = i2s_set_clk(
    AUDIO_I2S_PORT,
    BUTTON_BEEP_SAMPLE_RATE,
    I2S_BITS_PER_SAMPLE_16BIT,
    I2S_CHANNEL_MONO
  );
  if (i2sErr != ESP_OK) {
    USBSerial.print("[AUDIO] i2s_set_clk failed: ");
    USBSerial.println((int)i2sErr);
    i2s_driver_uninstall(AUDIO_I2S_PORT);
    audioI2SReady = false;
    return;
  }

  i2s_zero_dma_buffer(AUDIO_I2S_PORT);
  audioI2SReady = true;

  // formatOnFail=true giúp tạo LittleFS lần đầu trên board mới.
  audioFSReady = LittleFS.begin(true);
  if (!audioFSReady) {
    USBSerial.println("[AUDIO] LittleFS mount failed - button beep still available, voice disabled");
  }

  audioBeepQueue = xQueueCreate(8, sizeof(uint8_t));
  audioVoiceQueue = xQueueCreate(8, sizeof(const char*));

  if (audioBeepQueue == nullptr || audioVoiceQueue == nullptr) {
    USBSerial.println("[AUDIO] Queue creation failed");
    audioI2SReady = false;
    return;
  }

  BaseType_t result = xTaskCreatePinnedToCore(
    audioTask,
    "ATE_Audio",
    8192,
    nullptr,
    1,
    &audioTaskHandle,
    0
  );

  if (result != pdPASS) {
    USBSerial.println("[AUDIO] Audio task creation failed");
    audioTaskHandle = nullptr;
    audioI2SReady = false;
  }
}

// ==========================================
// AUDIO / LITTLEFS - SAFE LOCAL OTA SUPPORT
// ==========================================
void writeAudioSilence() {
  if (!audioI2SReady) return;

  // Xóa DMA TX để tránh giữ lại sample cuối khi dừng audio.
  i2s_zero_dma_buffer(AUDIO_I2S_PORT);
}

bool pauseAudioAndUnmountLittleFSForOta() {
  // Ngăn request mới ngay lập tức.
  audioFsOtaPauseRequested = true;

  // Bỏ các beep/voice còn đang chờ. Voice đang phát sẽ tự thoát ở
  // playVoiceWavInternal(), đóng File, rồi audioTask xác nhận Paused.
  if (audioBeepQueue != nullptr) xQueueReset(audioBeepQueue);
  if (audioVoiceQueue != nullptr) xQueueReset(audioVoiceQueue);

  // Nếu audio task không tồn tại thì không có reader nào cần chờ.
  if (audioTaskHandle == nullptr || !audioI2SReady) {
    audioFsOtaPaused = true;
  } else {
    unsigned long startWait = millis();
    while (!audioFsOtaPaused && (millis() - startWait < AUDIO_FS_OTA_PAUSE_TIMEOUT_MS)) {
      delay(5);
    }
  }

  if (!audioFsOtaPaused) {
    USBSerial.println("[OTA-FS] Audio pause timeout - LittleFS NOT unmounted");
    return false;
  }

  // Chỉ unmount sau khi audio task đã xác nhận không còn giữ WAV mở.
  // Web UI cũng đọc static assets từ LittleFS, vì vậy phải dừng WebServer trước.
  stopAteWebServer();

  if (audioFSReady) {
    LittleFS.end();
    audioFSReady = false;
  }

  USBSerial.println("[OTA-FS] Audio stopped, LittleFS unmounted safely");
  return true;
}

void resumeAudioAfterFilesystemOtaError() {
  // OTA lỗi: thử mount lại mà KHÔNG format để không làm mất dữ liệu còn lại.
  if (!audioFSReady) {
    audioFSReady = LittleFS.begin(false);
  }

  if (audioFSReady) {
    USBSerial.println("[OTA-FS] LittleFS remounted after OTA error");
  } else {
    USBSerial.println("[OTA-FS] LittleFS remount failed after OTA error - voice disabled");
  }

  // Cho phép audio task chạy lại. Beep vẫn hoạt động kể cả khi FS mount fail;
  // voice chỉ hoạt động khi audioFSReady == true.
  audioFsOtaPauseRequested = false;

  unsigned long startWait = millis();
  while (audioFsOtaPaused && (millis() - startWait < 500UL)) {
    delay(5);
  }

  // Web vẫn có thể chạy cả khi FS mount fail (sẽ hiện fallback page + API status).
  if (WiFi.status() == WL_CONNECTED && !wifiPortalActive && !localOtaInProgress) {
    setupAteWebServer();
  }

  if (audioFSReady) queueVoice(VOICE_OTA_FAILED);
}

void requestButtonBeep() {
  if (!audioI2SReady || audioBeepQueue == nullptr || audioFsOtaPauseRequested) return;
  uint8_t token = 1;
  xQueueSend(audioBeepQueue, &token, 0); // Không block main loop nếu queue đầy.
}

void queueVoice(const char* filePath) {
  if (!audioI2SReady || !audioFSReady || audioVoiceQueue == nullptr || filePath == nullptr || audioFsOtaPauseRequested) return;
  xQueueSend(audioVoiceQueue, &filePath, 0); // Không block ATE logic.
}

void queueToolVoice(int toolIndex) {
  if (toolIndex == 0) queueVoice(VOICE_TOOL_1);
  else if (toolIndex == 1) queueVoice(VOICE_TOOL_2);
  else if (toolIndex == 2) queueVoice(VOICE_TOOL_3);
}

void playButtonBeepInternal() {
  if (!audioI2SReady || audioFsOtaPauseRequested) return;

  if (i2s_set_clk(
        AUDIO_I2S_PORT,
        BUTTON_BEEP_SAMPLE_RATE,
        I2S_BITS_PER_SAMPLE_16BIT,
        I2S_CHANNEL_MONO) != ESP_OK) {
    return;
  }

  const uint32_t totalSamples = (BUTTON_BEEP_SAMPLE_RATE * BUTTON_BEEP_DURATION_MS) / 1000UL;
  const uint32_t fadeSamples = BUTTON_BEEP_SAMPLE_RATE / 200UL; // ~5 ms fade in/out
  const float phaseStep = 2.0f * PI * (float)BUTTON_BEEP_FREQ_HZ / (float)BUTTON_BEEP_SAMPLE_RATE;

  int16_t buffer[128];
  uint32_t generated = 0;

  while (generated < totalSamples) {
    if (audioFsOtaPauseRequested) break;

    uint32_t chunkSamples = min((uint32_t)128, totalSamples - generated);

    for (uint32_t i = 0; i < chunkSamples; i++) {
      uint32_t sampleIndex = generated + i;
      float envelope = 1.0f;

      if (sampleIndex < fadeSamples) {
        envelope = (float)sampleIndex / (float)fadeSamples;
      } else if (sampleIndex > totalSamples - fadeSamples) {
        envelope = (float)(totalSamples - sampleIndex) / (float)fadeSamples;
      }

      float sample = sinf(phaseStep * (float)sampleIndex);
      buffer[i] = (int16_t)((float)BUTTON_BEEP_AMPLITUDE * envelope * sample);
    }

    size_t bytesWritten = 0;
    esp_err_t err = i2s_write(
      AUDIO_I2S_PORT,
      buffer,
      chunkSamples * sizeof(int16_t),
      &bytesWritten,
      portMAX_DELAY
    );
    if (err != ESP_OK) break;

    generated += chunkSamples;
  }
}

bool parseWavHeader(File& file, uint32_t& sampleRate, uint16_t& channels, uint16_t& bitsPerSample, uint32_t& dataSize) {
  sampleRate = 0;
  channels = 0;
  bitsPerSample = 0;
  dataSize = 0;

  uint8_t riffHeader[12];
  if (file.read(riffHeader, sizeof(riffHeader)) != sizeof(riffHeader)) return false;
  if (memcmp(riffHeader, "RIFF", 4) != 0 || memcmp(riffHeader + 8, "WAVE", 4) != 0) return false;

  bool fmtFound = false;

  while (file.available()) {
    uint8_t chunkHeader[8];
    if (file.read(chunkHeader, sizeof(chunkHeader)) != sizeof(chunkHeader)) return false;

    uint32_t chunkSize = readLE32(chunkHeader + 4);

    if (memcmp(chunkHeader, "fmt ", 4) == 0) {
      if (chunkSize < 16) return false;

      uint8_t fmt[16];
      if (file.read(fmt, sizeof(fmt)) != sizeof(fmt)) return false;

      uint16_t audioFormat = readLE16(fmt + 0);
      channels = readLE16(fmt + 2);
      sampleRate = readLE32(fmt + 4);
      bitsPerSample = readLE16(fmt + 14);

      // Chỉ hỗ trợ WAV PCM chuẩn để playback nhẹ và ổn định.
      if (audioFormat != 1) return false;

      if (chunkSize > 16) file.seek(file.position() + (chunkSize - 16));
      if (chunkSize & 1U) file.seek(file.position() + 1); // RIFF chunk padding
      fmtFound = true;
    }
    else if (memcmp(chunkHeader, "data", 4) == 0) {
      if (!fmtFound) return false;
      dataSize = chunkSize;
      return true; // File pointer đang đứng đúng đầu PCM data.
    }
    else {
      file.seek(file.position() + chunkSize + (chunkSize & 1U));
    }
  }

  return false;
}

bool playVoiceWavInternal(const char* filePath) {
  if (!audioI2SReady || !audioFSReady || filePath == nullptr || audioFsOtaPauseRequested) return false;

  File file = LittleFS.open(filePath, "r");
  if (!file) {
    USBSerial.print("[AUDIO] Missing voice file: ");
    USBSerial.println(filePath);
    return false;
  }

  uint32_t sampleRate = 0;
  uint16_t channels = 0;
  uint16_t bitsPerSample = 0;
  uint32_t dataSize = 0;

  if (!parseWavHeader(file, sampleRate, channels, bitsPerSample, dataSize)) {
    USBSerial.print("[AUDIO] Invalid WAV: ");
    USBSerial.println(filePath);
    file.close();
    return false;
  }

  if (channels != 1 || bitsPerSample != 16 || sampleRate < 8000 || sampleRate > 96000) {
    USBSerial.print("[AUDIO] WAV must be PCM 16-bit MONO: ");
    USBSerial.println(filePath);
    file.close();
    return false;
  }

  if (i2s_set_clk(
        AUDIO_I2S_PORT,
        sampleRate,
        I2S_BITS_PER_SAMPLE_16BIT,
        I2S_CHANNEL_MONO) != ESP_OK) {
    file.close();
    return false;
  }

  uint8_t buffer[1024];
  uint32_t remaining = dataSize;

  while (remaining > 0 && file.available()) {
    // Filesystem OTA được ưu tiên tuyệt đối: dừng playback, thoát vòng lặp
    // và đóng File trước khi audioTask báo Paused cho OTA callback.
    if (audioFsOtaPauseRequested) break;

    // Button feedback được ưu tiên cả khi đang phát voice.
    uint8_t beepToken = 0;
    if (audioBeepQueue != nullptr && xQueueReceive(audioBeepQueue, &beepToken, 0) == pdTRUE) {
      playButtonBeepInternal();
      i2s_set_clk(
        AUDIO_I2S_PORT,
        sampleRate,
        I2S_BITS_PER_SAMPLE_16BIT,
        I2S_CHANNEL_MONO
      );
    }

    size_t toRead = min((uint32_t)sizeof(buffer), remaining);
    // 16-bit PCM => luôn write số byte chẵn.
    if (toRead & 1U) toRead--;
    if (toRead == 0) break;

    size_t bytesRead = file.read(buffer, toRead);
    if (bytesRead == 0) break;
    if (bytesRead & 1U) bytesRead--;

    if (VOICE_VOLUME_PERCENT < 100) {
      int16_t* samples = reinterpret_cast<int16_t*>(buffer);
      size_t sampleCount = bytesRead / sizeof(int16_t);
      for (size_t i = 0; i < sampleCount; i++) {
        int32_t scaled = ((int32_t)samples[i] * VOICE_VOLUME_PERCENT) / 100;
        samples[i] = (int16_t)scaled;
      }
    }

    size_t bytesWritten = 0;
    esp_err_t writeErr = i2s_write(
      AUDIO_I2S_PORT,
      buffer,
      bytesRead,
      &bytesWritten,
      portMAX_DELAY
    );
    if (writeErr != ESP_OK || bytesWritten == 0) break;

    remaining -= min((uint32_t)bytesRead, (uint32_t)bytesWritten);
  }

  file.close();
  return true;
}

void audioTask(void* parameter) {
  (void)parameter;

  for (;;) {
    // ======================================================
    // LITTLEFS OTA PAUSE
    // ======================================================
    // Khi onStart() yêu cầu pause, playback hiện tại đã được yêu cầu thoát
    // và đóng File. Tới đây task gửi silence rồi xác nhận an toàn để callback
    // có thể LittleFS.end(). Trong suốt OTA filesystem không đọc/ghi audio.
    if (audioFsOtaPauseRequested) {
      if (!audioFsOtaPaused) {
        writeAudioSilence();
        audioFsOtaPaused = true;
      }

      while (audioFsOtaPauseRequested) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }

      audioFsOtaPaused = false;
      continue;
    }

    // 1) Beep nút luôn ưu tiên cao hơn voice đang chờ.
    uint8_t beepToken = 0;
    if (audioBeepQueue != nullptr && xQueueReceive(audioBeepQueue, &beepToken, 0) == pdTRUE) {
      playButtonBeepInternal();
      continue;
    }

    // 2) Voice sự kiện quan trọng.
    const char* voicePath = nullptr;
    if (audioVoiceQueue != nullptr &&
        xQueueReceive(audioVoiceQueue, &voicePath, pdMS_TO_TICKS(20)) == pdTRUE) {
      playVoiceWavInternal(voicePath);
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void detachServoSafe(int index) {
  usbServos[index].detach();                
  pinMode(USB_SERVO_PINS[index], OUTPUT);   
  digitalWrite(USB_SERVO_PINS[index], LOW); 
}
