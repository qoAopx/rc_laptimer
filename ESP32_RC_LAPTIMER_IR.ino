/*
 * ==============================================================================
 * RC Car Lap Timer (Photoelectric Sensor & Variable Deadtime)
 * [Revised Version]
 *
 * 主な修正点:
 *  - ISR内のmillis()をesp_timer_get_time()に変更（μs精度・安全）
 *  - volatile変数の読み取りを最初からnoInterrupts()で保護（データ競合解消）
 *  - ラップタイムをfloatでなくunsigned long(ms)で保持（精度損失防止）
 *  - デッドタイム最小値を設けてdead=0ms時のバグを修正
 *  - マジックナンバーを定数化
 *  - BLEコールバックをstaticインスタンスに変更（メモリリーク防止）
 *  - SETTING_PINにデバウンス処理を追加（LCD chatter防止）
 *  - formatTime()をsnprintfに変更しバッファサイズを明示
 *  - 時刻ソースをすべてμs(int64_t)に統一し、ms変換はその都度実施
 * ==============================================================================
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include "esp_timer.h"  // esp_timer_get_time() 用

// --- BLE設定 ---
#define BLE_NAME        "RC_LAPTIMER"
#define SERVICE_UUID    "12345678-1234-1234-1234-1234567890ab"
#define CHAR_UUID       "abcdefab-1234-5678-1234-abcdefabcdef"

// --- ピン定義 ---
const int PHOTO_PIN    = 26;  // 光電センサ入力ピン
const int TH_VR_PIN    = 32;  // 反応時間調整ボリューム (0～200ms)
const int ST_VR_PIN    = 33;  // デッドタイム調整ボリューム (0.0～5.0s)
const int LED_PIN      = 17;
const int SETTING_PIN  = 14;  // 設定モード切り替えスライドスイッチ
const int I2C_SDA      = 21;
const int I2C_SCL      = 22;

// --- 定数 ---
const int            ADC_READ_INTERVAL_MS   = 100;   // ADC読み取り周期
const int            LCD_UPDATE_INTERVAL_MS = 250;   // LCD更新周期
const unsigned long  MIN_DEAD_TIME_MS       = 500;   // デッドタイム最小値（ボリューム最小でも500ms）
const unsigned long  BLE_RECONNECT_DELAY_MS = 500;   // BLE再接続待ち時間
const int            LED_ON_MS              = 50;    // LED点灯時間
const int            LED_OFF_MS             = 100;   // LED消灯時間
const int            SETUP_FLASH_COUNT      = 2;     // 起動時LEDフラッシュ回数
const int            SWITCH_DEBOUNCE_MS     = 50;    // スライドスイッチデバウンス時間
const float          BEST_TIME_INIT_MS      = 99999000.0f; // ベストタイム初期値(ms)

// --- グローバル変数 ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
BLEServer*         pServer         = NULL;
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected    = false;
bool oldDeviceConnected = false;

// ボリューム設定値
float         sensingThreshMs = 0.0f;
float         deadTimeSec     = 0.0f;
unsigned long deadTimeMs      = MIN_DEAD_TIME_MS;

// ISRとmainループ間の共有変数（volatile）
// ★時刻はすべてesp_timer_get_time()のμs(int64_t)で統一
volatile bool    isSensorTriggered = false;
volatile int64_t isrTriggerTimeUs  = 0;  // μs単位（esp_timer_get_time()）

bool    isSensing         = false; // ISRが発火してからセンサー確定待ち中フラグ
int64_t sensorTriggerUs   = 0;    // 確定したトリガー時刻(μs)

// ラップタイム管理（ms単位で保持して精度損失を防ぐ）
int64_t       lastLapTimeUs  = 0;       // lastLapTimeをμs単位で保持
unsigned long currentLapMs   = 0;
unsigned long bestTimeMs      = (unsigned long)BEST_TIME_INIT_MS;
unsigned long prevLapMs       = 0;
bool          isFirstRun      = true;
volatile bool isTriggered     = true;   // 起動時はReady(true)

// --- 非ブロッキングLED制御 ---
int           ledFlashCount    = 0;
unsigned long nextLedToggleMs  = 0;
int           ledState         = LOW;

// --- デバウンス済みスイッチ状態 ---
bool          settingMode      = false;
bool          rawSettingMode   = false;
unsigned long lastSwitchMs     = 0;

// ============================================================
// BLEコールバック（staticインスタンスでメモリリークを防止）
// ============================================================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pSrv) override {
    deviceConnected = true;
    Serial.println(F("BLE Connected"));
  }
  void onDisconnect(BLEServer* pSrv) override {
    deviceConnected = false;
    Serial.println(F("BLE Disconnected"));
  }
};
static MyServerCallbacks bleCallbacks; // スタティックインスタンス

// ============================================================
// 光電センサ ISR
// ★ millis()はISR内で使用禁止 → esp_timer_get_time()を使用
// ★ isSensingはISR外で書き換えるのでvolatile不要だが、
//    isTriggeredはISR内で参照するためvolatileが必須
// ============================================================
void IRAM_ATTR handleSensorInterrupt() {
  if (!isSensing && isTriggered) {
    isSensorTriggered = true;
    isrTriggerTimeUs  = esp_timer_get_time(); // μs精度・ISR安全
  }
}

// ============================================================
// LED制御
// ============================================================

// setup()専用のブロッキングフラッシュ（初期化確認用）
void flashLEDBlocking(int count) {
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(LED_ON_MS);
    digitalWrite(LED_PIN, LOW);
    delay(LED_OFF_MS);
  }
}

// loop()内で使う非ブロッキングフラッシュ開始
void triggerFlashLED(int count) {
  if (count <= 0) return;
  ledFlashCount     = count * 2;
  digitalWrite(LED_PIN, HIGH);
  ledState          = HIGH;
  nextLedToggleMs   = millis() + LED_ON_MS;
}

// loop()内で毎回呼ぶLED状態更新
void updateLED() {
  if (ledFlashCount <= 0) return;
  if (millis() >= nextLedToggleMs) {
    if (ledState == HIGH) {
      digitalWrite(LED_PIN, LOW);
      ledState        = LOW;
      nextLedToggleMs = millis() + LED_OFF_MS;
    } else {
      digitalWrite(LED_PIN, HIGH);
      ledState        = HIGH;
      nextLedToggleMs = millis() + LED_ON_MS;
    }
    ledFlashCount--;
    if (ledFlashCount == 0) {
      digitalWrite(LED_PIN, LOW);
      ledState = LOW;
    }
  }
}

// ============================================================
// 時刻フォーマット（snprintfでバッファサイズを明示）
// ============================================================
void formatTime(unsigned long time_ms, char* outBuf, size_t bufSize) {
  // 0または異常値は "00:00.00" で表示
  if (time_ms == 0 || time_ms >= 3540000UL) {
    snprintf(outBuf, bufSize, "00:00.00     ");
    return;
  }
  unsigned long s_total = time_ms / 1000;
  int m  = (int)(s_total / 60);
  int s  = (int)(s_total % 60);
  int cs = (int)((time_ms % 1000) / 10); // センチ秒（0.01秒単位）
  snprintf(outBuf, bufSize, "%02d:%02d.%02d     ", m, s, cs);
}

// ============================================================
// スライドスイッチのデバウンス読み取り
// ============================================================
void updateSwitchDebounce() {
  bool raw = (digitalRead(SETTING_PIN) == LOW);
  if (raw != rawSettingMode) {
    rawSettingMode = raw;
    lastSwitchMs   = millis();
  }
  // デバウンス時間が経過したら確定
  if (millis() - lastSwitchMs >= (unsigned long)SWITCH_DEBOUNCE_MS) {
    settingMode = rawSettingMode;
  }
}

// ============================================================
// LCD表示更新
// ============================================================
void updateLCD(unsigned long lapMs, unsigned long bestMs) {
  char timeBuf[16];
  static bool lastSettingMode = !settingMode;
  static bool lastFirstRun    = true;

  // モードが切り替わったときだけ画面をクリアして固定ラベルを再描画
  if (settingMode != lastSettingMode || isFirstRun != lastFirstRun) {
    lcd.clear();
    lastSettingMode = settingMode;
    lastFirstRun    = isFirstRun;
    if (!settingMode && !isFirstRun) {
      lcd.setCursor(0, 0); lcd.print(F("TIME :"));
      lcd.setCursor(0, 1); lcd.print(F("BEST :"));
      lcd.setCursor(0, 2); lcd.print(F("CURR :"));
      lcd.setCursor(0, 3); lcd.print(F("PREV :"));
    }
  }

  if (settingMode) {
    // 設定確認モード
    lcd.setCursor(0, 0);
    lcd.print(F("--- SETTING MODE ---"));
    lcd.setCursor(19, 0);
    lcd.print(isTriggered ? F("^") : F("_"));

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
    lcd.setCursor(0, 0); lcd.print(F("--- RC LAP TIMER ---"));
    lcd.setCursor(0, 1); lcd.print(F("PASS THE PHOTO SENSOR"));
    lcd.setCursor(0, 2); lcd.print(F("TO START"));
    lcd.setCursor(0, 3); lcd.print(F("THE MEASUREMENT."));

  } else {
    // 計測中：現在の経過時間をリアルタイム表示
    unsigned long elapsedMs = (unsigned long)((esp_timer_get_time() - lastLapTimeUs) / 1000LL);

    formatTime(elapsedMs, timeBuf, sizeof(timeBuf));
    lcd.setCursor(7, 0); lcd.print(timeBuf);

    formatTime(bestMs, timeBuf, sizeof(timeBuf));
    lcd.setCursor(7, 1); lcd.print(timeBuf);

    formatTime(lapMs, timeBuf, sizeof(timeBuf));
    lcd.setCursor(7, 2); lcd.print(timeBuf);

    formatTime(prevLapMs, timeBuf, sizeof(timeBuf));
    lcd.setCursor(7, 3); lcd.print(timeBuf);

    lcd.setCursor(19, 0);
    lcd.print(isTriggered ? F("^") : F("_"));
  }
}

// ============================================================
// BLE初期化
// ============================================================
void BLESetup() {
  BLEDevice::init(BLE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(&bleCallbacks); // staticインスタンスを使用

  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY |
    BLECharacteristic::PROPERTY_INDICATE
  );
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);

  pinMode(TH_VR_PIN,   INPUT);
  pinMode(ST_VR_PIN,   INPUT);
  pinMode(PHOTO_PIN,   INPUT);
  pinMode(LED_PIN,     OUTPUT);
  pinMode(SETTING_PIN, INPUT_PULLUP);
  digitalWrite(LED_PIN, LOW);

  // 割り込み設定（FALLINGエッジで遮光検出）
  attachInterrupt(digitalPinToInterrupt(PHOTO_PIN), handleSensorInterrupt, FALLING);

  // LCD初期化
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(100);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print(F("--- RC LAP TIMER ---"));
  lcd.setCursor(0, 1); lcd.print(F("Initialize..."));

  BLESetup();
  flashLEDBlocking(SETUP_FLASH_COUNT);
  lcd.clear();

  // 初期状態：Ready（ラップ可能）
  isTriggered   = true;
  lastLapTimeUs = esp_timer_get_time(); // μsで記録

  Serial.println(F("\n--- SYSTEM READY ---"));
}

// ============================================================
// Loop
// ============================================================
void loop() {

  // ----------------------------------------------------------
  // 1. スイッチデバウンス更新
  // ----------------------------------------------------------
  updateSwitchDebounce();

  // ----------------------------------------------------------
  // 2. ADCボリューム読み取り（100ms周期・EWMAフィルタ）
  // ----------------------------------------------------------
  static unsigned long lastAdcMs = 0;
  if (millis() - lastAdcMs >= (unsigned long)ADC_READ_INTERVAL_MS) {
    lastAdcMs = millis();

    float rawThresh   = (analogRead(TH_VR_PIN) / 4095.0f) * 200.0f;
    float rawDeadTime = (analogRead(ST_VR_PIN)  / 4095.0f) * 5.0f;

    static bool adcInitialized = false;
    if (!adcInitialized) {
      sensingThreshMs = rawThresh;
      deadTimeSec     = rawDeadTime;
      adcInitialized  = true;
    } else {
      sensingThreshMs = sensingThreshMs * 0.9f + rawThresh   * 0.1f;
      deadTimeSec     = deadTimeSec     * 0.9f + rawDeadTime * 0.1f;
    }
    // ★デッドタイムに最小値を設けてdead=0ms時のバグを防止
    deadTimeMs = max((unsigned long)(deadTimeSec * 1000.0f), MIN_DEAD_TIME_MS);
  }

  // ----------------------------------------------------------
  // 3. ISRトリガーの排他的読み取り
  // ★ 最初からnoInterrupts()で囲んでデータ競合を完全解消
  // ----------------------------------------------------------
  bool    triggered   = false;
  int64_t triggerUs   = 0;

  noInterrupts();
  if (isSensorTriggered) {
    triggered         = true;
    isSensorTriggered = false;
    triggerUs         = isrTriggerTimeUs;
  }
  interrupts();

  if (triggered) {
    sensorTriggerUs = triggerUs;
    isSensing       = true; // 連続遮光時間の計測モード開始
  }

  // ----------------------------------------------------------
  // 4. 反応時間の検証（isSensingフェーズ）
  // ----------------------------------------------------------
  if (isSensing) {
    if (digitalRead(PHOTO_PIN) == HIGH) {
      // 遮光時間内に光が復帰 → ノイズとしてキャンセル
      isSensing = false;
    } else {
      int64_t elapsedUs = esp_timer_get_time() - sensorTriggerUs;
      if (elapsedUs >= (int64_t)(sensingThreshMs * 1000.0f)) {
        // 規定時間以上の遮光 → ラップ確定
        isSensing = false;

        if (isTriggered) {
          int64_t nowUs = sensorTriggerUs; // 割り込み発生時刻を使用

          if (!isFirstRun) {
            isTriggered  = false; // デッドタイム開始
            prevLapMs    = currentLapMs;
            currentLapMs = (unsigned long)((nowUs - lastLapTimeUs) / 1000LL);
            if (currentLapMs < bestTimeMs) bestTimeMs = currentLapMs;

            // BLE送信（秒単位のfloatに変換して送信）
            if (deviceConnected) {
              char bleBuf[32];
              float lapSec = currentLapMs / 1000.0f;
              snprintf(bleBuf, sizeof(bleBuf), "Lap:%6.2f", lapSec);
              pCharacteristic->setValue(bleBuf);
              pCharacteristic->notify();
            }

            triggerFlashLED(3);
            lastLapTimeUs = nowUs;

            // シリアルログ（デバッグ用）
            Serial.printf("Lap: %lu ms (Best: %lu ms)\n", currentLapMs, bestTimeMs);

          } else {
            // 初回通過（計測開始）
            isFirstRun    = false;
            isTriggered   = false;
            lastLapTimeUs = nowUs;
            triggerFlashLED(1);
            Serial.println(F("Measurement Started"));
          }
        }
      }
    }
  }

  // ----------------------------------------------------------
  // 5. デッドタイム終了 → Ready状態へ復帰
  // ----------------------------------------------------------
  if (!isTriggered) {
    unsigned long elapsedSinceLapMs =
      (unsigned long)((esp_timer_get_time() - lastLapTimeUs) / 1000LL);

    if (elapsedSinceLapMs >= deadTimeMs) {
      isTriggered = true;
      triggerFlashLED(1);
    }
  }

  // ----------------------------------------------------------
  // 6. LED非ブロッキング更新
  // ----------------------------------------------------------
  updateLED();

  // ----------------------------------------------------------
  // 7. LCD定期更新
  // ----------------------------------------------------------
  static unsigned long lastLcdMs = 0;
  if (millis() - lastLcdMs >= (unsigned long)LCD_UPDATE_INTERVAL_MS) {
    lastLcdMs = millis();
    updateLCD(currentLapMs, bestTimeMs);
  }

  // ----------------------------------------------------------
  // 8. BLE接続維持（非ブロッキング再アドバタイズ）
  // ----------------------------------------------------------
  static unsigned long disconnectMs        = 0;
  static bool          isAdvertisingPending = false;

  if (!deviceConnected && oldDeviceConnected) {
    disconnectMs         = millis();
    isAdvertisingPending = true;
    oldDeviceConnected   = false;
    Serial.println(F("BLE Pending re-advertise..."));
  }

  if (isAdvertisingPending && millis() - disconnectMs >= BLE_RECONNECT_DELAY_MS) {
    pServer->getAdvertising()->start();
    isAdvertisingPending = false;
    Serial.println(F("BLE Advertising restarted"));
  }

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = true;
  }

  delay(1);
}
