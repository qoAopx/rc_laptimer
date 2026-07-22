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
// ★ ストップウォッチ関連の変数
// ==========================================

/** @type {number|null} ストップウォッチの開始時刻（performance.now()基準） */
let lapStartTime = null;
/** @type {number|null} requestAnimationFrameのID */
let rafId = null;

/**
 * ★ ストップウォッチの表示をリアルタイム更新するループ
 */
function tickStopwatch() {
  if (lapStartTime === null) return;
  const elapsed = (performance.now() - lapStartTime) / 1000;
  document.getElementById("current-time").innerText = formatTime(elapsed);
  rafId = requestAnimationFrame(tickStopwatch);
}

/**
 * ★ ストップウォッチをリセットして0からカウント開始
 * @param {number} anchorTime - performance.now()基準の開始時刻
 */
function resetStopwatch(anchorTime) {
  if (rafId !== null) cancelAnimationFrame(rafId);
  lapStartTime = anchorTime;
  rafId = requestAnimationFrame(tickStopwatch);
}

// ==========================================
// ★ BLE接続状態による背景色制御
// ==========================================

/**
 * ★ body要素のクラスでBLE接続状態を背景色に反映する
 * @param {'disconnected'|'connected'|'lap'} state
 */
function setBLEState(state) {
  const body = document.body;
  body.classList.remove('ble-connected', 'ble-lap');
  if (state === 'connected') {
    body.classList.add('ble-connected');
  } else if (state === 'lap') {
    // 一度クラスを外して再付与することでアニメーションをリトリガーする
    void body.offsetWidth;
    body.classList.add('ble-connected', 'ble-lap');
  }
  // 'disconnected' は何もクラスを付けない（CSSデフォルトの濃いグレー）
}

// ==========================================
// BLE通信制御ロジック（変更なし）
// ==========================================

/**
 * BLEデバイス（ESP32）との接続シーケンスを開始する非同期関数
 */
async function connectBLE() {
  const status = document.getElementById("status");
  try {
    status.innerText = "SELECTING...";
    device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE_UUID] }],
    });

    device.addEventListener("gattserverdisconnected", onDisconnected);
    status.innerText = "CONNECTING...";

    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE_UUID);
    characteristic = await service.getCharacteristic(CHARACTERISTIC_UUID);

    await characteristic.startNotifications();
    characteristic.addEventListener("characteristicvaluechanged", handleNotify);

    status.innerText = "CONNECTED";
    setBLEState('connected'); // ★ 接続時：背景を黒に
  } catch (e) {
    status.innerText = "ERROR: " + e.message;
  }
}

/**
 * BLEデバイスが切断された際に呼び出されるコールバック関数
 */
function onDisconnected() {
  document.getElementById("status").innerText = "DISCONNECTED";
  setBLEState('disconnected'); // ★ 切断時：背景を濃いグレーに
  // ★ ストップウォッチを停止
  if (rafId !== null) {
    cancelAnimationFrame(rafId);
    rafId = null;
  }
}

/**
 * BLEデバイスとの接続を明示的に切断する関数
 */
function disconnectBLE() {
  if (device) device.gatt.disconnect();
}

// ==========================================
// カメラ制御・撮影ロジック（変更なし）
// ==========================================

/**
 * カメラの起動と停止を切り替える非同期関数
 */
async function toggleCamera() {
  const btn = document.getElementById('camera-btn');
  if (!stream) {
    try {
      stream = await navigator.mediaDevices.getUserMedia({
        video: { facingMode: "environment" },
        audio: false
      });
      video.srcObject = stream;
      video.play();
      btn.innerText = "STOP CAMERA";
      btn.style.background = "#555";
    } catch (err) {
      alert("カメラの起動に失敗しました: " + err);
    }
  } else {
    stream.getTracks().forEach(track => track.stop());
    stream = null;
    video.srcObject = null;
    btn.innerText = "START CAMERA";
    btn.style.background = "#f39c12";
  }
}

/**
 * 現在のカメラ映像をキャプチャし、ラップ情報付きの画像を生成・保存・表示する関数
 * @param {number} lapNum - 記録されたラップ数
 * @param {string} lapTimeStr - ★ ESP32から受信したラップタイム文字列
 */
function takePhoto(lapNum, lapTimeStr) {
  if (!stream) return;

  flash.style.opacity = 1;
  setTimeout(() => flash.style.opacity = 0, 100);

  const context = canvas.getContext('2d');
  canvas.width = video.videoWidth;
  canvas.height = video.videoHeight;
  context.drawImage(video, 0, 0, canvas.width, canvas.height);

  context.font = "bold 40px monospace";
  context.fillStyle = "yellow";
  // ★ DOMのcurrent-timeでなくESP32受信値をそのまま印字（lap-listと同一の値）
  context.fillText(`LAP ${lapNum}: ${lapTimeStr}`, 30, canvas.height - 30);

  const now = new Date();
  const formatted = now.toLocaleString('ja-JP', {
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit'
  });
  context.font = "bold 24px monospace";
  context.fillStyle = "yellow";
  context.fillText(formatted, 30, 40);


  const dataUrl = canvas.toDataURL("image/jpeg");

  const historyContainer = document.getElementById('photo-history');
  const img = document.createElement('img');
  img.src = dataUrl;
  img.className = 'captured-img';
  historyContainer.insertBefore(img, historyContainer.firstChild);

  const link = document.createElement('a');
  link.download = `lap_${lapNum}_${new Date().getTime()}.jpeg`;
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
  // ★ BLE受信の瞬間のタイムスタンプを記録（ストップウォッチのアンカーに使用）
  const receiveTime = performance.now();
  const val = new TextDecoder().decode(event.target.value);
  const match = val.match(/Lap:\s*([\d.]+)/);
  if (match) {
    const lapTime = parseFloat(match[1]);
    addLap(lapTime, receiveTime);
  }
}

// ==========================================
// アプリケーション状態の更新・表示ロジック
// ==========================================

/**
 * 受信した新しいラップタイムをアプリケーションに登録し、画面を更新する関数
 * @param {number} time - 計測されたラップタイム（秒）
 * @param {number} receiveTime - BLE受信時刻（performance.now()基準）
 */
function addLap(time, receiveTime) {
  lapTimes.unshift(time);

  // ★ ラップ通過時の背景を赤黒点滅させる
  setBLEState('lap');

  if (lapTimes.length === 1) {
    // ★ 初回通過（スタート検出）：BLE受信時刻を起点にストップウォッチ開始
    resetStopwatch(receiveTime);
  } else {
    // ★ 2周目以降：ラップ確定
    if (time < bestLap) {
      bestLap = time;
      document.getElementById("best-time").innerText = formatTime(bestLap);
    }
    takePhoto(lapTimes.length - 1, formatTime(time)); // ★ ESP32のラップタイムをそのまま渡す
    // ★ BLE受信時刻を起点に次のラップのストップウォッチをリセット
    resetStopwatch(receiveTime);
  }

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

  const displayLaps = Math.max(0, dataCount - 1);
  if (lapCountElem) lapCountElem.innerText = displayLaps;

  if (avgTimeElem) {
    if (dataCount > 1) {
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
  tbody.innerHTML = "";
  lapTimes.forEach((time, index) => {
    if (lapTimes.length - index - 1 != 0) {
      const row = tbody.insertRow();
      if (time === bestLap) {
        row.style = "background-color:#28a745;";
      }
      row.insertCell(0).innerText = lapTimes.length - index - 1;
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
  const cs = Math.floor((sec * 100) % 100);
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

  const text = lapTimes.slice().reverse().map((t, i) => `Lap ${i + 1}: ${formatTime(t)}`).join("\n");

  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text)
      .then(() => alert("コピーしました"))
      .catch(err => fallbackCopyTextToClipboard(text));
  } else {
    fallbackCopyTextToClipboard(text);
  }
}

/**
 * Clipboard API に対応していない環境向けのコピー用フォールバック関数
 * @param {string} text - コピーする文字列
 */
function fallbackCopyTextToClipboard(text) {
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
  if (confirm("データをすべて消去しますか？")) {
    lapTimes = [];
    bestLap = Infinity;

    // ★ ストップウォッチ停止・リセット
    if (rafId !== null) {
      cancelAnimationFrame(rafId);
      rafId = null;
    }
    lapStartTime = null;

    document.getElementById("best-time").innerText = "--:--.--";
    document.getElementById("current-time").innerText = "00:00.00";

    const lapCountElem = document.getElementById("lap-count");
    const avgTimeElem = document.getElementById("avg-time");
    if (lapCountElem) lapCountElem.innerText = "0";
    if (avgTimeElem) avgTimeElem.innerText = "--:--.--";

    document.getElementById("lap-list").innerHTML = "";
    document.getElementById('photo-history').innerHTML = "";
  }
}