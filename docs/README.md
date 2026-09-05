# RC Lap Timer Web Client

[index.html](index.html)と[laptimer.js](laptimer.js)で構成された、ESP32 RCラップタイマー用のモバイル向けBLE受信画面です。ネイティブアプリを使わず、BLE通知をラップ履歴・ストップウォッチ・音声通知・カメラ撮影へ連携します。

## 機能

- Web Bluetooth APIでESP32のBLE通知を購読
- 接続状態に応じたCONNECTボタンと背景色の表示（未接続: グレー、接続: 黒、ラップ受信: 赤く点滅）
- 初回のスタート通知を起点にした現在ラップのストップウォッチ
- 確定ラップの一覧、最速ラップのハイライト、周回数の表示
- ビープ音と日本語音声によるスタート・ラップ通知
- 背面カメラ優先のプレビュー、自動撮影、JPEGダウンロード、画面内の写真履歴
- ラップ番号・タイム・撮影日時を画像に描画し、EXIFにも記録
- ラップ一覧のコピー、表示中の計測・写真履歴のクリア、画面スリープ防止

## ファイル

```text
docs/
├── index.html              # レイアウト、CSS、操作ボタン
├── laptimer.js             # BLE、ストップウォッチ、カメラ、履歴処理
├── favicon.ico
└── apple-touch-icon.png
```

`index.html`はCDNの`piexifjs 1.0.6`も読み込みます。写真のEXIF情報を付加するため、初回利用時にはこのCDNへ接続できる必要があります。

## 画面と操作

```text
[CONNECT] [DISCONNECT]
[START CAMERA] [COPY] [CLEAR]

カメラプレビュー

LAPS                 CURRENT LAP
0                    00:00.00

▼ LAP TIME           # 初期状態で展開
  No.  LAP TIME
▼ PHOTO              # 初期状態では折りたたみ

STATUS: READY
```

| 操作 | 内容 |
| --- | --- |
| CONNECT | 対象BLEデバイスを選び、Notifyを開始します。 |
| DISCONNECT | 選択中のBLEデバイスを切断します。 |
| START CAMERA / STOP CAMERA | 背面カメラを優先して開始 / 停止します。 |
| COPY | 保持している全通知を古い順にクリップボードへコピーします。 |
| CLEAR | 確認後、ラップ履歴、写真履歴、表示中のストップウォッチを消去します。BLE・カメラ接続は維持されます。 |

最速ラップは一覧上で緑にハイライトされます。画面には独立した「BEST LAP」や平均タイム欄はありません。

## BLE仕様

| 項目 | 値 |
| --- | --- |
| Service UUID | `12345678-1234-1234-1234-1234567890ab` |
| Characteristic UUID | `abcdefab-1234-5678-1234-abcdefabcdef` |
| 購読方式 | GATT Notify（`startNotifications()`） |
| 受信形式 | `Lap:`の後に秒数（例: `Lap: 12.34`） |

接続時、ブラウザのデバイス選択画面ではこのService UUIDを公開する機器だけが候補になります。ESP32側のデバイス名は`RC_LAPTIMER`です。

### スタート通知とラップの扱い

ESP32は最初のセンサ通過時に`Lap: 0.00`を送ります。Webクライアントはこの**最初の通知をスタート**として扱い、短いビープと「スタート」の読み上げを行ってストップウォッチを開始します。

2件目以降の通知は確定ラップです。各ラップで、ビープ2回、`N周目、…`の読み上げ、一覧更新、ストップウォッチのリセットを行います。カメラが動作中なら、このタイミングで撮影します。

実装上、`LAPS`の数とCOPYの内容にはスタート通知も含まれます。一方、画面のラップ一覧と写真番号はスタート通知を除外します。この挙動はESP32からの通知列をそのまま保持する現在の実装に基づきます。

## 写真

カメラを開始している場合、確定ラップごとに現在の映像をJPEG化します。画像には`LAP N: MM:SS.cs`と日本時間の撮影日時を描画し、次の情報をEXIFに格納します。

- `Artist`: `qoAop`
- `Software`: `LapTimer`
- `DateTimeOriginal` / `DateTimeDigitized`
- `UserComment`: ラップ番号とラップタイムのJSON

作成した画像は自動ダウンロードされ、PHOTO欄の先頭にも追加されます。カメラが停止中の場合、写真処理は行いません。

## 動作条件

| 項目 | 要件 |
| --- | --- |
| ブラウザ | Web Bluetooth対応のChromeまたはEdge（主にAndroid / PC） |
| 配信 | HTTPSまたは`localhost`。GitHub Pagesでの公開に対応 |
| Bluetooth | BLE対応かつブラウザのBluetooth権限を許可できること |
| カメラ | `getUserMedia`を許可できること。背面カメラ搭載端末を推奨 |
| 音声 | ブラウザの自動再生ポリシーにより、先に画面を操作しておくことを推奨 |

Web Bluetooth APIはSafariとFirefoxでは通常利用できません。画面スリープ防止にはWake Lock APIを使用し、対応しないブラウザでは何もせずに続行します。クリップボード書き込みは安全なコンテキストではClipboard APIを使用し、それ以外では`execCommand`のフォールバックを試みます。

## 公開と利用

1. `docs/`をHTTPSで配信します。GitHub Pagesならリポジトリ設定で`docs`フォルダを公開元にします。
2. ESP32ラップタイマーを起動し、BLEアドバタイズ中にします。
3. ページを開き、CONNECTを押して`RC_LAPTIMER`を選択します。
4. `STATUS: CONNECTED`になったら、最初のセンサ通過で計測を始めます。
5. 通過写真が必要な場合は、走行前にSTART CAMERAを押してカメラ権限を許可します。

## ライセンス

MIT License
