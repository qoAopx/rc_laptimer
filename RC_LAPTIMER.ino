/*
 * ==============================================================================
 * RC Car Lap Timer (Hose Type) - LCD2004 (20x4) Version
 * ==============================================================================
 * [プログラム概要]
 * チューブ内の空気圧変化を微分（変化率）解析し、マシンの通過を検知します。
 * 計測データは I2C接続の 20x4 液晶ディスプレイに表示されます。
 * * [ピンアサイン]
 * - D2 : HX710B OUT (データ入力)
 * - D3 : HX710B SCK (同期クロック)
 * - D4 : 圧電ブザー (報知用)
 * - D7 : 液晶表示切り替えスイッチ
 * - A0 : 可変抵抗 (感度調整しきい値用)
 * - A1 : 可変抵抗 (センサーゲイン調整用)
 * - A4 : I2C SDA (液晶データ)
 * - A5 : I2C SCL (液晶クロック)
 * * [アルゴリズム詳説]
 * 1. 移動平均: ノイズを除去し、信号を滑らかにします。
 * 2. 微分処理: 気温変化等の「ゆっくりした変動」を無視し、「踏んだ瞬間」を捉えます。
 * 3. 二重カウント防止: 一度検知すると、圧力が抜けるまで再検知をロックします。
 * ==============================================================================
 */

// 2線による通信を行うためのI2C通信ライブラリです。Wireと呼ばれることもあります。
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// 液晶の初期設定：アドレス 0x27 (または0x3F), 20列, 4行
LiquidCrystal_I2C lcd(0x27, 20, 4);

// --- ピンの定義 ---
const int HX710B_OUT = 2; // HX710B センサーデータ出力ピン
const int HX710B_SCK = 3; // HX710B センサー同期クロック出力ピン
const int TH_VR_PIN = A0;    // Zスコアしきい値(zThreshold)調整用ボリュームの入力ピン
const int ST_VR_PIN = A1;    // 安定判定しきい値(zStable)調整用ボリュームの入力ピン
const int BUZZER_PIN = 4; // 報知用圧電ブザーの接続ピン
const int SETTING_PIN = 7; // 液晶画面の表示モード（計測/設定）切り替えスイッチ

// --- 定数 ---
const int UPDATE_LCD = 250; // 液晶ディスプレイの更新間隔 (ミリ秒)
const unsigned long FORMAT_MAX_MSEC = 3540000 ; // 最大計測時間 (59分00秒 = 3540,000ミリ秒)
const float BEST_TIME_MAX = 99999.99;          // ベストタイムの初期最大値 (秒)

// --- 非同期ブザー管理用構造体 ---
// メインループを止めずにメロディを再生するための定義
struct MelodyNote {
  int freq;      // 周波数 (Hz)
  int duration;  // 音の長さ (ms)
  int wait;      // 次の音までの待ち時間 (ms)
};

// --- グローバル変数 ---
// --- 統計情報・外れ値検知パラメータ (Z-scoreアルゴリズム用) ---
float zThreshold = 3.0;           // 外れ値判定用のZスコアしきい値 (ボリューム A0 で可変)
float zStable = 1.5;              // 踏み込み解除（安定）判定用のZスコアしきい値 (ボリューム A1 で可変)
float alpha = 0.1;                // 指数移動平均(EMA)の平滑化係数（0.0〜1.0）
float movingMean = 0.0;           // センサー値の逐次平均（移動平均）
float movingVar = 0.0;            // センサー値の逐次分散（移動分散）
bool isTraining = true;           // 起動直後の学習期間中かどうかのフラグ

// --- ラップ計測状態管理 ---
unsigned long lastLapTime = 0;    // 最後にラップを検知した時刻 (ms)
const unsigned long DEAD_TIME = 100; // 検知後に再入力を無視する不感時間 (ms)
float currentLap = 0.0;           // 今回のラップタイム (秒)
float bestTime = BEST_TIME_MAX;   // これまでの最速ラップタイム (秒)
float prevLap = 0.0;              // 前回のラップタイム (秒)
bool isFirstRun = true;           // 最初の通過（計測開始）待ちフラグ
bool isTriggered = false;         // 現在センサーが反応中（ホースを踏んでいる）かどうかのフラグ

/*
 * formatTime: 秒数を「00:00.00」形式の文字列に変換し、指定されたバッファに格納します。
 * Stringクラスを避け、メモリ断片化を防ぎます。
 */
void formatTime(float time_sec, char* outBuf) {
  unsigned long msec = (unsigned long)(time_sec * 1000);//ミリ秒に変換
  if (msec >= FORMAT_MAX_MSEC || msec <= 0) {
    strcpy(outBuf, "ERROR VALUE");
    return;
  }
  int m = (int)(msec / 60000);
  int s = (int)((msec % 60000) / 1000);
  int cs = (msec % 1000) / 10 ; // 999msec = 0.99secにする
  sprintf(outBuf, "%02d:%02d.%02d", m, s, cs);
}
/*
 * updateLCD: 液晶ディスプレイ(LCD2004)の表示内容を更新します。
 * @param lapTime  : 最新のラップタイム (秒)
 * @param bestTime : これまでのベストタイム (秒)
 * @param zScore   : 現在のZスコアを100倍した整数値（表示・デバッグ用）
 */
void updateLCD(float lapTime, float bestTime, int zScore) {
  char timeBuf[12];
  int threshold = zThreshold * 100; // しきい値を表示用に100倍
  int stable = zStable * 100;       // 安定しきい値を表示用に100倍

  // スイッチが押されている(LOW)かどうかでモード判定
  bool settingState = (digitalRead(SETTING_PIN) == LOW);

  if (settingState) {
    // --- 設定・デバッグモード ---
    // リアルタイムの数値を表示し、感度調整を容易にします。
    lcd.setCursor(0, 0);
    lcd.print(F("- SETTING MODE -"));

    // 2行目：現在のZスコア（変動の大きさ）
    lcd.setCursor(0, 1);
    lcd.print(F("SCOR:")); lcd.print(zScore);

    // 3行目：検知しきい値 (TH_VR_PIN で調整)
    lcd.setCursor(0, 2);
    lcd.print(F("TH  :")); lcd.print(threshold);

    // 4行目：安定判定しきい値 (ST_VR_PIN で調整)
    lcd.setCursor(0, 3);
    lcd.print(F("ST :")); lcd.print(stable);

    // シリアルプロッタ等での確認用出力
    int viewZscore = (zScore < threshold ) ? zScore : threshold * 1.1;
    Serial.print(F("VzScore:"));
    Serial.print(viewZscore);
    Serial.print(F(" +TH:"));
    Serial.print(threshold);
    Serial.print(F(" zStable:"));
    Serial.print(stable);
    Serial.print(F(" BASE:"));
    Serial.print(0);
    Serial.println();

  } else {
    // --- 通常計測モード ---
    // 1行目：現在の走行時間（未確定のラップ）
    lcd.setCursor(0, 0);
    lcd.print(F("TIME : "));
    formatTime(currentLap, timeBuf);
    lcd.print(timeBuf);

    // 2行目：これまでの記録の中でのベストタイム
    lcd.setCursor(0, 1);
    lcd.print(F("BEST : "));
    formatTime(bestTime, timeBuf);
    lcd.print(timeBuf);

    // 3行目：直前に確定したラップタイム
    lcd.setCursor(0, 2);
    lcd.print(F("CURR : "));
    formatTime(lapTime, timeBuf);
    lcd.print(timeBuf);

    // 4行目：その一つ前のラップタイム
    lcd.setCursor(0, 3);
    lcd.print(F("PREV : "));
    formatTime(prevLap, timeBuf);
    lcd.print(timeBuf);
  }
}

/*
 * readHX710B: HX710Bセンサーから24ビットの読み取り値を取得します。
 * @return : 符号拡張されたセンサーの生値 (long)
 * データピンがLOWになる（準備完了）のを待ってから、クロックを24回送ってデータを取得します。
 */
long readHX710B() {
  unsigned long startWait = millis();
  // センサーの準備ができるまで最大100ms待機
  while (digitalRead(HX710B_OUT) == HIGH) {
    if (millis() - startWait > 100) return 0; // タイムアウト時は0を返す
  }
  unsigned long value = 0;
  // 24ビットのデータを1ビットずつ読み出し
  for (int i = 0; i < 24; i++) {
    digitalWrite(HX710B_SCK, HIGH);
    value = (value << 1) | digitalRead(HX710B_OUT);
    digitalWrite(HX710B_SCK, LOW);
  }
  // 25番目のクロック（次回の設定用：ここでは標準のゲイン128を設定）
  digitalWrite(HX710B_SCK, HIGH);
  digitalWrite(HX710B_SCK, LOW);

  // 24ビット符号付きを32ビットlongに拡張 (2の補数対応)
  if (value & 0x800000) value |= 0xFF000000;
  return (long)value;
}

// --- 非同期メロディ管理 ---

// 起動時のメロディ定義
const MelodyNote BOOT_NOTES[] PROGMEM = {
  {880, 180, 200}, {988, 180, 200}, {1109, 180, 200}, {1319, 220, 240},
  {1175, 160, 180}, {1109, 160, 180}, {988, 200, 220}, {1319, 400, 450}
};

// ベストラップ更新時のファンファーレ定義
const MelodyNote FANFARE_NOTES[] PROGMEM = {
  {523, 180, 200}, {659, 180, 200}, {784, 180, 200}, {1047, 260, 280},
  {784, 160, 180}, {880, 160, 180}, {988, 180, 200}, {1047, 400, 450}
};

const MelodyNote* currentMelody = NULL; // 現在再生中のメロディへのポインタ
int melodySize = 0;                     // メロディのノート数
int melodyIndex = -1;                  // 次に再生するノートのインデックス（-1なら停止中）
unsigned long nextNoteTime = 0;         // 次の音を鳴らす予定時刻

/*
 * playMelody: 非同期メロディの再生を開始します。
 * @param melody : メロディ定数配列へのポインタ
 * @param size   : ノート数
 */
void playMelody(const MelodyNote* melody, int size) {
  currentMelody = melody;
  melodySize = size;
  melodyIndex = 0;
  nextNoteTime = 0;
}

/*
 * updateBuzzer: メインループ内で繰り返し呼び出し、メロディの再生を進行させます。
 * delay()を使わないため、センサーの読み取りを妨げません。
 */
void updateBuzzer() {
  if (melodyIndex == -1 || currentMelody == NULL) return;

  if (millis() >= nextNoteTime) {
    if (melodyIndex < melodySize) {
      // PROGMEMからノート情報を読み込む
      int freq = pgm_read_word(&(currentMelody[melodyIndex].freq));
      int dur = pgm_read_word(&(currentMelody[melodyIndex].duration));
      int wait = pgm_read_word(&(currentMelody[melodyIndex].wait));

      tone(BUZZER_PIN, freq, dur);
      nextNoteTime = millis() + wait;
      melodyIndex++;
    } else {
      // メロディ終了
      noTone(BUZZER_PIN);
      melodyIndex = -1;
      currentMelody = NULL;
    }
  }
}

/*
 * beep: 短い報知音を鳴らします（同期処理）。
 * @param count : 鳴らす回数
 * 非常に短いため delay() を使用していますが、メイン動作に大きな影響はありません。
 */
void beep(int count) {
  for(int i=0; i<count; i++) {
    tone(BUZZER_PIN, 3400, 20);
    delay(40);
  }
}

/*
 * setup: 電源投入時の初期化。
 * 各ピンの設定、LCDの初期化、およびセンサー値の初期学習を行います。
 */
void setup() {
  pinMode(HX710B_OUT, INPUT);
  pinMode(HX710B_SCK, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(SETTING_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  randomSeed(analogRead(A2)); // 未使用ピンのノイズからランダムシードを生成
  lcd.init();
  lcd.clear();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(F("LAPTIMER STARTING..."));

  Serial.println(F("LAPTIMER STARTING..."));
  playMelody(BOOT_NOTES, 8); // 起動音の再生開始

  // --- センサー学習フェーズ ---
  // メロディの再生が終わるまで（数秒間）、センサーの基準値を学習します。
  int initCount = 0;
  while (melodyIndex != -1) {
    updateBuzzer(); // メロディを進行させる
    long currentP = readHX710B();
    if (initCount == 0) {
      // 最初の1回目のデータで平均を初期化
      movingMean = (float)currentP;
      movingVar = 0.0;
    } else {
      // Welfordのアルゴリズムを使用して、逐次的に平均と分散を計算
      float diff = (float)currentP - movingMean;
      movingMean += diff / (initCount + 1);
      movingVar += diff * ((float)currentP - movingMean);
    }
    initCount++;
  }
  // 分散の初期化（初期のサンプリング数に基づき計算）
  if (initCount > 1) {
    movingVar /= (initCount - 1);
  }
  isTraining = false; // 学習完了、通常モードへ
}

/*
 * loop: メイン処理。
 * 微分値（slope）がボリュームで設定したしきい値を超えたらラップを記録します。
 */
void loop() {
  updateBuzzer(); // 非同期ブザー更新

  // 1. 各種設定ボリュームの読み取りとパラメータ更新
  // 外れ値判定のしきい値をボリューム(A0)で調整 (2.0 〜 10.0 の範囲)
  zThreshold = constrain(map(analogRead(TH_VR_PIN), 0, 650, 20, 100), 20, 100) / 10.0;

  // 圧力安定判定のしきい値をボリューム(A1)で調整 (0.0 〜 3.0 の範囲)
  zStable = constrain(map(analogRead(ST_VR_PIN), 0, 650, 0, 30), 0, 30) / 10.0;

  // センサー生値の取得
  long valHX7100 = readHX710B();

  // 2. Z-score方式による統計更新
  /*
   実装のポイント：Z-score（Zスコア）
    一般的には、以下の式で算出される
    値が一定のしきい値を超えた場合に「外れ値」と判定します。

    Z=abs(x-μ)/σ

      x : 現在の測定値
      μ : 移動平均（一定期間の平均）
      σ : 移動標準偏差（分散の平方根）
    目安: 一般的に（3シグマ）を外れ値とすることが多いですが、ノイズが多い環境では 4〜5 に設定して誤検知を防ぐこともあります。
  */

  float diff = (float)valHX7100 - movingMean;
  float stdDev = sqrt(movingVar);
  float zScore = (stdDev > 0.001) ? (abs(diff) / stdDev) : 0.0;

  // 3. 外れ値判定 測定値の外れ値検知する。計測値の分散と、
  bool isOutlier = (zScore > zThreshold && !isTraining);

  if (!isOutlier) {
    // 正常値（または学習中）：等平均・分散の統計情報を逐次更新 (EMAベース)
    movingMean += alpha * diff;
    movingVar = (1.0 - alpha) * (movingVar + alpha * diff * diff);
  }

  // 4. ラップタイム処理 (Zスコアベース)
  // チャタリング防止で一定時間経ってからトリガー検知
  // 圧力が安定してないとだめ
  if (isTriggered && (zScore < zStable ) && ((millis() - lastLapTime) > DEAD_TIME)) {
    beep(1);
    isTriggered = false;
    Serial.print(F("STABLE : "));
    Serial.println((millis() - lastLapTime) / 1000.0);
  }
  // 外れ値を検知していたらラップ処理を行う
  if (!isTriggered && isOutlier) {
    isTriggered = true; // チャタリング防止ロック
    unsigned long now = millis();
    if (!isFirstRun) {
      prevLap = currentLap;
      currentLap = (now - lastLapTime) / 1000.0;

      bool isBest = false;
      if (currentLap < bestTime) {
        // ベストタイム更新の判定（初期値 BEST_TIME_MAX を除外）
        if (BEST_TIME_MAX != bestTime) {
          isBest = true;
        }
        bestTime = currentLap;
      }

      // ベストラップならファンファーレ
      if (isBest) {
        playMelody(FANFARE_NOTES, 8);
        Serial.print(F("BEST : "));
        Serial.println((bestTime));
      } else {
        beep(2);
        Serial.print(F(" Lap : "));
        Serial.println((currentLap));
      }

    } else {
      // はじめにラップを開始したとき
      isFirstRun = false;
      beep(2);
      Serial.println(F("=== Lap START ==="));
    }
    lastLapTime = now;
  }

  // 液晶の定期更新（UPDATE_LCD ミリ秒おき）
  static unsigned long lastLcdUpdate = 0;
  if (millis() - lastLcdUpdate > UPDATE_LCD) {
    updateLCD(currentLap, bestTime, (int)(zScore * 100));
    lastLcdUpdate = millis();
  }

}
