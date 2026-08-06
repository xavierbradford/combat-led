/*
 * SLAVE ESP32 - connects to its own light's WiFi network and forwards
 * (selector, value) commands received over a wired UART link as
 * properly-formatted UDP packets to that light.
 *
 * The master/slave sync happens entirely over the wired serial link
 * between the two ESP32 boards (UART2, pins 16/17 by default) - not over
 * the lights' own WiFi broadcast protocol. Each board only ever talks to
 * the single light it's joined to.
 *
 * Wiring between the two boards:
 *   this board TX2 (GPIO17) -> master board RX2 (GPIO16)
 *   this board RX2 (GPIO16) <- master board TX2 (GPIO17)
 *   this board GND          -- master board GND   (must be common!)
 *
 * Packet layout sent to the light (10 data bytes + 2 byte CRC, sent as 24
 * ASCII hex chars):
 *
 *   4C 54 09 00 30 57 00 SEL 01 VAL CRC_HI CRC_LO
 *   \_____ fixed header ____/  |   |   |
 *                              |   |   `- value (meaning depends on SEL)
 *                              |   `----- constant, always 0x01 in captures
 *                              `--------- selector: which control is being set
 *
 * CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0x0000, no reflect, no xorout)
 * over the 10 data bytes, big-endian in the packet.
 *
 * NOTE: SEL=0x02 and SEL=0x05 are both confirmed to be plain 0-100 percentage
 * controls, but which one is Brightness vs Saturation hasn't been confirmed
 * yet - swap the selectors in ledSelector() below if it turns out backwards.
 *
 * ---------------------------------------------------------------------
 * Wired link protocol (this board <-> master board): framed frames, not
 * bare bytes. See the `wirelink` namespace below for the full rationale and
 * format - short version: each (selector, value) pair is wrapped in a
 * 4-byte frame with a start byte and checksum, so the receiver can detect
 * corruption and resynchronize on its own after a dropped byte, a noisy
 * wire, or the cable being unplugged and replugged - no handshake needed.
 * ---------------------------------------------------------------------
 */

#include <Arduino.h>
#include <WiFi.h>
#include <NetworkUdp.h>

// ---------------------------------------------------------------------
// WiFi credentials - THIS board's own light's network.
// ---------------------------------------------------------------------
const char *networkName = "COMBAT_LED2";
const char *networkPswd = "gvm_admin";

// The bulb listens for UDP broadcasts on this port. The broadcast address
// matches the subnet seen in the original captures (192.168.4.255) -
// change this if your bulb/router uses a different subnet.
const char *bulbBroadcastAddress = "192.168.4.255";
const uint16_t bulbPort = 2525;

// ---------------------------------------------------------------------
// Serial link to the master board (UART2, NOT the USB debug Serial).
// ---------------------------------------------------------------------
// ESP32 UART hardware supports up to 5,000,000 baud. 3,000,000 is used
// here as a safer default over breadboard/jumper wiring; bump it to
// 5000000 if your wiring is short and clean. For 4-byte frames sent
// occasionally, baud rate barely matters for throughput - this is mostly
// about minimizing latency, and either value is already far faster than
// the WiFi hop that follows it.
const unsigned long linkBaud = 3000000;
const int linkRxPin = 16; // GPIO16 = RX2
const int linkTxPin = 17; // GPIO17 = TX2
HardwareSerial SerialLink(2);

boolean connected = false;
NetworkUDP udp;

// ---------------------------------------------------------------------
// LED protocol (light-facing UDP packet, unchanged)
// ---------------------------------------------------------------------
namespace led {

constexpr uint8_t HEADER[7] = {0x4C, 0x54, 0x09, 0x00, 0x30, 0x57, 0x00};
constexpr uint8_t FIXED_MID = 0x01; // byte 8, constant in every capture so far

enum class Type : uint8_t {
  Power,
  Channel,
  Hue,
  Brightness,
  Cct,
  Saturation,
};

// CRC-16/CCITT-FALSE: poly 0x1021, init 0x0000, no input/output reflection,
// no final XOR.
inline uint16_t crc16CcittFalse(const uint8_t *data, size_t len) {
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < len; i++) {
    crc ^= ((uint16_t)data[i]) << 8;
    for (int b = 0; b < 8; b++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

// Build and send a frame directly from a raw (selector, value) pair - this
// is what the slave uses for everything it receives over the serial link,
// since the wire format only ever needs those two bytes regardless of
// what they semantically mean. Only called once the link layer below has
// already validated the pair, so no further range-checking happens here.
inline bool sendRaw(NetworkUDP &sock, uint8_t selector, uint8_t value,
                     const char *addr = nullptr, uint16_t port = 0) {
  if (addr == nullptr) addr = bulbBroadcastAddress;
  if (port == 0) port = bulbPort;

  uint8_t data[10];
  memcpy(data, HEADER, 7);
  data[7] = selector;
  data[8] = FIXED_MID;
  data[9] = value;

  uint16_t crc = crc16CcittFalse(data, 10);
  uint8_t bytes[12];
  memcpy(bytes, data, 10);
  bytes[10] = (uint8_t)(crc >> 8);
  bytes[11] = (uint8_t)(crc & 0xFF);

  char buf[25];
  for (int i = 0; i < 12; i++) {
    sprintf(&buf[i * 2], "%02X", bytes[i]);
  }
  buf[24] = '\0';

  Serial.printf("relaying sel=0x%02X val=0x%02X -> %s\n", selector, value, buf);

  sock.beginPacket(addr, port);
  sock.write((const uint8_t *)buf, 24);
  return sock.endPacket() == 1;
}

} // namespace led

// ---------------------------------------------------------------------
// Wired link protocol
//
// Frame format (4 bytes on the wire):
//   [0] START      constant 0xA5
//   [1] SELECTOR
//   [2] VALUE
//   [3] CHECKSUM   = START ^ SELECTOR ^ VALUE
//
// Why framed instead of bare (selector, value) pairs:
//  - A bare 2-byte stream has no way to recover if a byte is dropped,
//    corrupted, or injected (e.g. noise picked up while the wire is
//    unplugged/floating) - everything after that point desyncs
//    selector/value from then on, permanently. Framing with a start byte
//    + checksum lets the receiver detect corruption and resynchronize on
//    the next plausible frame instead of drifting forever.
//  - This also means "unplug and replug at any time" just works: there's
//    no connection handshake or state to reestablish between the boards.
//    The receiver is always scanning for the next valid frame, so as
//    soon as bytes start flowing again it picks back up on its own.
//  - A whitelist of valid (selector, value) combinations is checked
//    *after* the checksum passes, as a last line of defense: even a
//    corrupted frame that happens to checksum correctly (1-in-256 odds)
//    still can't reach the light if it doesn't match a control/range we
//    actually recognize.
// ---------------------------------------------------------------------
namespace wirelink {

constexpr uint8_t FRAME_START = 0xA5;
// If a partial frame sits half-received for longer than this, assume the
// rest is never coming (e.g. the cable was unplugged mid-byte) and
// discard it rather than misinterpreting later, unrelated bytes as its
// tail.
constexpr unsigned long FRAME_TIMEOUT_MS = 50;

enum class RxState : uint8_t { WAIT_START, WAIT_SEL, WAIT_VAL, WAIT_CHK };

RxState rxState = RxState::WAIT_START;
uint8_t rxSel = 0;
uint8_t rxVal = 0;
unsigned long lastByteMs = 0;

inline uint8_t checksum(uint8_t sel, uint8_t val) {
  return FRAME_START ^ sel ^ val;
}

// Known-good (selector, value) ranges. Anything outside these is refused
// rather than forwarded to the light.
inline bool isValidCommand(uint8_t sel, uint8_t val) {
  switch (sel) {
    case 0x00: return val <= 1;    // power: off/on
    case 0x01: return true;        // channel: not yet range-limited
    case 0x02: return val <= 100;  // brightness or saturation (%)
    case 0x03: return true;        // CCT raw step, full byte range valid
    case 0x04: return val <= 71;   // hue: 0..71 -> 0..355 degrees
    case 0x05: return val <= 100;  // saturation or brightness (%)
    default:   return false;       // unknown selector - reject outright
  }
}

// Feed one incoming byte into the frame parser. Returns true and fills
// selOut/valOut if a complete, checksum-valid, range-valid frame was
// just decoded.
inline bool feed(uint8_t b, uint8_t &selOut, uint8_t &valOut) {
  unsigned long now = millis();
  if (rxState != RxState::WAIT_START && (now - lastByteMs) > FRAME_TIMEOUT_MS) {
    rxState = RxState::WAIT_START; // stale partial frame - e.g. after unplug
  }
  lastByteMs = now;

  bool decoded = false;
  switch (rxState) {
    case RxState::WAIT_START:
      if (b == FRAME_START) rxState = RxState::WAIT_SEL;
      break;
    case RxState::WAIT_SEL:
      rxSel = b;
      rxState = RxState::WAIT_VAL;
      break;
    case RxState::WAIT_VAL:
      rxVal = b;
      rxState = RxState::WAIT_CHK;
      break;
    case RxState::WAIT_CHK: {
      if (b == checksum(rxSel, rxVal) && isValidCommand(rxSel, rxVal)) {
        selOut = rxSel;
        valOut = rxVal;
        decoded = true;
      } else {
        Serial.printf("link: dropped bad frame sel=0x%02X val=0x%02X chk=0x%02X\n",
                      rxSel, rxVal, b);
      }
      // If this checksum byte happens to equal FRAME_START, treat it as
      // the start of the next frame immediately instead of waiting for a
      // fresh start byte - keeps resync fast when frames are back-to-back.
      rxState = (b == FRAME_START) ? RxState::WAIT_SEL : RxState::WAIT_START;
      break;
    }
  }
  Serial.println("feed finished");
  return decoded;
}

} // namespace wirelink

// ---------------------------------------------------------------------
// Arduino sketch
// ---------------------------------------------------------------------
void connectToWiFi(const char *ssid, const char *pwd);
void WiFiEvent(WiFiEvent_t event);

void setup() {
  Serial.begin(115200);
  Serial.println("Starting slave");
  SerialLink.begin(linkBaud, SERIAL_8N1, linkRxPin, linkTxPin);
  // Bias the RX line high (matches the UART idle state) so an unplugged
  // or floating serial cable doesn't get read as a stream of spurious
  // start bits. If you still see junk on long/noisy wiring, add a
  // physical pull-up resistor (e.g. 10k to 3.3V) right at this pin too.
  connectToWiFi(networkName, networkPswd);
}

void loop() {
  // Always drain and parse the link, even before WiFi is connected -
  // this keeps the frame parser in sync and avoids the RX buffer filling
  // up while we wait on WiFi.
  while (SerialLink.available()) {
    uint8_t b = (uint8_t)SerialLink.read();
    uint8_t sel, val;
    if (wirelink::feed(b, sel, val)) {
      if (connected) {
        led::sendRaw(udp, sel, val);
      } else {
        Serial.println("not connected");
      }
    }
  }
}

void connectToWiFi(const char *ssid, const char *pwd) {
  Serial.println("Connecting to WiFi network: " + String(ssid));

  WiFi.disconnect(true);
  WiFi.onEvent(WiFiEvent); // called from a separate FreeRTOS task
  WiFi.begin(ssid, pwd);

  Serial.println("Waiting for WIFI connection...");
}

void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("WiFi connected! IP address: ");
      Serial.println(WiFi.localIP());
      udp.begin(WiFi.localIP(), bulbPort);
      led::sendRaw(udp, 0, 1);
      connected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi lost connection");
      connected = false;
      break;
    default:
      break;
  }
}
