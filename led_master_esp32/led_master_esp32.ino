/*
 * Dependencies:
 *  Benoit - ArduinoJson@6.21.6
 *  ESP32Async - AsyncTCP@3.5.0
 *  ESP32Async - ESP Async TCP@2.0.0
 *  ESP32Async - ESP Async WebServer@3.11.2
 *  IRremote by shirriff, z3t0, ArminJo v4.7.1
 *
 * MASTER ESP32 - connects to its own light's WiFi network, controls that
 * light directly (same protocol as the original app), forwards every
 * command over a wired UART link to the slave board (which relays it to
 * the second light), and hosts a mobile-first match-control web UI at
 * http://arena.local/.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <NetworkUdp.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <IRremote.hpp>
#include "arena_ui.h"

// ---------------------------------------------------------------------
// WiFi credentials
// ---------------------------------------------------------------------
const char *networkName = "COMBAT_LED";
const char *networkPswd = "gvm_admin";

const char *bulbBroadcastAddress = "192.168.4.255";
const uint16_t bulbPort = 2525;

// Static IP so the arena web UI is always reachable at the same address,
// regardless of what IP the bulb's AP (DHCP) hands out.
IPAddress arenaIp(192, 168, 4, 2);
IPAddress arenaGateway(192, 168, 4, 1);
IPAddress arenaSubnet(255, 255, 255, 0);
IPAddress arenaDns(192, 168, 4, 1);

// ---------------------------------------------------------------------
// Serial link to slave board
// ---------------------------------------------------------------------
const unsigned long linkBaud = 3000000;
const int linkRxPin = 16; // GPIO16 = RX2
const int linkTxPin = 17; // GPIO17 = TX2
HardwareSerial SerialLink(2);

boolean connected = false;
NetworkUDP udp;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ---------------------------------------------------------------------
// LED protocol
// ---------------------------------------------------------------------
namespace led {

constexpr uint8_t HEADER[7] = {0x4C, 0x54, 0x09, 0x00, 0x30, 0x57, 0x00};
constexpr uint8_t FIXED_MID = 0x01;

enum class Type : uint8_t {
  Power,
  Channel,
  Hue,
  Brightness,
  Cct,
  Saturation,
};

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

struct Command {
  Type type;
  uint8_t value;

  static Command power(bool on) {
    return Command{Type::Power, (uint8_t)(on ? 1 : 0)};
  }

  static Command channel(uint8_t ch) {
    return Command{Type::Channel, ch};
  }

  static Command hue(uint8_t raw) {
    return Command{Type::Hue, (uint8_t)min((uint32_t)raw, (uint32_t)71)};
  }

  static Command hueFromDegrees(uint16_t degrees) {
    uint16_t deg = degrees % 360;
    uint8_t raw = (uint8_t)min((uint32_t)((deg + 2) / 5), (uint32_t)71);
    return hue(raw);
  }

  static Command brightness(uint8_t percent) {
    return Command{Type::Brightness, (uint8_t)min((uint32_t)percent, (uint32_t)100)};
  }

  static Command cct(uint8_t raw) {
    return Command{Type::Cct, raw};
  }

  static Command cctFromKelvin(uint32_t kelvin) {
    uint32_t raw = (kelvin + 50) / 100;
    if (raw > 255) raw = 255;
    return cct((uint8_t)raw);
  }

  static Command saturation(uint8_t percent) {
    return Command{Type::Saturation, (uint8_t)min((uint32_t)percent, (uint32_t)100)};
  }

  uint8_t selector() const {
    switch (type) {
      case Type::Power:      return 0x00;
      case Type::Channel:    return 0x01;
      case Type::Brightness: return 0x02;
      case Type::Cct:        return 0x03;
      case Type::Hue:        return 0x04;
      case Type::Saturation: return 0x05;
    }
    return 0x00;
  }

  uint8_t valueByte() const {
    return value;
  }

  void dataBytes(uint8_t out[10]) const {
    memcpy(out, HEADER, 7);
    out[7] = selector();
    out[8] = FIXED_MID;
    out[9] = valueByte();
  }

  void toBytes(uint8_t out[12]) const {
    uint8_t data[10];
    dataBytes(data);
    uint16_t crc = crc16CcittFalse(data, 10);
    memcpy(out, data, 10);
    out[10] = (uint8_t)(crc >> 8);
    out[11] = (uint8_t)(crc & 0xFF);
  }

  String toHexString() const {
    uint8_t bytes[12];
    toBytes(bytes);
    char buf[25];
    for (int i = 0; i < 12; i++) {
      sprintf(&buf[i * 2], "%02X", bytes[i]);
    }
    buf[24] = '\0';
    return String(buf);
  }

  bool send(NetworkUDP &sock, const char *addr = nullptr, uint16_t port = 0) const {
    if (addr == nullptr) addr = bulbBroadcastAddress;
    if (port == 0) port = bulbPort;

    String hex = toHexString();
    sock.beginPacket(addr, port);
    sock.write((const uint8_t *)hex.c_str(), hex.length());
    return sock.endPacket() == 1;
  }
};

} // namespace led

// ---------------------------------------------------------------------
// Wired Link Protocol
// ---------------------------------------------------------------------
namespace wirelink {

constexpr uint8_t FRAME_START = 0xA5;

inline uint8_t checksum(uint8_t sel, uint8_t val) {
  return FRAME_START ^ sel ^ val;
}

inline void sendFrame(HardwareSerial &s, uint8_t sel, uint8_t val) {
  uint8_t frame[4] = {FRAME_START, sel, val, checksum(sel, val)};
  s.write(frame, 4);
}

} // namespace wirelink

void sendAndForward(const led::Command &cmd) {
  cmd.send(udp);
  wirelink::sendFrame(SerialLink, cmd.selector(), cmd.valueByte());
}

void send(const led::Command &cmd) {
  cmd.send(udp);
}

void forward(const led::Command &cmd) {
  wirelink::sendFrame(SerialLink, cmd.selector(), cmd.valueByte());
}

// White is produced in RGB mode via saturation(0) - the CCT command
// (selector 0x03) is ignored by these bulbs and leaves them in whatever
// colour they were showing, so it can't be used for white.
void setWhite(uint8_t brightness) {
  sendAndForward(led::Command::saturation(0));
  sendAndForward(led::Command::brightness(brightness));
}

// Defined here so display helpers below can use the enumerators.
namespace arena { enum class Phase : uint8_t { Cleanup, Ready, Countdown, Match, Judging, Announcement }; }

// ---------------------------------------------------------------------
// Fitness Timer Display (IR remote)
// ---------------------------------------------------------------------
namespace display {

// KY-005 S (Signal) pin. On ESP32 any GPIO works for IR output.
constexpr int IR_SEND_PIN = 4;
constexpr uint8_t REMOTE_ADDRESS = 0x01;

enum Button : uint8_t {
  BTN_POWER       = 0x0A,
  BTN_EDIT        = 0x0B,
  BTN_CLOCK       = 0x53,
  BTN_UP          = 0x52,
  BTN_DOWN        = 0x51,
  BTN_STOPWATCH   = 0x50,
  BTN_EMOM        = 0x1B,
  BTN_TABATA      = 0x4B,
  BTN_HIIT        = 0x54,
  BTN_FGB         = 0x4A,
  BTN_ALARM_UP    = 0x43,
  BTN_COLOR_DOWN  = 0x42,
  BTN_START       = 0x5E,
  BTN_RESET       = 0x5D,
  BTN_RESTART     = 0x5B,
  BTN_STOP        = 0x59,
  BTN_ENTER       = 0x5A,
  BTN_EXIT        = 0x41,
  BTN_MUTE        = 0x1A,
  BTN_PREPARATION = 0x57,
  BTN_12_24H      = 0x4F,
  BTN_NUM_0       = 0x03,
  BTN_NUM_1       = 0x0E,
  BTN_NUM_2       = 0x06,
  BTN_NUM_3       = 0x0F,
  BTN_NUM_4       = 0x12,
  BTN_NUM_5       = 0x07,
  BTN_NUM_6       = 0x13,
  BTN_NUM_7       = 0x16,
  BTN_NUM_8       = 0x02,
  BTN_NUM_9       = 0x17,
};

constexpr size_t IR_QUEUE_SIZE = 8;
constexpr uint16_t IR_GAP_MS = 200;

uint8_t irQueue[IR_QUEUE_SIZE];
size_t irQueueHead = 0;
size_t irQueueTail = 0;
unsigned long lastIrSendMs = 0;

bool displayPoweredOn = true;
int lastCountdownDigitSent = -1;

bool enqueue(Button button) {
  size_t next = (irQueueHead + 1) % IR_QUEUE_SIZE;
  if (next == irQueueTail) return false;
  irQueue[irQueueHead] = static_cast<uint8_t>(button);
  irQueueHead = next;
  return true;
}

bool dequeue(uint8_t &button) {
  if (irQueueHead == irQueueTail) return false;
  button = irQueue[irQueueTail];
  irQueueTail = (irQueueTail + 1) % IR_QUEUE_SIZE;
  return true;
}

void begin() {
  IrSender.begin(IR_SEND_PIN);
}

void processQueue() {
  unsigned long now = millis();
  if ((now - lastIrSendMs) < IR_GAP_MS) return;

  uint8_t button;
  if (dequeue(button)) {
    Serial.print("IR send 0x");
    Serial.println(button, HEX);
    IrSender.sendNEC(REMOTE_ADDRESS, button, 0);
    lastIrSendMs = now;
  }
}

void onPhaseEntered(arena::Phase newPhase, arena::Phase oldPhase) {
  if (newPhase == arena::Phase::Countdown) {
    lastCountdownDigitSent = -1;
  }

  switch (newPhase) {
    case arena::Phase::Ready:
      if (oldPhase == arena::Phase::Cleanup) {
        if (!displayPoweredOn) {
          enqueue(BTN_POWER);
          displayPoweredOn = true;
        }
        enqueue(BTN_STOPWATCH);
      }
      break;

    case arena::Phase::Match:
      if (oldPhase == arena::Phase::Countdown) {
        enqueue(BTN_DOWN);
        enqueue(BTN_START);
      }
      break;

    case arena::Phase::Judging:
      if (oldPhase == arena::Phase::Match && displayPoweredOn) {
        enqueue(BTN_POWER);
        displayPoweredOn = false;
      }
      break;

    case arena::Phase::Announcement:
      if (oldPhase == arena::Phase::Match && displayPoweredOn) {
        enqueue(BTN_POWER);
        displayPoweredOn = false;
      }
      break;

    default:
      break;
  }
}

void onCountdownTick(unsigned long remainingMs) {
  int secs = (int)((remainingMs + 999) / 1000); // ceil
  if (secs < 1 || secs > 3 || secs == lastCountdownDigitSent) return;

  Button digit;
  switch (secs) {
    case 3: digit = BTN_NUM_3; break;
    case 2: digit = BTN_NUM_2; break;
    case 1: digit = BTN_NUM_1; break;
    default: return;
  }

  enqueue(digit);
  lastCountdownDigitSent = secs;
}

} // namespace display

// ---------------------------------------------------------------------
// Arena State Machine
// ---------------------------------------------------------------------
namespace arena {

enum class Winner : uint8_t { None, Red, Blue };

struct MatchState {
  // Boot straight into Ready so a fresh event starts on the starting
  // phase; Cleanup is still entered after each match via "Begin Cleanup".
  Phase phase = Phase::Ready;
  Winner winner = Winner::None;
  uint32_t version = 0;
};

struct Snapshot {
  Phase phase;
  Winner winner;
  uint32_t version;
  unsigned long remainingMs;
  unsigned long matchRemainingMs;
};

const unsigned long COUNTDOWN_MS = 3000;
const unsigned long MATCH_MS = 120000; // 2 minutes

MatchState matchState;
unsigned long countdownStartedAt = 0;
unsigned long matchStartedAt = 0;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

const char *phaseName(Phase p) {
  switch (p) {
    case Phase::Cleanup:      return "Cleanup";
    case Phase::Ready:        return "Ready";
    case Phase::Countdown:    return "Countdown";
    case Phase::Match:        return "Match";
    case Phase::Judging:      return "Judging";
    case Phase::Announcement: return "Announcement";
  }
  return "Unknown";
}

const char *winnerName(Winner w) {
  switch (w) {
    case Winner::Red:  return "red";
    case Winner::Blue: return "blue";
    default:           return "none";
  }
}

unsigned long remainingCountdownMsLocked() {
  if (matchState.phase != Phase::Countdown) return 0;
  unsigned long elapsed = millis() - countdownStartedAt;
  return (elapsed >= COUNTDOWN_MS) ? 0 : (COUNTDOWN_MS - elapsed);
}

unsigned long remainingMatchMsLocked() {
  if (matchState.phase != Phase::Match) return 0;
  unsigned long elapsed = millis() - matchStartedAt;
  return (elapsed >= MATCH_MS) ? 0 : (MATCH_MS - elapsed);
}

Snapshot snapshot() {
  Snapshot s;
  portENTER_CRITICAL(&stateMux);
  s.phase = matchState.phase;
  s.winner = matchState.winner;
  s.version = matchState.version;
  s.remainingMs = remainingCountdownMsLocked();
  s.matchRemainingMs = remainingMatchMsLocked();
  portEXIT_CRITICAL(&stateMux);
  return s;
}

String toJson(const Snapshot &s) {
  StaticJsonDocument<256> doc;
  doc["phase"] = phaseName(s.phase);
  doc["winner"] = winnerName(s.winner);
  doc["version"] = s.version;
  doc["countdownRemainingMs"] = s.remainingMs;
  doc["matchRemainingMs"] = s.matchRemainingMs;
  doc["serverTime"] = millis();
  String out;
  serializeJson(doc, out);
  return out;
}

bool applyAction(const String &action, uint32_t expectedVersion, Snapshot &outSnap) {
  bool applied = false;
  portENTER_CRITICAL(&stateMux);
  if (matchState.version == expectedVersion) {
    switch (matchState.phase) {
      case Phase::Cleanup:
        if (action == "endCleanup") {
          matchState.phase = Phase::Ready;
          applied = true;
        }
        break;
      case Phase::Ready:
        if (action == "startMatch") {
          matchState.phase = Phase::Countdown;
          matchState.winner = Winner::None;
          countdownStartedAt = millis();
          applied = true;
        }
        break;
      case Phase::Countdown:
        break;
      case Phase::Match:
        if (action == "redWin") {
          matchState.phase = Phase::Announcement;
          matchState.winner = Winner::Red;
          applied = true;
        } else if (action == "blueWin") {
          matchState.phase = Phase::Announcement;
          matchState.winner = Winner::Blue;
          applied = true;
        } else if (action == "judges") {
          matchState.phase = Phase::Judging;
          applied = true;
        }
        break;
      case Phase::Judging:
        if (action == "redWin") {
          matchState.phase = Phase::Announcement;
          matchState.winner = Winner::Red;
          applied = true;
        } else if (action == "blueWin") {
          matchState.phase = Phase::Announcement;
          matchState.winner = Winner::Blue;
          applied = true;
        }
        break;
      case Phase::Announcement:
        if (action == "newMatch") {
          matchState.phase = Phase::Cleanup;
          matchState.winner = Winner::None;
          applied = true;
        }
        break;
    }
    if (applied) matchState.version++;
  }
    outSnap.phase = matchState.phase;
    outSnap.winner = matchState.winner;
    outSnap.version = matchState.version;
    outSnap.remainingMs = remainingCountdownMsLocked();
    outSnap.matchRemainingMs = remainingMatchMsLocked();
    portEXIT_CRITICAL(&stateMux);
    return applied;
}

bool tickCountdown() {
  bool changed = false;
  portENTER_CRITICAL(&stateMux);
  if (matchState.phase == Phase::Countdown &&
      (millis() - countdownStartedAt) >= COUNTDOWN_MS) {
    matchState.phase = Phase::Match;
    matchState.version++;
    matchStartedAt = millis();
    changed = true;
  }
  portEXIT_CRITICAL(&stateMux);
  return changed;
}

bool tickMatch() {
  bool changed = false;
  portENTER_CRITICAL(&stateMux);
  if (matchState.phase == Phase::Match &&
      (millis() - matchStartedAt) >= MATCH_MS) {
    matchState.phase = Phase::Judging;
    matchState.version++;
    changed = true;
  }
  portEXIT_CRITICAL(&stateMux);
  return changed;
}

} // namespace arena

void updateLightingForPhase(const arena::Snapshot &snap);

// ---------------------------------------------------------------------
// Arena Web Server & Websocket setup
// ---------------------------------------------------------------------
bool arenaServerStarted = false;

void startArenaServer() {
  if (arenaServerStarted) return;
  arenaServerStarted = true;

  if (!MDNS.begin("arena")) {
    Serial.println("mDNS init failed - use IP instead");
  } else {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS responder started: http://arena.local/");
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", arena::toJson(arena::snapshot()));
  });

  server.on(
      "/api/action", HTTP_POST,
      [](AsyncWebServerRequest *request) {},
      nullptr,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index == 0) {
          request->_tempObject = new String();
        }
        String *body = reinterpret_cast<String *>(request->_tempObject);
        body->concat((const char *)data, len);
        if (index + len != total) return;

        StaticJsonDocument<128> in;
        DeserializationError err = deserializeJson(in, *body);
        String action = err ? String("") : String((const char *)(in["action"] | ""));
        uint32_t expectedVersion = err ? 0 : (uint32_t)(in["version"] | 0);
        delete body;
        request->_tempObject = nullptr;

        arena::Snapshot snap;
        bool applied = arena::applyAction(action, expectedVersion, snap);
        String out = arena::toJson(snap);

        request->send(applied ? 200 : 409, "application/json", out);
        if (applied) {
          ws.textAll(out);
          updateLightingForPhase(snap);
        }
      });

  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
                void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      client->text(arena::toJson(arena::snapshot()));
    }
  });
  server.addHandler(&ws);

  server.begin();
  Serial.println("Arena web UI started");
}

// ---------------------------------------------------------------------
// Microphone Sound Detection
// ---------------------------------------------------------------------
// The sound sensor module's AO output is an audio waveform riding on a
// DC quiescent point. Loudness is measured as the peak-to-peak swing of
// the ADC samples over a rolling window - the same AMP value printed by
// the sound_sensor_test sketch. During a Match the lights sit at a
// default 50% white; any sound above the threshold snaps them to a
// full-brightness red/blue flash for at least FLASH_HOLD_MS.
namespace mic {

constexpr int AO_PIN = 34;                // ADC1_CH6, input-only, unused by master
constexpr unsigned long WINDOW_MS = 40;   // rolling loudness window
constexpr unsigned long SAMPLE_INTERVAL_MS = 2; // ~500 Hz sampling
constexpr int HIT_THRESHOLD = 60;         // amplitude that counts as a hit
constexpr unsigned long FLASH_HOLD_MS = 50; // minimum flash duration

// Default (quiet) look for the Match phase.
constexpr int DEFAULT_BRIGHTNESS = 50;

int minVal = 4095;
int maxVal = 0;
unsigned long lastSampleMs = 0;
unsigned long windowStartMs = 0;

void begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(AO_PIN, ADC_11db);
}

// Call once per loop() pass. Returns the loudness of the last completed
// sampling window, or -1 if no window has finished since the last call.
int readAmplitude() {
  unsigned long now = millis();
  if ((now - lastSampleMs) < SAMPLE_INTERVAL_MS) return -1;
  lastSampleMs = now;

  int v = analogRead(AO_PIN);
  if (v < minVal) minVal = v;
  if (v > maxVal) maxVal = v;

  if ((now - windowStartMs) < WINDOW_MS) return -1;

  int amplitude = maxVal - minVal;
  minVal = 4095;
  maxVal = 0;
  windowStartMs = now;
  return amplitude;
}

} // namespace mic

// ---------------------------------------------------------------------
// Non-Blocking Lighting State Controller
// ---------------------------------------------------------------------
enum class AnnounceStep { Alternating, Strobe, HoldWinner };

struct LightingState {
  arena::Phase lastPhase = arena::Phase::Cleanup;
  unsigned long lastUpdateMs = 0;
  
  // Countdown flash state (current brightness, 255 forces first send)
  uint8_t lastCountdownBrightness = 255;

  // Judging variables
  uint8_t lastJudgingBrightness = 0;
  uint8_t lastSlaveJudgingBrightness = 0;

  // Match: mic-driven red/blue flash state
  bool micFlashActive = false;
  unsigned long micLastHitMs = 0;

  // Announcement sequence tracking
  AnnounceStep announceStep = AnnounceStep::Alternating;
  int announceIndex = 0;
  arena::Winner winner = arena::Winner::None;
};

LightingState lightState;

void updateLightingForPhase(const arena::Snapshot &snap) {
  bool phaseChanged = (snap.phase != lightState.lastPhase);
  arena::Phase oldPhase = lightState.lastPhase;
  lightState.lastPhase = snap.phase;
  lightState.lastUpdateMs = millis();
  lightState.winner = snap.winner;

  if (phaseChanged) {
    display::onPhaseEntered(snap.phase, oldPhase);
  }

  sendAndForward(led::Command::power(true));

  switch (snap.phase) {
    case arena::Phase::Cleanup:
      // neutral white
      sendAndForward(led::Command::saturation(0));
      sendAndForward(led::Command::brightness(100));
      break;
    case arena::Phase::Ready:
      // Master = Red Corner (Hue 0°), Slave = Blue Corner (Hue 240°)
      send(led::Command::hueFromDegrees(0));
      forward(led::Command::hueFromDegrees(240));
      sendAndForward(led::Command::saturation(100));
      sendAndForward(led::Command::brightness(100));
      break;

    case arena::Phase::Countdown:
      // Red on Master (0°), Blue on Slave (240°)
      send(led::Command::hueFromDegrees(0));
      forward(led::Command::hueFromDegrees(240));
      sendAndForward(led::Command::saturation(100));
      
      // Instant flash to full at the first second boundary; the fade is
      // handled by processLightingAnimations().
      lightState.lastCountdownBrightness = 100;
      sendAndForward(led::Command::brightness(100));
      break;

    case arena::Phase::Match:
      // Default look: 50% white. processMicBrightness() snaps to a
      // red/blue 100% flash on loud sounds.
      lightState.micFlashActive = false;
      lightState.micLastHitMs = 0;
      setWhite(mic::DEFAULT_BRIGHTNESS);
      break;

    case arena::Phase::Judging:
      // Red on Master, Blue on Slave - breathing alternately during
      // judging (driven in processLightingAnimations()).
      send(led::Command::hueFromDegrees(0));
      forward(led::Command::hueFromDegrees(240));
      sendAndForward(led::Command::saturation(100));
      lightState.lastJudgingBrightness = 255;
      lightState.lastSlaveJudgingBrightness = 255;
      break;

    case arena::Phase::Announcement:
      lightState.announceStep = AnnounceStep::Alternating;
      lightState.announceIndex = 0;
      sendAndForward(led::Command::saturation(100));
      sendAndForward(led::Command::brightness(100));
      send(led::Command::hueFromDegrees(0));
      forward(led::Command::hueFromDegrees(240));
      break;
  }
}

void processLightingAnimations(unsigned long countdownRemainingMs) {
  unsigned long now = millis();

  if (lightState.lastPhase == arena::Phase::Countdown) {
    // At every countdown second boundary the light flashes to full
    // brightness, then fades linearly back to 0 over that second -
    // driven by the same clock as the countdown digits.
    if (countdownRemainingMs > 0) {
      unsigned long elapsed = arena::COUNTDOWN_MS - countdownRemainingMs;
      unsigned long elapsedInSecond = elapsed % 1000;
      uint8_t b = (uint8_t)((100 * (1000 - elapsedInSecond)) / 1000);
      if (abs((int)b - (int)lightState.lastCountdownBrightness) >= 3) {
        sendAndForward(led::Command::brightness(b));
        lightState.lastCountdownBrightness = b;
      }
    }
  }
  else if (lightState.lastPhase == arena::Phase::Judging) {
    // Both lights breathe 10%-100%, always on, opposite phase: when the
    // Master (red) is peaking the Slave (blue) is dimmest and vice versa.
    float sineVal = (sin((now / 1500.0) * M_PI) + 1.0) / 2.0; // 0..1
    uint8_t masterBrightness = 10 + (uint8_t)(sineVal * 90.0);
    uint8_t slaveBrightness = 10 + (uint8_t)((1.0 - sineVal) * 90.0);

    if (abs((int)masterBrightness - (int)lightState.lastJudgingBrightness) >= 3) {
      send(led::Command::brightness(masterBrightness));
      lightState.lastJudgingBrightness = masterBrightness;
    }
    if (abs((int)slaveBrightness - (int)lightState.lastSlaveJudgingBrightness) >= 3) {
      forward(led::Command::brightness(slaveBrightness));
      lightState.lastSlaveJudgingBrightness = slaveBrightness;
    }
  } 
  else if (lightState.lastPhase == arena::Phase::Announcement) {
    unsigned long elapsed = now - lightState.lastUpdateMs;

    switch (lightState.announceStep) {
      case AnnounceStep::Alternating: {
        int step = lightState.announceIndex;
        if (step >= 16) {
          lightState.announceStep = AnnounceStep::Strobe;
          lightState.announceIndex = 0;
          lightState.lastUpdateMs = now;
          sendAndForward(led::Command::saturation(0));
          break;
        }

        unsigned long stepDelay = 300 - 25 * (step / 2);
        if (elapsed >= stepDelay) {
          lightState.announceIndex++;
          lightState.lastUpdateMs = now;

          if (lightState.announceIndex % 2 == 0) {
            send(led::Command::hueFromDegrees(0));
            forward(led::Command::hueFromDegrees(240));
          } else {
            send(led::Command::hueFromDegrees(240));
            forward(led::Command::hueFromDegrees(0));
          }
        }
        break;
      }

      case AnnounceStep::Strobe: {
        if (lightState.announceIndex >= 24) {
          lightState.announceStep = AnnounceStep::HoldWinner;
          lightState.lastUpdateMs = now;

          sendAndForward(led::Command::saturation(100));
          sendAndForward(led::Command::brightness(100));

          if (lightState.winner == arena::Winner::Red) {
            sendAndForward(led::Command::hueFromDegrees(0));
          } else if (lightState.winner == arena::Winner::Blue) {
            sendAndForward(led::Command::hueFromDegrees(240));
          } else {
            sendAndForward(led::Command::saturation(0));
          }
          break;
        }

        if (elapsed >= 50) {
          lightState.announceIndex++;
          lightState.lastUpdateMs = now;

          if (lightState.announceIndex % 2 == 1) {
            send(led::Command::brightness(100));
            forward(led::Command::brightness(0));
          } else {
            send(led::Command::brightness(0));
            forward(led::Command::brightness(100));
          }
        }
        break;
      }

      case AnnounceStep::HoldWinner:
        break;
    }
  }
}

// Drives the Match-phase lights from the mic: quiet => default 50% white
// at 4500K; any hit snaps to a full-brightness red/blue flash held for at
// least FLASH_HOLD_MS (retriggered by sustained sound, no gradient).
void processMicBrightness() {
  if (lightState.lastPhase != arena::Phase::Match) return;

  int amplitude = mic::readAmplitude();
  if (amplitude < 0) return; // no completed sampling window yet

  unsigned long now = millis();
  bool hit = (amplitude >= mic::HIT_THRESHOLD);

  if (hit) {
    bool wasActive = lightState.micFlashActive;
    lightState.micFlashActive = true;
    lightState.micLastHitMs = now;
    if (!wasActive) {
      // Rising edge: snap on. Red on Master, Blue on Slave.
      send(led::Command::hueFromDegrees(0));
      forward(led::Command::hueFromDegrees(240));
      sendAndForward(led::Command::saturation(100));
      sendAndForward(led::Command::brightness(100));
    }
  } else if (lightState.micFlashActive &&
             (now - lightState.micLastHitMs) >= mic::FLASH_HOLD_MS) {
    // Sound stopped: back to the default white.
    lightState.micFlashActive = false;
    setWhite(mic::DEFAULT_BRIGHTNESS);
  }
}

// ---------------------------------------------------------------------
// Arduino Sketch Setup & Loop
// ---------------------------------------------------------------------
void connectToWiFi(const char *ssid, const char *pwd);
void WiFiEvent(WiFiEvent_t event);

unsigned long lastCountdownBroadcast = 0;
const unsigned long COUNTDOWN_TICK_MS = 200;

void setup() {
  Serial.begin(115200);
  SerialLink.begin(linkBaud, SERIAL_8N1, linkRxPin, linkTxPin);
  display::begin();
  mic::begin();
  connectToWiFi(networkName, networkPswd);
}

void loop() {
  if (!connected) return;

  bool changed = arena::tickCountdown();
  changed |= arena::tickMatch();

  arena::Snapshot snap = arena::snapshot();

  if (changed) {
    ws.textAll(arena::toJson(snap));
    updateLightingForPhase(snap);
    lastCountdownBroadcast = millis();
  } else {
    if (snap.phase == arena::Phase::Countdown &&
        (millis() - lastCountdownBroadcast) >= COUNTDOWN_TICK_MS) {
      lastCountdownBroadcast = millis();
      ws.textAll(arena::toJson(snap));
    }
  }

  if (snap.phase == arena::Phase::Countdown) {
    display::onCountdownTick(snap.remainingMs);
  }

  processLightingAnimations(snap.remainingMs);
  processMicBrightness();
  display::processQueue();
}

void connectToWiFi(const char *ssid, const char *pwd) {
  Serial.println("Connecting to WiFi network: " + String(ssid));

  WiFi.disconnect(true);
  WiFi.onEvent(WiFiEvent);
  WiFi.config(arenaIp, arenaGateway, arenaSubnet, arenaDns);
  WiFi.begin(ssid, pwd);

  Serial.println("Waiting for WIFI connection...");
}

void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("WiFi connected! IP address: ");
      Serial.println(WiFi.localIP());
      udp.begin(WiFi.localIP(), bulbPort);
      connected = true;
      startArenaServer();
      updateLightingForPhase(arena::snapshot());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi lost connection");
      connected = false;
      break;
    default:
      break;
  }
}