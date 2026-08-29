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
 * #define LoRa_ModeSettingPin_M0 4
 * #define LoRa_ModeSettingPin_M1 13
 * #define LoRa_RxPin 18
 * #define LoRa_TxPin 23
 * #define LoRa_AUXPin 34
 * #define LoRa_BaudRate 9600
 */

/*
Chip type:          ESP32-D0WD-V3 (revision v3.1)
Features:           Wi-Fi, BT, Dual Core + LP Core, 240MHz, Vref calibration in eFuse, Coding Scheme None
Crystal frequency:  40MHz
*/
// @arduino-config Board: esp32:esp32:esp32
// @arduino-config Port: /dev/cu.usbserial-240
// @arduino-config CPUFrequency: 240
// @arduino-config FlashFrequency: 80
// @arduino-config FlashMode: qio
// @arduino-config FlashSize: 4M
// @arduino-config PartitionScheme: huge_app
// @arduino-config UploadSpeed: 460800
// @arduino-config EraseFlash: all

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

#include "esp_system.h" // リセット理由の取得に必要なヘッダーファイル
#include "esp_timer.h"  // esp_timer_get_time() 用
#include "esp32_e220900t22s_jp_lib_v2.h"  // ★ CLEALINK E220 LoRaライブラリ（内部でfirmware.hをinclude）

// --- BLE設定 ---
#define BLE_NAME "RC_LAPTIMER"
#define SERVICE_UUID "12345678-1234-1234-1234-1234567890ab"
#define CHAR_UUID "abcdefab-1234-5678-1234-abcdefabcdef"

// --- ピン定義 ---
const int PHOTO_PIN = 26;  // 光電センサ入力ピン
const int TH_VR_PIN = 32;  // 反応時間調整ボリューム (0～200ms)
const int ST_VR_PIN = 33;  // デッドタイム調整ボリューム (0.0～5.0s)
const int LED_PIN = 17;
const int BUZZER_PIN = 19;
const int SETTING_PIN = 14;  // 設定モード切り替えスライドスイッチ
const int I2C_SDA = 21;
const int I2C_SCL = 22;

// --- 定数 ---
const int ADC_READ_INTERVAL_MS = 100;              // ADC読み取り周期
const int LCD_UPDATE_INTERVAL_MS = 333;            // LCD更新周期
const unsigned long MIN_DEAD_TIME_MS = 500;        // デッドタイム最小値（ボリューム最小でも500ms）
const unsigned long BLE_RECONNECT_DELAY_MS = 500;  // BLE再接続待ち時間
const int LED_ON_MS = 50;                          // LED点灯時間
const int LED_OFF_MS = 100;                        // LED消灯時間
const int SETUP_FLASH_COUNT = 2;                   // 起動時LEDフラッシュ回数
const int SWITCH_DEBOUNCE_MS = 50;                 // スライドスイッチデバウンス時間
const unsigned long BEST_TIME_INIT_MS = 99999000UL;  // ベストタイム初期値(ms)
const int NOTE_FREQ = 4000;                         // ブザー音の高さ（Hz）：4000Hz
const unsigned long TONE_DURATION = 100;            // ブザー鳴らす時間（ミリ秒）

// --- グローバル変数 ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
volatile bool deviceConnected = false;
volatile bool oldDeviceConnected = false;
volatile bool bleStateChanged = false;  // ★ 追加：BLE状態変化通知フラグ

// ボリューム設定値
float sensingThreshMs = 0.0f;
float deadTimeSec = 0.0f;
unsigned long deadTimeMs = MIN_DEAD_TIME_MS;

// ISRとmainループ間の共有変数（volatile）
// ★時刻はすべてesp_timer_get_time()のμs(int64_t)で統一
volatile bool isSensorTriggered = false;
volatile int64_t isrTriggerTimeUs = 0;  // μs単位（esp_timer_get_time()）

volatile bool isSensing = false;  // ISRが発火してからセンサー確定待ち中フラグ
volatile int64_t sensorTriggerUs = 0;      // 確定したトリガー時刻(μs)

// ラップタイム管理（ms単位で保持して精度損失を防ぐ）
int64_t lastLapTimeUs = 0;  // lastLapTimeをμs単位で保持
unsigned long currentLapMs = 0;
unsigned long bestTimeMs = (unsigned long)BEST_TIME_INIT_MS;
unsigned long prevLapMs = 0;
bool isFirstRun = true;
volatile bool isTriggered = true;  // 起動時はReady(true)

// --- 非ブロッキングLED制御 ---
int ledFlashCount = 0;
unsigned long nextLedToggleMs = 0;
int ledState = LOW;

// --- デバウンス済みスイッチ状態 ---
bool settingMode = false;
bool rawSettingMode = false;
unsigned long lastSwitchMs = 0;

// --- ブザー状態管理 ---
bool isBuzzerRinging = false;   // ブザーが鳴っているかどうか
int64_t buzzerStartTime = 0; // ブザーを鳴らし始めた時刻

// --- LoRa関連（★追加） ---
// ピン配置・UARTボーレートは esp32_e220900t22s_jp_lib_v2.h 内のデフォルト定義をそのまま使用：
//   LoRa_ModeSettingPin_M0 = 4 / LoRa_ModeSettingPin_M1 = 13
//   LoRa_RxPin = 18 / LoRa_TxPin = 23 / LoRa_AUXPin = 34 / LoRa_BaudRate = 9600
// 配線を変える場合は esp32_e220900t22s_jp_lib_v2.h 側の #define を書き換えてください。
CLoRa lora;                         // ★ LoRaライブラリインスタンス
bool loraInitialized = false;       // ★ LoRa初期化成否フラグ
unsigned long loraTxCount = 0;      // ★ LoRa送信成功カウント（シリアル表示用）
unsigned long loraTxFailCount = 0;  // ★ LoRa送信失敗カウント（シリアル表示用）

// リセット理由を文字列で返す関数
const char* getResetReasonString() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    //                             "01234567890123456789"
    case ESP_RST_POWERON:   return "PowerON(NormalStart)";
    case ESP_RST_EXT:       return "EXTERNAL PIN RESET  ";
    case ESP_RST_SW:        return "SOFTWARE RESET ESP32";
    case ESP_RST_PANIC:     return "CRASH/PANIC (Except)";
    case ESP_RST_INT_WDT:   return "WATCHDOG (Interrupt)";
    case ESP_RST_TASK_WDT:  return "WATCHDOG (Task)     ";
    case ESP_RST_WDT:       return "WATCHDOG (Other)    ";
    case ESP_RST_DEEPSLEEP: return "DEEP SLEEP WAKEUP   ";
    case ESP_RST_BROWNOUT:  return "BROWNOUT VoltageDrop";
    case ESP_RST_SDIO:      return "SDIO RESET          ";
    default:                return "UNKNOWN             ";
  }
}
// ============================================================
// BLEコールバック（staticインスタンスでメモリリークを防止）
// ============================================================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pSrv) override {
    deviceConnected = true;
    bleStateChanged = true;  // ★ フラグを立てるだけ
    Serial.println(F("BLE Connected"));
  }
  void onDisconnect(BLEServer* pSrv) override {
    deviceConnected = false;
    bleStateChanged = true;  // ★ フラグを立てるだけ
    Serial.println(F("BLE Disconnected"));
  }
};
static MyServerCallbacks bleCallbacks;  // スタティックインスタンス

// ============================================================
// 光電センサ ISR
// ★ millis()はISR内で使用禁止 → esp_timer_get_time()を使用
// ★ isSensingはISR外で書き換えるのでvolatile不要だが、
//    isTriggeredはISR内で参照するためvolatileが必須
// ============================================================
void IRAM_ATTR handleSensorInterrupt() {
  if (!isSensing && isTriggered) {
    isSensorTriggered = true;
    isrTriggerTimeUs = esp_timer_get_time();  // μs精度・ISR安全
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
  ledFlashCount = count * 2;
  digitalWrite(LED_PIN, HIGH);
  ledState = HIGH;
  nextLedToggleMs = millis() + LED_ON_MS;
}

// loop()内で毎回呼ぶLED状態更新
void updateLED() {
  if (ledFlashCount <= 0) return;
  if (millis() >= nextLedToggleMs) {
    if (ledState == HIGH) {
      digitalWrite(LED_PIN, LOW);
      ledState = LOW;
      nextLedToggleMs = millis() + LED_OFF_MS;
    } else {
      digitalWrite(LED_PIN, HIGH);
      ledState = HIGH;
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
  const unsigned long MAX_TIME_MS = 59UL * 60UL * 1000UL;  // ★ 59分59.99秒以上は異常値扱い
  // 0または異常値は "00:00.00" で表示
  if (time_ms == 0 || time_ms >= MAX_TIME_MS) {
    snprintf(outBuf, bufSize, "00:00.00   ");
    return;
  }
  // BLE送信と同じロジックで一度Floatにしてから整数に戻す。
  char bleBuf[32];
  float lapSec = time_ms / 1000.0f;
  snprintf(bleBuf, sizeof(bleBuf), "%6.2f", lapSec);
  float rounded_sec = atof(bleBuf); // floatに変換
  unsigned long rounded_ms = (int)(rounded_sec * 1000UL);
  int m  = (int)(rounded_ms / 60000UL);          // 1分 = 60,000ms
  int s  = (int)((rounded_ms % 60000UL) / 1000);  // 余りのミリ秒から「秒」を算出
  int cs = (int)((rounded_ms % 1000) / 10);       // さらに余ったミリ秒から「センチ秒」を算出
  snprintf(outBuf, bufSize, "%02d:%02d.%02d     ", m, s, cs);
  // Serial.printf("formatTime: %lu , %6.2f , %s\n", time_ms, lapSec, outBuf);
}

// ============================================================
// スライドスイッチのデバウンス読み取り
// ============================================================
void updateSwitchDebounce() {
  bool raw = (digitalRead(SETTING_PIN) == LOW);
  if (raw != rawSettingMode) {
    rawSettingMode = raw;
    lastSwitchMs = millis();
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
  static bool lastFirstRun = true;

  // ★ BLE状態変化時は clear() せず、下のif文を強制発火させるだけにする
  //    （clear()は下のブロックで1回だけ行う。二重呼び出し防止）
  if (bleStateChanged) {
    bleStateChanged = false;
    lastSettingMode = !settingMode;  // 強制再描画トリガー
  }

  // --- 一定時間ごとにLCDを強制リセットして復旧させる処理 ---
  static unsigned long lastLcdRecoveryMs = 0;
  if (millis() - lastLcdRecoveryMs >= 3*60*1000) { // 10秒周期
    lastLcdRecoveryMs = millis();
    lcd.init();      // LCD内部の制御ICを強制初期化（これで固まりが解けます）
    lcd.backlight(); // バックライトを再点灯
    // フラグ等をリセットして、画面全体を再描画させる
    lastSettingMode = !settingMode;   // 強制再描画トリガー
    Serial.println(F("LCD Force Reset"));
  }

  // モードが切り替わったときだけ画面をクリアして固定ラベルを再描画
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
    // ★ delay()を使わず、clear()直後の描画は次のloop()周期（最大250ms後）に任せる
    return;
  }

  if (settingMode) {
    char strBuf[40];
    // 設定確認モード
    lcd.setCursor(0, 0);
    lcd.print(F("--- SETTING MODE ---"));

    snprintf(strBuf, sizeof(strBuf), "SENSOR TH: %4.0f ms  ", sensingThreshMs);
    lcd.setCursor(0, 1);
    lcd.print(strBuf);

    snprintf(strBuf, sizeof(strBuf), "DEAD TIME: %4.1f s ", deadTimeSec);
    lcd.setCursor(0, 2);
    lcd.print(strBuf);

    snprintf(strBuf, sizeof(strBuf), "SENSOR   : %s  ", (digitalRead(PHOTO_PIN) == HIGH ? F("BLOCK") : F("CLEAR")));
    lcd.setCursor(0, 3);
    lcd.print(strBuf);

    lcd.setCursor(18, 1);
    lcd.print(isTriggered ? F("OK") : F("__"));
    lcd.setCursor(18, 2);
    lcd.print(loraInitialized ? F("L+") : F("L-"));
    lcd.setCursor(18, 3);
    lcd.print(deviceConnected ? F("B+") : F("B-"));

  } else if (isFirstRun) {
    lcd.setCursor(0, 0);
    lcd.print(F("--- RC LAP TIMER ---"));
    lcd.setCursor(0, 1);
    lcd.print(F("   LAP TIMER READY  "));
    lcd.setCursor(0, 2);
    lcd.print(F("PASS SENSOR TO START"));
    lcd.setCursor(0, 3);
    lcd.print(getResetReasonString());

  } else {
    // 計測中：現在の経過時間をリアルタイム表示
    unsigned long elapsedMs = (unsigned long)((esp_timer_get_time() - lastLapTimeUs) / 1000LL);

    formatTime(elapsedMs, timeBuf, sizeof(timeBuf));
    lcd.setCursor(7, 0);
    lcd.print(timeBuf);

    formatTime(bestMs, timeBuf, sizeof(timeBuf));
    lcd.setCursor(7, 1);
    lcd.print(timeBuf);

    formatTime(lapMs, timeBuf, sizeof(timeBuf));
    lcd.setCursor(7, 2);
    lcd.print(timeBuf);

    formatTime(prevLapMs, timeBuf, sizeof(timeBuf));
    lcd.setCursor(7, 3);
    lcd.print(timeBuf);

    lcd.setCursor(18, 1);
    lcd.print(isTriggered ? F("OK") : F("__"));
    lcd.setCursor(18, 2);
    lcd.print(loraInitialized ? F("L+") : F("L-"));
    lcd.setCursor(18, 3);
    lcd.print(deviceConnected ? F("B+") : F("B-"));
  }
}

// ============================================================
// BLE初期化
// ============================================================
void BLESetup() {
  BLEDevice::init(BLE_NAME);

  // ★ BLE送信出力を最大（+9dBm）に設定
  // ESP_PWR_LVL_N12 〜 ESP_PWR_LVL_P9 の範囲で指定可能
  // P9 = +9dBm（最大）、デフォルトは P3 = +3dBm
  BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
  BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_ADV);   // ★ アドバタイズ出力も最大化
  BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_SCAN);  // ★ スキャン出力も最大化

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(&bleCallbacks);  // staticインスタンスを使用

  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic =
      pService->createCharacteristic(CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY |
                                                    BLECharacteristic::PROPERTY_INDICATE);
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();
}

// ============================================================
// LoRa初期化（★追加）
// .iniファイルを使わずライブラリ標準のデフォルト設定値
// （SetDefaultConfigValue）をそのまま書き込む。
//
// ★ .cppの実装を確認した結果、InitLoRaModule()の内部で
//   SwitchToConfigurationMode() と SerialLoRa.begin() が
//   自動的に実行されることが分かったため、ここでは呼び出さない
//   （二重初期化を避けるため）。
// ★ InitLoRaModule()実行後もコンフィグモード(M0=1,M1=1)のまま
//   なので、送信可能にするため明示的に SwitchToNormalMode() を呼ぶ。
// ============================================================
void LoRaSetupE220() {
  Serial.println(F("[LoRa] Initializing E220-900T22S/L(JP) [Ver.1.x Compatible Mode]..."));
  Serial.printf("[LoRa] Pins: M0=%d M1=%d RXD=%d TXD=%d AUX=%d  Baud=%d\n",
                LoRa_ModeSettingPin_M0, LoRa_ModeSettingPin_M1, LoRa_RxPin, LoRa_TxPin,
                LoRa_AUXPin, LoRa_BaudRate);

  // ★ .iniファイルは使わず、ライブラリ内蔵のデフォルト設定値をそのまま使用
  lora.SetDefaultConfigValue(lora.config);

  //lora.SwitchToConfigurationMode();
  //delay(500);  // ★ 20ms→500msに伸ばして安定待ちを確保

  // 必要であればここでデフォルト値の一部だけ上書きできます（例）。
  // ビットエンコード値は利用ガイド記載の default_config 定義／データシートに準拠してください。
  // lora.config.own_channel = 0x00;
  // lora.config.target_channel = 0x00;

  // InitLoRaModule()内部でコンフィグモードへの移行とUART初期化が行われ、
  // 上記configの内容がモジュールのレジスタ00H～07Hへ書き込まれる
  if (lora.InitLoRaModule(lora.config)) {
    // レジスタ書き込みコマンドに対するモジュールからの応答が
    // 期待バイト数と一致しない場合に失敗となる（配線不良・電源不足等）
    Serial.println(F("[LoRa] InitLoRaModule() FAILED. Check wiring / module power supply."));
    loraInitialized = false;
    return;
  }

  // 設定完了後もコンフィグモードのままなので、送受信可能な
  // ノーマルモード(M0=0,M1=0)へ明示的に戻す
  lora.SwitchToNormalMode();

  loraInitialized = true;
  Serial.printf(
      "[LoRa] Init OK.  own_address=0x%04X  own_channel=%d  target_address=0x%04X  target_channel=%d\n",
      lora.config.own_address, lora.config.own_channel, lora.config.target_address, lora.config.target_channel);
}

// ============================================================
// LoRaパケット送信（★追加）
// BLE Notifyと同一のペイロード文字列を渡して送信する。
// 動作確認用に送信結果・所要時間を詳細にシリアル出力する。
//
// ★ 注意：SendFrame()はsubpacket_sizeを超えるデータ長の場合のみ1を返し、
//   それ以外（UART送信自体の成否は未検知）は常に0を返す実装になっている。
//   そのため下記の「OK/FAIL」は主に「送信データ長エラーの有無」を
//   示すものであり、電波としての送信成功を保証するものではない。
// ★ また、ライブラリ内部の送信前ビジーチェック（AUXピン監視）が
//   実質的に機能していないため、極端に短い間隔で連続送信すると
//   モジュール側の処理が追いつかない可能性がある
//   （本ダミースケッチの送信間隔は5～70秒なので通常は問題にならない）。
// ============================================================
void sendLoRaPacket(const char* payload) {
  if (!loraInitialized) {
    Serial.println(F("[LoRa][TX] Skipped: LoRa module not initialized."));
    return;
  }

  size_t payloadLen = strlen(payload);
  unsigned long txStartMs = millis();

  int result = lora.SendFrame(lora.config, (uint8_t*)payload, payloadLen);

  unsigned long txDurationMs = millis() - txStartMs;

  if (result == 0) {
    loraTxCount++;
    Serial.printf("[LoRa][TX] OK   payload=\"%s\" (%u bytes)  duration=%lums  total_ok=%lu\n",
                  payload, (unsigned)payloadLen, txDurationMs, loraTxCount);
  } else {
    loraTxFailCount++;
    Serial.printf("[LoRa][TX] FAIL SendFrame()=%d (payload too long for subpacket_size)  payload=\"%s\"  duration=%lums  total_fail=%lu\n",
                  result, payload, txDurationMs, loraTxFailCount);
  }
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  // 起動時に前回の終了理由を出力
  Serial.println(F("\n================================="));
  Serial.print(F("[SYS] Last Reset Reason: "));
  Serial.println(getResetReasonString());
  Serial.println(F("=================================\n"));

  pinMode(TH_VR_PIN, INPUT);
  pinMode(ST_VR_PIN, INPUT);
  pinMode(PHOTO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(SETTING_PIN, INPUT_PULLUP);
  digitalWrite(LED_PIN, LOW);

  // 割り込み設定（FALLINGエッジで遮光検出）
  attachInterrupt(digitalPinToInterrupt(PHOTO_PIN), handleSensorInterrupt, RISING);

  // LCD初期化
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000); // 10kHz（標準モード）に明示的に落とす（デフォルトは400kHzの場合あり）
  Wire.setTimeOut(100);  // タイムアウト設定

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(F("--- RC LAP TIMER ---"));
  lcd.setCursor(0, 1);
  lcd.print(F("Initialize..."));
  lcd.setCursor(0, 3);
  lcd.print(getResetReasonString());
  delay(1000); //電力降下を防止するためにウエイト

  // BLS初期化
  BLESetup();
  delay(1000); //電力降下を防止するためにウエイト

  int countLoRa = 0;
  while (countLoRa < 5) {
    LoRaSetupE220();
    delay(1000); //電力降下を防止するためにウエイト
    if (loraInitialized) break;
    ++countLoRa;
  }

  flashLEDBlocking(SETUP_FLASH_COUNT);
  lcd.clear();

  // 初期状態：Ready（ラップ可能）
  isTriggered = true;
  lastLapTimeUs = esp_timer_get_time();  // μsで記録

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

    float rawThresh = (analogRead(TH_VR_PIN) / 4095.0f) * 200.0f;
    float rawDeadTime = (analogRead(ST_VR_PIN) / 4095.0f) * 5.0f;

    static bool adcInitialized = false;
    if (!adcInitialized) {
      sensingThreshMs = rawThresh;
      deadTimeSec = rawDeadTime;
      adcInitialized = true;
    } else {
      sensingThreshMs = sensingThreshMs * 0.9f + rawThresh * 0.1f;
      deadTimeSec = deadTimeSec * 0.9f + rawDeadTime * 0.1f;
    }
    // ★デッドタイムに最小値を設けてdead=0ms時のバグを防止
    deadTimeMs = max((unsigned long)(deadTimeSec * 1000.0f), MIN_DEAD_TIME_MS);
  }

  // ----------------------------------------------------------
  // 3. ISRトリガーの排他的読み取り
  // ★ 最初からnoInterrupts()で囲んでデータ競合を完全解消
  // ----------------------------------------------------------
  bool triggered = false;
  int64_t triggerUs = 0;

  noInterrupts();
  if (isSensorTriggered) {
    triggered = true;
    isSensorTriggered = false;
    triggerUs = isrTriggerTimeUs;
  }
  interrupts();

  if (triggered) {
    sensorTriggerUs = triggerUs;
    isSensing = true;  // 連続遮光時間の計測モード開始
  }

  // ----------------------------------------------------------
  // 4. 反応時間の検証（isSensingフェーズ）
  // ----------------------------------------------------------
  if (isSensing) {
    if (digitalRead(PHOTO_PIN) == LOW) {
      // 遮光時間内に光が復帰 → ノイズとしてキャンセル
      isSensing = false;
      // シリアルログ（デバッグ用）
      int64_t elapsedUs = esp_timer_get_time() - sensorTriggerUs;
      Serial.printf("Noise Signal (sensingThreshMs %.0f us) (elapsedUs %lu us)\n", sensingThreshMs * 1000.0f, elapsedUs);

    } else {
      int64_t elapsedUs = esp_timer_get_time() - sensorTriggerUs;
      if (elapsedUs >= (int64_t)(sensingThreshMs * 1000.0f)) {
        // 規定時間以上の遮光 → ラップ確定
        isSensing = false;

        if (isTriggered) {
          int64_t nowUs = sensorTriggerUs;  // 割り込み発生時刻を使用

          if (!isFirstRun) {
            isTriggered = false;  // デッドタイム開始
            prevLapMs = currentLapMs;
            currentLapMs = (unsigned long)((nowUs - lastLapTimeUs) / 1000LL);
            if (currentLapMs < bestTimeMs) bestTimeMs = currentLapMs;

            // BLE送信（秒単位のfloatに変換して送信）
            char bleBuf[32];
            float lapSec = currentLapMs / 1000.0f;
            snprintf(bleBuf, sizeof(bleBuf), "Lap:%6.2f", lapSec);
            if (deviceConnected) {
              pCharacteristic->setValue(bleBuf);
              pCharacteristic->notify();
            }
            sendLoRaPacket(bleBuf);  // ★ BLEと同一ペイロードをLoRaでも送信

            triggerFlashLED(1);
            lastLapTimeUs = nowUs;

            // シリアルログ（デバッグ用）
            Serial.printf("Lap: %lu ms (Best: %lu ms) (elapsedUs %lu us )\n", currentLapMs, bestTimeMs, elapsedUs);

          } else {
            // 初回通過（計測開始）
            isFirstRun = false;
            isTriggered = false;
            lastLapTimeUs = nowUs;
            triggerFlashLED(1);
            Serial.println(F("Measurement Started"));

            // BLE送信（秒単位のfloatに変換して送信）初回通過はLap:0.00として送信
            char bleBuf[32];
            float lapSec = 0.0f;
            snprintf(bleBuf, sizeof(bleBuf), "Lap:%6.2f", lapSec);
            if (deviceConnected) {
              pCharacteristic->setValue(bleBuf);
              pCharacteristic->notify();
            }
            sendLoRaPacket(bleBuf);  // ★ BLEと同一ペイロードをLoRaでも送信
          }

          // センサを通過したらブザーは鳴らす
          tone(BUZZER_PIN, NOTE_FREQ); // 時間指定なしで音を出す
          buzzerStartTime = esp_timer_get_time(); // 鳴らし始めた時刻を記録
          isBuzzerRinging = true;         // ブザー鳴動中フラグをON
        }
      }
    }
  }

  // ----------------------------------------------------------
  // 5. デッドタイム終了 → Ready状態へ復帰
  // ----------------------------------------------------------
  if (!isTriggered) {
    unsigned long elapsedSinceLapMs = (unsigned long)((esp_timer_get_time() - lastLapTimeUs) / 1000LL);

    if (elapsedSinceLapMs >= deadTimeMs) {
      isTriggered = true;
      triggerFlashLED(1);
    }
  }

  // ブザーが鳴っていたら停止
  if (isBuzzerRinging) {
    // 指定した時間（100ms）が経過したかチェック
    unsigned long elapsedBuzzerMs = (unsigned long)((esp_timer_get_time() - buzzerStartTime) / 1000LL);
    if (elapsedBuzzerMs >= TONE_DURATION) {
      noTone(BUZZER_PIN);        // 音を止める
      isBuzzerRinging = false;   // ブザー鳴動中フラグをOFF
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
  static unsigned long disconnectMs = 0;
  static bool isAdvertisingPending = false;

  if (!deviceConnected && oldDeviceConnected) {
    disconnectMs = millis();
    isAdvertisingPending = true;
    oldDeviceConnected = false;
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
