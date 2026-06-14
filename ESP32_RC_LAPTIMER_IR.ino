/*
 * ==============================================================================
 * RC Car Lap Timer (Photoelectric Sensor & Variable Deadtime - Fixed Auto-Ready)
 * ==============================================================================
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>

// --- BLE設定 ---
#define BLE_NAME "RC_LAPTIMER"
#define SERVICE_UUID "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcdefab-1234-5678-1234-abcdefabcdef"

// --- ピン定義 ---
const int PHOTO_PIN = 26;     // 光電センサ入力ピン
const int TH_VR_PIN = 32;     // 反応時間調整ボリューム (0 ~ 200ms)
const int ST_VR_PIN = 33;     // デッドタイム調整ボリューム (0.0 ~ 5.0秒)
const int LED_PIN = 17;
const int SETTING_PIN = 14;   // 設定モード切り替えスライドスイッチ
const int I2C_SDA = 21;       // LCD用I2C
const int I2C_SCL = 22;       // LCD用I2C

// --- グローバル変数 ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

const int UPDATE_LCD = 250;
const float BEST_TIME_MAX = 99999.99;

// ボリュームからの設定保持用
float sensingThreshMs = 0.0;  // 反応時間 (0~200ms)
float deadTimeSec = 0.0;      // デッドタイム (0.0~5.0s)
unsigned long deadTimeMs = 0;

// 割り込み・センシング制御用
volatile bool isSensorTriggered = false;
volatile unsigned long isrTriggerTime = 0;
unsigned long sensorTriggerTime = 0;
volatile bool isSensing = false;

unsigned long lastLapTime = 0;
float currentLap = 0.0;
float bestTime = BEST_TIME_MAX;
float prevLap = 0.0;
bool isFirstRun = true;
volatile bool isTriggered = true;      // 起動時はラップ可能(Ready)なので true(1) からスタート

// --- 非ブロッキングLED点滅用制御変数 ---
int ledFlashCount = 0;
unsigned long nextLedToggleTime = 0;
int ledState = LOW;

// --- BLEコールバック ---
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println(F("BLE Connected"));
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println(F("BLE Disconnect"));
  }
};

// --- 光電センサ 割り込みサービスルーチン (ISR) ---
void IRAM_ATTR handleSensorInterrupt() {
  // まだ判定中(isSensing)でなく、かつラップ可能状態(isTriggered == true)のときのみトリガーを受け付ける
  if (!isSensing && isTriggered) {
    isSensorTriggered = true;
    isrTriggerTime = millis();
  }
}

void flashLEDBlocking(int count) {
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(50);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
}

void triggerFlashLED(int count) {
  if (count <= 0) return;
  ledFlashCount = count * 2;
  digitalWrite(LED_PIN, HIGH);
  ledState = HIGH;
  nextLedToggleTime = millis() + 50;
}

void updateLED() {
  if (ledFlashCount > 0) {
    if (millis() >= nextLedToggleTime) {
      if (ledState == HIGH) {
        digitalWrite(LED_PIN, LOW);
        ledState = LOW;
        nextLedToggleTime = millis() + 100;
      } else {
        digitalWrite(LED_PIN, HIGH);
        ledState = HIGH;
        nextLedToggleTime = millis() + 50;
      }
      ledFlashCount--;
      if (ledFlashCount == 0) {
        digitalWrite(LED_PIN, LOW);
        ledState = LOW;
      }
    }
  }
}

void formatTime(float time_sec, char* outBuf) {
  if (time_sec >= 3540.0 || time_sec <= 0) {
    strcpy(outBuf, "00:00.00     ");
    return;
  }
  int m = (int)(time_sec / 60);
  int s = (int)time_sec % 60;
  int cs = (int)(time_sec * 100) % 100;
  sprintf(outBuf, "%02d:%02d.%02d     ", m, s, cs);
}

// --- 表示更新 ---
void updateLCD(float lapTime, float bestTime) {
  char timeBuf[16];
  bool settingMode = (digitalRead(SETTING_PIN) == LOW);
  static bool lastSettingMode = !settingMode;
  static bool lastFirstRun = true;

  if (settingMode != lastSettingMode || isFirstRun != lastFirstRun) {
    lcd.clear();
    lastSettingMode = settingMode;
    lastFirstRun = isFirstRun;
    if (!settingMode && !isFirstRun) {
      lcd.setCursor(0, 0);
      lcd.print(F("TIME :"));
      lcd.setCursor(0, 1);
      lcd.print(F("BEST :"));
      lcd.setCursor(0, 2);
      lcd.print(F("CURR :"));
      lcd.setCursor(0, 3);
      lcd.print(F("PREV :"));
    }
  }

  if (settingMode) {
    // スライドスイッチON時の設定確認モード
    lcd.setCursor(0, 0);
    lcd.print(F("--- SETTING MODE ---"));
    lcd.setCursor(19, 0);
    lcd.print(isTriggered == 0 ? F("_") : F("^")); // 0のとき"_"、1のとき"^"
    lcd.setCursor(19, 3);
    
    lcd.setCursor(0, 1);
    lcd.print(F("SENSOR TH: "));
    lcd.print(sensingThreshMs, 1);
    lcd.print(F(" ms  "));
    
    lcd.setCursor(0, 2);
    lcd.print(F("DEAD TIME: "));
    lcd.print(deadTimeSec, 2);
    lcd.print(F(" s  "));
    
    lcd.setCursor(0, 3);
    lcd.print(F("SENSOR   : "));
    lcd.print(digitalRead(PHOTO_PIN) == LOW ? F("BLOCK (H)") : F("CLEAR (L)"));
  } else if (isFirstRun) {
    lcd.setCursor(0, 0);
    lcd.print(F("--- RC LAP TIMER ---"));
    lcd.setCursor(0, 1);
    lcd.print(F("PASS THE PHOTO SENSOR"));
    lcd.setCursor(0, 2);
    lcd.print(F("TO START"));
    lcd.setCursor(0, 3);
    lcd.print(F("THE MEASUREMENT."));
  } else {
    float lap = (millis() - lastLapTime) / 1000.0;
    formatTime(lap, timeBuf);
    lcd.setCursor(7, 0);
    lcd.print(timeBuf);
    formatTime(bestTime, timeBuf);
    lcd.setCursor(7, 1);
    lcd.print(timeBuf);
    formatTime(lapTime, timeBuf);
    lcd.setCursor(7, 2);
    lcd.print(timeBuf);
    formatTime(prevLap, timeBuf);
    lcd.setCursor(7, 3);
    lcd.print(timeBuf);
    lcd.setCursor(19, 0);
    lcd.print(isTriggered == 0 ? F("_") : F("^"));
  }
}

void BLESetup() {
  BLEDevice::init(BLE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE);
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();
}

void setup() {
  Serial.begin(115200);
  pinMode(TH_VR_PIN, INPUT);
  pinMode(ST_VR_PIN, INPUT);
  
  // 光電センサピン設定 (PNP)
  pinMode(PHOTO_PIN, INPUT);
  // 遮光開始（立ち上がりエッジ）で割り込みを設定
  attachInterrupt(digitalPinToInterrupt(PHOTO_PIN), handleSensorInterrupt, FALLING);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(SETTING_PIN, INPUT_PULLUP);

  // LCD用のI2C初期化
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(100); // I2Cのタイムアウトを設定してハングアップ防止
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(F("--- RC LAP TIMER ---"));
  lcd.setCursor(0, 1);
  lcd.print(F("Initialize..."));

  BLESetup();
  flashLEDBlocking(2);
  lcd.clear();
  
  isTriggered = true; // 起動時はラップ可能(Ready)状態で開始
  lastLapTime = millis();
  Serial.println(F("\n--- SYSTEM READY (PHOTO) ---"));
}

void loop() {
  // 1. ボリューム値の周期読み取りとノイズフィルタリング
  static unsigned long lastAdcReadTime = 0;
  if (millis() - lastAdcReadTime >= 100) {
    lastAdcReadTime = millis();
    float rawSensingThresh = (analogRead(TH_VR_PIN) / 4095.0) * 200.0;
    float rawDeadTimeSec = (analogRead(ST_VR_PIN) / 4095.0) * 5.0;
    
    static bool adcInitialized = false;
    if (!adcInitialized) {
      sensingThreshMs = rawSensingThresh;
      deadTimeSec = rawDeadTimeSec;
      adcInitialized = true;
    } else {
      sensingThreshMs = (sensingThreshMs * 0.9) + (rawSensingThresh * 0.1);
      deadTimeSec = (deadTimeSec * 0.9) + (rawDeadTimeSec * 0.1);
    }
    deadTimeMs = (unsigned long)(deadTimeSec * 1000.0);
  }

  // 2. 割り込み検知時の判定開始処理（排他制御）
  bool triggered = false;
  unsigned long triggerTime = 0;
  
  if (isSensorTriggered) {
    noInterrupts();
    triggered = isSensorTriggered;
    if (triggered) {
      isSensorTriggered = false;
      triggerTime = isrTriggerTime;
    }
    interrupts();
  }

  if (triggered) {
    sensorTriggerTime = triggerTime;
    isSensing = true; // 連続遮光時間の計測モードに入る
  }

  // 3. 反応時間の検証ロジック
  if (isSensing) {
    if (digitalRead(PHOTO_PIN) == HIGH) {
      // 指定時間内に光が復帰した（受光状態に戻った）場合はノイズとみなしてキャンセル
      isSensing = false;
    } else if (millis() - sensorTriggerTime >= (unsigned long)sensingThreshMs) {
      isSensing = false;
      
      // ラップ可能状態(isTriggered == true)のときのみ確定処理を行う
      if (isTriggered) {
        unsigned long now = sensorTriggerTime; // 割り込み発生時の正確なタイムスタンプを使用
        
        if (!isFirstRun) {
          isTriggered = false; // ★デッドタイム開始のためフラグをfalse(0)にする -> 表示は「_」になる
          prevLap = currentLap;
          currentLap = (now - lastLapTime) / 1000.0;
          if (currentLap < bestTime) bestTime = currentLap;
          
          // BLE送信
          if (deviceConnected) {
            char bleBuf[32];
            snprintf(bleBuf, sizeof(bleBuf), "Lap:%6.2f", currentLap);
            pCharacteristic->setValue(bleBuf);
            pCharacteristic->notify();
          }
          triggerFlashLED(3);
          lastLapTime = now;
        } else {
          // 初回通過（計測開始）
          isFirstRun = false;
          isTriggered = false; // 初回通過直後もデッドタイムを適用し、不可状態(0)にする
          lastLapTime = now;
          triggerFlashLED(1);
          Serial.println(F("Measurement Started"));
        }
      }
    }
  }

  // 4. デッドタイム終了 ＆ センサー復帰（★時間経過のみで確実に復帰するよう修正）
  // 現在デッドタイム中(isTriggered == false)で、設定されたデッドタイム時間を超えたら自動でReadyにする
  if (!isTriggered && (millis() - lastLapTime >= deadTimeMs)) {
    isTriggered = true; // ★ラップ可能状態に戻す(1) -> 表示が「^」になる
    triggerFlashLED(1);
  }

  // LEDの非ブロッキング更新処理
  updateLED();

  // LCDの定期更新
  static unsigned long lastLcd = 0;
  if (millis() - lastLcd > UPDATE_LCD) {
    updateLCD(currentLap, bestTime);
    lastLcd = millis();
  }

  // BLE接続維持処理（非ブロッキング）
  static unsigned long disconnectTime = 0;
  static bool isAdvertisingPending = false;

  if (!deviceConnected && oldDeviceConnected) {
    disconnectTime = millis();
    isAdvertisingPending = true;
    oldDeviceConnected = false;
    Serial.println(F("BLE Disconnected. Pending readvertising..."));
  }

  if (isAdvertisingPending) {
    if (millis() - disconnectTime >= 500) {
      pServer->getAdvertising()->start();
      isAdvertisingPending = false;
      Serial.println(F("BLE Advertising restarted"));
    }
  }

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  delay(1);
}