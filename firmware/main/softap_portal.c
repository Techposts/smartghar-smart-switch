// SmartGhar Smart Switch — SoftAP config portal implementation.
// See softap_portal.h for the contract and docs/softap-config-portal.md for the
// design. The portal owns the AP netif + HTTP server only; all switch state is
// read/written through the sw_portal_* accessors in main.c.

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_mac.h"

#include "softap_portal.h"

static const char *TAG = "portal";

static httpd_handle_t s_httpd = NULL;
static esp_netif_t   *s_ap_netif = NULL;
static volatile bool  s_active = false;
static int64_t        s_last_activity_us = 0;
static esp_timer_handle_t s_idle_timer = NULL;

#define PORTAL_IDLE_TIMEOUT_US  (10LL * 60 * 1000000)   // 10 min idle → close

// ── The page ─────────────────────────────────────────────────────────────────
// One tiny dependency-free file in the hub's editorial voice. JS uses single
// quotes throughout (this is a C string — a stray \" pair would silently
// vanish via concatenation and kill the script).
static const char PAGE[] =
"<!doctype html><html><head><meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>SmartGhar Smart Switch</title>\n"
"<style>\n"
":root{--ink:#0f1620;--ink3:#6b7886;--line:#e6e9ee;--rain:#4a6b9c;--leaf:#3d7a4f;--rust:#a8442e;--paper:#fafbfc}\n"
"*{box-sizing:border-box}body{margin:0;background:#e8eaee;font:15px/1.5 Georgia,serif;color:var(--ink)}\n"
".wrap{max-width:560px;margin:0 auto;padding:24px 16px 60px}\n"
".card{background:var(--paper);border:1px solid var(--line);padding:20px;margin-bottom:14px}\n"
"h1{font-size:21px;font-weight:500;margin:0 0 2px}h1 em{font-style:italic;color:var(--rain)}\n"
".sub{font:600 10px system-ui;letter-spacing:.22em;text-transform:uppercase;color:var(--ink3);margin-bottom:18px}\n"
"h2{font-size:15px;font-weight:600;margin:0 0 12px;border-bottom:1px solid var(--line);padding-bottom:8px}\n"
".row{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px dotted var(--line);font-size:14px}\n"
".row b{font-family:ui-monospace,Menlo,monospace;font-weight:600}\n"
".lbl{display:block;font:600 11px system-ui;letter-spacing:.06em;text-transform:uppercase;color:var(--ink3);margin:12px 0 4px}\n"
"input{width:100%;padding:9px 10px;border:1px solid var(--line);background:#fff;font:inherit;font-size:14px}\n"
".btn{display:inline-block;border:1px solid var(--ink);background:var(--ink);color:#fff;font:600 13px system-ui;padding:10px 18px;cursor:pointer;margin-top:14px}\n"
".btn.gh{background:transparent;color:var(--ink)}\n"
".btn.warn{border-color:var(--rust);background:var(--rust)}\n"
".note{font-size:12.5px;color:var(--ink3);margin-top:8px}\n"
".ok{color:var(--leaf)}.err{color:var(--rust)}\n"
"#msg{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);background:var(--ink);color:#fff;font:600 13px system-ui;padding:10px 18px;display:none}\n"
".grid2{display:grid;grid-template-columns:1fr 1fr;gap:0 14px}\n"
"</style></head><body><div class='wrap'>\n"
"<h1>Smart<em>Switch</em></h1><div class='sub' id='ssid-sub'>by SmartGhar &middot; config portal</div>\n"
"<div class='card'><h2>Live status</h2><div id='st'>Loading&hellip;</div>\n"
"<button class='btn' id='rbtn' onclick='relay()'>&hellip;</button>\n"
"<p class='note'>Switching here behaves like pressing the physical button &mdash; if a hub is paired, its automation pauses (manual hold) until you resume it there.</p></div>\n"
"<div class='card'><h2>Electrical settings</h2><div class='grid2'>\n"
"<div><span class='lbl'>Mains voltage (V)</span><input id='volt' type='number' min='90' max='270'></div>\n"
"<div><span class='lbl'>Over-current trip (A)</span><input id='imax' type='number' min='0.5' step='0.1'></div>\n"
"<div><span class='lbl'>Dry-run floor (A, 0=off)</span><input id='drymin' type='number' min='0' step='0.1'></div>\n"
"<div><span class='lbl'>Max run time (min)</span><input id='runmax' type='number' min='1'></div></div>\n"
"<button class='btn' onclick='saveCfg()'>Save settings</button>\n"
"<p class='note'>Same settings the hub pushes via its Switches tab &mdash; whichever was set last wins.</p></div>\n"
"<div class='card'><h2>Current-sensor calibration</h2>\n"
"<p class='note' style='margin-top:0'>Run a known load through the switch (e.g. a 1000&nbsp;W heater &asymp; 4.3&nbsp;A at 230&nbsp;V), turn it on, enter its real current, and calibrate. The live reading above should then match.</p>\n"
"<span class='lbl'>Actual load current (A)</span><input id='cal' type='number' min='0.1' step='0.1'>\n"
"<button class='btn' onclick='calib()'>Calibrate</button>\n"
"<div class='note' id='calout'></div></div>\n"
"<div class='card'><h2>Home WiFi (standalone mode)</h2>\n"
"<p class='note' style='margin-top:0'>Optional &mdash; for running this switch WITHOUT a TankSync hub. Credentials are stored now; the standalone WiFi/MQTT mode arrives in a firmware update.</p>\n"
"<span class='lbl'>WiFi network</span><input id='wssid' maxlength='32'>\n"
"<span class='lbl'>WiFi password</span><input id='wpass' type='password' maxlength='64'>\n"
"<div class='grid2'><div><span class='lbl'>MQTT broker (optional)</span><input id='whost' maxlength='64'></div>\n"
"<div><span class='lbl'>Port</span><input id='wport' type='number' value='8883'></div></div>\n"
"<button class='btn' onclick='saveWifi()'>Save WiFi</button></div>\n"
"<div class='card'><h2>Done?</h2>\n"
"<button class='btn warn' onclick='exitAp()'>Close portal</button>\n"
"<p class='note'>The access point also closes by itself after 10 minutes of inactivity. Reopen it anytime: hold the button for 4 seconds.</p></div>\n"
"</div><div id='msg'></div>\n"
"<script>\n"
"var seeded=false;\n"
"function toast(t,ok){var m=document.getElementById('msg');m.textContent=t;m.style.background=ok===false?'#a8442e':'#0f1620';m.style.display='block';setTimeout(function(){m.style.display='none'},2600)}\n"
"function g(i){return document.getElementById(i)}\n"
"async function api(p,m){var r=await fetch(p,{method:m||'GET'});return r.json()}\n"
"async function load(){try{var s=await api('/api/info');\n"
"g('ssid-sub').textContent='by SmartGhar \\u00b7 '+s.ssid;\n"
"var f=[];if(s.fault&1)f.push('over-current');if(s.fault&2)f.push('over-temp');if(s.fault&4)f.push('welded contact');if(s.fault&8)f.push('dry-run');\n"
"g('st').innerHTML=\n"
"\"<div class='row'><span>Relay</span><b class='\"+(s.relay?'ok':'')+\"'>\"+(s.relay?'ON':'OFF')+'</b></div>'+\n"
"\"<div class='row'><span>Current</span><b>\"+(s.load_ma/1000).toFixed(2)+' A</b></div>'+\n"
"\"<div class='row'><span>Power</span><b>\"+Math.round(s.load_ma*s.mains_v/1000000*1000)+' W</b></div>'+\n"
"\"<div class='row'><span>Board temp</span><b>\"+(s.temp_c10/10).toFixed(1)+' \\u00b0C</b></div>'+\n"
"\"<div class='row'><span>Hub</span><b>\"+(s.paired?'paired':'not paired')+'</b></div>'+\n"
"\"<div class='row'><span>Firmware</span><b>v\"+s.fw+'</b></div>'+\n"
"(f.length?\"<div class='row'><span>Fault</span><b class='err'>\"+f.join(', ')+'</b></div>':'');\n"
"g('rbtn').textContent=s.relay?'Turn relay OFF':'Turn relay ON';window._relay=s.relay;\n"
"if(!seeded){seeded=true;g('volt').value=s.mains_v;g('imax').value=(s.oc_limit_ma/1000).toFixed(1);g('drymin').value=(s.dry_min_ma/1000).toFixed(1);g('runmax').value=Math.round(s.runmax_s/60);}\n"
"}catch(e){g('st').textContent='Connection lost \\u2014 did the portal close?'}}\n"
"async function relay(){var r=await api('/api/relay?on='+(window._relay?0:1),'POST');toast(r.ok?'Relay switched':'Failed',r.ok);load()}\n"
"async function saveCfg(){var q='volt='+g('volt').value+'&imax='+Math.round(g('imax').value*1000)+'&drymin='+Math.round(g('drymin').value*1000)+'&runmax='+Math.round(g('runmax').value*60);\n"
"var r=await api('/api/config?'+q,'POST');toast(r.ok?'Settings saved':(r.err||'Rejected'),r.ok)}\n"
"async function calib(){var a=parseFloat(g('cal').value);if(!a||a<=0){toast('Enter the real current first',false);return}\n"
"var r=await api('/api/calibrate?ma='+Math.round(a*1000),'POST');\n"
"if(r.ok){g('calout').innerHTML=\"<span class='ok'>Calibrated \\u2014 scale \"+r.scale.toFixed(3)+' mA/count. Live reading should now match.</span>';toast('Calibrated')}\n"
"else{g('calout').innerHTML=\"<span class='err'>\"+(r.err||'Calibration failed')+'</span>';toast('Calibration failed',false)}load()}\n"
"async function saveWifi(){var s=g('wssid').value.trim();if(!s){toast('Enter the WiFi network name',false);return}\n"
"var q='ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(g('wpass').value)+'&host='+encodeURIComponent(g('whost').value.trim())+'&port='+(parseInt(g('wport').value)||8883);\n"
"var r=await api('/api/wifi?'+q,'POST');toast(r.ok?'WiFi saved \\u2014 used when standalone mode ships':'Failed',r.ok)}\n"
"async function exitAp(){toast('Closing portal\\u2026');try{await api('/api/exit','POST')}catch(e){}}\n"
"load();setInterval(load,3000);\n"
"</script></body></html>\n";

// ── Helpers ──────────────────────────────────────────────────────────────────
static void touch(void) { s_last_activity_us = esp_timer_get_time(); }

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++, o++) {
        if (*s == '+') *o = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = { s[1], s[2], 0 };
            *o = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o = *s;
    }
    *o = 0;
}

// Read one query param into buf (url-decoded). Returns true if present.
static bool qparam(httpd_req_t *req, const char *key, char *buf, size_t len) {
    char q[256];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, buf, len) != ESP_OK) return false;
    url_decode(buf);
    return true;
}

static long qparam_long(httpd_req_t *req, const char *key, long dflt) {
    char b[20];
    return qparam(req, key, b, sizeof(b)) ? atol(b) : dflt;
}

static esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// ── Handlers ─────────────────────────────────────────────────────────────────
static esp_err_t h_page(httpd_req_t *req) {
    touch();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_info(httpd_req_t *req) {
    touch();
    sw_status_t st;
    sw_portal_get_status(&st);
    char j[360];
    snprintf(j, sizeof(j),
        "{\"fw\":\"%s\",\"ssid\":\"SmartGhar-Switch-%02X%02X\",\"paired\":%d,"
        "\"relay\":%d,\"load_ma\":%d,\"temp_c10\":%d,\"fault\":%u,"
        "\"mains_v\":%u,\"oc_limit_ma\":%lu,\"dry_min_ma\":%lu,\"runmax_s\":%lu,"
        "\"zmct_scale\":%.3f}",
        st.fw, st.mac[4], st.mac[5], st.paired ? 1 : 0,
        st.relay_on ? 1 : 0, st.load_ma, st.temp_c10, (unsigned)st.fault,
        (unsigned)st.mains_v, (unsigned long)st.oc_limit_ma,
        (unsigned long)st.dry_min_ma, (unsigned long)st.max_runtime_s,
        st.zmct_scale);
    return send_json(req, j);
}

static esp_err_t h_relay(httpd_req_t *req) {
    touch();
    sw_portal_set_relay(qparam_long(req, "on", 0) != 0);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_config(httpd_req_t *req) {
    touch();
    sw_status_t st;
    sw_portal_get_status(&st);
    long volt   = qparam_long(req, "volt",   st.mains_v);
    long imax   = qparam_long(req, "imax",   st.oc_limit_ma);
    long drymin = qparam_long(req, "drymin", st.dry_min_ma);
    long runmax = qparam_long(req, "runmax", st.max_runtime_s);
    if (volt < 90 || volt > 270)
        return send_json(req, "{\"ok\":false,\"err\":\"Voltage must be 90-270 V\"}");
    if (imax < 500)
        return send_json(req, "{\"ok\":false,\"err\":\"Over-current trip must be at least 0.5 A\"}");
    if (runmax < 60) runmax = 60;
    sw_portal_apply_config((uint16_t)volt, (uint32_t)imax, (uint32_t)drymin, (uint32_t)runmax);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_calibrate(httpd_req_t *req) {
    touch();
    long ma = qparam_long(req, "ma", 0);
    if (ma <= 0) return send_json(req, "{\"ok\":false,\"err\":\"Enter the load's real current\"}");
    float scale = sw_portal_calibrate((int)ma);
    if (scale <= 0)
        return send_json(req, "{\"ok\":false,\"err\":\"No measurable current - is the load on? (Bare test boards have no sensor.)\"}");
    char j[64];
    snprintf(j, sizeof(j), "{\"ok\":true,\"scale\":%.3f}", scale);
    return send_json(req, j);
}

static esp_err_t h_wifi(httpd_req_t *req) {
    touch();
    char ssid[33] = {0}, pass[65] = {0}, host[65] = {0};
    if (!qparam(req, "ssid", ssid, sizeof(ssid)) || !ssid[0])
        return send_json(req, "{\"ok\":false,\"err\":\"ssid required\"}");
    qparam(req, "pass", pass, sizeof(pass));
    qparam(req, "host", host, sizeof(host));
    long port = qparam_long(req, "port", 8883);
    sw_portal_save_wifi(ssid, pass, host, (uint16_t)port);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_exit(httpd_req_t *req) {
    send_json(req, "{\"ok\":true}");
    // Stop from a timer, not from inside the server's own handler context.
    esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = (esp_timer_cb_t)portal_stop, .name = "portal_exit" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 300000);  // 300ms
    return ESP_OK;
}

static void idle_check(void *arg) {
    (void)arg;
    if (s_active && esp_timer_get_time() - s_last_activity_us > PORTAL_IDLE_TIMEOUT_US) {
        ESP_LOGI(TAG, "portal idle 10 min — closing");
        portal_stop();
    }
}

// ── Lifecycle ────────────────────────────────────────────────────────────────
bool portal_active(void) { return s_active; }

void portal_start(void) {
    if (s_active) { touch(); return; }

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   // STA MAC — matches the hub's default name suffix

    // The AP must share the STA channel (single radio); read the live channel so
    // ESP-NOW to the hub keeps working underneath the portal.
    uint8_t chan = 1;
    wifi_second_chan_t sc;
    esp_wifi_get_channel(&chan, &sc);

    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "SmartGhar-Switch-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel = chan;
    ap.ap.authmode = WIFI_AUTH_OPEN;       // deliberate: short-lived, user-initiated AP
    ap.ap.max_connection = 2;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 8;
    hc.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &hc) == ESP_OK) {
        const httpd_uri_t routes[] = {
            { .uri = "/",              .method = HTTP_GET,  .handler = h_page },
            { .uri = "/api/info",      .method = HTTP_GET,  .handler = h_info },
            { .uri = "/api/relay",     .method = HTTP_POST, .handler = h_relay },
            { .uri = "/api/config",    .method = HTTP_POST, .handler = h_config },
            { .uri = "/api/calibrate", .method = HTTP_POST, .handler = h_calibrate },
            { .uri = "/api/wifi",      .method = HTTP_POST, .handler = h_wifi },
            { .uri = "/api/exit",      .method = HTTP_POST, .handler = h_exit },
        };
        for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
            httpd_register_uri_handler(s_httpd, &routes[i]);
    }

    if (!s_idle_timer) {
        const esp_timer_create_args_t a = { .callback = idle_check, .name = "portal_idle" };
        esp_timer_create(&a, &s_idle_timer);
    }
    esp_timer_start_periodic(s_idle_timer, 30 * 1000000LL);

    touch();
    s_active = true;
    ESP_LOGI(TAG, "portal UP — join %s, open http://192.168.4.1/ (chan %u)",
             (char *)ap.ap.ssid, (unsigned)chan);
}

void portal_stop(void) {
    if (!s_active) return;
    s_active = false;
    if (s_idle_timer) esp_timer_stop(s_idle_timer);
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);      // back to STA-only; ESP-NOW unaffected
    ESP_LOGI(TAG, "portal DOWN — STA-only");
}
