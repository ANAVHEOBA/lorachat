# ESP32LoRaChat

Offline browser-based mesh chat for ESP32 boards connected through LoRa.

The phone connects to the nearby ESP32 WiFi access point. The ESP32 serves the chat page locally and carries messages over LoRa to other ESP32 nodes. No internet is used.

## What This Version Adds

- Node presets: `NODE_PRESET 1`, `NODE_PRESET 2`, and `NODE_PRESET 3`
- Default broadcast chat destination so every node can receive the same group chat
- LoRa packet header with protocol version, packet type, destination, origin, relay, hop limit, and message ID
- PSK-based encrypted LoRa payloads using AES-128-CTR and HMAC-SHA256/64
- Optional Shannon/Markov-gated text compression before encryption for compressible DATA packets
- Duplicate packet filtering by origin/message ID
- Delayed LoRa rebroadcast with hop-limit decrement for simple long-distance relays
- Authenticated node heartbeat packets so the UI can show which mesh nodes are available
- One-hop ACK/retry still available if `CHAT_DESTINATION` is changed from `BROADCAST_ADDRESS` to `PEER_ADDRESS`
- Message length limits
- Browser UI served from SPIFFS static files instead of being compiled into the firmware binary
- Browser UI with queued, sending, sent, relayed, retrying, delivered, failed, and received states
- Meshtastic-inspired browser panels for chat, known nodes, queues, and radio state
- AS32-TTL-100 UART radio backend for the screenshot wiring, with framed serial transport and CRC
- Optional SPI LoRa backend for boards wired to raw SX127x-style modules
- Hop-count display for received messages; RSSI/SNR display only on radio backends that expose it

## Required Arduino Libraries

Install these in Arduino IDE:

- ESP32 board support package
- LoRa by Sandeep Mistry, only if compiling with `RADIO_BACKEND_SPI_LORA`

The sketch also uses ESP32 core libraries included with the board package:

- `WiFi.h`
- `WebServer.h`
- `SPIFFS.h`
- `SPI.h`, only for the optional SPI LoRa backend
- ESP32 mbedTLS headers for AES, SHA-256, and HMAC

## Local CLI Build

This workspace has a local Arduino CLI install in `.tools/` and uses ESP32 core `1.0.6`.

From the project root, compile with:

```sh
PATH="$PWD/.tools:$PATH" ./.tools/arduino-cli --config-file arduino-cli.yaml compile --fqbn esp32:esp32:esp32 --build-path .build/ESP32LoRaChat ESP32LoRaChat
```

Upload after compiling by replacing the port with your ESP32 serial port:

```sh
PATH="$PWD/.tools:$PATH" ./.tools/arduino-cli --config-file arduino-cli.yaml upload -p /dev/cu.usbserial-0001 --fqbn esp32:esp32:esp32 --build-path .build/ESP32LoRaChat ESP32LoRaChat
```

To compile and upload a different preset from the CLI without editing the file:

```sh
PATH="$PWD/.tools:$PATH" ./.tools/arduino-cli --config-file arduino-cli.yaml compile --fqbn esp32:esp32:esp32 --build-path .build/ESP32LoRaChat-preset2 --build-property "compiler.cpp.extra_flags=-DNODE_PRESET=2 -DRADIO_BACKEND=RADIO_BACKEND_AS32_UART" ESP32LoRaChat
PATH="$PWD/.tools:$PATH" ./.tools/arduino-cli --config-file arduino-cli.yaml upload -p /dev/cu.usbserial-0001 --fqbn esp32:esp32:esp32 --build-path .build/ESP32LoRaChat-preset2 ESP32LoRaChat
```

The helper script wraps those commands and auto-detects common ESP32 USB serial ports when exactly one is present:

```sh
tools/flash_node.sh A
tools/flash_node.sh B
tools/flash_node.sh C
```

If auto-detection cannot choose a port, pass it explicitly:

```sh
tools/flash_node.sh A /dev/cu.usbserial-0001
```

For TTGO/Heltec style boards, override the board profile when needed:

```sh
FQBN=esp32:esp32:ttgo-lora32-v1 tools/flash_node.sh A /dev/cu.usbserial-0001
```

The flash helper defaults to the AS32 UART backend. To build for a raw SPI LoRa board instead:

```sh
RADIO_BACKEND=SPI_LORA tools/flash_node.sh A /dev/cu.usbserial-0001
```

The browser UI is separate from the firmware binary. Build and upload the SPIFFS web image after flashing each board, or whenever the files under `ESP32LoRaChat/data/static/` change:

```sh
tools/build_webfs.sh
tools/upload_webfs.sh /dev/cu.usbserial-0001
```

This local ESP32 Arduino core has SPIFFS tooling available, not LittleFS. The architecture is the same as the larger firmware repo: compiled firmware serves API routes, while static frontend files live in a flash filesystem.

Open the serial monitor at 115200 baud:

```sh
tools/monitor_node.sh /dev/cu.usbserial-0001
```

## Desktop Simulation

The protocol can be simulated without ESP32 hardware. From the project root:

```sh
mkdir -p .build/sim
c++ -std=c++17 -Wall -Wextra -Werror tools/mesh_protocol_sim.cpp -o .build/sim/mesh_protocol_sim
.build/sim/mesh_protocol_sim
```

This checks encrypted broadcast relay across a line topology, duplicate suppression in a triangle topology, direct one-hop authenticated ACK behavior, wrong-key rejection, compressed payload round-trip, and compressed relay forwarding. It does not test SPI wiring, LoRa module health, antenna matching, RF range, or ESP32 WiFi behavior.

## Hardware Pins

The default firmware backend matches the AS32-TTL-100 wiring shown in the node screenshots:

```text
ESP32 GPIO16/RX2 -> AS32 TXD
ESP32 GPIO17/TX2 -> AS32 RXD
ESP32 GPIO25     -> AS32 MD0
ESP32 GPIO26     -> AS32 MD1
ESP32 GPIO27     -> AS32 AUX
ESP32 GND        -> AS32 GND/common ground
External 5V      -> AS32 VCC
```

The firmware drives `MD0=LOW` and `MD1=LOW` for normal transparent UART mode and uses `AUX` to avoid transmitting while the module is busy. All AS32 modules must use the same UART baud rate, channel, address/mode settings, air data rate, and antenna band.

Important electrical checks:

- Attach the correct 433 MHz antenna before transmitting.
- Power the AS32 from a stable external 5V regulator, with common ground to the ESP32.
- Confirm the AS32 TXD logic level is safe for the ESP32 RX2 pin. ESP32 GPIOs are not 5V tolerant, so use a divider or level shifter if the module outputs 5V UART.

Node 3 OLED wiring from the screenshot does not conflict with the AS32 pins:

```text
OLED SCK  -> ESP32 GPIO18
OLED MOSI -> ESP32 GPIO23
OLED RES  -> ESP32 GPIO22
OLED DC   -> ESP32 GPIO21
OLED CS   -> ESP32 GPIO4
OLED VCC  -> ESP32 3V3
OLED GND  -> ESP32 GND
```

OLED output is not enabled in this sketch yet; the browser UI is the current frontend.

### Optional SPI LoRa Pins

If you compile with `RADIO_BACKEND=SPI_LORA`, use these pins:

```text
ESP32 GPIO5  -> LoRa NSS/CS
ESP32 GPIO18 -> LoRa SCK
ESP32 GPIO19 -> LoRa MISO
ESP32 GPIO23 -> LoRa MOSI
3.3V         -> LoRa VCC
GND          -> LoRa GND
```

If the SPI LoRa module needs `RST` or `DIO0`, wire them and update:

```cpp
const int8_t LORA_RESET = -1;
const int8_t LORA_DIO0 = -1;
```

## Frequency

For SPI LoRa, the sketch defaults to:

```cpp
const long LORA_FREQUENCY_HZ = 433000000L;
```

Use the frequency that matches your exact LoRa module, antenna, and allowed local band. For 915 MHz SPI hardware, change it to:

```cpp
const long LORA_FREQUENCY_HZ = 915000000L;
```

Do not run a 433 MHz antenna/module with 915 MHz code, or the reverse. For AS32 modules, this sketch does not reprogram the module frequency; set every AS32 module to matching 433 MHz/channel settings with the vendor configuration tool before field use if they are not already matched.

## Encryption Key

Every node in one chat group must use the same PSK:

```cpp
const char *CHAT_CRYPTO_PSK = "change-this-lora-chat-key";
```

Change that value before field use. The default value is public and only proves that the crypto path works; it does not provide privacy.

The PSK is hashed into separate AES and HMAC keys at boot. DATA, HELLO, and ACK frames are authenticated. A node with the wrong PSK will ignore the packet and will not relay it. DATA payload compression happens before encryption and only when the encoded payload is actually smaller.

## Upload Flow

1. Open `ESP32LoRaChat.ino` in Arduino IDE.
2. For the first ESP32, keep:

   ```cpp
   #define NODE_PRESET 1
   ```

3. Upload to the first ESP32.
4. For the second ESP32, change it to:

   ```cpp
   #define NODE_PRESET 2
   ```

5. Upload to the second ESP32.
6. For a third relay/chat node, change it to:

   ```cpp
   #define NODE_PRESET 3
   ```

7. Upload to the third ESP32.

## Connect

First ESP32:

```text
WiFi: LoRaChat-A
Password: chatpass123
URL: http://192.168.4.1
```

Second ESP32:

```text
WiFi: LoRaChat-B
Password: chatpass123
URL: http://192.168.4.1
```

Each phone connects to the WiFi network of the nearby ESP32. Open the URL, type a name, and send a message.

Third ESP32:

```text
WiFi: LoRaChat-C
Password: chatpass123
URL: http://192.168.4.1
```

Each ESP32 access point normally uses the same local gateway IP, `192.168.4.1`, for the phone connected to that one node. The other LoRa nodes are not reachable as browser IP addresses through LoRa. Instead, the UI `Nodes` tab shows mesh node addresses such as `0xAA`, `0xBB`, and `0xCC`, plus last-heard activity from encrypted heartbeat packets.

Users do not need Bluetooth for this app. The phone/laptop connects to the nearby ESP32 over WiFi, opens the local web UI, and the ESP32 carries messages to other nodes over LoRa.

Friendly node names are backend/default device identity, such as `Node A`, `Node B`, and `Node C`. They are included in encrypted heartbeat packets so other nodes can show those names in the `Nodes` tab.

## Expected Behavior

Default mode is mesh broadcast:

- Outgoing messages start as `queued`, then `sending`, then `sent`.
- If another node rebroadcasts your message and your node hears it, the status becomes `relayed`.
- The `Nodes` tab should begin showing other powered nodes after their periodic heartbeat is heard or relayed.
- Received messages show how many mesh hops the packet used. RSSI/SNR appears only on the SPI LoRa backend; AS32 transparent UART mode does not expose that telemetry to the ESP32.
- Duplicate packets are dropped, so the same broadcast should not appear multiple times.

For direct one-hop ACK/retry mode, change:

```cpp
const uint8_t CHAT_DESTINATION = BROADCAST_ADDRESS;
```

to:

```cpp
const uint8_t CHAT_DESTINATION = PEER_ADDRESS;
```

In direct mode, a received packet gets an ACK. If no ACK arrives after retries, the message becomes `failed`.

## Voice Notes And Calls

WhatsApp-style voice notes and live voice calls are not practical on this AS32/LoRa design.

Voice notes from a browser are usually many kilobytes even when compressed. This mesh packet format is intentionally capped for short text, and LoRa airtime is too scarce to move audio clips reliably across three nodes without long delays and a high chance of loss.

The built-in Shannon/Markov-gated compression is for short text payloads only. It is not an audio codec and does not make live voice calls practical over this LoRa link.

Live voice calls are a worse fit: they need continuous two-way bandwidth, low latency, jitter handling, and packet loss recovery. That should use WiFi, BLE audio, cellular, or another higher-throughput radio, not this LoRa mesh. For this project, LoRa should carry text, small commands, status, and maybe very short canned alerts.

## Range Test

Use at least two flashed nodes. Use three nodes if you want to prove relay behavior.

1. Flash node A, node B, and optionally node C:

   ```sh
   tools/flash_node.sh A /dev/cu.usbserial-0001
   tools/upload_webfs.sh /dev/cu.usbserial-0001

   tools/flash_node.sh B /dev/cu.usbserial-0001
   tools/upload_webfs.sh /dev/cu.usbserial-0001

   tools/flash_node.sh C /dev/cu.usbserial-0001
   tools/upload_webfs.sh /dev/cu.usbserial-0001
   ```

2. Power the nodes from stable USB or battery power and attach matched antennas before transmitting.
3. Connect one phone or laptop to `LoRaChat-A`, and another to `LoRaChat-B`.
4. Open `http://192.168.4.1` on each connected device.
5. Send short test messages from A to B and B to A.
6. Watch the UI status fields:

   - `Radio` should change from `no packets` to a recent packet age.
   - `Signal` shows RSSI/SNR only for the SPI LoRa backend. With AS32 it remains blank because the modem hides RSSI/SNR.
   - `Queue` should return to `0 total` after send/relay work finishes.
   - Received message metadata should show sender address and hop count. SPI LoRa builds also show RSSI/SNR.

7. For a relay test, place B between A and C. Keep A and C far enough apart that direct reception is weak or absent, then send from A while connected to `LoRaChat-A` and confirm it appears on `LoRaChat-C`.

## Protocol Shape

The custom LoRa header is 8 bytes:

```text
version, type, to, from, relay, hopLimit, idHigh, idLow
```

The high bit of `type` is a compression flag. The low 7 bits carry the base packet type: DATA, ACK, or HELLO.

The header stays visible so relays can forward packets. The frame after the header is encrypted/authenticated:

```text
nonce[4], ciphertext[N], tag[8]
```

For DATA packets, the plaintext may first pass through a small text codec. A Shannon entropy estimate and a simple Markov class-entropy estimate decide whether compression is worth trying, then the firmware still requires the compressed byte count to beat the original before setting the compression flag.

For a local broadcast, `from` and `relay` are both this node. For a rebroadcast, `from` stays as the original sender and `relay` becomes the current node. `hopLimit` is decremented on each relay. The relay forwards the original encrypted frame unchanged.

This firmware is not Meshtastic-compatible. The useful concepts from the larger firmware tree are implemented in a compact custom protocol here: packet history, naive flooding, direct ACK/retry, WiFi sleep disabled, separated web frontend assets, adaptive text compression, and PSK-based payload encryption using standard ESP32 mbedTLS.

For AS32 UART, the custom mesh packet is wrapped in a serial transport frame:

```text
magic[2], length[2], meshPacket[N], crc16[2]
```

That wrapper is needed because AS32 transparent mode gives the ESP32 a byte stream, not the packet boundary API that raw SPI LoRa libraries provide.

## Frontend Split

The compiled backend serves these routes:

```text
/api/send
/api/messages
/api/status
/api/nodes
```

The frontend files are normal web files:

```text
ESP32LoRaChat/data/static/index.html
ESP32LoRaChat/data/static/style.css
ESP32LoRaChat/data/static/app.js
```

At runtime, the ESP32 maps browser requests to SPIFFS:

```text
/          -> /static/index.html
/style.css -> /static/style.css
/app.js    -> /static/app.js
```

If the SPIFFS image has not been uploaded, the firmware serves a small compiled fallback page so the backend still proves it is running.

## Firmware Tree Map

The larger firmware repo is GPLv3, so this sketch does not copy its implementation. The useful local references were:

- `firmware-develop/src/mesh/FloodingRouter.h`: broadcast flooding rules, duplicate drop, random rebroadcast delay
- `firmware-develop/src/mesh/PacketHistory.h`: recent packet history keyed by origin/message ID
- `firmware-develop/src/mesh/ReliableRouter.h`: one-hop ACK/retry idea
- `firmware-develop/src/mesh/RadioInterface.cpp`: packet header carries origin, destination, relay, and hop fields
- `firmware-develop/src/mesh/RF95Interface.cpp`: conservative SX127x transmit power guidance
- `firmware-develop/src/mesh/wifi/WiFiAPClient.cpp`: disable WiFi sleep/persistence behavior for stable node networking
- `firmware-develop/platformio.ini`: separate device UI dependency pattern; this project uses SPIFFS static files for the same firmware/frontend split idea

## Practical Notes

- Keep messages short. LoRa is slow compared with WiFi.
- Use a real antenna. Do not transmit without an antenna attached.
- If messages fail, verify frequency, addresses, PSK, wiring, power, and antenna match first.
- All AS32 boards must use the same AS32 channel/air-rate/UART settings and `CHAT_CRYPTO_PSK`.
- All SPI LoRa boards must use the same frequency, spreading factor, bandwidth, coding rate, sync word, and `CHAT_CRYPTO_PSK`.
