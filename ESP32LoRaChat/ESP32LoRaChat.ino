#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <LoRa.h>
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

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

const uint8_t PROTOCOL_VERSION = 2;
const uint8_t PACKET_TYPE_DATA = 1;
const uint8_t PACKET_TYPE_ACK = 2;
const uint8_t PACKET_HEADER_SIZE = 8;
const uint8_t CRYPTO_NONCE_SIZE = 4;
const uint8_t CRYPTO_TAG_SIZE = 8;
const uint8_t CRYPTO_AES_KEY_SIZE = 16;
const uint8_t CRYPTO_HMAC_KEY_SIZE = 32;

const uint8_t MAX_RETRIES = 3;
const unsigned long ACK_TIMEOUT_MS = 4500;
const unsigned long RELAY_DELAY_MIN_MS = 250;
const unsigned long RELAY_DELAY_MAX_MS = 950;
const uint8_t TX_QUEUE_SIZE = 8;
const uint8_t RELAY_QUEUE_SIZE = 4;
const uint8_t HISTORY_SIZE = 60;
const uint8_t SEEN_CACHE_SIZE = 16;
const size_t MAX_SENDER_LEN = 18;
const size_t MAX_MESSAGE_LEN = 120;
const size_t MAX_PLAINTEXT_LEN = MAX_SENDER_LEN + 1 + MAX_MESSAGE_LEN;
const size_t MAX_FRAME_LEN = CRYPTO_NONCE_SIZE + MAX_PLAINTEXT_LEN + CRYPTO_TAG_SIZE;

WebServer server(80);

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
  uint8_t to = BROADCAST_ADDRESS;
  uint8_t from = 0;
  uint16_t id = 0;
  uint8_t hopLimit = 0;
  uint8_t frameLen = 0;
  uint8_t frame[MAX_FRAME_LEN] = {0};
  unsigned long sendAt = 0;
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

int lastPacketRssi = 0;
float lastPacketSnr = 0;
unsigned long lastPacketAt = 0;

uint8_t cryptoAesKey[CRYPTO_AES_KEY_SIZE] = {0};
uint8_t cryptoHmacKey[CRYPTO_HMAC_KEY_SIZE] = {0};

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>LoRa Chat</title>
  <style>
    :root {
      --bg: #101419;
      --panel: #18202a;
      --line: #2b3745;
      --text: #eef3f8;
      --muted: #97a6b6;
      --accent: #3dbb8f;
      --outgoing: #245c4d;
      --incoming: #263445;
      --danger: #d66b6b;
      --warn: #d6b15f;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background: var(--bg);
      color: var(--text);
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      display: flex;
      justify-content: center;
    }
    .app {
      width: min(720px, 100vw);
      min-height: 100vh;
      display: grid;
      grid-template-rows: auto 1fr auto;
      background: var(--panel);
      border-left: 1px solid var(--line);
      border-right: 1px solid var(--line);
    }
    header {
      padding: 14px 16px;
      border-bottom: 1px solid var(--line);
      display: flex;
      gap: 12px;
      align-items: center;
      justify-content: space-between;
    }
    .title {
      min-width: 0;
    }
    h1 {
      font-size: 18px;
      line-height: 1.2;
      margin: 0 0 4px;
      font-weight: 650;
    }
    #status {
      color: var(--muted);
      font-size: 13px;
      overflow-wrap: anywhere;
    }
    .namebox {
      display: flex;
      gap: 8px;
      align-items: center;
    }
    .namebox input {
      width: 140px;
      max-width: 34vw;
    }
    main {
      overflow-y: auto;
      padding: 16px;
      display: flex;
      flex-direction: column;
      gap: 10px;
    }
    .msg {
      max-width: min(82%, 520px);
      padding: 10px 12px;
      border: 1px solid var(--line);
      border-radius: 8px;
      overflow-wrap: anywhere;
    }
    .msg.out { align-self: flex-end; background: var(--outgoing); }
    .msg.in { align-self: flex-start; background: var(--incoming); }
    .meta {
      display: flex;
      gap: 8px;
      align-items: baseline;
      color: var(--muted);
      font-size: 12px;
      margin-bottom: 5px;
    }
    .sender {
      color: var(--text);
      font-weight: 650;
    }
    .body {
      white-space: pre-wrap;
      line-height: 1.35;
    }
    .state {
      color: var(--muted);
      font-size: 12px;
      margin-top: 6px;
    }
    .state.failed { color: var(--danger); }
    .state.retrying,
    .state.queued,
    .state.sending { color: var(--warn); }
    footer {
      border-top: 1px solid var(--line);
      padding: 12px;
      display: grid;
      grid-template-columns: 1fr auto;
      gap: 10px;
    }
    input,
    button {
      border: 1px solid var(--line);
      border-radius: 8px;
      color: var(--text);
      background: #111820;
      font: inherit;
      min-height: 42px;
    }
    input {
      padding: 0 12px;
      min-width: 0;
    }
    button {
      padding: 0 16px;
      background: var(--accent);
      border-color: var(--accent);
      color: #07110e;
      font-weight: 700;
      cursor: pointer;
    }
    button:disabled {
      opacity: 0.45;
      cursor: not-allowed;
    }
    @media (max-width: 520px) {
      header {
        align-items: stretch;
        flex-direction: column;
      }
      .namebox input {
        width: 100%;
        max-width: none;
      }
      .msg {
        max-width: 92%;
      }
    }
  </style>
</head>
<body>
  <div class="app">
    <header>
      <div class="title">
        <h1>LoRa Chat</h1>
        <div id="status">Starting</div>
      </div>
      <div class="namebox">
        <input id="name" maxlength="18" autocomplete="nickname" placeholder="Name">
      </div>
    </header>
    <main id="messages"></main>
    <footer>
      <input id="message" maxlength="120" autocomplete="off" placeholder="Message">
      <button id="send" type="button">Send</button>
    </footer>
  </div>
  <script>
    const messagesEl = document.getElementById('messages');
    const statusEl = document.getElementById('status');
    const nameEl = document.getElementById('name');
    const messageEl = document.getElementById('message');
    const sendEl = document.getElementById('send');
    let lastRender = '';

    nameEl.value = localStorage.getItem('loraChatName') || 'User';

    function timeLabel(ms) {
      const totalSeconds = Math.floor(ms / 1000);
      const minutes = Math.floor(totalSeconds / 60);
      const seconds = totalSeconds % 60;
      return `${minutes}m ${seconds}s`;
    }

    function renderMessages(items) {
      const signature = JSON.stringify(items);
      if (signature === lastRender) return;
      const wasNearBottom = messagesEl.scrollHeight - messagesEl.scrollTop - messagesEl.clientHeight < 80;
      lastRender = signature;
      messagesEl.textContent = '';
      items.forEach(item => {
        const row = document.createElement('section');
        row.className = `msg ${item.outgoing ? 'out' : 'in'}`;

        const meta = document.createElement('div');
        meta.className = 'meta';

        const sender = document.createElement('span');
        sender.className = 'sender';
        sender.textContent = item.sender || (item.outgoing ? 'Me' : 'Peer');
        meta.appendChild(sender);

        const age = document.createElement('span');
        age.textContent = timeLabel(item.ageMs);
        meta.appendChild(age);
        row.appendChild(meta);

        const body = document.createElement('div');
        body.className = 'body';
        body.textContent = item.text;
        row.appendChild(body);

        const state = document.createElement('div');
        state.className = `state ${item.status}`;
        const hopText = `${item.hops || 0} ${(item.hops || 0) === 1 ? 'hop' : 'hops'}`;
        state.textContent = item.outgoing ? item.status : `rssi ${item.rssi} dBm | ${hopText}`;
        row.appendChild(state);

        messagesEl.appendChild(row);
      });
      if (wasNearBottom) messagesEl.scrollTop = messagesEl.scrollHeight;
    }

    async function refresh() {
      try {
        const [messagesRes, statusRes] = await Promise.all([
          fetch('/api/messages', { cache: 'no-store' }),
          fetch('/api/status', { cache: 'no-store' })
        ]);
        const messages = await messagesRes.json();
        const status = await statusRes.json();
        renderMessages(messages.messages || []);
        statusEl.textContent = `node ${status.node} to ${status.destination} | wifi ${status.clients} | queue ${status.queue} | hop ${status.hopLimit} | ${status.crypto} | ${status.radio}`;
      } catch (err) {
        statusEl.textContent = 'ESP32 connection lost';
      }
    }

    async function sendMessage() {
      const sender = nameEl.value.trim() || 'User';
      const text = messageEl.value.trim();
      if (!text) return;
      localStorage.setItem('loraChatName', sender);
      sendEl.disabled = true;
      try {
        const body = new URLSearchParams({ sender, message: text });
        const res = await fetch('/api/send', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body
        });
        if (!res.ok) throw new Error(await res.text());
        messageEl.value = '';
        await refresh();
      } catch (err) {
        statusEl.textContent = 'send rejected';
      } finally {
        sendEl.disabled = false;
        messageEl.focus();
      }
    }

    sendEl.addEventListener('click', sendMessage);
    messageEl.addEventListener('keydown', event => {
      if (event.key === 'Enter') sendMessage();
    });
    setInterval(refresh, 1000);
    refresh();
  </script>
</body>
</html>
)rawliteral";

String hexByte(uint8_t value);
String destinationLabel();
String jsonEscape(const String &value);
String cleanField(String value, size_t maxLen);
void sendNoStoreJson(int code, const String &payload);
void handleRoot();
void handleSend();
void handleMessages();
void handleStatus();
void handleNotFound();
bool enqueueOutgoing(const String &sender, const String &text, uint16_t *idOut);
void processTxQueue();
void startNextPending();
void sendPendingPacket();
void sendDataPacket(uint8_t to, uint16_t id, const String &sender, const String &text);
void sendDataPayload(uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const String &payload);
void sendFramePacket(uint8_t type, uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const uint8_t *frame, size_t frameLen);
void sendAckPacket(uint8_t to, uint16_t id);
void receiveLoRaPacket();
void handleIncomingData(uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const uint8_t *frame, size_t frameLen);
void handleIncomingAck(uint8_t to, uint8_t from, uint16_t id, const uint8_t *frame, size_t frameLen);
void processRelayQueue();
bool scheduleRelay(uint8_t to, uint8_t from, uint16_t id, uint8_t hopLimit, const uint8_t *frame, size_t frameLen);
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

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RESET, LORA_DIO0);

  Serial.println();
  Serial.println("Starting LoRa radio");
  if (!LoRa.begin(LORA_FREQUENCY_HZ)) {
    Serial.println("LoRa init failed. Check wiring, module frequency, and power.");
    while (true) delay(1000);
  }

  LoRa.setSpreadingFactor(LORA_SPREADING_FACTOR);
  LoRa.setSignalBandwidth(LORA_SIGNAL_BANDWIDTH);
  LoRa.setCodingRate4(LORA_CODING_RATE_DENOMINATOR);
  LoRa.setPreambleLength(LORA_PREAMBLE_LENGTH);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setTxPower(LORA_TX_POWER_DBM, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.enableCrc();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/send", HTTP_POST, handleSend);
  server.on("/api/messages", HTTP_GET, handleMessages);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.print("AP: ");
  Serial.println(AP_SSID);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());
  Serial.print("Node: ");
  Serial.print(hexByte(NODE_ADDRESS));
  Serial.print(" -> Destination: ");
  Serial.println(destinationLabel());
  Serial.print("LoRa: SF");
  Serial.print(LORA_SPREADING_FACTOR);
  Serial.print(" BW ");
  Serial.print(LORA_SIGNAL_BANDWIDTH);
  Serial.print(" Hz CR 4/");
  Serial.print(LORA_CODING_RATE_DENOMINATOR);
  Serial.print(" hop ");
  Serial.println(LORA_HOP_LIMIT);
  Serial.println("Crypto: AES-128-CTR + HMAC-SHA256/64");
  if (strcmp(CHAT_CRYPTO_PSK, "change-this-lora-chat-key") == 0) {
    Serial.println("WARNING: default chat PSK is still configured.");
  }
}

void loop() {
  server.handleClient();
  receiveLoRaPacket();
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

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
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
  response += "\",\"rssi\":";
  response += String(lastPacketRssi);
  response += ",\"snr\":";
  response += String(lastPacketSnr, 1);
  response += "}";
  sendNoStoreJson(200, response);
}

void handleNotFound() {
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
  LoRa.beginPacket();
  LoRa.write(PROTOCOL_VERSION);
  LoRa.write(type);
  LoRa.write(to);
  LoRa.write(from);
  LoRa.write(relay);
  LoRa.write(hopLimit);
  LoRa.write((uint8_t)(id >> 8));
  LoRa.write((uint8_t)(id & 0xFF));
  LoRa.write(frame, frameLen);
  LoRa.endPacket();
}

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

void receiveLoRaPacket() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  lastPacketRssi = LoRa.packetRssi();
  lastPacketSnr = LoRa.packetSnr();
  lastPacketAt = millis();

  if (packetSize < PACKET_HEADER_SIZE) {
    while (LoRa.available()) LoRa.read();
    return;
  }

  uint8_t version = LoRa.read();
  uint8_t type = LoRa.read();
  uint8_t to = LoRa.read();
  uint8_t from = LoRa.read();
  uint8_t relay = LoRa.read();
  uint8_t hopLimit = LoRa.read();
  uint16_t id = ((uint16_t)LoRa.read() << 8) | (uint8_t)LoRa.read();

  int frameLen = packetSize - PACKET_HEADER_SIZE;
  if (frameLen < (int)(CRYPTO_NONCE_SIZE + CRYPTO_TAG_SIZE) || frameLen > (int)MAX_FRAME_LEN) {
    while (LoRa.available()) LoRa.read();
    return;
  }

  uint8_t frame[MAX_FRAME_LEN];
  bool shortRead = false;
  for (int i = 0; i < frameLen; i++) {
    if (!LoRa.available()) {
      shortRead = true;
      break;
    }
    frame[i] = (uint8_t)LoRa.read();
  }
  while (LoRa.available()) LoRa.read();
  if (shortRead) return;

  if (version != PROTOCOL_VERSION) return;
  if (hopLimit > LORA_HOP_LIMIT) return;

  if (type == PACKET_TYPE_DATA) {
    handleIncomingData(to, from, relay, hopLimit, id, frame, (size_t)frameLen);
  } else if (type == PACKET_TYPE_ACK && to == NODE_ADDRESS) {
    handleIncomingAck(to, from, id, frame, (size_t)frameLen);
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
    if (relay != NODE_ADDRESS && verifyFrameOnly(PACKET_TYPE_DATA, to, from, id, frame, frameLen)) cancelRelay(from, id);
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
    scheduleRelay(to, from, id, hopLimit - 1, frame, frameLen);
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

void handleIncomingAck(uint8_t to, uint8_t from, uint16_t id, const uint8_t *frame, size_t frameLen) {
  if (!pendingTx.active || from != pendingTx.to || id != pendingTx.id) return;
  if (!verifyFrameOnly(PACKET_TYPE_ACK, to, from, id, frame, frameLen)) {
    Serial.print("ACK auth failed ");
    Serial.println(id);
    return;
  }

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

    sendFramePacket(PACKET_TYPE_DATA, item.to, item.from, NODE_ADDRESS, item.hopLimit, item.id, item.frame, item.frameLen);

    Serial.print("RELAY DATA ");
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

bool scheduleRelay(uint8_t to, uint8_t from, uint16_t id, uint8_t hopLimit, const uint8_t *frame, size_t frameLen) {
  if (frameLen > MAX_FRAME_LEN) return false;

  for (uint8_t i = 0; i < RELAY_QUEUE_SIZE; i++) {
    if (relayQueue[i].active && relayQueue[i].from == from && relayQueue[i].id == id) return true;
  }

  for (uint8_t i = 0; i < RELAY_QUEUE_SIZE; i++) {
    PendingRelay &item = relayQueue[i];
    if (item.active) continue;

    item.active = true;
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
