#include "WifiSetup.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>


#include "config.h"

namespace {
const IPAddress kApIp(192, 168, 4, 1);
constexpr uint32_t kTryTimeoutMs = 40000;  // how long a newly entered network gets to connect
constexpr uint32_t kNudgeMs = 3000;  // join not started by now: retry the connection once
constexpr uint32_t kLingerMs = 90000;      // setup network stays up this long after a join so the phone can read the result

// Disconnect reason codes from the ESP32 Wi-Fi stack that tell us why a join failed.
constexpr int kReasonAuthExpire = 2;
constexpr int kReasonAssocLeave = 8;      // we hung up ourselves
constexpr int kReason4wayTimeout = 15;
constexpr int kReasonLeavingBss = 36;     // we hung up ourselves
constexpr int kReasonNoApFound = 201;
constexpr int kReasonAuthFail = 202;
constexpr int kReasonAssocFail = 203;
constexpr int kReasonHandshakeTimeout = 204;
constexpr int kReasonConnectionFail = 205;

const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RTD Logger setup</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--ink:#1b1f24;--mute:#5b6570;--line:#d9dee4;--accent:#0b6bcb;--bad:#b3261e;--ok:#1b7a3a}
@media(prefers-color-scheme:dark){:root{--bg:#14171a;--card:#1d2125;--ink:#e8ebee;--mute:#9aa4af;--line:#323941;--accent:#5aa7ff;--bad:#ff8a80;--ok:#6fd98c}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,sans-serif}
main{max-width:30rem;margin:0 auto;padding:1.5rem 16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:1.25rem}
h1{font-size:1.35rem;margin:0 0 .25rem}
.sub{color:var(--mute);margin:0 0 1.25rem;font-size:.95rem}
label{display:block;font-weight:600;margin:1rem 0 .35rem;font-size:.95rem}
input[type=text],input[type=password]{width:100%;padding:.7rem .8rem;font:inherit;color:var(--ink);background:var(--bg);border:1px solid var(--line);border-radius:8px}
input:focus{outline:2px solid var(--accent);outline-offset:1px}
.list{margin-top:.5rem;border:1px solid var(--line);border-radius:8px;max-height:11rem;overflow:auto}
.list button{display:flex;justify-content:space-between;width:100%;padding:.65rem .8rem;font:inherit;color:var(--ink);background:none;border:0;border-bottom:1px solid var(--line);text-align:left;cursor:pointer}
.list button:last-child{border-bottom:0}
.list button:hover{background:var(--bg)}
.list .note{padding:.65rem .8rem;color:var(--mute)}
.sig{color:var(--mute);font-size:.85rem}
.row{display:flex;justify-content:space-between;align-items:center;margin-top:.5rem}
.show{display:flex;gap:.4rem;align-items:center;font-weight:400;margin:0}
.link{background:none;border:0;color:var(--accent);font:inherit;cursor:pointer;padding:0}
#go{width:100%;margin-top:1.25rem;padding:.8rem;font:inherit;font-weight:600;color:#fff;background:var(--accent);border:0;border-radius:8px;cursor:pointer}
#go:disabled{opacity:.55;cursor:default}
#status{display:none;margin-top:1.25rem;border:1px solid var(--line);border-radius:8px;padding:.9rem 1rem}
#status h2{font-size:1rem;margin:0 0 .35rem}
#status p{margin:.35rem 0 0;color:var(--mute);font-size:.95rem}
#status.working h2::before{content:"";display:inline-block;width:.9em;height:.9em;margin-right:.5em;border:2px solid var(--line);border-top-color:var(--accent);border-radius:50%;animation:spin 1s linear infinite;vertical-align:-.1em}
@keyframes spin{to{transform:rotate(360deg)}}
#status.ok{border-color:var(--ok)}#status.ok h2{color:var(--ok)}
#status.fail{border-color:var(--bad)}#status.fail h2{color:var(--bad)}
#status a{display:inline-block;margin-top:.6rem;color:var(--accent);font-weight:600;word-break:break-all}
</style></head><body><main><div class="card">
<h1>Connect your logger to Wi-Fi</h1>
<p class="sub">Device __NAME__. It only works on 2.4&nbsp;GHz networks.</p>
<form id="f">
<label for="ssid">Wi-Fi network</label>
<input type="text" id="ssid" maxlength="32" required autocomplete="off" autocapitalize="off" placeholder="Pick one below or type a name">
<div class="list" id="list"><div class="note">Scanning&hellip;</div></div>
<div class="row"><button type="button" class="link" id="rescan">Scan again</button></div>
<label for="pass">Password</label>
<input type="password" id="pass" maxlength="63" autocomplete="off" placeholder="Leave empty for an open network">
<div class="row"><label class="show"><input type="checkbox" id="show"> Show password</label></div>
<button id="go" type="submit">Connect</button>
</form>
<div id="status" role="status"><h2 id="st"></h2><p id="sd"></p><a id="dash" hidden></a></div>
</div></main>
<script>
const $=id=>document.getElementById(id);
function show(cls,title,detail){const s=$('status');s.className=cls;s.style.display='block';$('st').textContent=title;$('sd').textContent=detail||''}
function bars(r){return r>-55?'strong':r>-70?'good':'weak'}
async function scan(){
  $('list').innerHTML='<div class="note">Scanning&hellip;</div>';
  for(let i=0;i<10;i++){
    try{
      const d=await (await fetch('/scan')).json();
      if(d.networks){
        const l=$('list');l.textContent='';
        if(!d.networks.length){l.innerHTML='<div class="note">No networks found. Type the name above.</div>';return}
        d.networks.forEach(n=>{
          const b=document.createElement('button');b.type='button';
          const a=document.createElement('span');a.textContent=n.ssid+(n.open?'':' \u{1F512}');
          const s=document.createElement('span');s.className='sig';s.textContent=bars(n.rssi);
          b.append(a,s);b.onclick=()=>{$('ssid').value=n.ssid;$('pass').focus()};l.append(b)});
        return}
    }catch(e){}
    await new Promise(r=>setTimeout(r,1500));
  }
  $('list').innerHTML='<div class="note">Scan did not finish. Type the name above.</div>';
}
// The phone can lose the setup network for a few seconds while the logger joins the router, so a failed poll
// is not an error: keep asking until the logger answers or about a minute has passed.
async function watch(name){
  const t0=Date.now();
  while(Date.now()-t0<70000){
    await new Promise(r=>setTimeout(r,1000));
    let d;
    try{d=await (await fetch('/status',{cache:'no-store'})).json()}catch(e){
      show('working','Connecting to '+name+'…','Your phone may disconnect from the setup network for a few seconds. Stay on it, the result will appear here.');continue}
    if(d.state==='ok'){
      show('ok','Connected to '+name,'The logger is online and sending data. Switch your phone back to your normal Wi-Fi, then open the dashboard (it asks for the dashboard login):');
      const a=$('dash');a.hidden=false;a.href=d.dashboard;a.textContent=d.dashboard;$('f').style.display='none';return}
    if(d.state==='failed'){
      show('fail','Could not connect to '+name,d.error);$('go').disabled=false;return}
    show('working',d.stage==='address'?'Joined '+name+', getting an address…':'Connecting to '+name+'…','Checking the name and password with the router. This takes up to 40 seconds.');
  }
  show('fail','No answer from the logger','Reconnect your phone to the RTD-Logger setup network and check this page again, or try again.');$('go').disabled=false;
}
$('f').onsubmit=async e=>{
  e.preventDefault();$('go').disabled=true;
  const name=$('ssid').value;
  show('working','Sending details…','');
  try{await fetch('/connect',{method:'POST',body:new URLSearchParams({ssid:name,pass:$('pass').value})})}catch(err){}
  watch(name);
};
$('rescan').onclick=scan;
$('show').onchange=e=>{$('pass').type=e.target.checked?'text':'password'};
scan();
</script></body></html>)HTML";

enum class Try { Idle, Connecting, Ok, Failed };

Preferences prefs;  // namespace "wifi": ssid / pass
WebServer server(80);
DNSServer dns;

String savedSsid, savedPass;
String apName;
bool portalOn = false;

Try tryState = Try::Idle;
String trySsid, tryPass, tryError;
uint32_t tryStartMs = 0;
bool tryNudged = false;
uint32_t okAtMs = 0;

// Set from the Wi-Fi event task, read from loop().
volatile int gReason = 0;     // last disconnect reason that was not our own hang-up
volatile bool gAssoc = false;  // joined the router's radio network
volatile bool gGotIp = false;  // and was given an address

uint32_t lastAttemptMs = 0;
uint32_t disconnectedSinceMs = 0;
uint32_t buttonDownMs = 0;

void beginWifi(const String& ssid, const String& pass) {
  WiFi.begin(ssid.c_str(), pass.isEmpty() ? nullptr : pass.c_str());
}

String jsonEscape(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

void sendJson(const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleRoot() {
  String page = FPSTR(kPage);
  page.replace("__NAME__", apName);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", page);
}

// Starts a scan on the first call and answers {"scanning":true} until it finishes; the next call after
// that returns the list and clears it, so "Scan again" always starts a fresh scan.
void handleScan() {
  if (tryState == Try::Connecting) {  // never scan while a join is in progress
    sendJson("{\"scanning\":true}");
    return;
  }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true);
    sendJson("{\"scanning\":true}");
    return;
  }
  if (n == WIFI_SCAN_RUNNING) {
    sendJson("{\"scanning\":true}");
    return;
  }
  String out = "{\"networks\":[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;  // hidden network: the user types it
    bool seen = false;
    for (int j = 0; j < i && !seen; j++) seen = (WiFi.SSID(j) == ssid);  // list is sorted by signal: keep the strongest
    if (seen) continue;
    if (!first) out += ',';
    first = false;
    out += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
           ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  out += "]}";
  WiFi.scanDelete();
  sendJson(out);
}

void startTry(const String& ssid, const String& pass) {
  trySsid = ssid;
  tryPass = pass;
  tryError = "";
  tryStartMs = millis();
  tryState = Try::Connecting;
  gReason = 0;
  gAssoc = false;
  gGotIp = false;
  Serial.printf("wifi setup: trying \"%s\"\n", trySsid.c_str());
  if (WiFi.status() == WL_CONNECTED) WiFi.disconnect();
  // With the setup network up, a join that has to hunt for the router across all channels can take 40 s or
  // more. Finding its channel with one quick scan first lets the join go straight there.
  int channel = 0, bestRssi = -1000;
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n; i++)
    if (WiFi.SSID(i) == trySsid && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      channel = WiFi.channel(i);
    }
  WiFi.scanDelete();
  Serial.printf("wifi setup: %s\n", channel ? ("network seen on channel " + String(channel)).c_str() : "network not seen in the scan, trying anyway");
  tryStartMs = millis();
  tryNudged = false;
  WiFi.begin(trySsid.c_str(), tryPass.isEmpty() ? nullptr : tryPass.c_str(), channel);
}

void handleConnect() {
  String ssid = server.arg("ssid");
  if (ssid.isEmpty() || ssid.length() > 32) {
    server.send(400, "application/json", "{\"error\":\"bad ssid\"}");
    return;
  }
  String pass = server.arg("pass");
  sendJson("{\"ok\":true}");  // answer first: the scan inside startTry takes a few seconds
  startTry(ssid, pass);
}

void handleStatus() {
  const char* state = tryState == Try::Connecting ? "connecting" : tryState == Try::Ok ? "ok" : tryState == Try::Failed ? "failed" : "idle";
  String out = String("{\"state\":\"") + state + "\",\"stage\":\"" + (gAssoc ? "address" : "join") + "\",\"error\":\"" +
               jsonEscape(tryError) + "\",\"dashboard\":\"" + (tryState == Try::Ok ? jsonEscape(DASHBOARD_URL) : String("")) + "\"}";
  sendJson(out);
}

// Phones probe a known address to detect a captive portal; sending every unknown URL to the setup page makes
// the "sign in to Wi-Fi" prompt open it by itself.
void handleRedirect() {
  server.sendHeader("Location", String("http://") + kApIp.toString() + "/", true);
  server.send(302, "text/plain", "");
}

void startPortal() {
  if (portalOn) return;
  WiFi.mode(WIFI_AP_STA);  // station side stays up so the entered network can be tried and the saved one retried
  WiFi.softAPConfig(kApIp, kApIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName.c_str(), WIFI_SETUP_PASSWORD);
  dns.start(53, "*", kApIp);
  server.begin();
  portalOn = true;
  tryState = Try::Idle;
  Serial.println("wifi setup: no usable network, setup network is open");
  Serial.printf("  join Wi-Fi \"%s\" with password \"%s\", then open http://%s/ if the page does not appear\n", apName.c_str(),
                WIFI_SETUP_PASSWORD, kApIp.toString().c_str());
}

void stopPortal() {
  if (!portalOn) return;
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalOn = false;
  tryState = Try::Idle;
  Serial.println("wifi setup: setup network closed");
}

void eraseAndRestart() {
  prefs.clear();
  Serial.println("wifi setup: BOOT button held, saved Wi-Fi erased, restarting into setup mode");
  delay(300);
  ESP.restart();
}

void watchButton(uint32_t now) {
  if (digitalRead(PIN_WIFI_RESET) != LOW) {
    buttonDownMs = 0;
    return;
  }
  if (!buttonDownMs) buttonDownMs = now ? now : 1;
  if (now - buttonDownMs >= WIFI_RESET_HOLD_MS) eraseAndRestart();
}

const char* explain(int reason) {
  switch (reason) {
    case kReasonNoApFound: return "That network was not found. Check the name, and that it is a 2.4 GHz network in range of the logger.";
    case kReasonAuthFail:
    case kReason4wayTimeout:
    case kReasonHandshakeTimeout: return "Wrong password for that network.";
    case kReasonAssocFail:
    case kReasonConnectionFail: return "The router refused the connection. It may block new devices or be full.";
    default: return nullptr;
  }
}

void fail(const char* why) {
  tryState = Try::Failed;
  tryError = why;
  WiFi.disconnect();
  lastAttemptMs = 0;  // lets supervise() go back to the saved network, if there is one
  Serial.printf("wifi setup: could not join \"%s\": %s\n", trySsid.c_str(), why);
}

void runTry(uint32_t now) {
  if (tryState == Try::Connecting) {
    if (gGotIp && WiFi.status() == WL_CONNECTED && WiFi.SSID() == trySsid) {
      prefs.putString("ssid", trySsid);
      prefs.putString("pass", tryPass);
      savedSsid = trySsid;
      savedPass = tryPass;
      tryState = Try::Ok;
      okAtMs = now;
      Serial.printf("wifi setup: joined \"%s\" and saved it\n", trySsid.c_str());
    } else if (!tryNudged && now - tryStartMs >= kNudgeMs && !gAssoc && gReason == 0) {
      // With the setup network up, the first connect request can stall without ever reaching the router.
      // Hanging up once makes the radio's auto-reconnect issue a fresh request, which goes through.
      tryNudged = true;
      Serial.println("wifi setup: no response yet, retrying the connection");
      WiFi.disconnect();
    } else if (const char* why = explain(gReason)) {
      fail(why);
    } else if (now - tryStartMs >= kTryTimeoutMs) {
      fail(gAssoc ? "Joined the network but the router gave the logger no address. Its DHCP may be full or blocking new devices."
                  : gReason == kReasonAuthExpire ? "The router did not accept the password (authentication timed out). Check it and try again."
                                                 : "The router did not answer. Move the logger closer and try again.");
    }
  } else if (tryState == Try::Ok && now - okAtMs >= kLingerMs) {
    stopPortal();  // the phone has had time to read the result
  }
}

// Keeps a saved network connected, and opens the setup network if it stays unreachable for too long.
void supervise(uint32_t now) {
  if (tryState == Try::Connecting) return;
  if (WiFi.status() == WL_CONNECTED) {
    disconnectedSinceMs = now;
    if (portalOn && tryState != Try::Ok && !savedSsid.isEmpty()) stopPortal();  // the saved network came back
    return;
  }
  if (savedSsid.isEmpty()) return;  // first-time setup: nothing to retry
  if (now - lastAttemptMs >= WIFI_RETRY_MS) {
    lastAttemptMs = now;
    Serial.println("wifi: connecting");
    WiFi.disconnect();
    beginWifi(savedSsid, savedPass);
  }
  if (!portalOn && now - disconnectedSinceMs >= WIFI_FALLBACK_MS) startPortal();
}
}  // namespace

namespace WifiSetup {

void begin() {
  uint64_t mac = ESP.getEfuseMac();  // low byte is the first octet of the MAC
  char buf[24];
  snprintf(buf, sizeof(buf), "RTD-Logger-%02X%02X", (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  apName = buf;

  pinMode(PIN_WIFI_RESET, INPUT_PULLUP);

  prefs.begin("wifi", false);  // read-write so a never-used namespace is created instead of logging an error
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");

  WiFi.persistent(false);  // our own NVS entry is the only copy of the credentials
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
      gAssoc = true;
      Serial.printf("wifi: associated with %s (channel %d)\n", WiFi.SSID().c_str(), info.wifi_sta_connected.channel);
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      gGotIp = true;
      Serial.printf("wifi: connected to %s, ip %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      int r = info.wifi_sta_disconnected.reason;
      if (r != kReasonAssocLeave && r != kReasonLeavingBss) gReason = r;
      gAssoc = false;
      gGotIp = false;
      Serial.printf("wifi: not connected (reason %d%s)\n", r,
                    r == kReasonNoApFound ? ": network not seen, the ESP32 needs a 2.4 GHz network"
                    : r == kReasonAuthFail ? ": wrong password" : "");
    }
  });

  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/status", HTTP_GET, handleStatus);
  server.onNotFound(handleRedirect);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  uint32_t now = millis();
  lastAttemptMs = now;
  disconnectedSinceMs = now;
  if (savedSsid.isEmpty()) {
    startPortal();
  } else {
    Serial.printf("wifi: connecting to saved network \"%s\"\n", savedSsid.c_str());
    beginWifi(savedSsid, savedPass);
  }
}

void poll() {
  uint32_t now = millis();
  watchButton(now);
  if (portalOn) {
    dns.processNextRequest();
    server.handleClient();
  }
  runTry(now);
  supervise(now);
}

}  // namespace WifiSetup
