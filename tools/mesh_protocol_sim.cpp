#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint8_t BROADCAST_ADDRESS = 0xFF;
constexpr uint8_t PROTOCOL_VERSION = 2;
constexpr uint8_t PACKET_TYPE_DATA = 1;
constexpr uint8_t PACKET_TYPE_ACK = 2;
constexpr uint8_t PACKET_TYPE_MASK = 0x7F;
constexpr uint8_t PACKET_FLAG_COMPRESSED = 0x80;
constexpr uint8_t LORA_HOP_LIMIT = 2;
constexpr size_t CRYPTO_NONCE_SIZE = 4;
constexpr size_t CRYPTO_TAG_SIZE = 8;
constexpr size_t COMPRESSION_MIN_PLAINTEXT_LEN = 32;
constexpr size_t COMPRESSION_MIN_GAIN = 2;
constexpr uint8_t COMPRESSION_ESCAPE = 0x7F;
constexpr uint8_t COMPRESSION_RUN = 0x7E;
constexpr uint8_t COMPRESSION_TOKEN_BASE = 0x80;
constexpr uint8_t COMPRESSION_MIN_RUN = 4;
constexpr double COMPRESSION_SHANNON_LIMIT = 5.35;
constexpr double COMPRESSION_MARKOV_LIMIT = 4.65;
const std::string DEFAULT_CRYPTO_KEY = "change-this-lora-chat-key";
const std::vector<std::string> COMPRESSION_DICTIONARY = {
    "User\n", " the ", " and ", " you ", " are ", " for ", " with ", " from ", " that ", " this ",
    " here", " there", " where", " when", " emergency", " message", " location", " battery",
    " signal", " relay", " node", " lora", " meet", " base", " help", " ok", " yes", " no",
    " at ", " to ", " in ", " is "
};

uint8_t basePacketType(uint8_t type)
{
    return type & PACKET_TYPE_MASK;
}

uint8_t compressionClass(uint8_t value)
{
    if (value == ' ' || value == '\n' || value == '\t') return 0;
    if (value >= 'a' && value <= 'z') return 1;
    if (value >= 'A' && value <= 'Z') return 2;
    if (value >= '0' && value <= '9') return 3;
    if (value == '.' || value == ',' || value == '?' || value == '!' || value == '-' || value == ':' || value == ';') {
        return 4;
    }
    return 5;
}

double estimateShannonBitsPerByte(const std::string &data)
{
    if (data.empty()) return 8.0;

    uint16_t counts[256] = {0};
    for (unsigned char value : data) counts[value]++;

    double entropy = 0.0;
    const double invLen = 1.0 / static_cast<double>(data.size());
    for (uint16_t count : counts) {
        if (count == 0) continue;
        const double p = static_cast<double>(count) * invLen;
        entropy -= p * (std::log(p) / std::log(2.0));
    }
    return entropy;
}

double estimateMarkovBitsPerByte(const std::string &data)
{
    if (data.size() < 2) return 8.0;

    constexpr uint8_t classCount = 6;
    uint16_t transitions[classCount][classCount] = {{0}};
    uint16_t rowTotals[classCount] = {0};

    for (size_t i = 1; i < data.size(); i++) {
        const uint8_t prev = compressionClass(static_cast<uint8_t>(data[i - 1]));
        const uint8_t curr = compressionClass(static_cast<uint8_t>(data[i]));
        transitions[prev][curr]++;
        rowTotals[prev]++;
    }

    double entropy = 0.0;
    const double invTransitions = 1.0 / static_cast<double>(data.size() - 1);
    for (uint8_t row = 0; row < classCount; row++) {
        if (rowTotals[row] == 0) continue;

        double rowEntropy = 0.0;
        const double invRow = 1.0 / static_cast<double>(rowTotals[row]);
        for (uint8_t col = 0; col < classCount; col++) {
            if (transitions[row][col] == 0) continue;
            const double p = static_cast<double>(transitions[row][col]) * invRow;
            rowEntropy -= p * (std::log(p) / std::log(2.0));
        }

        entropy += static_cast<double>(rowTotals[row]) * invTransitions * rowEntropy;
    }
    return entropy;
}

int findCompressionToken(const std::string &data, size_t pos, size_t *matchLen)
{
    int bestToken = -1;
    size_t bestLen = 0;

    for (size_t i = 0; i < COMPRESSION_DICTIONARY.size(); i++) {
        const std::string &entry = COMPRESSION_DICTIONARY[i];
        if (entry.size() <= bestLen || pos + entry.size() > data.size()) continue;
        if (data.compare(pos, entry.size(), entry) == 0) {
            bestToken = static_cast<int>(i);
            bestLen = entry.size();
        }
    }

    if (matchLen != nullptr) *matchLen = bestLen;
    return bestToken;
}

int estimateStaticDictionarySavings(const std::string &data)
{
    int savings = 0;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t runLen = 1;
        while (pos + runLen < data.size() && data[pos + runLen] == data[pos] && runLen < 255) runLen++;
        if (runLen >= COMPRESSION_MIN_RUN) {
            savings += static_cast<int>(runLen) - 3;
            pos += runLen;
            continue;
        }

        size_t matchLen = 0;
        const int token = findCompressionToken(data, pos, &matchLen);
        if (token >= 0 && matchLen > 1) {
            savings += static_cast<int>(matchLen) - 1;
            pos += matchLen;
        } else {
            pos++;
        }
    }
    return savings;
}

bool shouldTryCompression(const std::string &data)
{
    if (data.size() < COMPRESSION_MIN_PLAINTEXT_LEN) return false;
    if (estimateStaticDictionarySavings(data) >= static_cast<int>(COMPRESSION_MIN_GAIN)) return true;
    return estimateShannonBitsPerByte(data) <= COMPRESSION_SHANNON_LIMIT ||
           estimateMarkovBitsPerByte(data) <= COMPRESSION_MARKOV_LIMIT;
}

bool appendCompressedLiteral(uint8_t value, std::string *out)
{
    if (out == nullptr) return false;
    if (value >= COMPRESSION_RUN) {
        out->push_back(static_cast<char>(COMPRESSION_ESCAPE));
        out->push_back(static_cast<char>(value));
    } else {
        out->push_back(static_cast<char>(value));
    }
    return true;
}

bool tryCompressPlaintext(const std::string &data, std::string *out)
{
    if (out == nullptr || !shouldTryCompression(data)) return false;

    std::string encoded;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t runLen = 1;
        while (pos + runLen < data.size() && data[pos + runLen] == data[pos] && runLen < 255) runLen++;
        if (runLen >= COMPRESSION_MIN_RUN) {
            encoded.push_back(static_cast<char>(COMPRESSION_RUN));
            encoded.push_back(static_cast<char>(runLen));
            encoded.push_back(data[pos]);
            pos += runLen;
            continue;
        }

        size_t matchLen = 0;
        const int token = findCompressionToken(data, pos, &matchLen);
        if (token >= 0) {
            encoded.push_back(static_cast<char>(COMPRESSION_TOKEN_BASE + token));
            pos += matchLen;
            continue;
        }

        appendCompressedLiteral(static_cast<uint8_t>(data[pos]), &encoded);
        pos++;
    }

    if (encoded.size() + COMPRESSION_MIN_GAIN > data.size()) return false;
    *out = encoded;
    return true;
}

bool decompressPlaintext(const std::string &data, std::string *out)
{
    if (out == nullptr) return false;

    std::string decoded;
    size_t pos = 0;
    while (pos < data.size()) {
        const uint8_t value = static_cast<uint8_t>(data[pos++]);

        if (value == COMPRESSION_ESCAPE) {
            if (pos >= data.size()) return false;
            decoded.push_back(data[pos++]);
            continue;
        }

        if (value == COMPRESSION_RUN) {
            if (pos + 2 > data.size()) return false;
            const uint8_t count = static_cast<uint8_t>(data[pos++]);
            const char repeated = data[pos++];
            if (count < COMPRESSION_MIN_RUN) return false;
            decoded.append(count, repeated);
            continue;
        }

        if (value >= COMPRESSION_TOKEN_BASE) {
            const size_t index = static_cast<size_t>(value - COMPRESSION_TOKEN_BASE);
            if (index >= COMPRESSION_DICTIONARY.size()) return false;
            decoded += COMPRESSION_DICTIONARY[index];
            continue;
        }

        decoded.push_back(static_cast<char>(value));
    }

    *out = decoded;
    return true;
}

std::string hexByte(uint8_t value)
{
    const char *digits = "0123456789ABCDEF";
    std::string out = "0x00";
    out[2] = digits[(value >> 4) & 0x0F];
    out[3] = digits[value & 0x0F];
    return out;
}

uint64_t fnv1a(uint64_t hash, uint8_t value)
{
    hash ^= value;
    return hash * 1099511628211ULL;
}

uint64_t frameTag(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const std::string &nonce,
                  const std::string &ciphertext, const std::string &key)
{
    uint64_t hash = 1469598103934665603ULL;
    hash = fnv1a(hash, PROTOCOL_VERSION);
    hash = fnv1a(hash, type);
    hash = fnv1a(hash, to);
    hash = fnv1a(hash, from);
    hash = fnv1a(hash, static_cast<uint8_t>(id >> 8));
    hash = fnv1a(hash, static_cast<uint8_t>(id & 0xFF));
    for (unsigned char value : nonce) hash = fnv1a(hash, value);
    for (unsigned char value : ciphertext) hash = fnv1a(hash, value);
    for (unsigned char value : key) hash = fnv1a(hash, value);
    return hash;
}

void appendTag(std::string &frame, uint64_t tag)
{
    for (size_t i = 0; i < CRYPTO_TAG_SIZE; i++) {
        frame.push_back(static_cast<char>((tag >> (i * 8)) & 0xFF));
    }
}

uint64_t readTag(const std::string &frame, size_t pos)
{
    uint64_t tag = 0;
    for (size_t i = 0; i < CRYPTO_TAG_SIZE; i++) {
        tag |= static_cast<uint64_t>(static_cast<uint8_t>(frame[pos + i])) << (i * 8);
    }
    return tag;
}

std::string cryptForSim(const std::string &input, const std::string &nonce, const std::string &key)
{
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); i++) {
        uint8_t keyByte = static_cast<uint8_t>(key[(i + static_cast<uint8_t>(nonce[0])) % key.size()]);
        uint8_t nonceByte = static_cast<uint8_t>(nonce[i % nonce.size()]);
        output.push_back(static_cast<char>(static_cast<uint8_t>(input[i]) ^ keyByte ^ nonceByte));
    }
    return output;
}

std::string buildFrame(uint8_t &type, uint8_t to, uint8_t from, uint16_t id, const std::string &plaintext,
                       const std::string &key)
{
    std::string nonce;
    nonce.push_back(static_cast<char>(id >> 8));
    nonce.push_back(static_cast<char>(id & 0xFF));
    nonce.push_back(static_cast<char>(from));
    nonce.push_back(static_cast<char>(to));

    std::string payload = plaintext;
    type = basePacketType(type);
    std::string compressed;
    if (type == PACKET_TYPE_DATA && tryCompressPlaintext(plaintext, &compressed)) {
        payload = compressed;
        type |= PACKET_FLAG_COMPRESSED;
    }

    std::string ciphertext = cryptForSim(payload, nonce, key);
    std::string frame = nonce + ciphertext;
    appendTag(frame, frameTag(type, to, from, id, nonce, ciphertext, key));
    return frame;
}

bool openFrame(uint8_t type, uint8_t to, uint8_t from, uint16_t id, const std::string &frame, const std::string &key,
               std::string *plaintextOut)
{
    if (frame.size() < CRYPTO_NONCE_SIZE + CRYPTO_TAG_SIZE) return false;

    std::string nonce = frame.substr(0, CRYPTO_NONCE_SIZE);
    const size_t cipherLen = frame.size() - CRYPTO_NONCE_SIZE - CRYPTO_TAG_SIZE;
    std::string ciphertext = frame.substr(CRYPTO_NONCE_SIZE, cipherLen);
    const uint64_t storedTag = readTag(frame, CRYPTO_NONCE_SIZE + cipherLen);
    const uint64_t expectedTag = frameTag(type, to, from, id, nonce, ciphertext, key);
    if (storedTag != expectedTag) return false;

    if (plaintextOut != nullptr) {
        std::string plaintext = cryptForSim(ciphertext, nonce, key);
        if ((type & PACKET_FLAG_COMPRESSED) != 0 && !decompressPlaintext(plaintext, &plaintext)) return false;
        *plaintextOut = plaintext;
    }
    return true;
}

struct Packet {
    uint8_t type = PACKET_TYPE_DATA;
    uint8_t to = BROADCAST_ADDRESS;
    uint8_t from = 0;
    uint8_t relay = 0;
    uint8_t hopLimit = 0;
    uint16_t id = 0;
    std::string payload;
};

struct ChatEntry {
    uint16_t id = 0;
    uint8_t from = 0;
    bool outgoing = false;
    std::string sender;
    std::string text;
    std::string status;
    uint8_t hops = 0;
};

struct SeenPacket {
    uint8_t from = 0;
    uint16_t id = 0;
};

struct PendingRelay {
    bool active = false;
    int sendAt = 0;
    Packet packet;
};

struct Event {
    int at = 0;
    uint8_t src = 0;
    uint8_t dst = 0;
    Packet packet;
};

class Network;

class Node {
  public:
    explicit Node(uint8_t addr, std::string nodeName, std::string key = DEFAULT_CRYPTO_KEY)
        : address(addr), name(std::move(nodeName)), cryptoKey(std::move(key))
    {
    }

    void attach(Network *network) { net = network; }
    void sendChat(uint8_t to, uint16_t id, const std::string &sender, const std::string &text);
    void receive(const Packet &packet);
    void processRelays(int now);

    uint8_t address = 0;
    std::string name;
    std::vector<ChatEntry> history;

  private:
    friend class Network;

    void sendDataPayload(uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id, const std::string &payload);
    void sendFramePacket(Packet packet);
    void sendAckPacket(uint8_t to, uint16_t id);
    void handleIncomingData(const Packet &packet);
    void handleIncomingAck(const Packet &packet);
    bool wasSeen(uint8_t from, uint16_t id) const;
    void rememberSeen(uint8_t from, uint16_t id);
    void addHistory(uint16_t id, uint8_t from, bool outgoing, const std::string &sender, const std::string &text,
                    const std::string &status, uint8_t hops);
    void markOutgoingStatus(uint16_t id, const std::string &status);
    void scheduleRelay(const Packet &packet, uint8_t nextHopLimit);
    void cancelRelay(uint8_t from, uint16_t id);

    Network *net = nullptr;
    std::string cryptoKey;
    std::vector<SeenPacket> seen;
    std::vector<PendingRelay> relays;
    bool pendingActive = false;
    uint8_t pendingTo = 0;
    uint16_t pendingId = 0;
};

class Network {
  public:
    void addNode(Node &node)
    {
        node.attach(this);
        nodes.push_back(&node);
    }

    void addLink(uint8_t a, uint8_t b)
    {
        links.emplace_back(a, b);
        links.emplace_back(b, a);
    }

    void transmit(uint8_t src, const Packet &packet)
    {
        for (Node *node : nodes) {
            if (node->address == src) continue;
            if (!isLinked(src, node->address)) continue;
            events.push_back({now, src, node->address, packet});
        }
    }

    void run()
    {
        int guard = 0;
        while ((!events.empty() || hasPendingRelays()) && guard++ < 100) {
            if (!hasDueEvents() && !hasDueRelays()) {
                now = nextDueTime();
            }

            deliverDueEvents();

            for (Node *node : nodes) {
                node->processRelays(now);
            }
        }

        assert(guard < 100);
    }

    int currentTime() const { return now; }

  private:
    bool isLinked(uint8_t a, uint8_t b) const
    {
        return std::find(links.begin(), links.end(), std::make_pair(a, b)) != links.end();
    }

    bool hasDueEvents() const
    {
        return std::any_of(events.begin(), events.end(), [&](const Event &event) { return event.at <= now; });
    }

    bool hasDueRelays() const;
    bool hasPendingRelays() const;
    int nextDueTime() const;

    void deliverDueEvents()
    {
        bool delivered = true;
        while (delivered) {
            delivered = false;
            for (size_t i = 0; i < events.size(); i++) {
                if (events[i].at > now) continue;
                Event event = events[i];
                events.erase(events.begin() + static_cast<long>(i));
                for (Node *node : nodes) {
                    if (node->address == event.dst) {
                        node->receive(event.packet);
                        break;
                    }
                }
                delivered = true;
                break;
            }
        }
    }

    int now = 0;
    std::vector<Node *> nodes;
    std::vector<std::pair<uint8_t, uint8_t>> links;
    std::vector<Event> events;

    friend class Node;
};

void Node::sendChat(uint8_t to, uint16_t id, const std::string &sender, const std::string &text)
{
    const std::string payload = sender + "\n" + text;
    pendingActive = to != BROADCAST_ADDRESS;
    pendingTo = to;
    pendingId = id;

    rememberSeen(address, id);
    addHistory(id, address, true, sender, text, "queued", 0);
    markOutgoingStatus(id, "sending");
    sendDataPayload(to, address, address, LORA_HOP_LIMIT, id, payload);

    if (to == BROADCAST_ADDRESS) {
        pendingActive = false;
        markOutgoingStatus(id, "sent");
    }
}

void Node::receive(const Packet &packet)
{
    const uint8_t type = basePacketType(packet.type);
    if (type == PACKET_TYPE_DATA) {
        handleIncomingData(packet);
    } else if (type == PACKET_TYPE_ACK && packet.to == address) {
        handleIncomingAck(packet);
    }
}

void Node::processRelays(int now)
{
    for (PendingRelay &relay : relays) {
        if (!relay.active || relay.sendAt > now) continue;
        relay.active = false;
        relay.packet.relay = address;
        sendFramePacket(relay.packet);
        return;
    }
}

void Node::sendDataPayload(uint8_t to, uint8_t from, uint8_t relay, uint8_t hopLimit, uint16_t id,
                           const std::string &payload)
{
    Packet packet;
    packet.type = PACKET_TYPE_DATA;
    packet.to = to;
    packet.from = from;
    packet.relay = relay;
    packet.hopLimit = hopLimit;
    packet.id = id;
    packet.payload = buildFrame(packet.type, to, from, id, payload, cryptoKey);
    sendFramePacket(packet);
}

void Node::sendFramePacket(Packet packet)
{
    net->transmit(address, packet);
}

void Node::sendAckPacket(uint8_t to, uint16_t id)
{
    Packet packet;
    packet.type = PACKET_TYPE_ACK;
    packet.to = to;
    packet.from = address;
    packet.relay = address;
    packet.hopLimit = 0;
    packet.id = id;
    packet.payload = buildFrame(packet.type, to, address, id, "", cryptoKey);
    sendFramePacket(packet);
}

void Node::handleIncomingData(const Packet &packet)
{
    const bool addressedToUs = packet.to == address || packet.to == BROADCAST_ADDRESS;

    if (packet.from == address) {
        if (packet.to == BROADCAST_ADDRESS && packet.relay != address &&
            openFrame(packet.type, packet.to, packet.from, packet.id, packet.payload, cryptoKey, nullptr)) {
            markOutgoingStatus(packet.id, "relayed");
        }
        return;
    }

    if (wasSeen(packet.from, packet.id)) {
        if (packet.relay != address &&
            openFrame(packet.type, packet.to, packet.from, packet.id, packet.payload, cryptoKey, nullptr)) {
            cancelRelay(packet.from, packet.id);
        }
        return;
    }

    std::string plaintext;
    if (!openFrame(packet.type, packet.to, packet.from, packet.id, packet.payload, cryptoKey, &plaintext)) return;

    rememberSeen(packet.from, packet.id);

    if (addressedToUs) {
        if (packet.to == address) sendAckPacket(packet.from, packet.id);

        const size_t splitAt = plaintext.find('\n');
        const std::string sender = splitAt == std::string::npos ? hexByte(packet.from) : plaintext.substr(0, splitAt);
        const std::string text = splitAt == std::string::npos ? plaintext : plaintext.substr(splitAt + 1);
        const uint8_t hops = packet.hopLimit >= LORA_HOP_LIMIT ? 0 : static_cast<uint8_t>(LORA_HOP_LIMIT - packet.hopLimit);
        addHistory(packet.id, packet.from, false, sender, text, "received", hops);
    }

    if (packet.hopLimit > 0 && packet.to != address) {
        scheduleRelay(packet, static_cast<uint8_t>(packet.hopLimit - 1));
    }
}

void Node::handleIncomingAck(const Packet &packet)
{
    if (!pendingActive || packet.from != pendingTo || packet.id != pendingId) return;
    if (!openFrame(PACKET_TYPE_ACK, packet.to, packet.from, packet.id, packet.payload, cryptoKey, nullptr)) return;
    markOutgoingStatus(packet.id, "delivered");
    pendingActive = false;
}

bool Node::wasSeen(uint8_t from, uint16_t id) const
{
    return std::any_of(seen.begin(), seen.end(), [&](const SeenPacket &packet) {
        return packet.from == from && packet.id == id;
    });
}

void Node::rememberSeen(uint8_t from, uint16_t id)
{
    seen.push_back({from, id});
}

void Node::addHistory(uint16_t id, uint8_t from, bool outgoing, const std::string &sender, const std::string &text,
                      const std::string &status, uint8_t hops)
{
    history.push_back({id, from, outgoing, sender, text, status, hops});
}

void Node::markOutgoingStatus(uint16_t id, const std::string &status)
{
    for (auto item = history.rbegin(); item != history.rend(); ++item) {
        if (item->outgoing && item->id == id) {
            item->status = status;
            return;
        }
    }
}

void Node::scheduleRelay(const Packet &packet, uint8_t nextHopLimit)
{
    for (const PendingRelay &relay : relays) {
        if (relay.active && relay.packet.from == packet.from && relay.packet.id == packet.id) return;
    }

    Packet relayPacket = packet;
    relayPacket.relay = address;
    relayPacket.hopLimit = nextHopLimit;
    relays.push_back({true, net->currentTime() + 1, relayPacket});
}

void Node::cancelRelay(uint8_t from, uint16_t id)
{
    for (PendingRelay &relay : relays) {
        if (relay.active && relay.packet.from == from && relay.packet.id == id) {
            relay.active = false;
        }
    }
}

bool Network::hasDueRelays() const
{
    for (const Node *node : nodes) {
        for (const PendingRelay &relay : node->relays) {
            if (relay.active && relay.sendAt <= now) return true;
        }
    }
    return false;
}

bool Network::hasPendingRelays() const
{
    for (const Node *node : nodes) {
        for (const PendingRelay &relay : node->relays) {
            if (relay.active) return true;
        }
    }
    return false;
}

int Network::nextDueTime() const
{
    int next = 1000000;
    for (const Event &event : events) next = std::min(next, event.at);
    for (const Node *node : nodes) {
        for (const PendingRelay &relay : node->relays) {
            if (relay.active) next = std::min(next, relay.sendAt);
        }
    }
    return next;
}

size_t receivedFrom(const Node &node, uint8_t from)
{
    return static_cast<size_t>(std::count_if(node.history.begin(), node.history.end(), [&](const ChatEntry &item) {
        return !item.outgoing && item.from == from;
    }));
}

std::string outgoingStatus(const Node &node, uint16_t id)
{
    for (auto item = node.history.rbegin(); item != node.history.rend(); ++item) {
        if (item->outgoing && item->id == id) return item->status;
    }
    return "";
}

void testLineRelay()
{
    Node a(0xAA, "A");
    Node b(0xBB, "B");
    Node c(0xCC, "C");
    Network net;
    net.addNode(a);
    net.addNode(b);
    net.addNode(c);
    net.addLink(a.address, b.address);
    net.addLink(b.address, c.address);

    a.sendChat(BROADCAST_ADDRESS, 100, "alice", "line relay");
    net.run();

    assert(receivedFrom(b, a.address) == 1);
    assert(receivedFrom(c, a.address) == 1);
    assert(c.history.back().hops == 1);
    assert(outgoingStatus(a, 100) == "relayed");
    std::cout << "PASS line relay: A reached C through B\n";
}

void testDuplicateSuppression()
{
    Node a(0xAA, "A");
    Node b(0xBB, "B");
    Node c(0xCC, "C");
    Network net;
    net.addNode(a);
    net.addNode(b);
    net.addNode(c);
    net.addLink(a.address, b.address);
    net.addLink(a.address, c.address);
    net.addLink(b.address, c.address);

    a.sendChat(BROADCAST_ADDRESS, 101, "alice", "triangle duplicate");
    net.run();

    assert(receivedFrom(b, a.address) == 1);
    assert(receivedFrom(c, a.address) == 1);
    assert(outgoingStatus(a, 101) == "relayed");
    std::cout << "PASS duplicate suppression: C kept one copy in triangle network\n";
}

void testDirectAck()
{
    Node a(0xAA, "A");
    Node b(0xBB, "B");
    Network net;
    net.addNode(a);
    net.addNode(b);
    net.addLink(a.address, b.address);

    a.sendChat(b.address, 102, "alice", "direct ack");
    net.run();

    assert(receivedFrom(b, a.address) == 1);
    assert(outgoingStatus(a, 102) == "delivered");
    std::cout << "PASS direct ACK: B acknowledged A\n";
}

void testWrongKeyRejected()
{
    Node a(0xAA, "A", "shared-key");
    Node b(0xBB, "B", "different-key");
    Network net;
    net.addNode(a);
    net.addNode(b);
    net.addLink(a.address, b.address);

    a.sendChat(BROADCAST_ADDRESS, 103, "alice", "wrong key");
    net.run();

    assert(receivedFrom(b, a.address) == 0);
    assert(outgoingStatus(a, 103) == "sent");
    std::cout << "PASS wrong key rejection: B ignored unauthenticated ciphertext\n";
}

void testCompressionRoundTrip()
{
    const std::string payload =
        "User\nwhere are you where are you with the relay node at the base and the signal is ok";
    uint8_t type = PACKET_TYPE_DATA;
    const std::string frame = buildFrame(type, BROADCAST_ADDRESS, 0xAA, 104, payload, DEFAULT_CRYPTO_KEY);
    assert((type & PACKET_FLAG_COMPRESSED) != 0);

    std::string opened;
    assert(openFrame(type, BROADCAST_ADDRESS, 0xAA, 104, frame, DEFAULT_CRYPTO_KEY, &opened));
    assert(opened == payload);
    std::cout << "PASS compression round trip: Shannon/Markov-gated payload restored\n";
}

void testCompressedRelay()
{
    Node a(0xAA, "A");
    Node b(0xBB, "B");
    Node c(0xCC, "C");
    Network net;
    net.addNode(a);
    net.addNode(b);
    net.addNode(c);
    net.addLink(a.address, b.address);
    net.addLink(b.address, c.address);

    const std::string text = "where are you where are you with the relay node at the base and the signal is ok";
    a.sendChat(BROADCAST_ADDRESS, 105, "User", text);
    net.run();

    assert(receivedFrom(c, a.address) == 1);
    assert(c.history.back().text == text);
    std::cout << "PASS compressed relay: compressed DATA crossed A-B-C\n";
}

} // namespace

int main()
{
    testLineRelay();
    testDuplicateSuppression();
    testDirectAck();
    testWrongKeyRejected();
    testCompressionRoundTrip();
    testCompressedRelay();
    std::cout << "All mesh protocol simulation checks passed.\n";
    return 0;
}
