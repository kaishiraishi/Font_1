#include <Arduino.h>
#include <BLEDevice.h>
#include "WS_Flow.h"

// ===================== 1) 定数・グローバル =====================
// 固定メッセージ（各機で変えてOK / ASCII英数のみ推奨）
static const char* USER_MESSAGE = "Waveshare ESP32-S3-Matrix Text Testing!"; // 長文スクロール用

// NUS互換UUID（Nordic UART Service）
static const char* SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* CHAR_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // 相手→自分（Write）
static const char* CHAR_TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // 自分→相手（Notify）

// BLE設定
static const uint16_t DESIRED_MTU = 185;              // 希望値（交渉結果は相手依存）
static const uint32_t SEND_INTERVAL_MS = 1500;        // 定期送出
static const uint32_t ADV_UPDATE_INTERVAL_MS = 3000;  // 広告断片更新

// スキャンパラメータ（短すぎを避ける）
constexpr uint16_t SCAN_ITVL = 45;  // ms
constexpr uint16_t SCAN_WIN  = 30;  // ms

// 表示用テキストバッファ（描画は loop 側）
static char Text[128] = "Waveshare ESP32-S3-Matrix Text Testing!";

// 役割
enum Role { ROLE_UNKNOWN, ROLE_PERIPHERAL, ROLE_CENTRAL };
volatile Role currentRole = ROLE_UNKNOWN;

// サーバ側ハンドル
static BLEServer* g_server = nullptr;
static BLECharacteristic* g_chrRX = nullptr; // Write受け
static BLECharacteristic* g_chrTX = nullptr; // Notify送出
volatile bool g_serverConnected = false;

// クライアント側ハンドル
static BLEClient* g_client = nullptr;
static BLERemoteCharacteristic* g_rmtRX = nullptr; // 相手のWrite特性
static BLERemoteCharacteristic* g_rmtTX = nullptr; // 相手のNotify特性
volatile bool g_clientConnected = false;

// タイマ
static uint32_t lastSendMs = 0;
static uint32_t lastAdvUpdateMs = 0;

// 先行宣言
static void updateDisplay(const String& s);
static String macClean(const String& mac);
static String macClean(const char* mac);
static bool iAmInitiatorFor(const BLEAddress& peer);
static void setAdvPayloadWithMsg(const String& name, const String& msg);
static void setupPeripheral(const String& devName);
static void setupCentralScan();
static bool connectToServer(BLEAdvertisedDevice* adv);
static void sendMyMessage();
static void onNotifyCB(BLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify);
static void scanTask(void* pv);

// ===================== 2) ユーティリティ関数 =====================
static void updateDisplay(const String& s) {
  s.toCharArray(Text, sizeof(Text));
  // Matrix_ResetScroll(); // リンク不整合回避のため一時無効化（スクロール開始位置は現状維持）
  Serial.print("Display updated: ");
  Serial.println(s);
}

static String macClean(const char* macCStr) {
  String out; if (!macCStr) return out;
  out.reserve(12);
  for (size_t i = 0; macCStr[i] != '\0'; ++i) {
    char c = macCStr[i];
    if (c == ':') continue;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    out += c;
  }
  return out;
}

static String macClean(const String& mac) {
  return macClean(mac.c_str());
}

static bool iAmInitiatorFor(const BLEAddress& peer) {
  String my = macClean(BLEDevice::getAddress().toString());
  BLEAddress tmp(peer);
  String other = macClean(tmp.toString());
  return (my < other);
}

static void setAdvPayloadWithMsg(const String& name, const String& msg) {
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  if (!adv) return;
  String fragment = String("MSG:") + msg.substring(0, 18);
  BLEAdvertisementData ad; BLEAdvertisementData scan;
  ad.setName(name.c_str());
  ad.setCompleteServices(BLEUUID(SERVICE_UUID));
  ad.setFlags(0x06);
  scan.setName(name.c_str());
  scan.setServiceData(BLEUUID(SERVICE_UUID), fragment);
  adv->stop();
  adv->setAdvertisementData(ad);
  adv->setScanResponseData(scan);
  adv->setScanResponse(true);
  adv->start();
}

// ===================== 3) コールバック =====================
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    g_serverConnected = true;
    // 接続後に広告が止まる環境の保険
    BLEDevice::getAdvertising()->start();
  }
  void onDisconnect(BLEServer* s) override {
    g_serverConnected = false;
    BLEDevice::getAdvertising()->start(); // 即再広告
  }
};

class RXCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    String v = ch->getValue();
    if (v.length() > 0) {
      updateDisplay(v);
      if (g_chrTX && g_serverConnected) {
        g_chrTX->setValue((uint8_t*)v.c_str(), v.length());
        g_chrTX->notify();
      }
    }
  }
};

static void onNotifyCB(BLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
  if (data && len > 0) {
    String msg; msg.reserve(len);
    for (size_t i = 0; i < len; ++i) msg += (char)data[i];
    updateDisplay(msg);
  }
}

// ===================== 4) Peripheral 初期化 =====================
static void setupPeripheral(const String& devName) {
  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCB());

  // NUS サービス
  BLEService* svc = g_server->createService(SERVICE_UUID);

  // RX（Write/WriteNR）
  g_chrRX = svc->createCharacteristic(
    CHAR_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  g_chrRX->setCallbacks(new RXCB());

  // TX（Notify）
  g_chrTX = svc->createCharacteristic(
    CHAR_TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );

  svc->start();

  // 広告
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  // 初期ペイロード（断片）
  setAdvPayloadWithMsg(devName, USER_MESSAGE);
  // 広告開始
  adv->start();

  currentRole = ROLE_PERIPHERAL;
}

// ===================== 5) Central 側：スキャン＆接続 =====================
static bool connectToServer(BLEAdvertisedDevice* advDev) {
  if (!advDev) return false;

  if (!g_client) g_client = BLEDevice::createClient();
  if (!g_client->connect(advDev)) {
    return false;
  }

  // NUS サービス
  BLERemoteService* s = g_client->getService(BLEUUID(SERVICE_UUID));
  if (!s) {
    g_client->disconnect();
    return false;
  }

  g_rmtRX = s->getCharacteristic(BLEUUID(CHAR_RX_UUID));
  g_rmtTX = s->getCharacteristic(BLEUUID(CHAR_TX_UUID));
  if (!g_rmtRX || !g_rmtTX) {
    g_client->disconnect();
    g_rmtRX = nullptr; g_rmtTX = nullptr;
    return false;
  }

  // Notify購読
  if (g_rmtTX->canNotify()) {
    g_rmtTX->registerForNotify(onNotifyCB);
  }

  g_clientConnected = true;
  currentRole = ROLE_CENTRAL;
  updateDisplay("Connected as Central");
  return true;
}

class AdvCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice adv) override {
    if (!adv.isAdvertisingService(BLEUUID(SERVICE_UUID))) return;
    if (!iAmInitiatorFor(adv.getAddress())) return;
    BLEDevice::getScan()->stop();
    BLEAdvertisedDevice* heapCopy = new BLEAdvertisedDevice(adv);
    if (!connectToServer(heapCopy)) {
      // スキャン再開はバックグラウンドタスクに任せる
    }
  }
};

static void setupCentralScan() {
  BLEScan* scan = BLEDevice::getScan();
  if (!scan) return;

  scan->setAdvertisedDeviceCallbacks(new AdvCB(), false /*不重複*/);
  scan->setInterval(SCAN_ITVL);
  scan->setWindow(SCAN_WIN);
  scan->setActiveScan(true);

  // ブロッキングを避けるため、スキャンは別タスクで回す
  xTaskCreatePinnedToCore(scanTask, "bleScan", 4096, nullptr, 1, nullptr, 1);
}

static void scanTask(void* pv) {
  BLEScan* scan = BLEDevice::getScan();
  for (;;) {
    if (!scan) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      scan = BLEDevice::getScan();
      continue;
    }
    // 短いサイクルでスキャンし続ける（非同期）
    scan->start(5 /*sec*/, false); // 5秒ブロック（このタスク内のみ）
    scan->clearResults();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ===================== 6) 定期送出（"垂れ流し"） =====================
static void sendMyMessage() {
  const char* msg = USER_MESSAGE;
  size_t len = strlen(msg);
  if (len == 0) return;

  // 実効MTU（フォールバック）
  size_t mtu = DESIRED_MTU;
  if (g_client && g_client->isConnected()) {
    mtu = g_client->getMTU();
  }
  size_t maxChunk = (mtu > 3) ? (mtu - 3) : 20; // ATTヘッダ控除
  if (maxChunk > 244) maxChunk = 244;           // 安全上限

  if (currentRole == ROLE_CENTRAL && g_clientConnected && g_rmtRX) {
    // Central：相手のRXへWrite（with response）
    for (size_t off = 0; off < len; off += maxChunk) {
      size_t chunk = (off + maxChunk <= len) ? maxChunk : (len - off);
      g_rmtRX->writeValue((uint8_t*)(msg + off), chunk, true);
    }
  } else if (currentRole == ROLE_PERIPHERAL && g_serverConnected && g_chrTX) {
    // Peripheral：こちらのTXからNotify
    for (size_t off = 0; off < len; off += maxChunk) {
      size_t chunk = (off + maxChunk <= len) ? maxChunk : (len - off);
      g_chrTX->setValue((uint8_t*)(msg + off), chunk);
      g_chrTX->notify();
    }
  }

  updateDisplay(String(msg)); // 自端表示
}

// ===================== 7) setup() 拡張（順序厳守） =====================
void setup() {
  // 文字表示の基本シーケンスに合わせて、まずマトリクス初期化
  Matrix_Init();

  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ESP32-S3 Matrix BLE...");

  Serial.println("Matrix initialized");

  // 起動確認（全点灯テストは一時停止：リンク不整合回避）
  // Serial.println("Boot LED test (red, 1s)...");
  // Matrix_BootTest(255, 0, 0, 1000);
  // Serial.println("Boot LED test done");

  BLEDevice::init("Matrix");
  BLEDevice::setMTU(DESIRED_MTU);
  Serial.println("BLE initialized");

  String devName = String("Matrix-") + macClean(BLEDevice::getAddress().toString());
  Serial.print("Device name: ");
  Serial.println(devName);

  setupPeripheral(devName);
  Serial.println("Peripheral setup done");
  setupCentralScan();
  Serial.println("Central scan setup done");

  lastSendMs = millis();
  lastAdvUpdateMs = millis();
  Serial.println("Setup complete");
}

// ===================== 8) loop() 拡張（既存描画は維持） =====================
void loop() {
  // まずは表示を最優先で回す（参考コード準拠）
  Text_Flow(Text);
  delay(100);

  // --- 以下、既存のBLE処理は並行で動作 ---
  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    Serial.println("Sending message...");
    sendMyMessage();
  }

  if (millis() - lastAdvUpdateMs >= ADV_UPDATE_INTERVAL_MS) {
    lastAdvUpdateMs = millis();
    Serial.println("Updating advertisement...");
    setAdvPayloadWithMsg("Matrix", USER_MESSAGE);
  }

  if (currentRole == ROLE_CENTRAL && g_client && !g_client->isConnected()) {
    g_clientConnected = false;
    currentRole = ROLE_UNKNOWN;
    Serial.println("Connection lost, returning to WAITING");
    // updateDisplay("WAITING");  // 表示上書きはしない
    setupCentralScan();
    currentRole = ROLE_PERIPHERAL;
  }
}
