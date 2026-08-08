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
// ★ スリープ防止（Wake Lock API）
// ==========================================

/** @type {WakeLockSentinel|null} スリープ防止のWakeLockインスタンス */
let wakeLock = null;

/**
 * ★ スリープ防止を開始する
 */
async function requestWakeLock() {
  if ('wakeLock' in navigator) {
    try {
      wakeLock = await navigator.wakeLock.request('screen');
      // タブ復帰時に再取得（バックグラウンドに行くと自動解放されるため）
      document.addEventListener('visibilitychange', async () => {
        if (document.visibilityState === 'visible' && wakeLock === null) {
          wakeLock = await navigator.wakeLock.request('screen');
        }
      });
    } catch (e) {
      console.warn('Wake Lock取得失敗:', e);
    }
  }
}

// ページ読み込み時にスリープ防止を開始
requestWakeLock();

// ==========================================
// ★ ビープ音（Web Audio API）
// ==========================================

/** @type {AudioContext|null} */
let audioCtx = null;

/**
 * ★ ビープ音を鳴らす
 * @param {number} freq - 周波数 Hz（デフォルト880Hz）
 * @param {number} duration - 長さ ms（デフォルト150ms）
 */
function beep(freq = 880, duration = 150) {
  // AudioContextはユーザー操作後でないと生成できないため遅延初期化
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  const osc = audioCtx.createOscillator();
  const gain = audioCtx.createGain();
  osc.connect(gain);
  gain.connect(audioCtx.destination);
  osc.type = 'square';
  osc.frequency.setValueAtTime(freq, audioCtx.currentTime);
  gain.gain.setValueAtTime(0.3, audioCtx.currentTime);
  gain.gain.exponentialRampToValueAtTime(0.001, audioCtx.currentTime + duration / 1000);
  osc.start(audioCtx.currentTime);
  osc.stop(audioCtx.currentTime + duration / 1000);
}

// ==========================================
// ★ 音声読み上げ（Web Speech API）
// ==========================================

/**
 * ★ テキストを日本語音声で読み上げる
 * @param {string} text - 読み上げるテキスト
 */
function speak(text) {
  if (!('speechSynthesis' in window)) return; // 未対応ブラウザは無視
  // 前の発話が残っていたらキャンセルしてから新しい発話を積む（読み上げの詰まり防止）
  window.speechSynthesis.cancel();
  const utter = new SpeechSynthesisUtterance(text);
  utter.lang = 'ja-JP';
  utter.rate = 1.2;   // やや速め
  utter.pitch = 1.3;
  window.speechSynthesis.speak(utter);
}

/**
 * ★ ラップタイム（秒）を「〇〇点〇〇秒」の読み上げ用文字列に変換する
 * @param {number} sec - 秒数
 * @returns {string} 読み上げ用文字列（例："12点34秒"）
 */
function lapTimeToSpeechText(sec) {
  const wholeMin = Math.floor(sec / 60);// 分
  const msgMin = wholeMin === 0 ? '' : `${wholeMin}分`;
  const wholeSec = Math.floor(sec % 60);// 秒だけ
  const cs = Math.floor((sec * 100) % 100); // センチ秒（小数点2桁）
  return `${msgMin} ${wholeSec}秒 ${cs}`;
}

// ==========================================
// ★ CONNECTボタンのステータスアイコン更新
// ==========================================

/**
 * ★ CONNECTボタンの表示を接続状態に応じて更新する
 * @param {'disconnected'|'connecting'|'connected'} state
 */
function setConnectBtnState(state) {
  const btn = document.getElementById('connect-btn');
  if (!btn) return;
  const labels = {
    disconnected: '🔴 CONNECT',
    connecting:   '🟡 CONNECT',
    connected:    '🟢 CONNECT',
  };
  btn.innerText = labels[state] ?? '🔴 CONNECT';
}

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
// ★ BLE Notify ヘルスチェック（購読だけ再開する軽量リカバリ）
// ==========================================
// GATT接続(status=CONNECTED)は維持されたまま、Notify購読だけが
// 内部的に停止してデータが来なくなる現象（Bluefy/WKWebView等）への対策。
// 一定時間データを受信しなければ、まずは軽量な「Notify再購読」を試み、
// それでも失敗した場合のみGATT自体の再接続にフォールバックする。

/** @type {number} 最後にBLEデータを受信した時刻（Date.now()基準） */
let lastReceiveTime = Date.now();
/** @type {number|null} ヘルスチェックのインターバルID */
let healthCheckIntervalId = null;

const HEALTH_CHECK_INTERVAL_MS = 5000;  // 5秒ごとにチェック
const NO_DATA_TIMEOUT_MS = 20000;       // 20秒データが来なければ異常とみなす

/**
 * ★ ヘルスチェックを開始する（BLE接続確立時に呼ぶ）
 */
function startHealthCheck() {
  stopHealthCheck();
  lastReceiveTime = Date.now();
  healthCheckIntervalId = setInterval(async () => {
    if (!device || !device.gatt.connected) return; // 切断中は何もしない
    const elapsed = Date.now() - lastReceiveTime;
    if (elapsed > NO_DATA_TIMEOUT_MS) {
      console.warn(`Notify無受信 ${Math.floor(elapsed / 1000)}秒経過。購読を再開します。`);
      await resubscribeNotify();
    }
  }, HEALTH_CHECK_INTERVAL_MS);
}

/**
 * ★ ヘルスチェックを停止する
 */
function stopHealthCheck() {
  if (healthCheckIntervalId !== null) {
    clearInterval(healthCheckIntervalId);
    healthCheckIntervalId = null;
  }
}

/**
 * ★ GATT接続はそのままに、Notify購読だけをやり直す（軽量リカバリ）
 * 失敗した場合のみフルのGATT再接続にフォールバックする
 */
async function resubscribeNotify() {
  if (!characteristic) return;
  try {
    await characteristic.stopNotifications();
    await characteristic.startNotifications();
    lastReceiveTime = Date.now(); // 再購読直後にタイムアウト判定が再発火しないようリセット
    console.log("BLE Notify購読を再開しました");
  } catch (e) {
    console.warn("Notify再購読に失敗。GATT再接続を試みます:", e);
    await attemptReconnect();
  }
}

/**
 * ★ GATT接続自体を切断→再接続し、Notify購読をやり直す（フルリカバリ）
 * resubscribeNotify() が失敗した場合の最終手段
 */
async function attemptReconnect() {
  if (!device) return;
  const status = document.getElementById("status");
  try {
    status.innerText = "RECONNECTING...";
    setConnectBtnState('connecting');

    if (device.gatt.connected) {
      device.gatt.disconnect();
      await new Promise(r => setTimeout(r, 300));
    }

    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE_UUID);
    characteristic = await service.getCharacteristic(CHARACTERISTIC_UUID);
    await characteristic.startNotifications();
    characteristic.addEventListener("characteristicvaluechanged", handleNotify);

    lastReceiveTime = Date.now();
    status.innerText = "CONNECTED";
    setBLEState('connected');
    setConnectBtnState('connected');
    console.log("BLE GATT再接続に成功しました");
  } catch (e) {
    status.innerText = "RECONNECT FAILED";
    setConnectBtnState('disconnected');
    console.error("BLE再接続に失敗しました:", e);
  }
}

// ★ 画面がバックグラウンドから復帰した瞬間にも無受信時間をチェックする
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && device && device.gatt && device.gatt.connected) {
    const elapsed = Date.now() - lastReceiveTime;
    if (elapsed > NO_DATA_TIMEOUT_MS) {
      console.warn(`画面復帰時に無受信 ${Math.floor(elapsed / 1000)}秒を検出。購読を再開します。`);
      resubscribeNotify();
    }
  }
});

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
    setConnectBtnState('connecting'); // ★
    device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE_UUID] }],
    });

    device.addEventListener("gattserverdisconnected", onDisconnected);
    status.innerText = "CONNECTING...";
    setConnectBtnState('connecting'); // ★

    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE_UUID);
    characteristic = await service.getCharacteristic(CHARACTERISTIC_UUID);

    await characteristic.startNotifications();
    characteristic.addEventListener("characteristicvaluechanged", handleNotify);

    status.innerText = "CONNECTED";
    setBLEState('connected');       // ★ 接続時：背景を黒に
    setConnectBtnState('connected'); // ★
    startHealthCheck(); // ★ Notify無受信を監視するヘルスチェックを開始
  } catch (e) {
    status.innerText = "ERROR: " + e.message;
    setConnectBtnState('disconnected'); // ★ エラー時も未接続アイコンに戻す
  }
}

/**
 * BLEデバイスが切断された際に呼び出されるコールバック関数
 */
function onDisconnected() {
  document.getElementById("status").innerText = "DISCONNECTED";
  setBLEState('disconnected');        // ★ 切断時：背景を濃いグレーに
  setConnectBtnState('disconnected'); // ★
  stopHealthCheck(); // ★ ヘルスチェック停止
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
  lastReceiveTime = Date.now(); // ★ ヘルスチェック用に最終受信時刻を更新
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
    // ★ 初回通過（スタート検出）：短いビープ1回 ＋ 「スタート」読み上げ
    beep(660, 100);
    speak("スタート");
    resetStopwatch(receiveTime);
  } else {
    // ★ ラップ確定：高めのビープ2回
    beep(880, 120);
    setTimeout(() => beep(880, 120), 180);
    // ★ 2周目以降：ラップ確定
    if (time < bestLap) {
      bestLap = time;
      document.getElementById("best-time").innerText = formatTime(bestLap);
    }
    // ★ 「〇〇周、〇〇点〇〇秒」を読み上げ（表示上のラップ数と同じ lapTimes.length - 1 を使用）
    const lapNo = lapTimes.length - 1;
    speak(`${lapNo}周目、 ${lapTimeToSpeechText(time)}`);
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