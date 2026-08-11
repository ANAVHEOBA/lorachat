#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#define RADIO_BACKEND_AS32_UART 1
#define RADIO_BACKEND_SPI_LORA 2

#ifndef RADIO_BACKEND
#define RADIO_BACKEND RADIO_BACKEND_AS32_UART
#endif

#if RADIO_BACKEND == RADIO_BACKEND_SPI_LORA
#include <SPI.h>
#include <LoRa.h>
#endif

/*
  ESP32 LoRa Mesh Chat

  Each ESP32 creates its own WiFi access point for nearby phones.
  Chat messages are carried between ESP32 nodes over LoRa.

  Flash this sketch to each board with a different NODE_PRESET.

  Match LORA_FREQUENCY_HZ to your real LoRa module and antenna.
*/

#ifndef NODE_PRESET
#define NODE_PRESET 1
#endif

const uint8_t BROADCAST_ADDRESS = 0xFF;

#if NODE_PRESET == 1
const uint8_t NODE_ADDRESS = 0xAA;
const uint8_t PEER_ADDRESS = 0xBB;
const char *AP_SSID = "LoRaChat-A";
#elif NODE_PRESET == 2
const uint8_t NODE_ADDRESS = 0xBB;
const uint8_t PEER_ADDRESS = 0xAA;
const char *AP_SSID = "LoRaChat-B";
#elif NODE_PRESET == 3
const uint8_t NODE_ADDRESS = 0xCC;
const uint8_t PEER_ADDRESS = 0xAA;
const char *AP_SSID = "LoRaChat-C";
#else
#error "Set NODE_PRESET to 1, 2, or 3"
#endif

const char *AP_PASSWORD = "chatpass123";

#if RADIO_BACKEND != RADIO_BACKEND_AS32_UART && RADIO_BACKEND != RADIO_BACKEND_SPI_LORA
#error "Set RADIO_BACKEND to RADIO_BACKEND_AS32_UART or RADIO_BACKEND_SPI_LORA"
#endif

const long LORA_FREQUENCY_HZ = 433000000L;
const uint8_t LORA_SYNC_WORD = 0xF3;
const uint8_t LORA_SPREADING_FACTOR = 10;
const long LORA_SIGNAL_BANDWIDTH = 125000L;
const uint8_t LORA_CODING_RATE_DENOMINATOR = 5;
const uint8_t LORA_TX_POWER_DBM = 17;
const uint16_t LORA_PREAMBLE_LENGTH = 8;
const uint8_t LORA_HOP_LIMIT = 2;

// Default group chat mode. Change to PEER_ADDRESS for direct one-hop ACK/retry.
const uint8_t CHAT_DESTINATION = BROADCAST_ADDRESS;

// Change this on every node before field use. All nodes in one chat must match.
const char *CHAT_CRYPTO_PSK = "change-this-lora-chat-key";

const uint8_t LORA_SS = 5;
const uint8_t LORA_SCK = 18;
const uint8_t LORA_MISO = 19;
const uint8_t LORA_MOSI = 23;
const int8_t LORA_RESET = -1;
const int8_t LORA_DIO0 = -1;

const uint8_t AS32_RX_PIN = 16;   // ESP32 RX2 receives AS32 TXD.
const uint8_t AS32_TX_PIN = 17;   // ESP32 TX2 drives AS32 RXD.
const uint8_t AS32_MD0_PIN = 25;
const uint8_t AS32_MD1_PIN = 26;
const uint8_t AS32_AUX_PIN = 27;
const uint32_t AS32_BAUD = 9600;
const unsigned long AS32_IDLE_TIMEOUT_MS = 1200;
const unsigned long AS32_FRAME_TIMEOUT_MS = 250;
const uint8_t TRANSPORT_MAGIC_1 = 0xA7;
const uint8_t TRANSPORT_MAGIC_2 = 0x5A;

const uint8_t PROTOCOL_VERSION = 2;
const uint8_t PACKET_TYPE_DATA = 1;
const uint8_t PACKET_TYPE_ACK = 2;
const uint8_t PACKET_TYPE_HELLO = 3;
const uint8_t PACKET_HEADER_SIZE = 8;
const uint8_t CRYPTO_NONCE_SIZE = 4;
const uint8_t CRYPTO_TAG_SIZE = 8;
const uint8_t CRYPTO_AES_KEY_SIZE = 16;
const uint8_t CRYPTO_HMAC_KEY_SIZE = 32;

const uint8_t MAX_RETRIES = 3;
const unsigned long ACK_TIMEOUT_MS = 4500;
const unsigned long HELLO_INTERVAL_MS = 30000;
const unsigned long HELLO_FIRST_MIN_MS = 1200;
const unsigned long HELLO_FIRST_MAX_MS = 4500;
const unsigned long RELAY_DELAY_MIN_MS = 250;
const unsigned long RELAY_DELAY_MAX_MS = 950;
const uint8_t TX_QUEUE_SIZE = 8;
const uint8_t RELAY_QUEUE_SIZE = 4;
const uint8_t HISTORY_SIZE = 60;
const uint8_t SEEN_CACHE_SIZE = 16;
const uint8_t MESH_NODE_COUNT = 3;
const size_t MAX_SENDER_LEN = 18;
const size_t MAX_MESSAGE_LEN = 120;
const size_t MAX_PLAINTEXT_LEN = MAX_SENDER_LEN + 1 + MAX_MESSAGE_LEN;
const size_t MAX_FRAME_LEN = CRYPTO_NONCE_SIZE + MAX_PLAINTEXT_LEN + CRYPTO_TAG_SIZE;
const size_t MAX_RADIO_PACKET_LEN = PACKET_HEADER_SIZE + MAX_FRAME_LEN;

WebServer server(80);
bool staticFsReady = false;

struct ChatEntry {
  uint32_t seq = 0;
  uint16_t id = 0;
  uint8_t from = 0;
  bool outgoing = false;
  String sender;
  String text;
  String status;
  int rssi = 0;
  float snr = 0;
  uint8_t hops = 0;
  unsigned long createdAt = 0;
};

struct TxItem {
  uint16_t id = 0;
  uint8_t to = BROADCAST_ADDRESS;
  String sender;
  String text;
};

struct PendingTx {
  bool active = false;
  uint16_t id = 0;
  uint8_t to = BROADCAST_ADDRESS;
  String sender;
  String text;
  uint8_t attempts = 0;
  unsigned long nextRetryAt = 0;
};

struct SeenPacket {
  uint8_t from = 0;
  uint16_t id = 0;
  uint8_t relay = 0;
  uint8_t hopLimit = 0;
  unsigned long seenAt = 0;
};

struct PendingRelay {
  bool active = false;
  uint8_t type = PACKET_TYPE_DATA;
  uint8_t to = BROADCAST_ADDRESS;
  uint8_t from = 0;
  uint16_t id = 0;
  uint8_t hopLimit = 0;
  uint8_t frameLen = 0;
  uint8_t frame[MAX_FRAME_LEN] = {0};
  unsigned long sendAt = 0;
};

struct MeshNodeInfo {
  uint8_t address = 0;
  const char *name = "";
  const char *role = "";
  bool local = false;
  bool heard = false;
  uint32_t rxCount = 0;
  uint32_t txCount = 0;
  uint32_t relayCount = 0;
  uint8_t lastVia = 0;
  uint8_t lastHops = 0;
  int lastRssi = 0;
  float lastSnr = 0;
  unsigned long lastHeardAt = 0;

  MeshNodeInfo() {}

  MeshNodeInfo(uint8_t addressIn, const char *nameIn, const char *roleIn, bool localIn)
      : address(addressIn), name(nameIn), role(roleIn), local(localIn) {}
};

ChatEntry history[HISTORY_SIZE];
uint8_t historyCount = 0;
uint32_t nextSequence = 1;

TxItem txQueue[TX_QUEUE_SIZE];
uint8_t txHead = 0;
uint8_t txTail = 0;
uint8_t txCount = 0;
PendingTx pendingTx;
PendingRelay relayQueue[RELAY_QUEUE_SIZE];

SeenPacket seenPackets[SEEN_CACHE_SIZE];
uint8_t seenWriteIndex = 0;
uint16_t nextMessageId = 1;
unsigned long nextHelloAt = 0;

MeshNodeInfo meshNodes[MESH_NODE_COUNT] = {
  {0xAA, "Node A", "controller", NODE_ADDRESS == 0xAA},
  {0xBB, "Node B", "relay", NODE_ADDRESS == 0xBB},
  {0xCC, "Node C", "receiver", NODE_ADDRESS == 0xCC},
};

int lastPacketRssi = 0;
float lastPacketSnr = 0;
unsigned long lastPacketAt = 0;

#if RADIO_BACKEND == RADIO_BACKEND_AS32_UART
enum As32RxState {
  AS32_WAIT_MAGIC_1,
  AS32_WAIT_MAGIC_2,
  AS32_READ_LEN_HI,
  AS32_READ_LEN_LO,
  AS32_READ_PAYLOAD,
  AS32_READ_CRC_HI,
  AS32_READ_CRC_LO
};

As32RxState as32RxState = AS32_WAIT_MAGIC_1;
uint8_t as32RxBuffer[MAX_RADIO_PACKET_LEN] = {0};
uint16_t as32RxLen = 0;
uint16_t as32RxIndex = 0;
uint16_t as32RxCrc = 0;
unsigned long as32LastByteAt = 0;
#endif

uint8_t cryptoAesKey[CRYPTO_AES_KEY_SIZE] = {0};
uint8_t cryptoHmacKey[CRYPTO_HMAC_KEY_SIZE] = {0};

const char FALLBACK_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>LoRa Chat</title>
  <style>
    body { margin: 0; min-height: 100vh; background: #0f1216; color: #edf2f7; font-family: system-ui, sans-serif; display: grid; place-items: center; }
    main { width: min(520px, calc(100vw - 32px)); padding: 20px; border: 1px solid #303943; border-radius: 8px; background: #171c22; }
    h1 { margin: 0 0 10px; font-size: 22px; }
    p { margin: 8px 0; color: #94a3b3; line-height: 1.45; }
    code { color: #68d391; }
  </style>
</head>
<body>
  <main>
    <h1>LoRa Chat</h1>
    <p>Backend is running, but the SPIFFS web bundle is missing.</p>
    <p>Upload <code>ESP32LoRaChat/data</code> to the ESP32 filesystem.</p>
  </main>
</body>
</html>
)rawliteral";

String hexByte(uint8_t value);
String destinationLabel();
String radioBackendLabel();
String radioConfigLabel();
bool radioSignalAvailable();
String jsonEscape(const String &value);
String cleanField(String value, size_t maxLen);
String contentTypeForPath(const String &path);
void sendNoStoreJson(int code, const String &payload);
bool serveStaticFile(String path);
void handleRoot();
void handleSend();
void handleMessages();
void handleStatus();
void handleNodes();
void handleNotFound();
bool enqueueOutgoing(const String &sender, const String &text, uint16_t *idOut);
void processTxQueue();
void startNextPending();
void sendPendingPacket();
void processHelloBeacon();
void sendHelloPacket();
void sendDataPacket(uint8_t to, uint16_t id, const String &sender, const String &text);
void sendDataPayload(uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const String &payload);
void sendFramePacket(uint8_t type, uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const uint8_t *frame, size_t frameLen);
bool initRadio();
bool sendRadioPacket(const uint8_t *packet, size_t packetLen);
bool readRadioPacket(uint8_t *packet, size_t *packetLen);
void processRadioPacket(const uint8_t *packet, size_t packetLen);
uint16_t crc16Ccitt(const uint8_t *data, size_t len);
uint16_t crc16CcittUpdate(uint16_t crc, uint8_t data);
#if RADIO_BACKEND == RADIO_BACKEND_AS32_UART
bool waitAs32Idle(unsigned long timeoutMs);
void resetAs32Parser();
#endif
void sendAckPacket(uint8_t to, uint16_t id);
void receiveRadioPacket();
MeshNodeInfo *findMeshNode(uint8_t address);
String nodeName(uint8_t address);
void recordNodeHeard(uint8_t address, uint8_t via, uint8_t hopLimit, int rssi, float snr);
void recordRadioTx(uint8_t type, uint8_t from, uint8_t relay);
void handleIncomingData(uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const uint8_t *frame, size_t frameLen);
void handleIncomingHello(uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const uint8_t *frame, size_t frameLen);
void handleIncomingAck(uint8_t to, uint8_t from, uint16_t id, const uint8_t *frame, size_t frameLen);
void processRelayQueue();
bool scheduleRelay(uint8_t type, uint8_t to, uint8_t from, uint16_t id, uint8_t hopLimit, const uint8_t *frame, size_t frameLen);
void cancelRelay(uint8_t from, uint16_t id);
uint8_t countRelayQueue();
bool wasSeen(uint8_t from, uint16_t id);
void rememberSeen(uint8_t from, uint16_t id, uint8_t relay, uint8_t hopLimit);
void addHistory(uint16_t id, uint8_t from, bool outgoing, const String &sender, const String &text, const String &status, int rssi, float snr, uint8_t hops);
void markOutgoingStatus(uint16_t id, const String &status);
uint16_t allocateMessageId();
void initCryptoKeys();
void deriveCryptoKey(const char *label, uint8_t *out, size_t outLen);
void fillRandomBytes(uint8_t *out, size_t len);
void buildCryptoIv(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t nonce[CRYPTO_NONCE_SIZE], uint8_t iv[16]);
bool cryptPayload(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t nonce[CRYPTO_NONCE_SIZE], const uint8_t *input, size_t len, uint8_t *output);
bool computeFrameTag(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t nonce[CRYPTO_NONCE_SIZE], const uint8_t *ciphertext, size_t cipherLen, uint8_t tag[32]);
bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t len);
bool buildEncryptedFrame(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t *plaintext, size_t plaintextLen, uint8_t *frame, size_t *frameLen);
bool decryptFrame(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t *frame, size_t frameLen, String *plaintextOut);
bool verifyFrameOnly(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t *frame, size_t frameLen);

void setup() {
  Serial.begin(115200);
  delay(200);

  randomSeed(esp_random());
  nextMessageId = (uint16_t)esp_random();
  if (nextMessageId == 0) nextMessageId = 1;
  initCryptoKeys();

  Serial.println();
  Serial.print("Starting radio backend: ");
  Serial.println(radioBackendLabel());
  if (!initRadio()) {
    Serial.println("Radio init failed. Check wiring, module frequency, serial settings, and power.");
    while (true) delay(1000);
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  staticFsReady = SPIFFS.begin(true);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/send", HTTP_POST, handleSend);
  server.on("/api/messages", HTTP_GET, handleMessages);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/nodes", HTTP_GET, handleNodes);
  server.onNotFound(handleNotFound);
  server.begin();

  MeshNodeInfo *localNode = findMeshNode(NODE_ADDRESS);
  if (localNode != nullptr) {
    localNode->heard = true;
    localNode->lastHeardAt = millis();
  }

  Serial.print("AP: ");
  Serial.println(AP_SSID);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());
  if (staticFsReady) {
    Serial.print("SPIFFS: ");
    Serial.print(SPIFFS.usedBytes());
    Serial.print("/");
    Serial.print(SPIFFS.totalBytes());
    Serial.println(" bytes used");
  } else {
    Serial.println("SPIFFS mount failed. Web UI fallback will be served.");
  }
  Serial.print("Node: ");
  Serial.print(hexByte(NODE_ADDRESS));
  Serial.print(" -> Destination: ");
  Serial.println(destinationLabel());
  Serial.print("Radio: ");
  Serial.println(radioConfigLabel());
  Serial.println("Crypto: AES-128-CTR + HMAC-SHA256/64");
  if (strcmp(CHAT_CRYPTO_PSK, "change-this-lora-chat-key") == 0) {
    Serial.println("WARNING: default chat PSK is still configured.");
  }
  nextHelloAt = millis() + (unsigned long)random(HELLO_FIRST_MIN_MS, HELLO_FIRST_MAX_MS + 1);
}

void loop() {
  server.handleClient();
  receiveRadioPacket();
  processHelloBeacon();
  processRelayQueue();
  processTxQueue();
}

String hexByte(uint8_t value) {
  char buffer[5];
  snprintf(buffer, sizeof(buffer), "0x%02X", value);
  return String(buffer);
}

String destinationLabel() {
  if (CHAT_DESTINATION == BROADCAST_ADDRESS) return "broadcast";
  return hexByte(CHAT_DESTINATION);
}

String radioBackendLabel() {
#if RADIO_BACKEND == RADIO_BACKEND_AS32_UART
  return "AS32 UART";
#else
  return "SPI LoRa";
#endif
}

String radioConfigLabel() {
#if RADIO_BACKEND == RADIO_BACKEND_AS32_UART
  return String("AS32 UART ") + String(AS32_BAUD) + " baud | 433 MHz module | hop " + String(LORA_HOP_LIMIT);
#else
  return String("SF") + String(LORA_SPREADING_FACTOR) + " | " + String(LORA_SIGNAL_BANDWIDTH / 1000) + " kHz | "
         + String(LORA_TX_POWER_DBM) + " dBm | hop " + String(LORA_HOP_LIMIT);
#endif
}

bool radioSignalAvailable() {
#if RADIO_BACKEND == RADIO_BACKEND_AS32_UART
  return false;
#else
  return true;
#endif
}

String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if ((uint8_t)c < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

String cleanField(String value, size_t maxLen) {
  value.trim();
  value.replace("\r", " ");
  value.replace("\n", " ");
  while (value.indexOf("  ") >= 0) value.replace("  ", " ");
  if (value.length() > maxLen) value = value.substring(0, maxLen);
  return value;
}

void sendNoStoreJson(int code, const String &payload) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", payload);
}

String contentTypeForPath(const String &path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "text/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".ico")) return "image/x-icon";
  if (path.endsWith(".gz")) return "application/gzip";
  return "application/octet-stream";
}

bool serveStaticFile(String path) {
  if (!staticFsReady) return false;
  if (path.indexOf("..") >= 0) return false;

  String contentType = contentTypeForPath(path);
  String gzPath = path + ".gz";
  bool gzip = false;

  if (SPIFFS.exists(gzPath)) {
    path = gzPath;
    gzip = true;
  } else if (!SPIFFS.exists(path)) {
    return false;
  }

  File file = SPIFFS.open(path, FILE_READ);
  if (!file) return false;

  if (gzip) server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-store");

  server.streamFile(file, contentType);
  file.close();
  return true;
}

void handleRoot() {
  if (serveStaticFile("/static/index.html")) return;
  server.send_P(200, "text/html", FALLBACK_HTML);
}

void handleSend() {
  String sender = cleanField(server.arg("sender"), MAX_SENDER_LEN);
  String text = cleanField(server.arg("message"), MAX_MESSAGE_LEN);
  if (sender.length() == 0) sender = "User";

  if (text.length() == 0) {
    sendNoStoreJson(400, "{\"error\":\"empty message\"}");
    return;
  }

  uint16_t id = 0;
  if (!enqueueOutgoing(sender, text, &id)) {
    sendNoStoreJson(429, "{\"error\":\"radio queue full\"}");
    return;
  }

  sendNoStoreJson(202, String("{\"status\":\"queued\",\"id\":") + String(id) + "}");
}

void handleMessages() {
  String response = "{\"messages\":[";
  response.reserve(4096);

  for (uint8_t i = 0; i < historyCount; i++) {
    if (i > 0) response += ',';
    const ChatEntry &item = history[i];
    response += "{\"seq\":";
    response += String(item.seq);
    response += ",\"id\":";
    response += String(item.id);
    response += ",\"from\":\"";
    response += hexByte(item.from);
    response += "\",\"outgoing\":";
    response += item.outgoing ? "true" : "false";
    response += ",\"sender\":\"";
    response += jsonEscape(item.sender);
    response += "\",\"text\":\"";
    response += jsonEscape(item.text);
    response += "\",\"status\":\"";
    response += jsonEscape(item.status);
    response += "\",\"rssi\":";
    response += String(item.rssi);
    response += ",\"snr\":";
    response += String(item.snr, 1);
    response += ",\"signalAvailable\":";
    response += radioSignalAvailable() ? "true" : "false";
    response += ",\"hops\":";
    response += String(item.hops);
    response += ",\"ageMs\":";
    response += String(millis() - item.createdAt);
    response += '}';
  }

  response += "]}";
  sendNoStoreJson(200, response);
}

void handleStatus() {
  String radio;
  if (lastPacketAt == 0) {
    radio = "no packets";
  } else {
    radio = String("last ") + String((millis() - lastPacketAt) / 1000) + "s ago";
  }
  String response = "{\"node\":\"";
  response += hexByte(NODE_ADDRESS);
  response += "\",\"destination\":\"";
  response += destinationLabel();
  response += "\",\"mode\":\"";
  response += CHAT_DESTINATION == BROADCAST_ADDRESS ? "mesh-broadcast" : "direct";
  response += "\",\"backend\":\"";
  response += radioBackendLabel();
  response += "\",\"rf\":\"";
  response += jsonEscape(radioConfigLabel());
  response += "\",\"clients\":";
  response += String(WiFi.softAPgetStationNum());
  response += ",\"hopLimit\":";
  response += String(LORA_HOP_LIMIT);
  response += ",\"txPower\":";
  response += String(LORA_TX_POWER_DBM);
  response += ",\"spreadingFactor\":";
  response += String(LORA_SPREADING_FACTOR);
  response += ",\"bandwidth\":";
  response += String(LORA_SIGNAL_BANDWIDTH);
  response += ",\"crypto\":\"AES-CTR+HMAC\"";
  response += ",";
  response += "\"queue\":";
  response += String(txCount + (pendingTx.active ? 1 : 0) + countRelayQueue());
  response += ",\"txQueue\":";
  response += String(txCount + (pendingTx.active ? 1 : 0));
  response += ",\"relayQueue\":";
  response += String(countRelayQueue());
  response += ",\"pending\":";
  response += pendingTx.active ? "true" : "false";
  response += ",\"radio\":\"";
  response += jsonEscape(radio);
  response += "\",\"signalAvailable\":";
  response += radioSignalAvailable() ? "true" : "false";
  response += ",\"auxReady\":";
#if RADIO_BACKEND == RADIO_BACKEND_AS32_UART
  response += digitalRead(AS32_AUX_PIN) == HIGH ? "true" : "false";
#else
  response += "true";
#endif
  response += ",\"rssi\":";
  response += String(lastPacketRssi);
  response += ",\"snr\":";
  response += String(lastPacketSnr, 1);
  response += "}";
  sendNoStoreJson(200, response);
}

void handleNodes() {
  String response = "{\"nodes\":[";
  response.reserve(1400);

  for (uint8_t i = 0; i < MESH_NODE_COUNT; i++) {
    if (i > 0) response += ',';
    const MeshNodeInfo &node = meshNodes[i];

    response += "{\"address\":\"";
    response += hexByte(node.address);
    response += "\",\"name\":\"";
    response += jsonEscape(node.name);
    response += "\",\"role\":\"";
    response += jsonEscape(node.role);
    response += "\",\"local\":";
    response += node.local ? "true" : "false";
    response += ",\"heard\":";
    response += node.heard ? "true" : "false";
    response += ",\"rx\":";
    response += String(node.rxCount);
    response += ",\"tx\":";
    response += String(node.txCount);
    response += ",\"relay\":";
    response += String(node.relayCount);
    response += ",\"lastVia\":\"";
    response += node.lastVia == 0 ? "--" : hexByte(node.lastVia);
    response += "\",\"lastHops\":";
    response += String(node.lastHops);
    response += ",\"signalAvailable\":";
    response += radioSignalAvailable() ? "true" : "false";
    response += ",\"rssi\":";
    response += String(node.lastRssi);
    response += ",\"snr\":";
    response += String(node.lastSnr, 1);
    response += ",\"ageMs\":";
    if (node.local) {
      response += String(millis());
    } else if (node.heard) {
      response += String(millis() - node.lastHeardAt);
    } else {
      response += "null";
    }
    response += '}';
  }

  response += "]}";
  sendNoStoreJson(200, response);
}

void handleNotFound() {
  if (server.method() == HTTP_GET && !server.uri().startsWith("/api/")) {
    String path = server.uri();
    int queryIndex = path.indexOf('?');
    if (queryIndex >= 0) path = path.substring(0, queryIndex);
    if (path == "/") path = "/index.html";

    String staticPath = path.startsWith("/static/") ? path : "/static" + path;
    if (serveStaticFile(staticPath)) return;
    if (serveStaticFile("/static/index.html")) return;

    server.send_P(404, "text/html", FALLBACK_HTML);
    return;
  }

  sendNoStoreJson(404, "{\"error\":\"not found\"}");
}

bool enqueueOutgoing(const String &sender, const String &text, uint16_t *idOut) {
  if (txCount >= TX_QUEUE_SIZE) return false;

  uint16_t id = allocateMessageId();
  txQueue[txTail].id = id;
  txQueue[txTail].to = CHAT_DESTINATION;
  txQueue[txTail].sender = sender;
  txQueue[txTail].text = text;
  txTail = (txTail + 1) % TX_QUEUE_SIZE;
  txCount++;

  rememberSeen(NODE_ADDRESS, id, NODE_ADDRESS, LORA_HOP_LIMIT);
  addHistory(id, NODE_ADDRESS, true, sender, text, "queued", 0, 0, 0);
  if (idOut != nullptr) *idOut = id;
  return true;
}

void processTxQueue() {
  if (!pendingTx.active) {
    startNextPending();
    return;
  }

  if ((long)(millis() - pendingTx.nextRetryAt) < 0) return;

  if (pendingTx.attempts >= MAX_RETRIES) {
    markOutgoingStatus(pendingTx.id, "failed");
    pendingTx.active = false;
    startNextPending();
    return;
  }

  markOutgoingStatus(pendingTx.id, "retrying");
  sendPendingPacket();
}

void startNextPending() {
  if (pendingTx.active || txCount == 0) return;

  pendingTx.active = true;
  pendingTx.id = txQueue[txHead].id;
  pendingTx.to = txQueue[txHead].to;
  pendingTx.sender = txQueue[txHead].sender;
  pendingTx.text = txQueue[txHead].text;
  pendingTx.attempts = 0;
  pendingTx.nextRetryAt = 0;

  txHead = (txHead + 1) % TX_QUEUE_SIZE;
  txCount--;

  markOutgoingStatus(pendingTx.id, "sending");
  sendPendingPacket();
}

void sendPendingPacket() {
  pendingTx.attempts++;
  sendDataPacket(pendingTx.to, pendingTx.id, pendingTx.sender, pendingTx.text);

  if (pendingTx.to == BROADCAST_ADDRESS) {
    markOutgoingStatus(pendingTx.id, "sent");
    pendingTx.active = false;
    return;
  }

  pendingTx.nextRetryAt = millis() + ACK_TIMEOUT_MS;
}

void processHelloBeacon() {
  if ((long)(millis() - nextHelloAt) < 0) return;
  sendHelloPacket();
  nextHelloAt = millis() + HELLO_INTERVAL_MS + (unsigned long)random(0, 2000);
}

void sendHelloPacket() {
  uint16_t id = allocateMessageId();
  MeshNodeInfo *localNode = findMeshNode(NODE_ADDRESS);
  String payload = localNode == nullptr ? hexByte(NODE_ADDRESS) : String(localNode->name);
  payload += "\n";
  payload += localNode == nullptr ? "node" : localNode->role;

  uint8_t frame[MAX_FRAME_LEN];
  size_t frameLen = 0;
  if (!buildEncryptedFrame(PACKET_TYPE_HELLO, BROADCAST_ADDRESS, NODE_ADDRESS, id, (const uint8_t *)payload.c_str(), payload.length(), frame, &frameLen)) {
    Serial.print("Encrypt HELLO failed ");
    Serial.println(id);
    return;
  }

  rememberSeen(NODE_ADDRESS, id, NODE_ADDRESS, LORA_HOP_LIMIT);
  sendFramePacket(PACKET_TYPE_HELLO, BROADCAST_ADDRESS, NODE_ADDRESS, NODE_ADDRESS, LORA_HOP_LIMIT, id, frame, frameLen);
}

void sendDataPacket(uint8_t to, uint16_t id, const String &sender, const String &text) {
  String payload = sender + "\n" + text;
  sendDataPayload(to, NODE_ADDRESS, NODE_ADDRESS, LORA_HOP_LIMIT, id, payload);

  Serial.print("DATA ");
  Serial.print(id);
  Serial.print(" to ");
  Serial.print(to == BROADCAST_ADDRESS ? "broadcast" : hexByte(to));
  Serial.print(" attempt ");
  Serial.println(pendingTx.attempts);
}

void sendDataPayload(uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const String &payload) {
  uint8_t frame[MAX_FRAME_LEN];
  size_t frameLen = 0;
  if (!buildEncryptedFrame(PACKET_TYPE_DATA, to, from, id, (const uint8_t *)payload.c_str(), payload.length(), frame, &frameLen)) {
    Serial.print("Encrypt DATA failed ");
    Serial.println(id);
    return;
  }

  sendFramePacket(PACKET_TYPE_DATA, to, from, relay, hopLimit, id, frame, frameLen);
}

void sendFramePacket(uint8_t type, uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const uint8_t *frame, size_t frameLen) {
  if (frameLen > MAX_FRAME_LEN) return;

  uint8_t packet[MAX_RADIO_PACKET_LEN];
  packet[0] = PROTOCOL_VERSION;
  packet[1] = type;
  packet[2] = to;
  packet[3] = from;
  packet[4] = relay;
  packet[5] = hopLimit;
  packet[6] = (uint8_t)(id >> 8);
  packet[7] = (uint8_t)(id & 0xFF);
  memcpy(packet + PACKET_HEADER_SIZE, frame, frameLen);

  if (!sendRadioPacket(packet, PACKET_HEADER_SIZE + frameLen)) {
    Serial.print("Radio TX failed ");
    Serial.println(id);
  } else {
    recordRadioTx(type, from, relay);
  }
}

bool initRadio() {
#if RADIO_BACKEND == RADIO_BACKEND_AS32_UART
  pinMode(AS32_MD0_PIN, OUTPUT);
  pinMode(AS32_MD1_PIN, OUTPUT);
  digitalWrite(AS32_MD0_PIN, LOW);
  digitalWrite(AS32_MD1_PIN, LOW);
  pinMode(AS32_AUX_PIN, INPUT_PULLUP);
  Serial2.begin(AS32_BAUD, SERIAL_8N1, AS32_RX_PIN, AS32_TX_PIN);
  delay(50);
  resetAs32Parser();
  return waitAs32Idle(AS32_IDLE_TIMEOUT_MS);
#else
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RESET, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY_HZ)) return false;

  LoRa.setSpreadingFactor(LORA_SPREADING_FACTOR);
  LoRa.setSignalBandwidth(LORA_SIGNAL_BANDWIDTH);
  LoRa.setCodingRate4(LORA_CODING_RATE_DENOMINATOR);
  LoRa.setPreambleLength(LORA_PREAMBLE_LENGTH);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setTxPower(LORA_TX_POWER_DBM, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.enableCrc();
  return true;
#endif
}

bool sendRadioPacket(const uint8_t *packet, size_t packetLen) {
  if (packetLen < PACKET_HEADER_SIZE || packetLen > MAX_RADIO_PACKET_LEN) return false;

#if RADIO_BACKEND == RADIO_BACKEND_AS32_UART
  if (!waitAs32Idle(AS32_IDLE_TIMEOUT_MS)) return false;

  uint16_t crc = 0xFFFF;
  uint8_t lenHi = (uint8_t)(packetLen >> 8);
  uint8_t lenLo = (uint8_t)(packetLen & 0xFF);
  crc = crc16CcittUpdate(crc, lenHi);
  crc = crc16CcittUpdate(crc, lenLo);
  for (size_t i = 0; i < packetLen; i++) crc = crc16CcittUpdate(crc, packet[i]);

  Serial2.write(TRANSPORT_MAGIC_1);
  Serial2.write(TRANSPORT_MAGIC_2);
  Serial2.write(lenHi);
  Serial2.write(lenLo);
  Serial2.write(packet, packetLen);
  Serial2.write((uint8_t)(crc >> 8));
  Serial2.write((uint8_t)(crc & 0xFF));
  Serial2.flush();
  return waitAs32Idle(AS32_IDLE_TIMEOUT_MS);
#else
  LoRa.beginPacket();
  LoRa.write(packet, packetLen);
  LoRa.endPacket();
  return true;
#endif
}

bool readRadioPacket(uint8_t *packet, size_t *packetLen) {
#if RADIO_BACKEND == RADIO_BACKEND_AS32_UART
  if (as32RxState != AS32_WAIT_MAGIC_1 && (long)(millis() - as32LastByteAt) > (long)AS32_FRAME_TIMEOUT_MS) {
    resetAs32Parser();
  }

  while (Serial2.available()) {
    uint8_t b = (uint8_t)Serial2.read();
    as32LastByteAt = millis();

    switch (as32RxState) {
      case AS32_WAIT_MAGIC_1:
        if (b == TRANSPORT_MAGIC_1) as32RxState = AS32_WAIT_MAGIC_2;
        break;
      case AS32_WAIT_MAGIC_2:
        if (b == TRANSPORT_MAGIC_2) {
          as32RxState = AS32_READ_LEN_HI;
        } else if (b != TRANSPORT_MAGIC_1) {
          resetAs32Parser();
        }
        break;
      case AS32_READ_LEN_HI:
        as32RxLen = ((uint16_t)b) << 8;
        as32RxState = AS32_READ_LEN_LO;
        break;
      case AS32_READ_LEN_LO:
        as32RxLen |= b;
        if (as32RxLen < PACKET_HEADER_SIZE || as32RxLen > MAX_RADIO_PACKET_LEN) {
          resetAs32Parser();
        } else {
          as32RxIndex = 0;
          as32RxState = AS32_READ_PAYLOAD;
        }
        break;
      case AS32_READ_PAYLOAD:
        as32RxBuffer[as32RxIndex++] = b;
        if (as32RxIndex >= as32RxLen) as32RxState = AS32_READ_CRC_HI;
        break;
      case AS32_READ_CRC_HI:
        as32RxCrc = ((uint16_t)b) << 8;
        as32RxState = AS32_READ_CRC_LO;
        break;
      case AS32_READ_CRC_LO: {
        as32RxCrc |= b;
        uint16_t crc = 0xFFFF;
        crc = crc16CcittUpdate(crc, (uint8_t)(as32RxLen >> 8));
        crc = crc16CcittUpdate(crc, (uint8_t)(as32RxLen & 0xFF));
        for (uint16_t i = 0; i < as32RxLen; i++) crc = crc16CcittUpdate(crc, as32RxBuffer[i]);

        if (crc == as32RxCrc) {
          memcpy(packet, as32RxBuffer, as32RxLen);
          *packetLen = as32RxLen;
          lastPacketRssi = 0;
          lastPacketSnr = 0;
          lastPacketAt = millis();
          resetAs32Parser();
          return true;
        }

        Serial.println("AS32 frame CRC failed");
        resetAs32Parser();
        break;
      }
    }
  }

  return false;
#else
  int size = LoRa.parsePacket();
  if (!size) return false;

  lastPacketRssi = LoRa.packetRssi();
  lastPacketSnr = LoRa.packetSnr();
  lastPacketAt = millis();

  if (size < (int)PACKET_HEADER_SIZE || size > (int)MAX_RADIO_PACKET_LEN) {
    while (LoRa.available()) LoRa.read();
    return false;
  }

  size_t count = 0;
  while (LoRa.available() && count < MAX_RADIO_PACKET_LEN) {
    packet[count++] = (uint8_t)LoRa.read();
  }
  while (LoRa.available()) LoRa.read();

  if (count != (size_t)size) return false;
  *packetLen = count;
  return true;
#endif
}

void processRadioPacket(const uint8_t *packet, size_t packetLen) {
  if (packetLen < PACKET_HEADER_SIZE) return;

  uint8_t version = packet[0];
  uint8_t type = packet[1];
  uint8_t to = packet[2];
  uint8_t from = packet[3];
  uint8_t relay = packet[4];
  uint8_t hopLimit = packet[5];
  uint16_t id = ((uint16_t)packet[6] << 8) | packet[7];

  size_t frameLen = packetLen - PACKET_HEADER_SIZE;
  if (frameLen < (CRYPTO_NONCE_SIZE + CRYPTO_TAG_SIZE) || frameLen > MAX_FRAME_LEN) return;
  if (version != PROTOCOL_VERSION) return;
  if (hopLimit > LORA_HOP_LIMIT) return;

  const uint8_t *frame = packet + PACKET_HEADER_SIZE;
  if (type == PACKET_TYPE_DATA) {
    handleIncomingData(to, from, relay, hopLimit, id, frame, frameLen);
  } else if (type == PACKET_TYPE_HELLO) {
    handleIncomingHello(to, from, relay, hopLimit, id, frame, frameLen);
  } else if (type == PACKET_TYPE_ACK && to == NODE_ADDRESS) {
    handleIncomingAck(to, from, id, frame, frameLen);
  }
}

uint16_t crc16Ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) crc = crc16CcittUpdate(crc, data[i]);
  return crc;
}

uint16_t crc16CcittUpdate(uint16_t crc, uint8_t data) {
  crc ^= ((uint16_t)data) << 8;
  for (uint8_t i = 0; i < 8; i++) {
    if (crc & 0x8000) {
      crc = (crc << 1) ^ 0x1021;
    } else {
      crc <<= 1;
    }
  }
  return crc;
}

#if RADIO_BACKEND == RADIO_BACKEND_AS32_UART
bool waitAs32Idle(unsigned long timeoutMs) {
  unsigned long startedAt = millis();
  while (digitalRead(AS32_AUX_PIN) == LOW) {
    if ((long)(millis() - startedAt) >= (long)timeoutMs) return false;
    delay(1);
  }
  return true;
}

void resetAs32Parser() {
  as32RxState = AS32_WAIT_MAGIC_1;
  as32RxLen = 0;
  as32RxIndex = 0;
  as32RxCrc = 0;
}
#endif

void sendAckPacket(uint8_t to, uint16_t id) {
  uint8_t frame[MAX_FRAME_LEN];
  size_t frameLen = 0;
  if (!buildEncryptedFrame(PACKET_TYPE_ACK, to, NODE_ADDRESS, id, nullptr, 0, frame, &frameLen)) {
    Serial.print("Encrypt ACK failed ");
    Serial.println(id);
    return;
  }

  sendFramePacket(PACKET_TYPE_ACK, to, NODE_ADDRESS, NODE_ADDRESS, 0, id, frame, frameLen);

  Serial.print("ACK ");
  Serial.println(id);
}

void receiveRadioPacket() {
  uint8_t packet[MAX_RADIO_PACKET_LEN];
  size_t packetLen = 0;
  while (readRadioPacket(packet, &packetLen)) {
    processRadioPacket(packet, packetLen);
  }
}

MeshNodeInfo *findMeshNode(uint8_t address) {
  for (uint8_t i = 0; i < MESH_NODE_COUNT; i++) {
    if (meshNodes[i].address == address) return &meshNodes[i];
  }
  return nullptr;
}

String nodeName(uint8_t address) {
  MeshNodeInfo *node = findMeshNode(address);
  if (node == nullptr) return hexByte(address);
  return String(node->name);
}

void recordNodeHeard(uint8_t address, uint8_t via, uint8_t hopLimit, int rssi, float snr) {
  MeshNodeInfo *node = findMeshNode(address);
  if (node == nullptr) return;

  node->heard = true;
  node->rxCount++;
  node->lastVia = via;
  node->lastHops = hopLimit >= LORA_HOP_LIMIT ? 0 : LORA_HOP_LIMIT - hopLimit;
  node->lastRssi = rssi;
  node->lastSnr = snr;
  node->lastHeardAt = millis();
}

void recordRadioTx(uint8_t type, uint8_t from, uint8_t relay) {
  MeshNodeInfo *localNode = findMeshNode(NODE_ADDRESS);
  if (localNode == nullptr) return;

  localNode->heard = true;
  localNode->lastHeardAt = millis();
  if (type == PACKET_TYPE_DATA && from != NODE_ADDRESS && relay == NODE_ADDRESS) {
    localNode->relayCount++;
  } else {
    localNode->txCount++;
  }
}

void handleIncomingData(uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const uint8_t *frame, size_t frameLen) {
  bool addressedToUs = to == NODE_ADDRESS || to == BROADCAST_ADDRESS;

  if (from == NODE_ADDRESS) {
    if (to == BROADCAST_ADDRESS && relay != NODE_ADDRESS && verifyFrameOnly(PACKET_TYPE_DATA, to, from, id, frame, frameLen)) {
      markOutgoingStatus(id, "relayed");
    }
    return;
  }

  if (wasSeen(from, id)) {
    if (relay != NODE_ADDRESS && verifyFrameOnly(PACKET_TYPE_DATA, to, from, id, frame, frameLen)) {
      recordNodeHeard(from, relay, hopLimit, lastPacketRssi, lastPacketSnr);
      cancelRelay(from, id);
    }
    Serial.print("Duplicate DATA ");
    Serial.println(id);
    return;
  }

  String payload;
  if (!decryptFrame(PACKET_TYPE_DATA, to, from, id, frame, frameLen, &payload)) {
    Serial.print("DATA auth failed ");
    Serial.println(id);
    return;
  }

  rememberSeen(from, id, relay, hopLimit);
  recordNodeHeard(from, relay, hopLimit, lastPacketRssi, lastPacketSnr);

  if (addressedToUs) {
    if (to == NODE_ADDRESS) sendAckPacket(from, id);

    int splitAt = payload.indexOf('\n');
    String sender = splitAt >= 0 ? payload.substring(0, splitAt) : hexByte(from);
    String text = splitAt >= 0 ? payload.substring(splitAt + 1) : payload;
    sender = cleanField(sender, MAX_SENDER_LEN);
    text = cleanField(text, MAX_MESSAGE_LEN);

    uint8_t hops = hopLimit >= LORA_HOP_LIMIT ? 0 : LORA_HOP_LIMIT - hopLimit;
    addHistory(id, from, false, sender, text, "received", lastPacketRssi, lastPacketSnr, hops);
  }

  if (hopLimit > 0 && to != NODE_ADDRESS) {
    scheduleRelay(PACKET_TYPE_DATA, to, from, id, hopLimit - 1, frame, frameLen);
  }

  Serial.print("RX DATA ");
  Serial.print(id);
  Serial.print(" from ");
  Serial.print(hexByte(from));
  Serial.print(" via ");
  Serial.print(hexByte(relay));
  Serial.print(" hop ");
  Serial.println(hopLimit);
}

void handleIncomingHello(uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const uint8_t *frame, size_t frameLen) {
  if (from == NODE_ADDRESS) return;

  if (wasSeen(from, id)) {
    if (relay != NODE_ADDRESS && verifyFrameOnly(PACKET_TYPE_HELLO, to, from, id, frame, frameLen)) {
      recordNodeHeard(from, relay, hopLimit, lastPacketRssi, lastPacketSnr);
      cancelRelay(from, id);
    }
    return;
  }

  String payload;
  if (!decryptFrame(PACKET_TYPE_HELLO, to, from, id, frame, frameLen, &payload)) {
    Serial.print("HELLO auth failed ");
    Serial.println(id);
    return;
  }

  rememberSeen(from, id, relay, hopLimit);
  recordNodeHeard(from, relay, hopLimit, lastPacketRssi, lastPacketSnr);

  if (hopLimit > 0) {
    scheduleRelay(PACKET_TYPE_HELLO, to, from, id, hopLimit - 1, frame, frameLen);
  }

  Serial.print("RX HELLO ");
  Serial.print(id);
  Serial.print(" from ");
  Serial.print(hexByte(from));
  Serial.print(" via ");
  Serial.println(hexByte(relay));
}

void handleIncomingAck(uint8_t to, uint8_t from, uint16_t id, const uint8_t *frame, size_t frameLen) {
  if (!pendingTx.active || from != pendingTx.to || id != pendingTx.id) return;
  if (!verifyFrameOnly(PACKET_TYPE_ACK, to, from, id, frame, frameLen)) {
    Serial.print("ACK auth failed ");
    Serial.println(id);
    return;
  }

  recordNodeHeard(from, from, LORA_HOP_LIMIT, lastPacketRssi, lastPacketSnr);
  markOutgoingStatus(id, "delivered");
  pendingTx.active = false;

  Serial.print("RX ACK ");
  Serial.println(id);
}

void processRelayQueue() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < RELAY_QUEUE_SIZE; i++) {
    PendingRelay &item = relayQueue[i];
    if (!item.active) continue;
    if ((long)(now - item.sendAt) < 0) continue;

    sendFramePacket(item.type, item.to, item.from, NODE_ADDRESS, item.hopLimit, item.id, item.frame, item.frameLen);

    Serial.print(item.type == PACKET_TYPE_HELLO ? "RELAY HELLO " : "RELAY DATA ");
    Serial.print(item.id);
    Serial.print(" from ");
    Serial.print(hexByte(item.from));
    Serial.print(" hop ");
    Serial.println(item.hopLimit);

    item.active = false;
    item.frameLen = 0;
    return;
  }
}

bool scheduleRelay(uint8_t type, uint8_t to, uint8_t from, uint16_t id, uint8_t hopLimit, const uint8_t *frame, size_t frameLen) {
  if (frameLen > MAX_FRAME_LEN) return false;

  for (uint8_t i = 0; i < RELAY_QUEUE_SIZE; i++) {
    if (relayQueue[i].active && relayQueue[i].from == from && relayQueue[i].id == id) return true;
  }

  for (uint8_t i = 0; i < RELAY_QUEUE_SIZE; i++) {
    PendingRelay &item = relayQueue[i];
    if (item.active) continue;

    item.active = true;
    item.type = type;
    item.to = to;
    item.from = from;
    item.id = id;
    item.hopLimit = hopLimit;
    item.frameLen = (uint8_t)frameLen;
    memcpy(item.frame, frame, frameLen);
    item.sendAt = millis() + (unsigned long)random(RELAY_DELAY_MIN_MS, RELAY_DELAY_MAX_MS + 1);
    return true;
  }

  Serial.print("Relay queue full for DATA ");
  Serial.println(id);
  return false;
}

void cancelRelay(uint8_t from, uint16_t id) {
  for (uint8_t i = 0; i < RELAY_QUEUE_SIZE; i++) {
    PendingRelay &item = relayQueue[i];
    if (!item.active || item.from != from || item.id != id) continue;
    item.active = false;
    item.frameLen = 0;
  }
}

uint8_t countRelayQueue() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < RELAY_QUEUE_SIZE; i++) {
    if (relayQueue[i].active) count++;
  }
  return count;
}

bool wasSeen(uint8_t from, uint16_t id) {
  for (uint8_t i = 0; i < SEEN_CACHE_SIZE; i++) {
    if (seenPackets[i].from == from && seenPackets[i].id == id) return true;
  }
  return false;
}

void rememberSeen(uint8_t from, uint16_t id, uint8_t relay, uint8_t hopLimit) {
  seenPackets[seenWriteIndex].from = from;
  seenPackets[seenWriteIndex].id = id;
  seenPackets[seenWriteIndex].relay = relay;
  seenPackets[seenWriteIndex].hopLimit = hopLimit;
  seenPackets[seenWriteIndex].seenAt = millis();
  seenWriteIndex = (seenWriteIndex + 1) % SEEN_CACHE_SIZE;
}

void addHistory(uint16_t id, uint8_t from, bool outgoing, const String &sender, const String &text, const String &status, int rssi, float snr, uint8_t hops) {
  if (historyCount >= HISTORY_SIZE) {
    for (uint8_t i = 0; i < HISTORY_SIZE - 1; i++) history[i] = history[i + 1];
    historyCount = HISTORY_SIZE - 1;
  }

  ChatEntry &item = history[historyCount++];
  item.seq = nextSequence++;
  item.id = id;
  item.from = from;
  item.outgoing = outgoing;
  item.sender = sender;
  item.text = text;
  item.status = status;
  item.rssi = rssi;
  item.snr = snr;
  item.hops = hops;
  item.createdAt = millis();
}

void markOutgoingStatus(uint16_t id, const String &status) {
  for (int i = historyCount - 1; i >= 0; i--) {
    if (history[i].outgoing && history[i].id == id) {
      history[i].status = status;
      return;
    }
  }
}

void initCryptoKeys() {
  deriveCryptoKey("LoRaChat AES key v1", cryptoAesKey, sizeof(cryptoAesKey));
  deriveCryptoKey("LoRaChat HMAC key v1", cryptoHmacKey, sizeof(cryptoHmacKey));
}

void deriveCryptoKey(const char *label, uint8_t *out, size_t outLen) {
  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, (const unsigned char *)label, strlen(label));
  mbedtls_sha256_update_ret(&ctx, (const unsigned char *)CHAT_CRYPTO_PSK, strlen(CHAT_CRYPTO_PSK));
  mbedtls_sha256_finish_ret(&ctx, digest);
  mbedtls_sha256_free(&ctx);

  memcpy(out, digest, outLen);
  memset(digest, 0, sizeof(digest));
}

void fillRandomBytes(uint8_t *out, size_t len) {
  size_t pos = 0;
  while (pos < len) {
    uint32_t value = esp_random();
    for (uint8_t i = 0; i < 4 && pos < len; i++) {
      out[pos++] = (uint8_t)(value >> (i * 8));
    }
  }
}

void buildCryptoIv(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t nonce[CRYPTO_NONCE_SIZE], uint8_t iv[16]) {
  memset(iv, 0, 16);
  iv[0] = 'L';
  iv[1] = 'C';
  iv[2] = PROTOCOL_VERSION;
  iv[3] = type;
  iv[4] = to;
  iv[5] = from;
  iv[6] = (uint8_t)(id >> 8);
  iv[7] = (uint8_t)(id & 0xFF);
  memcpy(iv + 8, nonce, CRYPTO_NONCE_SIZE);
}

bool cryptPayload(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t nonce[CRYPTO_NONCE_SIZE], const uint8_t *input, size_t len, uint8_t *output) {
  if (len == 0) return true;

  uint8_t iv[16];
  uint8_t streamBlock[16];
  size_t ncOff = 0;
  buildCryptoIv(type, to, from, id, nonce, iv);
  memset(streamBlock, 0, sizeof(streamBlock));

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  int rc = mbedtls_aes_setkey_enc(&ctx, cryptoAesKey, CRYPTO_AES_KEY_SIZE * 8);
  if (rc == 0) {
    rc = mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, iv, streamBlock, input, output);
  }
  mbedtls_aes_free(&ctx);

  memset(iv, 0, sizeof(iv));
  memset(streamBlock, 0, sizeof(streamBlock));
  return rc == 0;
}

bool computeFrameTag(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t nonce[CRYPTO_NONCE_SIZE], const uint8_t *ciphertext, size_t cipherLen, uint8_t tag[32]) {
  if (cipherLen > MAX_PLAINTEXT_LEN) return false;

  uint8_t input[10 + MAX_PLAINTEXT_LEN];
  size_t pos = 0;
  input[pos++] = PROTOCOL_VERSION;
  input[pos++] = type;
  input[pos++] = to;
  input[pos++] = from;
  input[pos++] = (uint8_t)(id >> 8);
  input[pos++] = (uint8_t)(id & 0xFF);
  memcpy(input + pos, nonce, CRYPTO_NONCE_SIZE);
  pos += CRYPTO_NONCE_SIZE;
  if (cipherLen > 0) {
    memcpy(input + pos, ciphertext, cipherLen);
    pos += cipherLen;
  }

  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) return false;

  int rc = mbedtls_md_hmac(info, cryptoHmacKey, sizeof(cryptoHmacKey), input, pos, tag);
  memset(input, 0, sizeof(input));
  return rc == 0;
}

bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

bool buildEncryptedFrame(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t *plaintext, size_t plaintextLen, uint8_t *frame, size_t *frameLen) {
  if (plaintextLen > MAX_PLAINTEXT_LEN || frame == nullptr || frameLen == nullptr) return false;
  if (plaintextLen > 0 && plaintext == nullptr) return false;

  uint8_t *nonce = frame;
  uint8_t *ciphertext = frame + CRYPTO_NONCE_SIZE;
  uint8_t tag[32];

  fillRandomBytes(nonce, CRYPTO_NONCE_SIZE);
  if (!cryptPayload(type, to, from, id, nonce, plaintext, plaintextLen, ciphertext)) return false;
  if (!computeFrameTag(type, to, from, id, nonce, ciphertext, plaintextLen, tag)) return false;

  memcpy(ciphertext + plaintextLen, tag, CRYPTO_TAG_SIZE);
  *frameLen = CRYPTO_NONCE_SIZE + plaintextLen + CRYPTO_TAG_SIZE;
  memset(tag, 0, sizeof(tag));
  return true;
}

bool decryptFrame(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t *frame, size_t frameLen, String *plaintextOut) {
  if (frame == nullptr || frameLen < CRYPTO_NONCE_SIZE + CRYPTO_TAG_SIZE || frameLen > MAX_FRAME_LEN) return false;

  const uint8_t *nonce = frame;
  const uint8_t *ciphertext = frame + CRYPTO_NONCE_SIZE;
  size_t cipherLen = frameLen - CRYPTO_NONCE_SIZE - CRYPTO_TAG_SIZE;
  const uint8_t *storedTag = ciphertext + cipherLen;
  uint8_t computedTag[32];

  if (!computeFrameTag(type, to, from, id, nonce, ciphertext, cipherLen, computedTag)) return false;
  bool tagOk = constantTimeEqual(computedTag, storedTag, CRYPTO_TAG_SIZE);
  memset(computedTag, 0, sizeof(computedTag));
  if (!tagOk) return false;

  if (plaintextOut == nullptr) return true;

  uint8_t plaintext[MAX_PLAINTEXT_LEN];
  if (!cryptPayload(type, to, from, id, nonce, ciphertext, cipherLen, plaintext)) {
    memset(plaintext, 0, sizeof(plaintext));
    return false;
  }

  plaintextOut->remove(0);
  plaintextOut->reserve(cipherLen);
  for (size_t i = 0; i < cipherLen; i++) *plaintextOut += (char)plaintext[i];
  memset(plaintext, 0, sizeof(plaintext));
  return true;
}

bool verifyFrameOnly(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const uint8_t *frame, size_t frameLen) {
  return decryptFrame(type, to, from, id, frame, frameLen, nullptr);
}

uint16_t allocateMessageId() {
  uint16_t id = nextMessageId++;
  if (nextMessageId == 0) nextMessageId = 1;
  return id == 0 ? allocateMessageId() : id;
}
