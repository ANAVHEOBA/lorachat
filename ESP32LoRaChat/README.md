# ESP32LoRaChat

Offline browser-based mesh chat for ESP32 boards connected through LoRa.

The phone connects to the nearby ESP32 WiFi access point. The ESP32 serves the chat page locally and carries messages over LoRa to other ESP32 nodes. No internet is used.

## What This Version Adds

- Node presets: `NODE_PRESET 1`, `NODE_PRESET 2`, and `NODE_PRESET 3`
- Default broadcast chat destination so every node can receive the same group chat
- LoRa packet header with protocol version, packet type, destination, origin, relay, hop limit, and message ID
- PSK-based encrypted LoRa payloads using AES-128-CTR and HMAC-SHA256/64
- Duplicate packet filtering by origin/message ID
- Delayed LoRa rebroadcast with hop-limit decrement for simple long-distance relays
- One-hop ACK/retry still available if `CHAT_DESTINATION` is changed from `BROADCAST_ADDRESS` to `PEER_ADDRESS`
- Message length limits
- Browser UI with queued, sending, sent, relayed, retrying, delivered, failed, and received states
- Basic RSSI and hop-count display for received messages

## Required Arduino Libraries

Install these in Arduino IDE:

- ESP32 board support package
- LoRa by Sandeep Mistry

The sketch also uses ESP32 core libraries included with the board package:

- `WiFi.h`
- `WebServer.h`
- `SPI.h`
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
PATH="$PWD/.tools:$PATH" ./.tools/arduino-cli --config-file arduino-cli.yaml compile --fqbn esp32:esp32:esp32 --build-path .build/ESP32LoRaChat-preset2 --build-property compiler.cpp.extra_flags=-DNODE_PRESET=2 ESP32LoRaChat
PATH="$PWD/.tools:$PATH" ./.tools/arduino-cli --config-file arduino-cli.yaml upload -p /dev/cu.usbserial-0001 --fqbn esp32:esp32:esp32 --build-path .build/ESP32LoRaChat-preset2 ESP32LoRaChat
```

## Desktop Simulation

The protocol can be simulated without ESP32 hardware. From the project root:

```sh
mkdir -p .build/sim
c++ -std=c++17 -Wall -Wextra -Werror tools/mesh_protocol_sim.cpp -o .build/sim/mesh_protocol_sim
.build/sim/mesh_protocol_sim
```

This checks encrypted broadcast relay across a line topology, duplicate suppression in a triangle topology, direct one-hop authenticated ACK behavior, and wrong-key rejection. It does not test SPI wiring, LoRa module health, antenna matching, RF range, or ESP32 WiFi behavior.

## Hardware Pins

Default pins:

```text
ESP32 GPIO5  -> LoRa NSS/CS
ESP32 GPIO18 -> LoRa SCK
ESP32 GPIO19 -> LoRa MISO
ESP32 GPIO23 -> LoRa MOSI
3.3V         -> LoRa VCC
GND          -> LoRa GND
```

If your LoRa module needs `RST` or `DIO0`, wire them and update:

```cpp
const int8_t LORA_RESET = -1;
const int8_t LORA_DIO0 = -1;
```

## Frequency

The sketch defaults to:

```cpp
const long LORA_FREQUENCY_HZ = 433000000L;
```

Use the frequency that matches your exact LoRa module, antenna, and allowed local band. For 915 MHz hardware, change it to:

```cpp
const long LORA_FREQUENCY_HZ = 915000000L;
```

Do not run a 433 MHz antenna/module with 915 MHz code, or the reverse.

## Encryption Key

Every node in one chat group must use the same PSK:

```cpp
const char *CHAT_CRYPTO_PSK = "change-this-lora-chat-key";
```

Change that value before field use. The default value is public and only proves that the crypto path works; it does not provide privacy.

The PSK is hashed into separate AES and HMAC keys at boot. Data and ACK frames are authenticated. A node with the wrong PSK will ignore the packet and will not relay it.

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

## Expected Behavior

Default mode is mesh broadcast:

- Outgoing messages start as `queued`, then `sending`, then `sent`.
- If another node rebroadcasts your message and your node hears it, the status becomes `relayed`.
- Received messages show RSSI and how many LoRa hops the packet used.
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

## Protocol Shape

The custom LoRa header is 8 bytes:

```text
version, type, to, from, relay, hopLimit, idHigh, idLow
```

The header stays visible so relays can forward packets. The frame after the header is encrypted/authenticated:

```text
nonce[4], ciphertext[N], tag[8]
```

For a local broadcast, `from` and `relay` are both this node. For a rebroadcast, `from` stays as the original sender and `relay` becomes the current node. `hopLimit` is decremented on each relay. The relay forwards the original encrypted frame unchanged.

This firmware is not Meshtastic-compatible. The useful concepts from the larger firmware tree are implemented in a compact custom protocol here: packet history, naive flooding, direct ACK/retry, WiFi sleep disabled, conservative SX127x transmit power, and PSK-based payload encryption using standard ESP32 mbedTLS.

## Firmware Tree Map

The larger firmware repo is GPLv3, so this sketch does not copy its implementation. The useful local references were:

- `firmware-develop/src/mesh/FloodingRouter.h`: broadcast flooding rules, duplicate drop, random rebroadcast delay
- `firmware-develop/src/mesh/PacketHistory.h`: recent packet history keyed by origin/message ID
- `firmware-develop/src/mesh/ReliableRouter.h`: one-hop ACK/retry idea
- `firmware-develop/src/mesh/RadioInterface.cpp`: packet header carries origin, destination, relay, and hop fields
- `firmware-develop/src/mesh/RF95Interface.cpp`: conservative SX127x transmit power guidance
- `firmware-develop/src/mesh/wifi/WiFiAPClient.cpp`: disable WiFi sleep/persistence behavior for stable node networking

## Practical Notes

- Keep messages short. LoRa is slow compared with WiFi.
- Use a real antenna. Do not transmit without an antenna attached.
- If messages fail, verify frequency, addresses, PSK, wiring, power, and antenna match first.
- All boards must use the same LoRa settings: frequency, spreading factor, bandwidth, coding rate, sync word, and `CHAT_CRYPTO_PSK`.
