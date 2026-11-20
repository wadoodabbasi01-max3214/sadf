#include "Pch.h"
#include "LocalMenu.h"
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iphlpapi.h>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr unsigned kAllGroups = 0x07u;
std::atomic<unsigned> g_boxes{ kAllGroups };
std::atomic<unsigned> g_names{ 0u };
std::atomic<unsigned> g_roles{ kAllGroups };
std::atomic<unsigned> g_heldItems{ 0u };
std::atomic<unsigned> g_skeletons{ 0u };
std::atomic<bool> g_carBoxes{ false };
std::atomic<bool> g_carNames{ false };
std::atomic<bool> g_carDistance{ false };
std::atomic<bool> g_transparentBackground{ false };
std::atomic<bool> g_blackBackground{ true };
std::mutex g_aimMutex;
LocalMenu::AimSettings g_aimSettings{};
std::string g_aimStatus = "Makcu not connected";
std::atomic<bool> g_running{ false };
std::atomic<bool> g_exitRequested{ false };
SOCKET g_lanListener = INVALID_SOCKET;
SOCKET g_localListener = INVALID_SOCKET;
std::thread g_server;
std::string g_url;

unsigned GroupBit(LocalMenu::Group group) {
    const int index = static_cast<int>(group);
    return index >= 0 && index < 3 ? (1u << index) : 0u;
}

void SetMask(std::atomic<unsigned>& mask, unsigned bit, bool enabled) {
    if (enabled) mask.fetch_or(bit);
    else mask.fetch_and(~bit);
}

void SendAll(SOCKET socket, const std::string& value) {
    size_t sent = 0;
    while (sent < value.size()) {
        const int n = send(socket, value.data() + sent, static_cast<int>(value.size() - sent), 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

bool IsPrivateIPv4(ULONG address) {
    const ULONG ip = ntohl(address);
    return (ip >> 24) == 10 ||
        ((ip >> 20) == 0xAC1 && ((ip >> 16) & 0xFF) >= 16 && ((ip >> 16) & 0xFF) <= 31) ||
        (ip >> 16) == 0xC0A8;
}

bool FindLanAddress(sockaddr_in& result) {
    ULONG bytes = 16 * 1024;
    std::vector<BYTE> storage(bytes);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &bytes);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        storage.resize(bytes);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &bytes);
    }
    if (rc != NO_ERROR) return false;

    sockaddr_in fallback{};
    bool hasFallback = false;
    for (const bool preferred : { true, false }) {
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            const bool isPreferred = adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211;
            if (isPreferred != preferred) continue;
            for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next) {
                if (!item->Address.lpSockaddr || item->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* candidate = reinterpret_cast<const sockaddr_in*>(item->Address.lpSockaddr);
                if (!IsPrivateIPv4(candidate->sin_addr.s_addr)) continue;
                if (preferred) { result = *candidate; return true; }
                if (!hasFallback) { fallback = *candidate; hasFallback = true; }
            }
        }
        if (hasFallback) { result = fallback; return true; }
    }
    return false;
}

void Reply(SOCKET socket, int status, const char* type, const std::string& body) {
    const char* label = status == 200 ? "OK" : "Bad Request";
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + label +
        "\r\nContent-Type: " + type + "\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n";
    if (std::string(type).find("text/html") == 0) head += "Cache-Control: no-store\r\n";
    head += "Access-Control-Allow-Origin: http://127.0.0.1:17845\r\n\r\n";
    SendAll(socket, head + body);
}

const char kPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>RiggaRun</title><style>
:root{color-scheme:dark;font:12px/1.4 "Segoe UI",Arial,sans-serif;--bg:#0d0d13;--panel:#101017;--line:#34313e;--pink:#c94fe0;--pink2:#df70ed;--muted:#9a97a2;--text:#e9e7ee}*{box-sizing:border-box}button,select{font-family:inherit}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:12px;background:#08080b;color:var(--text)}.window{width:min(540px,100%);height:548px;display:flex;flex-direction:column;overflow:hidden;background:var(--bg);border:1px solid #514658;border-radius:6px;box-shadow:0 18px 55px #0009}.titlebar{height:35px;flex:none;padding:0 14px;display:flex;align-items:center;border-bottom:1px solid var(--pink);font-size:13px;font-weight:600}.brandmark{color:#b550e0;margin-right:8px;font-size:16px}.tabs{height:43px;flex:none;display:grid;grid-template-columns:repeat(5,1fr);background:#111117;border-bottom:1px solid #302d38}.tab{border:0;color:#a6a2ad;background:transparent;font:inherit;cursor:pointer;position:relative}.tab:hover{color:#fff;background:#1a1921}.tab.active{color:#f4eff8}.tab.active:after{content:"";height:2px;position:absolute;bottom:0;left:12%;right:12%;background:var(--pink2)}.tab-icon{color:#aaa4b4;margin-right:5px;font-size:13px}.tab.active .tab-icon{color:#bd5ce1}.content{flex:1;min-height:0;padding:11px 9px 8px}.page{display:none;height:100%}.page.active{display:block}.layout{height:100%;display:grid;grid-template-columns:1fr 1fr;gap:15px}.groupbox{height:100%;min-width:0;margin:0;padding:12px 9px;border:1px solid #3a3743;border-radius:4px;background:#0e0e14}.groupbox legend{padding:0 5px;color:#d9d5e0;font-size:12px}.select-group{width:100%;height:27px;margin-top:2px;padding:0 8px;color:#eee;appearance:none;background-color:#191820;background-image:linear-gradient(45deg,transparent 50%,#aaa 50%),linear-gradient(135deg,#aaa 50%,transparent 50%);background-position:calc(100% - 13px) 11px,calc(100% - 9px) 11px;background-size:4px 4px;background-repeat:no-repeat;border:1px solid #46414e;border-radius:2px;font:inherit}.hint{margin:11px 0;color:var(--muted);font-size:11px}.checkrow{display:flex;align-items:center;gap:9px;padding:7px 6px;margin:1px 0;border-radius:3px;cursor:pointer;color:#dfdce5;font-size:12px}.checkrow:hover{background:#1a1820}.checkrow input{appearance:none;width:13px;height:13px;flex:none;margin:0;border:1px solid #514d59;border-radius:2px;background:#17161c;display:grid;place-items:center}.checkrow input:checked{border-color:var(--pink);background:#b74acb}.checkrow input:checked:after{content:"";width:5px;height:5px;background:#f8ddff}.checkrow input:focus-visible,.tab:focus-visible,.closebtn:focus-visible,.select-group:focus-visible{outline:2px solid #ec9afa;outline-offset:2px}.status{display:flex;align-items:center;gap:7px;color:#8f8b98;font-size:10px;margin:14px 5px 0}.dot{width:6px;height:6px;border-radius:50%;background:#63d38f;box-shadow:0 0 7px #63d38f66}.settings{height:100%;padding:7px 5px}.settings h2{font-size:14px;font-weight:500;margin:2px 0 4px}.settings .intro{color:var(--muted);font-size:11px;margin:0 0 12px}.settingcard{padding:10px 11px;background:#101017;border:1px solid var(--line);border-radius:4px;margin:8px 0}.settingcard strong{display:block;font-size:12px;font-weight:500;margin-bottom:2px}.settingcard p{margin:0;color:var(--muted);font-size:10px}.settingcard .checkrow{padding:7px 2px 1px}.danger{display:flex;align-items:center;justify-content:space-between;gap:12px;border-color:#593445;background:#171118;margin-top:18px}.danger strong{color:#f0c5e9}.closebtn{border:1px solid #97507f;background:#281624;color:#f3dff0;padding:7px 10px;border-radius:3px;font:inherit;white-space:nowrap;cursor:pointer}.closebtn:hover{background:#46203c}.footer{height:24px;flex:none;border-top:1px solid #25232c;padding:5px 11px;color:#77737e;font-size:9px;display:flex;justify-content:space-between}.modal{display:none;position:fixed;inset:0;place-items:center;background:#000a;padding:20px}.modal.open{display:grid}.dialog{width:min(340px,100%);padding:18px;background:#15141b;border:1px solid #68435f;border-radius:5px;box-shadow:0 20px 70px #000b}.dialog h3{margin:0 0 7px;font-size:14px}.dialog p{color:#aaa5ad;margin:0 0 17px;font-size:11px}.actions{display:flex;justify-content:flex-end;gap:8px}.actions button{padding:7px 11px;border:1px solid #4a4850;border-radius:3px;background:#222127;color:white;font:inherit;cursor:pointer}.actions .confirm{background:#7b3049;border-color:#a64b67}@media(max-width:500px){body{padding:0}.window{width:100%;height:100vh;border-radius:0}.tabs{font-size:10px}.tab-icon{display:block;margin:0 0 1px}}
 .aim-layout{height:100%;overflow:auto;display:grid;grid-template-columns:1fr 1fr;align-content:start;gap:9px}.aim-panel{height:auto;min-height:0;padding:8px}.aim-panel .checkrow{padding:5px 4px;margin:0}.aim-slider{margin:8px 3px}.aim-slider label{display:flex;justify-content:space-between;gap:8px;margin-bottom:3px;color:#dfdce5}.aim-slider output{color:#c9a0d7}.aim-slider input{width:100%;accent-color:#c94fe0}.aim-select,.aim-port{height:27px;width:100%;padding:0 7px;color:#eee;background:#191820;border:1px solid #46414e;border-radius:2px;font:inherit}.aim-status{padding:7px 5px;color:#9a97a2;font-size:10px}.aim-disabled{opacity:.45;pointer-events:none}@media(max-width:500px){.aim-layout{grid-template-columns:1fr}}
.settings{overflow-y:auto}
</style></head><body>
<main class="window"><header class="titlebar"><span class="brandmark">ϟ</span>RiggaRun</header>
<nav class="tabs" role="tablist"><button class="tab" id="tab-aimbot" role="tab" aria-selected="false" onclick="showPage('aimbot')"><span class="tab-icon">⌖</span>Aimbot</button><button class="tab active" id="tab-visuals" role="tab" aria-selected="true" onclick="showPage('visuals')"><span class="tab-icon">◉</span>Visuals</button><button class="tab" id="tab-misc" role="tab" aria-selected="false" onclick="showPage('misc')"><span class="tab-icon">⛶</span>Misc</button><button class="tab" id="tab-world" role="tab" aria-selected="false" onclick="showPage('world')"><span class="tab-icon">☁</span>World</button><button class="tab" id="tab-settings" role="tab" aria-selected="false" onclick="showPage('settings')"><span class="tab-icon">⚙</span>Settings</button></nav>
<section class="content"><div class="page active" id="page-visuals"><div class="layout"><fieldset class="groupbox"><legend>Player Configuration</legend><select id="group" class="select-group" aria-label="Player group"><option value="civilian">Civilian</option><option value="robber">Robber</option><option value="cop">Cop</option></select></fieldset>
<fieldset class="groupbox"><legend>Visual Options</legend><p class="hint" id="group-title">Civilian</p><label class="checkrow"><input id="boxes" type="checkbox"><span>Box</span></label><label class="checkrow"><input id="skeleton" type="checkbox"><span>Skeleton</span></label><label class="checkrow"><input id="names" type="checkbox"><span>Username</span></label><label class="checkrow"><input id="role" type="checkbox"><span>Role</span></label><label class="checkrow"><input id="held" type="checkbox"><span>Held Item</span></label><div class="status"><span class="dot"></span><span id="connection">Connected</span></div></fieldset></div></div>
<div class="page" id="page-aimbot"><div class="aim-layout">
<fieldset class="groupbox aim-panel"><legend>Aim</legend>
<label class="checkrow"><input id="aim-enabled" type="checkbox"><span>Enable aim</span></label>
<label class="checkrow"><input id="show-fov" type="checkbox"><span>Show FOV</span></label>
<label class="checkrow"><input id="dynamic-fov" type="checkbox"><span>Dynamic FOV while ADS</span></label>
<div id="ads-fov-wrap" class="aim-slider"><label for="ads-fov"><span>ADS FOV</span><output id="ads-fov-value"></output></label><input id="ads-fov" type="range" min="1" max="60" step="1"></div>
<div class="aim-slider"><label for="fov"><span>FOV</span><output id="fov-value"></output></label><input id="fov" type="range" min="1" max="60" step="1"></div>
<label class="checkrow"><input id="show-deadzone" type="checkbox"><span>Show deadzone FOV</span></label>
<div class="aim-slider"><label for="deadzone"><span>Deadzone</span><output id="deadzone-value"></output></label><input id="deadzone" type="range" min="0" max="20" step="0.5"></div>
</fieldset>
<fieldset class="groupbox aim-panel"><legend>Target and input</legend>
<div class="hint">Aim key</div><select id="aim-key" class="aim-select"><option value="5">Mouse 4</option><option value="6">Mouse 5</option><option value="2">Right mouse</option><option value="164">Left Alt</option><option value="160">Left Shift</option><option value="17">Ctrl</option><option value="69">E</option><option value="67">C</option></select>
<div class="hint">Select target bones</div><label class="checkrow"><input class="aim-bone" data-bone="1" type="checkbox"><span>Head</span></label><label class="checkrow"><input class="aim-bone" data-bone="2" type="checkbox"><span>Neck</span></label><label class="checkrow"><input class="aim-bone" data-bone="4" type="checkbox"><span>Chest</span></label><label class="checkrow"><input class="aim-bone" data-bone="8" type="checkbox"><span>Pelvis</span></label>
<div class="aim-slider"><label for="smoothness"><span>Smoothness</span><output id="smoothness-value"></output></label><input id="smoothness" type="range" min="1" max="30" step="1"></div>
<label class="checkrow"><input id="ads-smooth-enabled" type="checkbox"><span>Separate ADS smoothness</span></label><div id="ads-smooth-wrap" class="aim-slider"><label for="ads-smooth"><span>ADS smoothness</span><output id="ads-smooth-value"></output></label><input id="ads-smooth" type="range" min="1" max="30" step="1"></div>
<div class="aim-slider"><label for="input-gain"><span>Input gain</span><output id="input-gain-value"></output></label><input id="input-gain" type="range" min="0.1" max="3" step="0.1"></div>
</fieldset></div></div><div class="page" id="page-misc"></div><div class="page" id="page-world"><div class="layout"><fieldset class="groupbox"><legend>World ESP</legend><select id="world-type" class="select-group" aria-label="World entity type"><option value="vehicle">Vehicle</option></select><div id="world-vehicle-options"><p class="hint">Vehicle overlays</p><label class="checkrow"><input id="car-boxes" type="checkbox"><span>3D Boxes</span></label><label class="checkrow"><input id="car-names" type="checkbox"><span>Name</span></label><label class="checkrow"><input id="car-distance" type="checkbox"><span>Distance</span></label></div></fieldset></div></div>
<div class="page" id="page-settings"><div class="settings"><h2>Settings</h2><p class="intro">Overlay appearance and input device</p><div class="settingcard"><strong>Makcu input</strong><p>RiggaRun will find the Makcu serial port automatically. Connect the device to this PC.</p><div class="checkrow" style="justify-content:space-between"><span id="aim-status">Makcu disconnected</span><button id="makcu-connect" class="closebtn" data-connected="false">Connect Makcu</button></div></div><div class="settingcard"><strong>Transparent background</strong><p>Show the game through the overlay background.</p><label class="checkrow"><input id="transparent" type="checkbox"><span>Enabled</span></label></div><div class="settingcard"><strong>Black background</strong><p>Use black when the background is opaque.</p><label class="checkrow"><input id="black" type="checkbox"><span>Enabled</span></label></div><div class="settingcard danger"><div><strong>Stop cheat</strong><p>Stops the overlay and control server.</p></div><button id="close-app" class="closebtn">Stop RiggaRun</button></div></div></div></section>
<footer class="footer"><span>RiggaRun · local control</span><span>Changes apply immediately</span></footer></main>
<div class="modal" id="modal" role="dialog" aria-modal="true" aria-labelledby="dialog-title"><div class="dialog"><h3 id="dialog-title">Close RiggaRun?</h3><p>This stops the overlay. You can start it again by relaunching the app.</p><div class="actions"><button id="cancel-close">Cancel</button><button class="confirm" id="confirm-close">Close application</button></div></div></div>
)HTML" R"HTML(<script>
const names={civilian:'Civilian',robber:'Robber',cop:'Cop'};let group='civilian';
function showPage(page){document.querySelectorAll('.tab').forEach(x=>{const on=x.id==='tab-'+page;x.classList.toggle('active',on);x.setAttribute('aria-selected',on)});document.querySelectorAll('.page').forEach(x=>x.classList.toggle('active',x.id==='page-'+page))}
async function refresh(){try{const r=await fetch('/api/state?group='+group,{cache:'no-store'});if(!r.ok)throw Error();const s=await r.json();document.querySelector('#boxes').checked=s.boxes;document.querySelector('#skeleton').checked=s.skeleton;document.querySelector('#names').checked=s.names;document.querySelector('#role').checked=s.role;document.querySelector('#held').checked=s.heldItem;document.querySelector('#car-boxes').checked=s.carBoxes;document.querySelector('#car-names').checked=s.carNames;document.querySelector('#car-distance').checked=s.carDistance;document.querySelector('#transparent').checked=s.transparent;document.querySelector('#black').checked=s.blackBackground;document.querySelector('#connection').textContent='Connected to RiggaRun'}catch{document.querySelector('#connection').textContent='Waiting for RiggaRun…'}}
async function setFeature(feature,enabled){const r=await fetch('/api/setting?group='+group+'&feature='+feature+'&enabled='+(enabled?'true':'false'),{method:'POST'});if(!r.ok)refresh()}
async function setAim(field,value){const q=encodeURIComponent(String(value));const r=await fetch('/api/aim/setting?field='+encodeURIComponent(field)+'&value='+q,{method:'POST'});if(!r.ok)refreshAim()}
function toggleAimExtras(){document.querySelector('#ads-fov-wrap').style.display=document.querySelector('#dynamic-fov').checked?'block':'none';document.querySelector('#ads-smooth-wrap').style.display=document.querySelector('#ads-smooth-enabled').checked?'block':'none'}
function setRange(id,value){const e=document.querySelector('#'+id);e.value=value;document.querySelector('#'+id+'-value').textContent=Number(value).toFixed(id==='input-gain'?1:(id==='deadzone'?1:0))}
async function refreshAim(){try{const r=await fetch('/api/aim/state',{cache:'no-store'});if(!r.ok)throw Error();const s=await r.json();document.querySelector('#aim-enabled').checked=s.enabled;document.querySelector('#show-fov').checked=s.showFov;document.querySelector('#show-deadzone').checked=s.showDeadzoneFov;document.querySelector('#dynamic-fov').checked=s.dynamicFov;document.querySelector('#ads-smooth-enabled').checked=s.adsSmoothnessEnabled;document.querySelector('#aim-key').value=s.aimKey;document.querySelectorAll('.aim-bone').forEach(e=>e.checked=(s.boneMask&Number(e.dataset.bone))!==0);setRange('smoothness',s.smoothness);setRange('fov',s.fov);setRange('deadzone',s.deadzone);setRange('ads-fov',s.adsFov);setRange('ads-smooth',s.adsSmoothness);setRange('input-gain',s.inputGain);document.querySelector('#aim-status').textContent=s.status;const c=document.querySelector('#makcu-connect');c.dataset.connected=s.makcuConnected?'true':'false';c.textContent=s.makcuConnected?'Disconnect Makcu':'Connect Makcu';toggleAimExtras()}catch{document.querySelector('#aim-status').textContent='Aim service unavailable'}}
document.querySelector('#group').addEventListener('change',e=>{group=e.target.value;document.querySelector('#group-title').textContent=names[group];refresh()});
document.querySelector('#boxes').addEventListener('change',e=>setFeature('boxes',e.target.checked));document.querySelector('#skeleton').addEventListener('change',e=>setFeature('skeleton',e.target.checked));document.querySelector('#names').addEventListener('change',e=>setFeature('names',e.target.checked));document.querySelector('#role').addEventListener('change',e=>setFeature('role',e.target.checked));document.querySelector('#held').addEventListener('change',e=>setFeature('held',e.target.checked));
document.querySelector('#car-boxes').addEventListener('change',e=>setFeature('carBoxes',e.target.checked));document.querySelector('#car-names').addEventListener('change',e=>setFeature('carNames',e.target.checked));document.querySelector('#car-distance').addEventListener('change',e=>setFeature('carDistance',e.target.checked));
document.querySelector('#transparent').addEventListener('change',e=>setFeature('transparent',e.target.checked));document.querySelector('#black').addEventListener('change',e=>setFeature('black',e.target.checked));
document.querySelector('#aim-enabled').addEventListener('change',e=>setAim('enabled',e.target.checked));document.querySelector('#show-fov').addEventListener('change',e=>setAim('showFov',e.target.checked));document.querySelector('#show-deadzone').addEventListener('change',e=>setAim('showDeadzoneFov',e.target.checked));document.querySelector('#dynamic-fov').addEventListener('change',e=>{setAim('dynamicFov',e.target.checked);toggleAimExtras()});document.querySelector('#ads-smooth-enabled').addEventListener('change',e=>{setAim('adsSmoothnessEnabled',e.target.checked);toggleAimExtras()});document.querySelector('#aim-key').addEventListener('change',e=>setAim('aimKey',e.target.value));document.querySelectorAll('.aim-bone').forEach(e=>e.addEventListener('change',()=>{let mask=0;document.querySelectorAll('.aim-bone:checked').forEach(x=>mask|=Number(x.dataset.bone));setAim('boneMask',mask)}));
 [ ['smoothness','smoothness'],['fov','fov'],['deadzone','deadzone'],['ads-fov','adsFov'],['ads-smooth','adsSmoothness'],['input-gain','inputGain']].forEach(([id,key])=>{const e=document.querySelector('#'+id);e.addEventListener('input',()=>document.querySelector('#'+id+'-value').textContent=Number(e.value).toFixed(id==='input-gain'||id==='deadzone'?1:0));e.addEventListener('change',()=>setAim(key,e.value))});document.querySelector('#makcu-connect').addEventListener('click',e=>{const next=e.currentTarget.dataset.connected!=='true';setAim('makcuConnected',next);e.currentTarget.dataset.connected=next?'true':'false';e.currentTarget.textContent=next?'Disconnect Makcu':'Connect Makcu'});
const modal=document.querySelector('#modal');document.querySelector('#close-app').onclick=()=>modal.classList.add('open');document.querySelector('#cancel-close').onclick=()=>modal.classList.remove('open');document.querySelector('#confirm-close').onclick=async()=>{await fetch('/api/exit',{method:'POST'});document.querySelector('#dialog-title').textContent='RiggaRun is closing';document.querySelector('#confirm-close').disabled=true};modal.addEventListener('click',e=>{if(e.target===modal)modal.classList.remove('open')});refresh();refreshAim();setInterval(refresh,2500);setInterval(refreshAim,1500);
</script></body></html>)HTML";

std::string QueryValue(const std::string& request, const char* key) {
    const size_t lineEnd = request.find("\r\n");
    const std::string line = request.substr(0, lineEnd);
    const std::string marker = std::string(key) + "=";
    size_t position = line.find(marker);
    if (position == std::string::npos) return {};
    position += marker.size();
    const size_t end = line.find_first_of("& ", position);
    return line.substr(position, end == std::string::npos ? end : end - position);
}

bool ParseGroup(const std::string& value, LocalMenu::Group& out) {
    if (value == "civilian") out = LocalMenu::Group::Civilian;
    else if (value == "robber") out = LocalMenu::Group::Robber;
    else if (value == "cop") out = LocalMenu::Group::Cop;
    else return false;
    return true;
}

void ServeClient(SOCKET client) {
    char buffer[4096]{};
    const int n = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) return;
    const std::string request(buffer, static_cast<size_t>(n));
    if (request.rfind("GET / HTTP/", 0) == 0) {
        Reply(client, 200, "text/html; charset=utf-8", kPage);
    } else if (request.rfind("GET /api/state", 0) == 0) {
        LocalMenu::Group group{};
        if (!ParseGroup(QueryValue(request, "group"), group)) group = LocalMenu::Group::Civilian;
        Reply(client, 200, "application/json", std::string("{\"boxes\":") + (LocalMenu::BoxesEnabled(group) ? "true" : "false") +
            ",\"names\":" + (LocalMenu::NamesEnabled(group) ? "true" : "false") +
            ",\"role\":" + (LocalMenu::RolesEnabled(group) ? "true" : "false") +
            ",\"skeleton\":" + (LocalMenu::SkeletonsEnabled(group) ? "true" : "false") +
            ",\"heldItem\":" + (LocalMenu::HeldItemEnabled(group) ? "true" : "false") +
            ",\"carBoxes\":" + (LocalMenu::CarBoxesEnabled() ? "true" : "false") +
            ",\"carNames\":" + (LocalMenu::CarNamesEnabled() ? "true" : "false") +
            ",\"carDistance\":" + (LocalMenu::CarDistanceEnabled() ? "true" : "false") +
            ",\"transparent\":" + (LocalMenu::TransparentBackground() ? "true" : "false") +
            ",\"blackBackground\":" + (LocalMenu::BlackBackground() ? "true}" : "false}"));
    } else if (request.rfind("GET /api/aim/state", 0) == 0) {
        const auto s = LocalMenu::GetAimSettings();
        Reply(client, 200, "application/json", std::string("{\"enabled\":") + (s.enabled ? "true" : "false") +
            ",\"showFov\":" + (s.showFov ? "true" : "false") +
            ",\"showDeadzoneFov\":" + (s.showDeadzoneFov ? "true" : "false") +
            ",\"dynamicFov\":" + (s.dynamicFov ? "true" : "false") +
            ",\"adsSmoothnessEnabled\":" + (s.adsSmoothnessEnabled ? "true" : "false") +
            ",\"makcuConnected\":" + (s.makcuConnected ? "true" : "false") +
            ",\"aimKey\":" + std::to_string(s.aimKey) +
            ",\"boneMask\":" + std::to_string(s.boneMask) +
            ",\"smoothness\":" + std::to_string(s.smoothness) +
            ",\"fov\":" + std::to_string(s.fov) +
            ",\"deadzone\":" + std::to_string(s.deadzone) +
            ",\"adsFov\":" + std::to_string(s.adsFov) +
            ",\"adsSmoothness\":" + std::to_string(s.adsSmoothness) +
            ",\"inputGain\":" + std::to_string(s.inputGain) +
            ",\"status\":\"" + LocalMenu::GetAimStatus() + "\"}");
    } else if (request.rfind("POST /api/aim/setting?", 0) == 0) {
        if (!LocalMenu::SetAimSetting(QueryValue(request, "field"), QueryValue(request, "value"))) {
            Reply(client, 400, "application/json", "{\"error\":\"invalid aim setting\"}");
            return;
        }
        Reply(client, 200, "application/json", "{\"ok\":true}");
    } else if (request.rfind("POST /api/setting?", 0) == 0) {
        LocalMenu::Group group = LocalMenu::Group::Civilian;
        const std::string feature = QueryValue(request, "feature");
        const std::string enabled = QueryValue(request, "enabled");
        const bool groupFeature = feature == "boxes" || feature == "names" || feature == "role" || feature == "held" || feature == "skeleton";
        if ((groupFeature && !ParseGroup(QueryValue(request, "group"), group)) ||
            (feature != "boxes" && feature != "names" && feature != "role" && feature != "held" && feature != "skeleton" && feature != "carBoxes" && feature != "carNames" && feature != "carDistance" && feature != "transparent" && feature != "black") ||
            (enabled != "true" && enabled != "false")) {
            Reply(client, 400, "application/json", "{\"error\":\"invalid setting\"}");
            return;
        }
        LocalMenu::SetFeature(group, feature, enabled == "true");
        Reply(client, 200, "application/json", "{\"ok\":true}");
    } else if (request.rfind("POST /api/exit", 0) == 0) {
        g_exitRequested.store(true);
        Reply(client, 200, "application/json", "{\"ok\":true}");
    } else {
        Reply(client, 400, "application/json", "{\"error\":\"not found\"}");
    }
}

void ServerLoop() {
    while (g_running.load()) {
        fd_set ready;
        FD_ZERO(&ready);
        if (g_lanListener != INVALID_SOCKET) FD_SET(g_lanListener, &ready);
        if (g_localListener != INVALID_SOCKET) FD_SET(g_localListener, &ready);
        timeval timeout{ 0, 500000 };
        const int selected = select(0, &ready, nullptr, nullptr, &timeout);
        if (!g_running.load()) break;
        if (selected == SOCKET_ERROR) continue;
        SOCKET listeners[] = { g_lanListener, g_localListener };
        for (SOCKET listener : listeners) {
            if (listener == INVALID_SOCKET || !FD_ISSET(listener, &ready)) continue;
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            ServeClient(client);
            shutdown(client, SD_SEND);
            closesocket(client);
        }
    }
}
}

bool LocalMenu::Start() {
    if (g_running.exchange(true)) return true;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { g_running.store(false); return false; }
    sockaddr_in lanAddress{};
    if (!FindLanAddress(lanAddress)) { WSACleanup(); g_running.store(false); return false; }
    lanAddress.sin_family = AF_INET;
    lanAddress.sin_port = htons(17845);
    g_lanListener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    g_localListener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_lanListener == INVALID_SOCKET || g_localListener == INVALID_SOCKET) {
        if (g_lanListener != INVALID_SOCKET) closesocket(g_lanListener);
        if (g_localListener != INVALID_SOCKET) closesocket(g_localListener);
        g_lanListener = g_localListener = INVALID_SOCKET;
        WSACleanup(); g_running.store(false); return false;
    }
    sockaddr_in localAddress{};
    localAddress.sin_family = AF_INET;
    localAddress.sin_port = htons(17845);
    localAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool lanReady = bind(g_lanListener, reinterpret_cast<sockaddr*>(&lanAddress), sizeof(lanAddress)) != SOCKET_ERROR && listen(g_lanListener, 8) != SOCKET_ERROR;
    const bool localReady = bind(g_localListener, reinterpret_cast<sockaddr*>(&localAddress), sizeof(localAddress)) != SOCKET_ERROR && listen(g_localListener, 8) != SOCKET_ERROR;
    if (!lanReady || !localReady) {
        closesocket(g_lanListener); closesocket(g_localListener);
        g_lanListener = g_localListener = INVALID_SOCKET;
        WSACleanup(); g_running.store(false); return false;
    }
    char ipText[INET_ADDRSTRLEN]{};
    if (!InetNtopA(AF_INET, &lanAddress.sin_addr, ipText, sizeof(ipText))) {
        closesocket(g_lanListener); closesocket(g_localListener);
        g_lanListener = g_localListener = INVALID_SOCKET;
        WSACleanup(); g_running.store(false); return false;
    }
    g_url = std::string("http://") + ipText + ":17845/";
    g_server = std::thread(ServerLoop);
    return true;
}

void LocalMenu::Stop() {
    if (!g_running.exchange(false)) return;
    if (g_lanListener != INVALID_SOCKET) { shutdown(g_lanListener, SD_BOTH); closesocket(g_lanListener); }
    if (g_localListener != INVALID_SOCKET) { shutdown(g_localListener, SD_BOTH); closesocket(g_localListener); }
    if (g_server.joinable()) g_server.join();
    g_lanListener = g_localListener = INVALID_SOCKET;
    WSACleanup();
}

bool LocalMenu::BoxesEnabled(Group group) { return (g_boxes.load() & GroupBit(group)) != 0; }
bool LocalMenu::NamesEnabled(Group group) { return (g_names.load() & GroupBit(group)) != 0; }
bool LocalMenu::RolesEnabled(Group group) { return (g_roles.load() & GroupBit(group)) != 0; }
bool LocalMenu::HeldItemEnabled(Group group) { return (g_heldItems.load() & GroupBit(group)) != 0; }
bool LocalMenu::SkeletonsEnabled(Group group) { return (g_skeletons.load() & GroupBit(group)) != 0; }
bool LocalMenu::CarBoxesEnabled() { return g_carBoxes.load(); }
bool LocalMenu::CarNamesEnabled() { return g_carNames.load(); }
bool LocalMenu::CarDistanceEnabled() { return g_carDistance.load(); }
bool LocalMenu::TransparentBackground() { return g_transparentBackground.load(); }
bool LocalMenu::BlackBackground() { return g_blackBackground.load(); }

void LocalMenu::SetFeature(Group group, const std::string& feature, bool enabled) {
    if (feature == "carBoxes") { g_carBoxes.store(enabled); return; }
    if (feature == "carNames") { g_carNames.store(enabled); return; }
    if (feature == "carDistance") { g_carDistance.store(enabled); return; }
    const unsigned bit = GroupBit(group);
    if (!bit) return;
    if (feature == "boxes") SetMask(g_boxes, bit, enabled);
    else if (feature == "names") SetMask(g_names, bit, enabled);
    else if (feature == "role") SetMask(g_roles, bit, enabled);
    else if (feature == "held") SetMask(g_heldItems, bit, enabled);
    else if (feature == "skeleton") SetMask(g_skeletons, bit, enabled);
    else if (feature == "transparent") g_transparentBackground.store(enabled);
    else if (feature == "black") g_blackBackground.store(enabled);
}

bool LocalMenu::ConsumeExitRequest() { return g_exitRequested.exchange(false); }
LocalMenu::AimSettings LocalMenu::GetAimSettings() { std::lock_guard<std::mutex> lock(g_aimMutex); return g_aimSettings; }
bool LocalMenu::SetAimSetting(const std::string& field, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_aimMutex);
    auto parseBool = [&]() { return value == "true" ? 1 : value == "false" ? 0 : -1; };
    auto parseFloat = [&](float min, float max, float& out) {
        try { size_t used = 0; float parsed = std::stof(value, &used); if (used != value.size() || !std::isfinite(parsed) || parsed < min || parsed > max) return false; out = parsed; return true; }
        catch (...) { return false; }
    };
    if (field == "enabled" || field == "showFov" || field == "showDeadzoneFov" || field == "dynamicFov" || field == "adsSmoothnessEnabled" || field == "makcuConnected") {
        const int parsed = parseBool(); if (parsed < 0) return false;
        const bool enabled = parsed != 0;
        if (field == "enabled") g_aimSettings.enabled = enabled;
        else if (field == "showFov") g_aimSettings.showFov = enabled;
        else if (field == "showDeadzoneFov") g_aimSettings.showDeadzoneFov = enabled;
        else if (field == "dynamicFov") g_aimSettings.dynamicFov = enabled;
        else if (field == "adsSmoothnessEnabled") g_aimSettings.adsSmoothnessEnabled = enabled;
        else g_aimSettings.makcuConnected = enabled;
        return true;
    }
    if (field == "smoothness") return parseFloat(1.f, 30.f, g_aimSettings.smoothness);
    if (field == "fov") return parseFloat(1.f, 60.f, g_aimSettings.fov);
    if (field == "deadzone") return parseFloat(0.f, 20.f, g_aimSettings.deadzone);
    if (field == "adsFov") return parseFloat(1.f, 60.f, g_aimSettings.adsFov);
    if (field == "adsSmoothness") return parseFloat(1.f, 30.f, g_aimSettings.adsSmoothness);
    if (field == "inputGain") return parseFloat(0.1f, 3.f, g_aimSettings.inputGain);
    if (field == "aimKey") {
        try { size_t used = 0; int key = std::stoi(value, &used); if (used != value.size()) return false;
            if (key != 2 && key != 5 && key != 6 && key != 164 && key != 160 && key != 17 && key != 69 && key != 67) return false;
            g_aimSettings.aimKey = key; return true; }
        catch (...) { return false; }
    }
    if (field == "boneMask") {
        try { size_t used = 0; unsigned long mask = std::stoul(value, &used); if (used != value.size() || mask > 15) return false; g_aimSettings.boneMask = static_cast<std::uint32_t>(mask); return true; }
        catch (...) { return false; }
    }
    return false;
}
void LocalMenu::SetAimStatus(const std::string& status) { std::lock_guard<std::mutex> lock(g_aimMutex); g_aimStatus = status; }
std::string LocalMenu::GetAimStatus() { std::lock_guard<std::mutex> lock(g_aimMutex); return g_aimStatus; }
std::string LocalMenu::Url() { return g_url; }
