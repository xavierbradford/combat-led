// Arena web UI page (HTML/CSS/JS), split out into its own header on
// purpose: the Arduino IDE's automatic function-prototype generator
// only scans the .ino file, and its naive text scan gets confused by
// raw string literals that contain code-like text (e.g. the JS below
// has "(function () {" in it) if they live directly in the .ino -
// it can misparse that as real C++ and fail to compile. Keeping it in
// a .h file (a normal sketch tab) sidesteps that scanner entirely.
#pragma once

static const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>Arena</title>
<style>
  :root {
    --brand: #1fa29b;
    --red: #d64545;
    --blue: #3b6fd6;
    --gray: #6b7280;
  }
  * { box-sizing: border-box; }
  html, body {
    margin: 0; padding: 0;
    background: #ffffff;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    color: #1a1a1a;
  }
  .topbar {
    background: var(--brand);
    padding: 14px 16px;
    display: flex;
    justify-content: center;
    align-items: center;
  }
  .topbar svg { width: 100%; max-width: 480px; height: auto; display: block; }
  .topbar img { width: 100%; max-width: 400px; height: auto; display: block; }
  main { max-width: 480px; margin: 0 auto; padding: 20px 16px 40px; }
  .panel {
    border: 1px solid #e5e7eb;
    border-radius: 16px;
    padding: 24px 20px;
    text-align: center;
    box-shadow: 0 1px 3px rgba(0,0,0,0.06);
  }
  .phase-label {
    font-size: 14px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--gray);
    margin-bottom: 6px;
  }
  .phase-name { font-size: 32px; font-weight: 700; margin-bottom: 4px; }
  .winner-banner { font-size: 22px; font-weight: 700; margin: 12px 0 4px; }
  .winner-banner.red { color: var(--red); }
  .winner-banner.blue { color: var(--blue); }
  .countdown-number { font-size: 56px; font-weight: 800; margin: 8px 0; }
  .buttons { display: flex; flex-direction: column; gap: 12px; margin-top: 20px; }
  .buttons.row { flex-direction: row; }
  button {
    flex: 1;
    font-size: 18px;
    font-weight: 600;
    padding: 16px 12px;
    border: none;
    border-radius: 12px;
    color: white;
    background: var(--brand);
    -webkit-tap-highlight-color: transparent;
  }
  button:active { opacity: 0.85; }
  button:disabled { opacity: 0.5; }
  button.red { background: var(--red); }
  button.blue { background: var(--blue); }
  button.gray { background: var(--gray); }
  .conn-status { text-align: center; font-size: 12px; color: var(--gray); margin-top: 16px; }
  .conn-status.offline { color: var(--red); }
</style>
</head>
<body>
  <div class="topbar">
    <img src="data:image/webp;base64,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" alt="STEM Coliseum">
  </div>
  <main>
    <div class="panel">
      <div class="phase-label">Current Phase</div>
      <div class="phase-name" id="phaseName">-</div>
      <div id="extra"></div>
      <div class="buttons" id="buttons"></div>
    </div>
    <div class="conn-status" id="connStatus">Connecting&hellip;</div>
  </main>
<script>
(function () {
  var phaseNameEl = document.getElementById('phaseName');
  var extraEl = document.getElementById('extra');
  var buttonsEl = document.getElementById('buttons');
  var connStatusEl = document.getElementById('connStatus');

  var state = { phase: null, winner: 'none', version: 0, countdownRemainingMs: 0, matchRemainingMs: 0, serverTime: 0 };
  var pendingAction = false;
  var matchTimeEl = null;

  // Clock skew between the client's wall clock and the ESP32's millis()
  // clock, plus the network one-way latency to it. serverTime is captured
  // in toJson() server-side, so remaining = matchRemainingMs -
  // (estimatedServerNow - serverTime) keeps the countdown synced despite
  // message delay and drift.
  var serverOffsetMs = 0;
  var oneWayLatencyMs = 15;

  function formatMs(ms) {
    var totalSec = Math.max(0, Math.ceil(ms / 1000));
    var m = Math.floor(totalSec / 60);
    var s = totalSec % 60;
    return (m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + s;
  }

  function estimatedMatchRemainingMs() {
    if (state.phase !== 'Match') return 0;
    var serverNow = Date.now() + serverOffsetMs;
    var remaining = state.matchRemainingMs - (serverNow - state.serverTime);
    return Math.max(0, remaining);
  }

  function buttonDefs() {
    switch (state.phase) {
      case 'Cleanup':
        return [{ action: 'endCleanup', label: 'End Cleanup' }];
      case 'Ready':
        return [{ action: 'startMatch', label: 'Start Match' }];
      case 'Countdown':
        return [];
      case 'Match':
        return [
          { action: 'redWin', label: 'Red Win', cls: 'red' },
          { action: 'blueWin', label: 'Blue Win', cls: 'blue' },
          { action: 'judges', label: 'Judges', cls: 'gray' }
        ];
      case 'Judging':
        return [
          { action: 'redWin', label: 'Red Win', cls: 'red' },
          { action: 'blueWin', label: 'Blue Win', cls: 'blue' }
        ];
      case 'Announcement':
        return [{ action: 'newMatch', label: 'Begin Cleanup' }];
      default:
        return [];
    }
  }

  function render() {
    phaseNameEl.textContent = state.phase || '-';
    extraEl.innerHTML = '';
    buttonsEl.innerHTML = '';
    buttonsEl.className = 'buttons';

    if (state.phase === 'Countdown') {
      var secs = Math.max(0, Math.ceil(state.countdownRemainingMs / 1000));
      var div = document.createElement('div');
      div.className = 'countdown-number';
      div.textContent = secs;
      extraEl.appendChild(div);
    }

    if (state.phase === 'Match') {
      var label = document.createElement('div');
      label.className = 'phase-label';
      label.textContent = 'Time Remaining';
      extraEl.appendChild(label);

      matchTimeEl = document.createElement('div');
      matchTimeEl.className = 'countdown-number';
      matchTimeEl.textContent = formatMs(estimatedMatchRemainingMs());
      extraEl.appendChild(matchTimeEl);
    } else {
      matchTimeEl = null;
    }

    if (state.phase === 'Announcement' && state.winner !== 'none') {
      var banner = document.createElement('div');
      banner.className = 'winner-banner ' + state.winner;
      banner.textContent = (state.winner === 'red' ? 'RED' : 'BLUE') + ' WINS';
      extraEl.appendChild(banner);
    }

    var defs = buttonDefs();
    if (defs.length > 1) buttonsEl.className = 'buttons row';
    defs.forEach(function (d) {
      var btn = document.createElement('button');
      btn.textContent = d.label;
      if (d.cls) btn.className = d.cls;
      btn.disabled = pendingAction;
      btn.onclick = function () { sendAction(d.action); };
      buttonsEl.appendChild(btn);
    });
  }

  function applyState(s) {
    state.phase = s.phase;
    state.winner = s.winner;
    state.version = s.version;
    state.countdownRemainingMs = s.countdownRemainingMs || 0;
    if (typeof s.matchRemainingMs === 'number') state.matchRemainingMs = s.matchRemainingMs;
    if (typeof s.serverTime === 'number') {
      // serverTime is the ESP32's millis() when the snapshot was built.
      // Anchor the client clock to it: offset = serverTime - (receive
      // time), backed off by the measured one-way latency.
      state.serverTime = s.serverTime;
      serverOffsetMs = state.serverTime - (Date.now() - oneWayLatencyMs);
    }
    render();
  }

  function sendAction(action) {
    if (pendingAction) return;
    pendingAction = true;
    render();
    fetch('/api/action', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: action, version: state.version })
    }).then(function (res) {
      return res.json().then(function (body) { return body; });
    }).then(function (body) {
      // Whether accepted or rejected (409 = someone else already acted
      // first), the response body is always the authoritative current
      // state - apply it directly rather than waiting on the websocket.
      applyState(body);
    }).catch(function () {
      // network hiccup - websocket reconnect / poll fallback will resync
    }).finally(function () {
      pendingAction = false;
      render();
    });
  }

  var ws;
  var wsReconnectTimer;

  function connectWs() {
    var proto = location.protocol === 'https:' ? 'wss' : 'ws';
    ws = new WebSocket(proto + '://' + location.host + '/ws');
    ws.onopen = function () {
      connStatusEl.textContent = 'Connected';
      connStatusEl.className = 'conn-status';
    };
    ws.onmessage = function (evt) {
      try { applyState(JSON.parse(evt.data)); } catch (e) {}
    };
    ws.onclose = function () {
      connStatusEl.textContent = 'Reconnecting\u2026';
      connStatusEl.className = 'conn-status offline';
      clearTimeout(wsReconnectTimer);
      wsReconnectTimer = setTimeout(connectWs, 1000);
    };
    ws.onerror = function () { ws.close(); };
  }

  // Belt-and-suspenders fallback: some phones throttle background
  // network activity, which can leave a websocket silently stuck. Poll
  // the plain HTTP state endpoint periodically so a device always
  // self-corrects even if its socket died quietly. The round-trip time
  // is also measured here to keep the match countdown clock in sync.
  setInterval(function () {
    var t0 = Date.now();
    fetch('/api/state').then(function (r) { return r.json(); }).then(function (s) {
      var rtt = Date.now() - t0;
      if (rtt > 0) oneWayLatencyMs = rtt / 2;
      applyState(s);
    }).catch(function () {});
  }, 5000);

  // Smooth 1s match countdown between state pushes, anchored to the
  // last known server clock rather than only updating on message arrive.
  setInterval(function () {
    if (matchTimeEl) matchTimeEl.textContent = formatMs(estimatedMatchRemainingMs());
  }, 200);

  connectWs();
  render();
})();
</script>
</body>
</html>
)HTMLPAGE";
