// Clare Wi-Fi provisioning — open setup hotspot + DNS captive portal.
//
// Flow: clare_prov_start() stops the STA radio, brings up an open AP
// ("Clare-S3-XXXX", XXXX = STA MAC tail), starts a wildcard DNS responder on
// UDP 53 (every A query resolves to the AP address) and a tiny HTTP server
// on port 80.  Phones detect the portal (captive.apple.com,
// connectivitycheck.gstatic.com, ... are all redirected) and pop the config
// page.  /scan.json returns nearby APs, /save stores ssid/pass in NVS and
// schedules prov shutdown; the app then reconnects through clare_net with
// the stored credentials.
//
// No credentials or meeting contents are ever logged.

#include "clare_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace {

static const char *TAG = "clare_prov";

constexpr size_t PROV_SSID_MAX = 32;
constexpr size_t PROV_CREDS_SSID_MAX = 32;
constexpr size_t PROV_CREDS_PASS_MAX = 64;
constexpr size_t SCAN_MAX = 20;
constexpr size_t SCAN_JSON_MAX = 2048;
constexpr uint32_t PROV_FINISH_DELAY_MS = 800;

static char s_ap_ssid[PROV_SSID_MAX + 1] = "Clare-S3";
static esp_netif_t *s_ap_netif = nullptr;
static httpd_handle_t s_httpd = nullptr;
static TaskHandle_t s_dns_task = nullptr;
static volatile bool s_dns_run = false;
static bool s_active = false;
static bool s_ap_netif_created = false;
static clare_net_event_cb_t s_event_cb = nullptr;
static void *s_event_ctx = nullptr;
static esp_ip4_addr_t s_ap_ip4 = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) };  // captive-portal DNS/redirect target
static esp_event_handler_instance_t s_ap_handler = nullptr;
static bool s_handler_registered = false;
// Bumped on every session start: a deferred finish_task from an older
// session must never tear down a freshly opened one.
static std::atomic<uint32_t> s_session{0};

// Large scratch buffers live in PSRAM: internal RAM stays free for the
// Wi-Fi/TLS stacks (see clare_net.cpp memory notes).
EXT_RAM_BSS_ATTR static wifi_ap_record_t s_scan_records[SCAN_MAX];
EXT_RAM_BSS_ATTR static char s_scan_json[SCAN_JSON_MAX];

static void emit_event(clare_net_event_type_t type, const char *text = nullptr)
{
    clare_net_event_cb_t cb = s_event_cb;
    void *ctx = s_event_ctx;
    if (!cb) return;
    clare_net_event_t event = {
        .type = type,
        .text = text,
        .binary = nullptr,
        .binary_len = 0,
        .is_final = false,
        .status_code = 0,
        .error = ESP_OK,
        .is_delta = false,
    };
    cb(&event, ctx);
}

// --- NVS credentials --------------------------------------------------------

static esp_err_t credentials_save(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("clare_prov", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "pass", pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

// --- DNS captive portal ------------------------------------------------------

// Build a DNS response inside pkt (request in place, classic captive-portal
// trick): echo the question, answer every A query with the AP IPv4 address.
// Returns response length, 0 when the packet should be dropped.
static size_t dns_build_response(uint8_t *pkt, size_t len)
{
    if (len < 12) return 0;
    const uint16_t flags = static_cast<uint16_t>((pkt[2] << 8) | pkt[3]);
    if (flags & 0x8000) return 0;  // not a query
    // Skip the first qname (labels until the zero terminator).
    size_t qend = 12;
    while (qend < len && pkt[qend]) {
        qend += static_cast<size_t>(pkt[qend]) + 1;
    }
    qend += 1;                     // zero terminator
    if (qend + 4 > len) return 0;
    const uint16_t qtype = static_cast<uint16_t>((pkt[qend] << 8) | pkt[qend + 1]);
    qend += 4;                     // qtype + qclass

    pkt[2] = 0x81;                 // QR | opcode 0 | !AA | !TC | RD
    pkt[3] = 0x80 | (flags & 0x01);
    pkt[4] = 0; pkt[5] = 1;        // QDCOUNT = 1
    if (qtype != 1) {              // non-A query: NOERROR, no answer
        pkt[6] = pkt[7] = pkt[8] = pkt[9] = 0;
        return qend;
    }
    pkt[6] = 0; pkt[7] = 1;        // ANCOUNT = 1
    pkt[8] = pkt[9] = 0;
    if (qend + 16 > 512) return 0;
    uint8_t *a = pkt + qend;
    a[0] = 0xC0; a[1] = 0x0C;      // pointer to the question name
    a[2] = 0; a[3] = 1;            // TYPE A
    a[4] = 0; a[5] = 1;            // CLASS IN
    a[6] = 0; a[7] = 0; a[8] = 1; a[9] = 0x2C;  // TTL 300 s
    a[10] = 0; a[11] = 4;
    memcpy(a + 12, &s_ap_ip4.addr, 4);
    return qend + 16;
}

static void dns_task(void *)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed errno=%d", errno);
        s_dns_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "DNS bind failed errno=%d", errno);
        close(sock);
        s_dns_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    // 250 ms poll so a stop->restart cycle never races the port-53 release:
    // prov_stop waits ~1 s, comfortably covering several recv timeouts.
    struct timeval tv = {.tv_sec = 0, .tv_usec = 250 * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "DNS captive portal on udp/53 -> " IPSTR, IP2STR(&s_ap_ip4));
    uint8_t pkt[512];
    while (s_dns_run) {
        struct sockaddr_in src = {};
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(sock, pkt, sizeof(pkt), 0,
                             reinterpret_cast<struct sockaddr *>(&src), &src_len);
        if (n <= 0) continue;
        size_t out = dns_build_response(pkt, static_cast<size_t>(n));
        if (out > 0) {
            sendto(sock, pkt, out, 0, reinterpret_cast<struct sockaddr *>(&src), src_len);
        }
    }
    close(sock);
    s_dns_task = nullptr;
    vTaskDelete(nullptr);
}

// --- HTTP portal -------------------------------------------------------------

static const char kIndexHtml[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Clare Wi-Fi Setup</title>"
    "<style>"
    "body{font-family:-apple-system,Segoe UI,sans-serif;background:#101722;color:#F4F7FB;margin:0;padding:24px}"
    ".card{max-width:440px;margin:0 auto;background:#182231;border-radius:16px;padding:20px}"
    "h1{font-size:20px;margin:0 0 12px}"
    "select,input{width:100%;padding:12px;border-radius:8px;border:1px solid #394B63;background:#0F1622;color:#F4F7FB;box-sizing:border-box;margin:6px 0;font-size:16px}"
    "button{width:100%;padding:12px;border:0;border-radius:8px;background:#247C68;color:#fff;font-size:16px;margin-top:6px}"
    ".msg{margin-top:12px;font-size:14px;color:#9EB2C9}"
    "#result{color:#8ED1B2}"
    "</style></head><body><div class=card>"
    "<h1>Clare Wi-Fi Setup</h1>"
    "<div id=msg class=msg>Scanning networks...</div>"
    "<select id=ssid></select>"
    "<input id=pass type=password placeholder=\"Wi-Fi password (leave empty for open networks)\">"
    "<button onclick=save()>Connect</button>"
    "<div id=result class=msg></div>"
    "</div><script>"
    "async function scan(){"
    "var m=document.getElementById('msg');m.textContent='Scanning networks...';"
    "try{"
    "var r=await fetch('/scan.json');var aps=await r.json();"
    "var sel=document.getElementById('ssid');sel.innerHTML='';"
    "aps.forEach(function(a){var o=document.createElement('option');o.value=a.s;"
    "o.textContent=a.s+' ('+a.r+' dBm'+(a.a?', secured':'')+')';sel.appendChild(o);});"
    "m.textContent=aps.length?'Select your Wi-Fi network':'No networks found - tap here to retry';"
    "}catch(e){m.textContent='Scan failed - tap here to retry';}}"
    "async function save(){"
    "var s=document.getElementById('ssid').value;"
    "var p=document.getElementById('pass').value;"
    "var r=document.getElementById('result');"
    "if(!s){r.textContent='Please select a network first';return;}"
    "r.textContent='Saving...';"
    "try{"
    "await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)});"
    "r.textContent='Saved. Clare is reconnecting - you can close this page.';"
    "}catch(e){r.textContent='Save failed - please retry';}}"
    "document.getElementById('msg').addEventListener('click',scan);"
    "scan();"
    "</script></body></html>";

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Minimal JSON string escape: quote, backslash and control chars.
static size_t json_escape(char *out, size_t out_len, const char *in)
{
    size_t o = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(in);
         *p && o + 2 < out_len; ++p) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = static_cast<char>(*p);
        } else if (*p >= 0x20) {
            out[o++] = static_cast<char>(*p);
        }
    }
    out[o] = '\0';
    return o;
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start err=%d", static_cast<int>(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }
    uint16_t num = SCAN_MAX;
    err = esp_wifi_scan_get_ap_records(&num, s_scan_records);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }
    // Skip duplicate SSIDs (the radio reports one record per BSSID).
    size_t o = 0;
    int written = snprintf(s_scan_json + o, SCAN_JSON_MAX - o, "[");
    if (written > 0) o += static_cast<size_t>(written);
    int first = 1;
    for (uint16_t i = 0; i < num && o < SCAN_JSON_MAX - 96; ++i) {
        const wifi_ap_record_t *rec = &s_scan_records[i];
        const char *ssid = reinterpret_cast<const char *>(rec->ssid);
        if (!ssid[0]) continue;
        bool dup = false;
        for (uint16_t j = 0; j < i; ++j) {
            if (strcmp(ssid, reinterpret_cast<const char *>(s_scan_records[j].ssid)) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        char esc[72];
        json_escape(esc, sizeof(esc), ssid);
        written = snprintf(s_scan_json + o, SCAN_JSON_MAX - o, "%s{\"s\":\"%s\",\"r\":%d,\"a\":%d}",
                           first ? "" : ",", esc, rec->rssi,
                           rec->authmode == WIFI_AUTH_OPEN ? 0 : 1);
        if (written < 0) break;
        o += static_cast<size_t>(written);
        first = 0;
    }
    snprintf(s_scan_json + o, SCAN_JSON_MAX - o, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, s_scan_json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// In-place percent/url decoding of form data.
static void url_decode(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            ++r;
        } else if (r[0] == '%' && r[1] && r[2]) {
            int hi = hex_nibble(r[1]);
            int lo = hex_nibble(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = static_cast<char>((hi << 4) | lo);
                r += 3;
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// Extract one urlencoded form field without mutating body: finds "key=" in
// the '&'-separated form, percent-decodes the value into out.
// Returns ESP_OK, ESP_ERR_NOT_FOUND, or ESP_ERR_INVALID_SIZE when the
// decoded value would not fit (never truncates credentials).
static esp_err_t form_field(const char *body, const char *key,
                            char *out, size_t out_len)
{
    if (!body || !key || !out || out_len == 0) return ESP_ERR_INVALID_ARG;
    const size_t key_len = strlen(key);
    for (const char *p = body; p && *p;) {
        const char *end = strchr(p, '&');
        const size_t seg_len = end ? static_cast<size_t>(end - p) : strlen(p);
        if (seg_len > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const size_t vlen = seg_len - key_len - 1;
            if (vlen >= out_len) return ESP_ERR_INVALID_SIZE;
            memcpy(out, p + key_len + 1, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return ESP_OK;
        }
        p = end ? end + 1 : nullptr;
    }
    return ESP_ERR_NOT_FOUND;
}

// Deferred portal shutdown: never stop the httpd from inside its own worker
// task.  Gives the phone a moment to render the success page first.  The
// session argument guards against a stale run closing a newer session that
// opened in the meantime.
static void finish_task(void *arg)
{
    const uint32_t session = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    vTaskDelay(pdMS_TO_TICKS(PROV_FINISH_DELAY_MS));
    if (s_active && session == s_session.load()) {
        clare_prov_stop();
    }
    vTaskDelete(nullptr);
}

static esp_err_t save_handler(httpd_req_t *req)
{
    // Max legal form: "ssid=" + 32*3 urlencoded + "&pass=" + 64*3 = 299 bytes.
    char body[320] = {};
    if (req->content_len >= sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "form too large", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    size_t want = req->content_len;
    size_t got = 0;
    while (got < want) {
        int n = httpd_req_recv(req, body + got, want - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    body[got] = '\0';

    char ssid[PROV_CREDS_SSID_MAX + 1] = {};
    char pass[PROV_CREDS_PASS_MAX + 1] = {};
    esp_err_t ssid_err = form_field(body, "ssid", ssid, sizeof(ssid));
    esp_err_t pass_err = form_field(body, "pass", pass, sizeof(pass));
    if (ssid_err != ESP_OK || !ssid[0] || pass_err == ESP_ERR_INVALID_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid credentials", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    // A missing pass field means an open network; an oversized one was
    // already rejected above.
    if (pass_err != ESP_OK) pass[0] = '\0';

    esp_err_t err = credentials_save(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "credentials save failed err=%d", static_cast<int>(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "storage error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Credentials saved for ssid=%s (password hidden)", ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
                    "<html><body style=\"font-family:sans-serif;background:#101722;color:#F4F7FB;"
                    "padding:24px\"><h2>Saved</h2><p>Clare is reconnecting to your Wi-Fi. "
                    "You can close this page.</p></body></html>",
                    HTTPD_RESP_USE_STRLEN);
    emit_event(CLARE_NET_EVENT_PROV_CREDENTIALS_SAVED, ssid);
    // Shutdown is deferred to a separate task: httpd must never be stopped
    // from inside its own worker task, and the phone needs a moment to
    // render the success page before the AP goes away.
    xTaskCreate(finish_task, "prov_finish", 3072,
                reinterpret_cast<void *>(static_cast<uintptr_t>(s_session)), 4, nullptr);
    return ESP_OK;
}

// Captive-portal catch-all: every probe URL (generate_204, hotspot-detect,
// whatever the OS uses) bounces to the config page at the AP address.
static esp_err_t redirect_404_handler(httpd_req_t *req, httpd_err_code_t)
{
    char location[40] = {};
    snprintf(location, sizeof(location), "http://" IPSTR "/", IP2STR(&s_ap_ip4));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) return err;

    httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_handler, .user_ctx = nullptr};
    httpd_uri_t scan = {.uri = "/scan.json", .method = HTTP_GET, .handler = scan_handler, .user_ctx = nullptr};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler, .user_ctx = nullptr};
    httpd_register_uri_handler(s_httpd, &page);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404_handler);
    return ESP_OK;
}

// --- Wi-Fi AP events ---------------------------------------------------------

static void ap_event_handler(void *, esp_event_base_t base, int32_t id, void *)
{
    if (base != WIFI_EVENT || !s_active) return;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Phone connected to setup hotspot");
        emit_event(CLARE_NET_EVENT_PROV_CLIENT_CONNECTED);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Phone left the setup hotspot");
    }
}

} // namespace

extern "C" esp_err_t clare_prov_init(const clare_net_config_t *config)
{
    if (config) {
        s_event_cb = config->event_cb;
        s_event_ctx = config->ctx;
    }
    return ESP_OK;
}

extern "C" esp_err_t clare_prov_credentials_load(char *ssid, size_t ssid_len,
                                                 char *pass, size_t pass_len)
{
    if (!ssid || ssid_len == 0) return ESP_ERR_INVALID_ARG;
    ssid[0] = '\0';
    if (pass && pass_len) pass[0] = '\0';
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("clare_prov", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    size_t len = ssid_len;
    err = nvs_get_str(nvs, "ssid", ssid, &len);
    if (err == ESP_OK && pass && pass_len) {
        size_t pass_sz = pass_len;
        err = nvs_get_str(nvs, "pass", pass, &pass_sz);
    }
    nvs_close(nvs);
    return err;
}

extern "C" esp_err_t clare_prov_start(void)
{
    // Re-entering setup (e.g. right after saving, while the deferred finish
    // task is still pending) restarts the session instead of pretending the
    // old one is still alive — the finish task's session check then keeps
    // the new session open.
    if (s_active) {
        clare_prov_stop();
    }
    esp_err_t err = clare_net_init(nullptr);
    if (err != ESP_OK) return err;

    // The hotspot owns the radio while provisioning is active.
    err = clare_net_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA stop before provisioning err=%d", static_cast<int>(err));
    }

    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Clare-S3-%02X%02X", mac[4], mac[5]);

    if (!s_ap_netif_created) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "AP netif creation failed");
            return ESP_FAIL;
        }
        s_ap_netif_created = true;
    }
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr) {
        s_ap_ip4.addr = ip_info.ip.addr;
    }

    if (!s_handler_registered) {
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  &ap_event_handler, nullptr, &s_ap_handler);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_handler_registered = true;
    }

    wifi_config_t ap_cfg = {};
    strlcpy(reinterpret_cast<char *>(ap_cfg.ap.ssid), s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = static_cast<uint8_t>(strlen(s_ap_ssid));
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) return err;
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable Wi-Fi power save err=%d", static_cast<int>(err));
    }

    s_dns_run = true;
    BaseType_t task_ok = xTaskCreate(dns_task, "prov_dns", 3072, nullptr, 5, &s_dns_task);
    if (task_ok != pdPASS) {
        s_dns_run = false;
        (void)esp_wifi_stop();
        return ESP_ERR_NO_MEM;
    }

    err = start_httpd();
    if (err != ESP_OK) {
        s_dns_run = false;
        (void)esp_wifi_stop();
        ESP_LOGE(TAG, "Portal HTTP server failed err=%d", static_cast<int>(err));
        return err;
    }

    ++s_session;
    s_active = true;
    ESP_LOGI(TAG, "Setup hotspot \"%s\" up, portal on " IPSTR, s_ap_ssid, IP2STR(&s_ap_ip4));
    emit_event(CLARE_NET_EVENT_PROV_STARTED, s_ap_ssid);
    return ESP_OK;
}

extern "C" esp_err_t clare_prov_stop(void)
{
    if (!s_active) return ESP_OK;
    s_active = false;

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }
    s_dns_run = false;
    for (int i = 0; i < 100 && s_dns_task; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    // The AP event handler stays registered for the life of the firmware;
    // ap_event_handler gates on s_active, and registering it once avoids
    // leak/inconsistency on the failure paths above.
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STOP_STATE && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "Wi-Fi stop err=%d", static_cast<int>(err));
    }
    ESP_LOGI(TAG, "Setup hotspot closed");
    emit_event(CLARE_NET_EVENT_PROV_STOPPED);
    return ESP_OK;
}

extern "C" bool clare_prov_is_active(void)
{
    return s_active;
}

extern "C" const char *clare_prov_ap_ssid(void)
{
    return s_ap_ssid;
}
