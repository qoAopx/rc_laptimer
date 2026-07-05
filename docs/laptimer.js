/**
 * ============================================================================
 * RC LapTimer アプリケーション用 JavaScript
 * Web Bluetooth API を用いたESP32との通信、およびカメラ撮影・履歴管理を担う
 * ============================================================================
 */

// ==========================================
// BLE (Bluetooth Low Energy) 関連の設定・変数
// ==========================================

/** ESP32側で定義されているBLEサービスのUUID */
const SERVICE_UUID = "12345678-1234-1234-1234-1234567890ab";
/** ESP32側でラップタイムを送信してくるキャラクタリスティックのUUID */
const CHARACTERISTIC_UUID = "abcdefab-1234-5678-1234-abcdefabcdef";

/** @type {BluetoothDevice|null} 接続中のBLEデバイスインスタンス */
let device = null;
/** @type {BluetoothRemoteGATTCharacteristic|null} データ受信用のキャラクタリスティック */
let characteristic = null;

// ==========================================
// ラップタイム計測用の状態管理変数
// ==========================================

/** @type {number[]} 受信したラップタイム（秒）を格納する配列。先頭が最新データ。 */
let lapTimes = [];
/** @type {number} これまでに記録されたベストラップ（秒）。初期値は無限大。 */
let bestLap = Infinity;

// ==========================================
// カメラ（MediaDevices API）関連の変数
// ==========================================

/** @type {MediaStream|null} カメラの映像ストリーム */
let stream = null;
/** @type {HTMLVideoElement} リアルタイムのカメラ映像を表示するvideo要素 */
const video = document.getElementById('video');
/** @type {HTMLCanvasElement} 映像を静止画としてキャプチャするためのcanvas要素 */
const canvas = document.getElementById('canvas');
/** @type {HTMLElement} 撮影時の画面フラッシュ演出を担う要素 */
const flash = document.getElementById('shutter-flash');

// ==========================================
// BLE通信制御ロジック
// ==========================================

/**
 * BLEデバイス（ESP32）との接続シーケンスを開始する非同期関数
 */
async function connectBLE() {
  const status = document.getElementById("status");
  try {
    status.innerText = "SELECTING...";
    // 指定したサービスUUIDを持つデバイスを検索し、ユーザーに選択を促す
    device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE_UUID] }],
    });

    // デバイスの切断イベントを購読
    device.addEventListener("gattserverdisconnected", onDisconnected);
    status.innerText = "CONNECTING...";

    // GATTサーバーへの接続
    const server = await device.gatt.connect();
    // プライマリサービスの取得
    const service = await server.getPrimaryService(SERVICE_UUID);
    // データ送受信用キャラクタリスティックの取得
    characteristic = await service.getCharacteristic(CHARACTERISTIC_UUID);

    // デバイスからの通知（Notifications）の受信を開始する
    await characteristic.startNotifications();
    // キャラクタリスティックの値が更新された（データを受信した）際のイベントリスナーを登録
    characteristic.addEventListener("characteristicvaluechanged", handleNotify);

    status.innerText = "CONNECTED";
  } catch (e) {
    status.innerText = "ERROR: " + e.message;
  }
}

/**
 * BLEデバイスが切断された際に呼び出されるコールバック関数
 */
function onDisconnected() {
  document.getElementById("status").innerText = "DISCONNECTED";
}

/**
 * BLEデバイスとの接続を明示的に切断する関数
 */
function disconnectBLE() {
  if (device) device.gatt.disconnect();
}

// ==========================================
// カメラ制御・撮影ロジック
// ==========================================

/**
 * カメラの起動と停止を切り替える非同期関数
 */
async function toggleCamera() {
  const btn = document.getElementById('camera-btn');
  if (!stream) {
    // ストリームが存在しない場合はカメラを起動
    try {
      // 背面カメラ（environment）の映像のみを要求
      stream = await navigator.mediaDevices.getUserMedia({
        video: { facingMode: "environment" },
        audio: false
      });
      video.srcObject = stream;
      // iOSにおけるvideo要素の自動再生制限を回避するため、明示的にplay()を呼び出す
      video.play();
      // UIの更新（停止ボタンへ変更）
      btn.innerText = "STOP CAMERA";
      btn.style.background = "#555";
    } catch (err) {
      alert("カメラの起動に失敗しました: " + err);
    }
  } else {
    // ストリームが存在する場合はカメラを停止
    stream.getTracks().forEach(track => track.stop());
    stream = null;
    video.srcObject = null;
    // UIの更新（起動ボタンへ変更）
    btn.innerText = "START CAMERA";
    btn.style.background = "#f39c12";
  }
}

/**
 * 現在のカメラ映像をキャプチャし、ラップ情報付きの画像を生成・保存・表示する関数
 * @param {number} lapNum - 記録されたラップ数
 */
function takePhoto(lapNum) {
  // カメラが起動していなければ何もしない
  if (!stream) return;

  // 画面を一瞬白くするフラッシュ演出（opacityを1にしてから100ms後に0へ戻す）
  flash.style.opacity = 1;
  setTimeout(() => flash.style.opacity = 0, 100);

  // canvasに現在のvideo要素のフレームを描画
  const context = canvas.getContext('2d');
  canvas.width = video.videoWidth;
  canvas.height = video.videoHeight;
  context.drawImage(video, 0, 0, canvas.width, canvas.height);

  // 撮影画像に「LAP X: 00:00.00」というタイムスタンプを印字
  context.font = "bold 40px monospace";
  context.fillStyle = "yellow";
  const currentTimeStr = document.getElementById("current-time").innerText;
  context.fillText(`LAP ${lapNum}: ${currentTimeStr}`, 30, canvas.height - 30);

  // canvasの内容をPNG画像のデータURLに変換
  const dataUrl = canvas.toDataURL("image/png");

  // --- 画面下の写真履歴UIへ画像を追加 ---
  const historyContainer = document.getElementById('photo-history');
  const img = document.createElement('img');
  img.src = dataUrl;
  img.className = 'captured-img';
  // 新しい画像を履歴リストの最上部（先頭）に挿入
  historyContainer.insertBefore(img, historyContainer.firstChild);

  // --- 撮影画像を端末にダウンロード（既存ロジック維持） ---
  const link = document.createElement('a');
  link.download = `lap_${lapNum}_${new Date().getTime()}.png`;
  link.href = dataUrl;
  link.click();
}

// ==========================================
// BLEデータ受信・解析ロジック
// ==========================================

/**
 * BLEデバイスからNotifyイベントでデータを受信した際に呼び出されるコールバック関数
 * @param {Event} event - characteristicvaluechanged イベントオブジェクト
 */
function handleNotify(event) {
  // 受信したバイナリデータを文字列（UTF-8）にデコード
  const val = new TextDecoder().decode(event.target.value);
  // 文字列から "Lap: 12.345" のようなパターンを抽出し、数値部分を取り出す
  const match = val.match(/Lap:\s*([\d.]+)/);
  if (match) {
    const lapTime = parseFloat(match[1]);
    addLap(lapTime);
  }
}

// ==========================================
// アプリケーション状態の更新・表示ロジック
// ==========================================

/**
 * 受信した新しいラップタイムをアプリケーションに登録し、画面を更新する関数
 * @param {number} time - 計測されたラップタイム（秒）
 */
function addLap(time) {
  // 配列の先頭に新しいラップタイムを追加
  lapTimes.unshift(time);
  // 画面上の「現在のラップタイム」表示を更新
  document.getElementById("current-time").innerText = formatTime(time);

  // 1番目のデータはスタート時の通過（ラップではない）とみなし、2番目以降を有効ラップとして処理する
  if (lapTimes.length > 1) {
    // ベストラップの判定と更新
    if (time < bestLap) {
      bestLap = time;
      document.getElementById("best-time").innerText = formatTime(bestLap);
    }
    // 有効なラップ（No.2以降）が記録されたタイミングで自動撮影を実行
    // （配列の長さが2のとき、lapTimes.length - 1 は 1 となり「Lap 1」として扱われる）
    takePhoto(lapTimes.length - 1);
  }

  // 統計情報とラップ一覧テーブルを更新
  updateStats();
  updateTable();
}

/**
 * 走行周回数と平均ラップタイムを算出し、画面を更新する関数
 */
function updateStats() {
  const dataCount = lapTimes.length;
  const lapCountElem = document.getElementById("lap-count");
  const avgTimeElem = document.getElementById("avg-time");

  // 表示上のラップ数はデータ総数から1（スタート時）を引いた値（0未満にはしない）
  const displayLaps = Math.max(0, dataCount - 1);
  if (lapCountElem) lapCountElem.innerText = displayLaps;

  if (avgTimeElem) {
    // 有効なラップが存在する場合のみ平均を計算
    if (dataCount > 1) {
      // 直近のラップデータを1つ除外して平均を算出する（既存ロジックの維持）
      const lapsToAverage = lapTimes.slice(0, -1);
      const sum = lapsToAverage.reduce((a, b) => a + b, 0);
      const avg = sum / lapsToAverage.length;
      avgTimeElem.innerText = formatTime(avg);
    } else {
      avgTimeElem.innerText = "--:--.--";
    }
  }
}

/**
 * 画面上のラップタイム履歴テーブル（一覧）を更新する関数
 */
function updateTable() {
  const tbody = document.getElementById("lap-list");
  // テーブルの中身を一度リセット
  tbody.innerHTML = "";

  // 最新のラップタイム（配列の先頭）から順にテーブル行を追加
  lapTimes.forEach((time, index) => {
    if (lapTimes.length - index -1 != 0) {
      const row = tbody.insertRow();
      // 行番号（新しいものほど大きい数値になる）
      row.insertCell(0).innerText = lapTimes.length - index -1;
      // フォーマットされたラップタイム文字列
      row.insertCell(1).innerText = formatTime(time);
    }
  });
}

/**
 * 秒数（小数点含む）を "MM:SS.cs" 形式の文字列に変換するユーティリティ関数
 * @param {number} sec - 秒数
 * @returns {string} フォーマット済みの時間文字列 (例: "01:23.45")
 */
function formatTime(sec) {
  const m = Math.floor(sec / 60);
  const s = Math.floor(sec % 60);
  // 小数点第2位まで（センチ秒）を抽出
  const cs = Math.floor((sec * 100) % 100);
  // padStartを使用して常に2桁になるように0埋めを行う
  return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}.${cs.toString().padStart(2, '0')}`;
}

// ==========================================
// クリップボード連携・データクリアロジック
// ==========================================

/**
 * 記録されたすべてのラップタイムをクリップボードにコピーする関数
 */
function copyLaps() {
  if (lapTimes.length === 0) {
    alert("データがありません");
    return;
  }

  // 配列を反転させ、古いラップから順にテキスト化する
  const text = lapTimes.slice().reverse().map((t, i) => `Lap ${i + 1}: ${formatTime(t)}`).join("\n");

  // Clipboard API が利用可能かつセキュアコンテキストであるかを確認
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text)
      .then(() => alert("コピーしました"))
      .catch(err => fallbackCopyTextToClipboard(text)); // 失敗時はフォールバックを実行
  } else {
    // 古いブラウザや非SSL環境用のフォールバック
    fallbackCopyTextToClipboard(text);
  }
}

/**
 * Clipboard API に対応していない環境向けのコピー用フォールバック関数
 * @param {string} text - コピーする文字列
 */
function fallbackCopyTextToClipboard(text) {
  // 画面外に非表示のtextarea要素を作成し、そこにテキストを流し込んでから選択・コピーコマンドを発行する
  const textArea = document.createElement("textarea");
  textArea.value = text;
  document.body.appendChild(textArea);
  textArea.select();
  document.execCommand("copy");
  document.body.removeChild(textArea);
  alert("コピーしました");
}

/**
 * メモリ上のすべての計測データと、画面上の表示履歴を初期状態にリセットする関数
 */
function clearData() {
  // ユーザーに実行確認を求める
  if (confirm("データをすべて消去しますか？")) {
    // 変数の初期化
    lapTimes = [];
    bestLap = Infinity;

    // UI表示の初期化
    document.getElementById("best-time").innerText = "--:--.--";
    document.getElementById("current-time").innerText = "--:--.--";

    const lapCountElem = document.getElementById("lap-count");
    const avgTimeElem = document.getElementById("avg-time");
    if (lapCountElem) lapCountElem.innerText = "0";
    if (avgTimeElem) avgTimeElem.innerText = "--:--.--";

    // テーブルと写真履歴のDOM要素を空にする
    document.getElementById("lap-list").innerHTML = "";
    document.getElementById('photo-history').innerHTML = "";
  }
}