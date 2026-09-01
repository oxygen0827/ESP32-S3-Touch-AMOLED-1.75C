// Clare network transport for ESP32-C6.
//
// The implementation intentionally keeps all credentials and meeting data out
// of logs. HTTP uses the ESP-IDF client with the certificate bundle; both
// WebSocket audio channels use the documented Base64 JSON messages.

#include "clare_net.h"
#include "clare_ca_chain.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#ifndef CONFIG_CLARE_WIFI_SSID
#define CONFIG_CLARE_WIFI_SSID ""
#endif
#ifndef CONFIG_CLARE_WIFI_PASSWORD
#define CONFIG_CLARE_WIFI_PASSWORD ""
#endif
#ifndef CONFIG_CLARE_API_BASE_URL
#define CONFIG_CLARE_API_BASE_URL ""
#endif
#ifndef CONFIG_CLARE_TOPIC
#define CONFIG_CLARE_TOPIC ""
#endif

namespace {

static const char *TAG = "clare_net";

constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
constexpr EventBits_t WIFI_FAILED_BIT = BIT1;
constexpr int WIFI_MAX_RETRIES = 8;
constexpr size_t SESSION_ID_MAX = 96;
constexpr size_t URL_MAX = 384;
constexpr size_t SESSION_RESPONSE_MAX = 512;
constexpr size_t END_RESPONSE_MAX = 256;
constexpr size_t WS_RX_MAX = 16384;
constexpr size_t HOST_AUDIO_MAX = 12000;
// Send one 100 ms PCM frame (3200 bytes) per complete, unfragmented
// WebSocket text message.  This matches the production-proven S3 reference
// (vocat/products/ws_meeting_demo transcribe_ws.c): the Clare backend drops
// connections that stream fragmented continuation frames, which surfaced as
// mid-meeting WS disconnects (logs/clare_s3_mbedtls_fix_live_20260829.log).
// The old 640-byte fragmented scheme was a C6 memory workaround and is not
// needed on S3 with PSRAM.
constexpr size_t TRANSCRIBE_AUDIO_BATCH_MAX = 3200; // 1600 samples, 16 kHz mono PCM16 = 100 ms
constexpr size_t HOST_AUDIO_BATCH_MAX = 3200;       // short questions, same bound
constexpr size_t AUDIO_B64_MAX = (TRANSCRIBE_AUDIO_BATCH_MAX * 4 / 3) + 8;
constexpr size_t AUDIO_JSON_MAX = AUDIO_B64_MAX + 64;
constexpr uint32_t WS_AUDIO_SEND_TIMEOUT_MS = 1500; // 500 ms was too twitchy on cellular hotspots: a transient poll_write timeout killed healthy connections
// Largest contiguous heap block we are willing to require for an audio send.
// Set well below the observed failure point to give Wi-Fi/TLS headroom.
constexpr size_t AUDIO_SEND_MIN_HEAP_BLOCK = 2048;

enum class WsKind : uint8_t {
    Transcribe,
    Host,
};

struct WsContext {
    WsKind kind;
    esp_websocket_client_handle_t client;
    EventGroupHandle_t events;
    char *rx_buf;
    size_t rx_len;
    size_t rx_expected;
    size_t rx_cap;
    volatile bool connected;
    // Streaming answer_audio handling (host channel only). The C6 cannot
    // buffer a whole sentence, so we decode Base64 incrementally instead of
    // reassembling the JSON message.
    bool tts_probe_done;
    bool tts_mode;
    uint8_t probe[48];
    size_t probe_len;
    uint8_t b64_quad[4];
    size_t b64_quad_len;
};

struct HttpResponse {
    char *buf;
    size_t cap;
    size_t len;
    bool truncated;
};

static SemaphoreHandle_t s_lock = nullptr;
static EventGroupHandle_t s_wifi_events = nullptr;
static esp_netif_t *s_sta_netif = nullptr;
static esp_event_handler_instance_t s_wifi_handler = nullptr;
static esp_event_handler_instance_t s_ip_handler = nullptr;
static bool s_initialized = false;
static bool s_wifi_started = false;
static volatile bool s_wifi_connected = false;
static int s_wifi_retries = 0;
static clare_net_event_cb_t s_event_cb = nullptr;
static void *s_event_ctx = nullptr;
static char s_session_id[SESSION_ID_MAX] = {};
static WsContext s_transcribe = {WsKind::Transcribe, nullptr, nullptr, nullptr, 0, 0, 0, false, false, false, {}, 0, {}, 0};
static WsContext s_host = {WsKind::Host, nullptr, nullptr, nullptr, 0, 0, 0, false, false, false, {}, 0, {}, 0};

// Audio staging buffers must live in external RAM (EXT_RAM_BSS_ATTR):
// function-local and namespace statics land in internal .bss even with
// SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y, and ~20 KB of internal .bss here
// plus the summary buffers left <9 KB contiguous internal RAM, so the
// 10 KB websocket client task stack could not be allocated
// ("Error create websocket task", logs/clare_s3_e2e_test_20260829.log).
EXT_RAM_BSS_ATTR static uint8_t s_transcribe_audio_batch[TRANSCRIBE_AUDIO_BATCH_MAX];
static size_t s_transcribe_audio_batch_len = 0;
EXT_RAM_BSS_ATTR static uint8_t s_host_audio_batch[HOST_AUDIO_BATCH_MAX];
static size_t s_host_audio_batch_len = 0;
static SemaphoreHandle_t s_audio_send_lock = nullptr;
// Whole audio JSON message, built in one buffer so it goes out as a single
// WS frame (see TRANSCRIBE_AUDIO_BATCH_MAX comment).  Shared by both
// channels; every caller holds s_audio_send_lock while using it.
EXT_RAM_BSS_ATTR static char s_audio_json[AUDIO_JSON_MAX];

static bool take_lock(TickType_t timeout = pdMS_TO_TICKS(1000))
{
    return s_lock != nullptr && xSemaphoreTake(s_lock, timeout) == pdTRUE;
}

static void give_lock()
{
    if (s_lock != nullptr) {
        xSemaphoreGive(s_lock);
    }
}

static void emit_event(clare_net_event_type_t type,
                       const char *text = nullptr,
                       const uint8_t *binary = nullptr,
                       size_t binary_len = 0,
                       bool is_final = false,
                       int status_code = 0,
                       esp_err_t error = ESP_OK,
                       bool is_delta = false)
{
    clare_net_event_cb_t cb = nullptr;
    void *ctx = nullptr;
    if (take_lock()) {
        cb = s_event_cb;
        ctx = s_event_ctx;
        give_lock();
    }
    if (!cb) {
        return;
    }
    clare_net_event_t event = {
        .type = type,
        .text = text,
        .binary = binary,
        .binary_len = binary_len,
        .is_final = is_final,
        .status_code = status_code,
        .error = error,
        .is_delta = is_delta,
    };
    cb(&event, ctx);
}

// --- Streaming answer_audio (host channel) ---------------------------------

static constexpr char kTtsPrefix[] = "{\"type\":\"answer_audio\",\"data\":\"";
constexpr size_t kTtsPrefixLen = sizeof(kTtsPrefix) - 1;

// Incremental Base64 decoder: consumes any number of bytes, emits decoded
// MP3 through the application callback in bounded chunks.
static void tts_b64_reset(WsContext *ctx)
{
    ctx->b64_quad_len = 0;
}

static void tts_emit_chunk(const uint8_t *mp3, size_t len)
{
    if (len > 0) {
        emit_event(CLARE_NET_EVENT_HOST_ANSWER_AUDIO, nullptr, mp3, len);
    }
}

static void tts_b64_feed(WsContext *ctx, const uint8_t *in, size_t len, bool final_fragment)
{
    static uint8_t out[768];
    while (len > 0) {
        while (ctx->b64_quad_len < 4 && len > 0) {
            uint8_t c = *in++;
            --len;
            if (c == '"') {
                // Closing quote of the data field: ignore trailing JSON.
                ctx->b64_quad_len = 0;
                continue;
            }
            if (c == ',' || c == '}') continue;   // tolerate trailing fields
            ctx->b64_quad[ctx->b64_quad_len++] = c;
        }
        if (ctx->b64_quad_len < 4) break;
        size_t olen = 0;
        if (mbedtls_base64_decode(out, sizeof(out), &olen,
                                  ctx->b64_quad, 4) == 0 && olen > 0) {
            tts_emit_chunk(out, olen);
        }
        ctx->b64_quad_len = 0;
    }
    (void)final_fragment;
}


static bool valid_session_id(const char *id)
{
    if (!id || !id[0]) {
        return false;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(id); *p; ++p) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.')) {
            return false;
        }
    }
    return true;
}

static bool get_base_url(char *out, size_t out_len)
{
    const char *base = CONFIG_CLARE_API_BASE_URL;
    if (!base || !base[0] || !out || out_len < 2) {
        return false;
    }
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') {
        --n;
    }
    if (n == 0 || n >= out_len) {
        return false;
    }
    memcpy(out, base, n);
    out[n] = '\0';
    return true;
}

static bool make_http_url(char *out, size_t out_len, const char *path)
{
    char base[URL_MAX];
    if (!path || !get_base_url(base, sizeof(base))) {
        return false;
    }
    int written = snprintf(out, out_len, "%s/%s", base, path[0] == '/' ? path + 1 : path);
    return written > 0 && static_cast<size_t>(written) < out_len;
}

static bool make_ws_url(char *out, size_t out_len, const char *path)
{
    char base[URL_MAX];
    if (!path || !get_base_url(base, sizeof(base))) {
        return false;
    }
    const char *scheme = nullptr;
    const char *rest = nullptr;
    if (strncmp(base, "https://", 8) == 0) {
        scheme = "wss://";
        rest = base + 8;
    } else if (strncmp(base, "http://", 7) == 0) {
        scheme = "ws://";
        rest = base + 7;
    } else if (strncmp(base, "wss://", 6) == 0) {
        scheme = "wss://";
        rest = base + 6;
    } else if (strncmp(base, "ws://", 5) == 0) {
        scheme = "ws://";
        rest = base + 5;
    } else {
        return false;
    }
    int written = snprintf(out, out_len, "%s%s/%s", scheme, rest,
                           path[0] == '/' ? path + 1 : path);
    return written > 0 && static_cast<size_t>(written) < out_len;
}

static void report_error(esp_err_t err, int status_code = 0)
{
    emit_event(CLARE_NET_EVENT_ERROR, nullptr, nullptr, 0, false, status_code, err);
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (!event || !event->user_data) {
        return ESP_OK;
    }
    HttpResponse *response = static_cast<HttpResponse *>(event->user_data);
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    if (response->len >= response->cap - 1) {
        response->truncated = true;
        return ESP_OK;
    }
    size_t remaining = response->cap - response->len - 1;
    size_t copy_len = static_cast<size_t>(event->data_len) < remaining
                          ? static_cast<size_t>(event->data_len)
                          : remaining;
    memcpy(response->buf + response->len, event->data, copy_len);
    response->len += copy_len;
    response->buf[response->len] = '\0';
    if (copy_len < static_cast<size_t>(event->data_len)) {
        response->truncated = true;
    }
    return ESP_OK;
}

static void fill_http_config(esp_http_client_config_t *config,
                             const char *url,
                             esp_http_client_method_t method,
                             HttpResponse *response)
{
    *config = {};
    config->url = url;
    config->method = method;
    config->timeout_ms = 30000;
    config->buffer_size = 2048;
    config->buffer_size_tx = 1024;
    config->event_handler = http_event_handler;
    config->user_data = response;
    config->skip_cert_common_name_check = false;
#if !CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    config->cert_pem = kClareCaChainPem;
    config->cert_len = sizeof(kClareCaChainPem);  // includes NUL terminator
#endif
    // Pinned PEM is kept over the certificate bundle: the 4-cert server chain
    // ends in cross-signed ISRG Root X2 <- X1, and the bundle verify path then
    // needs an RSA-4096 software verify (~-0x4290 = RSA_PUBLIC_FAILED +
    // MPI_ALLOC_FAILED on the fragmented post-HTTP heap). The pinned chain
    // verifies with ECDSA only and needs far less contiguous heap.
    // NOTE: when CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y (route A), no CA may
    // be passed at all — esp-tls only applies VERIFY_NONE in its no-cacert
    // branch, a pinned cert_pem would force VERIFY_REQUIRED and break the
    // handshake against self-signed/local endpoints (-0x2700).
}

static esp_err_t http_perform(const char *url,
                              esp_http_client_method_t method,
                              const char *content_type,
                              const char *body,
                              char *response_buf,
                              size_t response_buf_len,
                              int *status_code,
                              bool *response_truncated)
{
    if (!url || !response_buf || response_buf_len < 2 || !status_code) {
        return ESP_ERR_INVALID_ARG;
    }
    HttpResponse response = {
        .buf = response_buf,
        .cap = response_buf_len,
        .len = 0,
        .truncated = false,
    };
    response_buf[0] = '\0';
    esp_http_client_config_t config;
    fill_http_config(&config, url, method, &response);
    ESP_LOGI(TAG, "HTTP before init free8=%u largest8=%u free_internal=%u largest_internal=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client allocation failed free=%u largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    if (content_type) {
        err = esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (err == ESP_OK && body) {
        err = esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    *status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP response status=%d bytes=%u truncated=%d",
             *status_code, static_cast<unsigned>(response.len), response.truncated ? 1 : 0);
    int tls_error = 0;
    int tls_flags = 0;
    if (err != ESP_OK) {
        (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
        // Keep diagnostics numeric; never print a response body or credential.
        ESP_LOGW(TAG, "HTTP request failed err=%d status=%d tls=%d flags=0x%x",
                 static_cast<int>(err), *status_code, tls_error, tls_flags);
    }
    esp_http_client_cleanup(client);
    // TEMP DIAGNOSTIC: heap state after HTTP teardown (remove after use)
    ESP_LOGI(TAG, "HTTP after cleanup free8=%u largest8=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    if (response_truncated) {
        *response_truncated = response.truncated;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (*status_code < 200 || *status_code >= 300) {
        ESP_LOGW(TAG, "HTTP response status=%d", *status_code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void wifi_event_handler(void *, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_wifi_retries = 0;
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
        }
        (void)esp_wifi_connect();
        emit_event(CLARE_NET_EVENT_WIFI_CONNECTING);
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *disconnect = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        s_wifi_connected = false;
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        if (s_wifi_started && s_wifi_retries < WIFI_MAX_RETRIES) {
            ++s_wifi_retries;
            (void)esp_wifi_connect();
        } else if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%d retry=%d",
                 disconnect ? static_cast<int>(disconnect->reason) : -1, s_wifi_retries);
        emit_event(CLARE_NET_EVENT_WIFI_DISCONNECTED);
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retries = 0;
        s_wifi_connected = true;
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        if (s_sta_netif) {
            esp_netif_dns_info_t dhcp_dns = {};
            bool have_dhcp_dns =
                esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dhcp_dns) == ESP_OK &&
                dhcp_dns.ip.type == ESP_IPADDR_TYPE_V4;
            if (have_dhcp_dns) {
                ESP_LOGI(TAG, "DHCP DNS main=" IPSTR,
                         IP2STR(&dhcp_dns.ip.u_addr.ip4));
            }
            // Keep the DHCP-provided resolver as the primary DNS. Public
            // resolvers are only backups: some hotspots (e.g. iOS Personal
            // Hotspot) do not route direct queries to public DNS servers.
            const esp_netif_dns_info_t public_dns[] = {
                {.ip = ESP_IP4ADDR_INIT(223, 5, 5, 5)},
                {.ip = ESP_IP4ADDR_INIT(1, 1, 1, 1)},
            };
            const esp_netif_dns_type_t dns_types[] = {
                ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK,
            };
            for (size_t i = 0; i < sizeof(dns_types) / sizeof(dns_types[0]); ++i) {
                esp_netif_dns_info_t dns = public_dns[i];
                esp_err_t dns_err = esp_netif_set_dns_info(s_sta_netif, dns_types[i], &dns);
                if (dns_err != ESP_OK) {
                    ESP_LOGW(TAG, "DNS override type=%u err=%d",
                             static_cast<unsigned>(dns_types[i]), static_cast<int>(dns_err));
                }
            }
        }
        emit_event(CLARE_NET_EVENT_WIFI_CONNECTED);
    }
}

static esp_err_t ensure_netif(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!s_sta_netif) {
            s_sta_netif = esp_netif_create_default_wifi_sta();
        }
    }
    return s_sta_netif ? ESP_OK : ESP_FAIL;
}

static void reset_ws_rx(WsContext *ctx)
{
    if (!ctx) {
        return;
    }
    free(ctx->rx_buf);
    ctx->rx_buf = nullptr;
    ctx->rx_len = 0;
    ctx->rx_expected = 0;
    ctx->rx_cap = 0;
}

static void reset_transcribe_audio_batch()
{
    s_transcribe_audio_batch_len = 0;
}

static void reset_host_audio_batch()
{
    s_host_audio_batch_len = 0;
}

static void reset_audio_batch_for(WsContext *ctx)
{
    if (ctx == &s_transcribe) {
        reset_transcribe_audio_batch();
    } else if (ctx == &s_host) {
        reset_host_audio_batch();
    }
}

static bool take_audio_send_lock()
{
    return s_audio_send_lock != nullptr &&
           xSemaphoreTake(s_audio_send_lock, portMAX_DELAY) == pdTRUE;
}

static void give_audio_send_lock()
{
    if (s_audio_send_lock) {
        xSemaphoreGive(s_audio_send_lock);
    }
}

static void emit_ws_state(WsContext *ctx, clare_net_event_type_t type)
{
    emit_event(type);
    if (ctx && ctx->events) {
        if (type == CLARE_NET_EVENT_TRANSCRIBE_CONNECTED ||
            type == CLARE_NET_EVENT_HOST_CONNECTED) {
            xEventGroupSetBits(ctx->events, BIT0);
        } else if (type == CLARE_NET_EVENT_TRANSCRIBE_DISCONNECTED ||
                   type == CLARE_NET_EVENT_HOST_DISCONNECTED) {
            xEventGroupSetBits(ctx->events, BIT1);
        }
    }
}

static const char *json_text_field(const cJSON *root)
{
    static const char *const names[] = {"text", "content", "transcript", "delta", "answer"};
    for (const char *name : names) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
        if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    }
    return "";
}

static bool json_bool_field(const cJSON *root, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, names[i]);
        if (cJSON_IsTrue(item)) return true;
    }
    return false;
}

static void dispatch_ws_json(WsContext *ctx, const char *payload, size_t payload_len)
{
    if (!ctx || !payload || payload_len == 0) {
        return;
    }
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root) {
        report_error(ESP_ERR_INVALID_RESPONSE);
        return;
    }
    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = cJSON_IsString(type_item) ? type_item->valuestring : nullptr;
    if (!type) {
        cJSON_Delete(root);
        return;
    }
    // DIAG: mirror the reference's "recv %s (len=%d)" trace.
    ESP_LOGI(TAG, "recv kind=%d type=%s len=%u", static_cast<int>(ctx->kind),
             type, static_cast<unsigned>(payload_len));
    if (strcmp(type, "error") == 0) {
        // DIAG: log server-side error messages (was intentionally silent).
        cJSON *message_item = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *message = cJSON_IsString(message_item) ? message_item->valuestring : "";
        ESP_LOGW(TAG, "server error msg kind=%d: %s", static_cast<int>(ctx->kind), message);
        emit_event(CLARE_NET_EVENT_ERROR, message, nullptr, 0, false, 0, ESP_FAIL);
    } else if (ctx->kind == WsKind::Transcribe &&
        (strcmp(type, "transcript") == 0 || strcmp(type, "transcription") == 0 ||
         strcmp(type, "transcript_update") == 0 || strcmp(type, "partial_transcript") == 0 ||
         strcmp(type, "final_transcript") == 0)) {
        // Live transcription display is intentionally disabled: the
        // transcribe channel is send-only, matching the stable vocat
        // reference (transcribe_ws.c never reads).  The rx->UI path was the
        // source of the glyph-NULL panic and added rx work in the latency
        // critical client task.  Transcripts and the final summary are
        // fetched over HTTP (clare_net_get_understanding).
    } else if (ctx->kind == WsKind::Host) {
        if (strcmp(type, "transcription") == 0 || strcmp(type, "transcript") == 0) {
            static const char *const host_final_names[] = {"is_final", "final"};
            ESP_LOGI(TAG, "host transcription peek=%.60s", json_text_field(root));  // TEMP DIAG
            emit_event(CLARE_NET_EVENT_HOST_TRANSCRIPTION, json_text_field(root), nullptr, 0,
                       json_bool_field(root, host_final_names,
                                       sizeof(host_final_names) / sizeof(host_final_names[0])));
        } else if (strcmp(type, "answer_text") == 0 || strcmp(type, "answer_delta") == 0 ||
                   strcmp(type, "text_delta") == 0) {
            ESP_LOGI(TAG, "host answer_text peek=%.60s", json_text_field(root));  // TEMP DIAG
            static const char *const done_names[] = {"done", "is_final", "final"};
            emit_event(CLARE_NET_EVENT_HOST_ANSWER_TEXT, json_text_field(root), nullptr, 0,
                       json_bool_field(root, done_names, sizeof(done_names) / sizeof(done_names[0])),
                       0, ESP_OK, strcmp(type, "answer_delta") == 0 || strcmp(type, "text_delta") == 0);
        } else if (strcmp(type, "answer_audio") == 0) {
            cJSON *data_item = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsString(data_item) && data_item->valuestring) {
                const char *encoded = data_item->valuestring;
                size_t encoded_len = strlen(encoded);
                size_t decoded_cap = encoded_len / 4U * 3U + 4U;
                uint8_t *decoded = static_cast<uint8_t *>(malloc(decoded_cap));
                if (decoded) {
                    size_t decoded_len = 0;
                    int rc = mbedtls_base64_decode(decoded, decoded_cap, &decoded_len,
                                                   reinterpret_cast<const unsigned char *>(encoded),
                                                   encoded_len);
                    if (rc == 0 && decoded_len > 0) {
                        emit_event(CLARE_NET_EVENT_HOST_ANSWER_AUDIO, nullptr,
                                   decoded, decoded_len);
                    } else {
                        report_error(ESP_ERR_INVALID_RESPONSE);
                    }
                    free(decoded);
                } else {
                    report_error(ESP_ERR_NO_MEM);
                }
            }
        } else if (strcmp(type, "done") == 0) {
            emit_event(CLARE_NET_EVENT_HOST_DONE);
        }
    }
    cJSON_Delete(root);
}

static void append_json_text(char *out, size_t out_len, const char *label, const char *value)
{
    if (!value || !value[0] || strlen(out) >= out_len - 1) return;
    size_t used = strlen(out);
    snprintf(out + used, out_len - used, "%s%s\n", label ? label : "", value);
}

static void append_json_array(char *out, size_t out_len, const char *label, const cJSON *array)
{
    if (!cJSON_IsArray(array)) return;
    for (const cJSON *item = array->child; item; item = item->next) {
        if (cJSON_IsString(item)) append_json_text(out, out_len, label, item->valuestring);
        else if (cJSON_IsObject(item)) {
            static const char *const names[] = {
                "text", "content", "headline", "title", "name", "summary",
                "description", "decision", "meetingGoal",
            };
            const cJSON *text = nullptr;
            for (const char *name : names) {
                text = cJSON_GetObjectItemCaseSensitive(item, name);
                if (cJSON_IsString(text)) break;
            }
            if (cJSON_IsString(text)) append_json_text(out, out_len, label, text->valuestring);
        }
    }
}

static void ws_event_handler(void *arg, esp_event_base_t, int32_t event_id, void *event_data)
{
    WsContext *ctx = static_cast<WsContext *>(arg);
    if (!ctx) {
        return;
    }
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ctx->connected = true;
        if (ctx->events) {
            xEventGroupClearBits(ctx->events, BIT1);
        }
        ESP_LOGI(TAG, "WebSocket connected kind=%d", static_cast<int>(ctx->kind));
        emit_ws_state(ctx, ctx->kind == WsKind::Transcribe
                              ? CLARE_NET_EVENT_TRANSCRIBE_CONNECTED
                              : CLARE_NET_EVENT_HOST_CONNECTED);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        ctx->connected = false;
        int close_status = 0;
        int error_type = 0;
        int socket_errno = 0;
        if (event_data) {
            const esp_websocket_event_data_t *data =
                static_cast<const esp_websocket_event_data_t *>(event_data);
            // esp_websocket_client is pinned to 1.5.0 (the S3-proven release);
            // close_status_code exists only in >=1.8.0.
            close_status = 0;
            error_type = static_cast<int>(data->error_handle.error_type);
            socket_errno = data->error_handle.esp_transport_sock_errno;
        }
        ESP_LOGI(TAG,
                 "WebSocket disconnected kind=%d event=%ld close=%d type=%d errno=%d",
                 static_cast<int>(ctx->kind), static_cast<long>(event_id), close_status,
                 error_type, socket_errno);
        if (ctx->events) {
            xEventGroupSetBits(ctx->events, BIT1);
        }
        emit_ws_state(ctx, ctx->kind == WsKind::Transcribe
                              ? CLARE_NET_EVENT_TRANSCRIBE_DISCONNECTED
                              : CLARE_NET_EVENT_HOST_DISCONNECTED);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        // TEMP DIAGNOSTIC: heap state at handshake failure (remove after use)
        ESP_LOGW(TAG, "WS ERR heap free8=%u largest8=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        esp_err_t err = ESP_FAIL;
        int status = 0;
        if (event_data) {
            esp_websocket_event_data_t *data = static_cast<esp_websocket_event_data_t *>(event_data);
            err = data->error_handle.esp_tls_last_esp_err;
            if (err == ESP_OK) {
                err = ESP_FAIL;
            }
            status = data->error_handle.esp_ws_handshake_status_code;
            // TEMP DIAGNOSTIC: full error detail for the WSS failure hunt
            // (remove after use)
            ESP_LOGW(TAG, "WS ERR detail kind=%d stack_err=0x%x flags=0x%x type=%d errno=%d",
                     static_cast<int>(ctx->kind),
                     data->error_handle.esp_tls_stack_err,
                     data->error_handle.esp_tls_cert_verify_flags,
                     static_cast<int>(data->error_handle.error_type),
                     data->error_handle.esp_transport_sock_errno);
        }
        if (ctx->events) {
            xEventGroupSetBits(ctx->events, BIT1);
        }
        ESP_LOGW(TAG, "WebSocket error kind=%d err=%d status=%d",
                 static_cast<int>(ctx->kind), static_cast<int>(err), status);
        if (status == 403 && ctx->kind == WsKind::Host) {
            // Server-side session rejection: the session id is no longer
            // valid. The application must recreate the session.  NOTE: only
            // the host channel drives session recreation — a transcribe 403
            // used to wrongly trigger the host recreate flow and hijack the
            // meeting's session id (logs/clare_s3_acceptance2_20260829.log);
            // the transcribe supervisor owns meeting-channel recovery.
            emit_event(CLARE_NET_EVENT_HOST_SESSION_REJECTED, nullptr, nullptr, 0,
                       false, status, err);
        } else {
            report_error(err, status);
        }
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || !event_data) {
        return;
    }
    esp_websocket_event_data_t *data = static_cast<esp_websocket_event_data_t *>(event_data);
    if (data->op_code != 0x1 || !data->data_ptr || data->data_len <= 0) {
        return;
    }
    size_t offset = data->payload_offset > 0 ? static_cast<size_t>(data->payload_offset) : 0;
    size_t frame_len = static_cast<size_t>(data->data_len);
    size_t expected = data->payload_len > 0 ? static_cast<size_t>(data->payload_len) : frame_len;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data->data_ptr);
    bool msg_complete = data->fin && (offset + frame_len >= expected);

    if (ctx->kind == WsKind::Host) {
        // Decide once per message whether this is a streamable answer_audio.
        if (offset == 0) {
            ctx->tts_mode = false;
            ctx->tts_probe_done = false;
            ctx->probe_len = 0;
            tts_b64_reset(ctx);
        }
        if (!ctx->tts_probe_done) {
            size_t need = frame_len < (kTtsPrefixLen - ctx->probe_len)
                              ? frame_len
                              : (kTtsPrefixLen - ctx->probe_len);
            memcpy(ctx->probe + ctx->probe_len, bytes, need);
            ctx->probe_len += need;
            bytes += need;
            frame_len -= need;
            if (memcmp(ctx->probe, kTtsPrefix, ctx->probe_len) != 0) {
                ctx->tts_probe_done = true;   // mismatch: plain JSON message
            } else if (ctx->probe_len >= kTtsPrefixLen) {
                ctx->tts_probe_done = true;
                ctx->tts_mode = true;
                emit_event(CLARE_NET_EVENT_HOST_TTS_START);
            } else {
                return;   // prefix still split across fragments; wait for more
            }
            // Seed the normal reassembly buffer with the probe bytes so the
            // non-audio path sees the complete message.
            if (!ctx->tts_mode) {
                ctx->rx_len = 0;
                ctx->rx_expected = expected;
                size_t required_cap = expected + 1U;
                if (!ctx->rx_buf || ctx->rx_cap < required_cap) {
                    char *expanded = static_cast<char *>(realloc(ctx->rx_buf, required_cap));
                    if (!expanded) { report_error(ESP_ERR_NO_MEM); return; }
                    ctx->rx_buf = expanded;
                    ctx->rx_cap = required_cap;
                }
                memcpy(ctx->rx_buf, ctx->probe, ctx->probe_len);
                ctx->rx_len = ctx->probe_len;
                offset = ctx->probe_len;
            } else {
                offset = kTtsPrefixLen;
                // DIAG: confirm TTS audio messages actually arrive.
                ESP_LOGI(TAG, "tts stream begin expected=%u",
                         static_cast<unsigned>(expected));
            }
        }
        if (ctx->tts_mode) {
            tts_b64_feed(ctx, bytes, frame_len, msg_complete);
            if (msg_complete) {
                ESP_LOGI(TAG, "tts stream end");
                emit_event(CLARE_NET_EVENT_HOST_TTS_END);
            }
            return;
        }
    }

    if (offset == 0) {
        ctx->rx_len = 0;
        ctx->rx_expected = expected;
    }
    if (expected > WS_RX_MAX || offset > WS_RX_MAX || frame_len > WS_RX_MAX - offset) {
        // DIAG: an oversized non-TTS message usually means the TTS prefix
        // probe failed to recognise a new server audio format — log a peek.
        ESP_LOGW(TAG, "rx oversize kind=%d expected=%u offset=%u frame=%u peek=%.60s",
                 static_cast<int>(ctx->kind), static_cast<unsigned>(expected),
                 static_cast<unsigned>(offset), static_cast<unsigned>(frame_len),
                 data->data_ptr ? reinterpret_cast<const char *>(data->data_ptr) : "");
        reset_ws_rx(ctx);
        report_error(ESP_ERR_NO_MEM);
        return;
    }
    size_t required_cap = expected + 1U;
    if (!ctx->rx_buf || ctx->rx_cap < required_cap) {
        char *expanded = static_cast<char *>(realloc(ctx->rx_buf, required_cap));
        if (!expanded) {
            report_error(ESP_ERR_NO_MEM);
            return;
        }
        ctx->rx_buf = expanded;
        ctx->rx_cap = required_cap;
    }
    // NOTE: must copy from the advanced cursor `bytes`, not data->data_ptr —
    // the TTS probe already consumed `need` bytes from the front of this
    // frame, and re-copying from the frame start duplicated the head of the
    // message and corrupted every host JSON >27 bytes (parse fail per
    // message -> "Network error" spam, no answer text/TTS,
    // logs/clare_s3_tts_check_20260829.log).
    memcpy(ctx->rx_buf + offset, bytes, frame_len);
    if (offset + frame_len > ctx->rx_len) {
        ctx->rx_len = offset + frame_len;
    }
    bool complete = data->fin && (ctx->rx_len >= ctx->rx_expected || ctx->rx_expected == 0);
    if (complete) {
        ctx->rx_buf[ctx->rx_len] = '\0';
        dispatch_ws_json(ctx, ctx->rx_buf, ctx->rx_len);
        ctx->rx_len = 0;
        ctx->rx_expected = 0;
    }
}

static esp_err_t ws_send_text_with_timeout(WsContext *ctx, const char *text,
                                            TickType_t timeout)
{
    if (!ctx || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_websocket_client_handle_t client = nullptr;
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    client = ctx->client;
    bool connected = ctx->connected;
    give_lock();
    if (!client || !connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int len = static_cast<int>(strlen(text));
    int sent = esp_websocket_client_send_text(client, text, len, timeout);
    return sent == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t ws_send_text(WsContext *ctx, const char *text)
{
    return ws_send_text_with_timeout(ctx, text, pdMS_TO_TICKS(1000));
}

static esp_err_t ws_send_audio_json(WsContext *ctx, const void *pcm, size_t pcm_len)
{
    if (!ctx || !pcm || pcm_len == 0 || pcm_len > TRANSCRIBE_AUDIO_BATCH_MAX || (pcm_len & 1U) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // Do not attempt a send when internal RAM is so fragmented that the
    // largest free block is below our threshold.  Pushing data into TLS/Wi-Fi
    // in this state is what caused bcn_timeout disconnects.
    const size_t largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest_free < AUDIO_SEND_MIN_HEAP_BLOCK) {
        ESP_LOGW(TAG, "audio send skipped: largest free block %u < %u",
                 static_cast<unsigned>(largest_free),
                 static_cast<unsigned>(AUDIO_SEND_MIN_HEAP_BLOCK));
        return ESP_ERR_NO_MEM;
    }
    // Build the complete JSON message in one buffer, then send it as a
    // single unfragmented text frame.  The payload is byte-identical to the
    // proven S3 reference — {"type":"audio","data":"<b64>"} with NO extra
    // fields: the backend's audio pipeline is schema-strict and killed the
    // TLS session (fatal alert -> Connection lost) after enough audio
    // messages carrying the extra "speaker" field
    // (logs/clare_s3_ask_diag3_20260829.log).
    static constexpr char prefix[] = "{\"type\":\"audio\",\"data\":\"";
    static constexpr char suffix[] = "\"}";
    memcpy(s_audio_json, prefix, sizeof(prefix) - 1U);
    size_t encoded_len = 0;
    int rc = mbedtls_base64_encode(
        reinterpret_cast<unsigned char *>(s_audio_json + sizeof(prefix) - 1U),
        AUDIO_JSON_MAX - (sizeof(prefix) - 1U), &encoded_len,
        static_cast<const unsigned char *>(pcm), pcm_len);
    if (rc != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t jlen = sizeof(prefix) - 1U + encoded_len;
    if (jlen + sizeof(suffix) - 1U > AUDIO_JSON_MAX || jlen > INT_MAX) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_audio_json + jlen, suffix, sizeof(suffix) - 1U);
    jlen += sizeof(suffix) - 1U;

    esp_websocket_client_handle_t client = nullptr;
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    client = ctx->client;
    bool connected = ctx->connected;
    give_lock();
    if (!client || !connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_text(client, s_audio_json,
                                              static_cast<int>(jlen),
                                              pdMS_TO_TICKS(WS_AUDIO_SEND_TIMEOUT_MS));
    return sent == static_cast<int>(jlen) ? ESP_OK : ESP_FAIL;
}

static esp_err_t flush_transcribe_audio_batch()
{
    if (s_transcribe_audio_batch_len == 0) {
        return ESP_OK;
    }
    esp_err_t err = ws_send_audio_json(&s_transcribe, s_transcribe_audio_batch,
                                       s_transcribe_audio_batch_len);
    if (err == ESP_OK) {
        reset_transcribe_audio_batch();
    }
    return err;
}

static esp_err_t flush_host_audio_batch()
{
    if (s_host_audio_batch_len == 0) {
        return ESP_OK;
    }
    esp_err_t err = ws_send_audio_json(&s_host, s_host_audio_batch, s_host_audio_batch_len);
    if (err == ESP_OK) {
        reset_host_audio_batch();
    }
    return err;
}

static esp_err_t ws_cleanup(WsContext *ctx)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_websocket_client_handle_t client = nullptr;
    if (take_lock()) {
        client = ctx->client;
        ctx->client = nullptr;
        ctx->connected = false;
        give_lock();
    }
    if (client) {
        // stop/destroy are intentionally called outside the event callback.
        // Hold the audio-send lock across the whole teardown: an in-flight
        // audio send must never touch a client that is being stopped or
        // freed.  Without this, destroy() raced the audio task's TLS write
        // and surfaced as mbedtls conf==NULL write errors (and, with dynamic
        // SSL buffers, as LoadProhibited panics) — see
        // logs/clare_s3_mbedtls_fix_live_20260829.log.
        const bool send_locked = take_audio_send_lock();
        (void)esp_websocket_client_stop(client);
        (void)esp_websocket_client_destroy(client);
        if (send_locked) {
            give_audio_send_lock();
        }
        // The WebSocket task's stack is reclaimed by the FreeRTOS idle task
        // after it self-deletes. Yield briefly before a rapid retry so that
        // failed handshakes do not accumulate detached task stacks.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    reset_ws_rx(ctx);
    // Do not clear a batch while the audio task is copying or sending it.
    // Cleanup normally follows stop_audio_task(), but the lock also covers
    // transport-error races where the final send is still unwinding.
    if (take_audio_send_lock()) {
        reset_audio_batch_for(ctx);
        give_audio_send_lock();
    } else {
        reset_audio_batch_for(ctx);
    }
    if (ctx->events) {
        xEventGroupClearBits(ctx->events, BIT0 | BIT1);
    }
    return ESP_OK;
}

static esp_err_t ws_connect(WsContext *ctx, const char *session_id)
{
    if (!ctx || !valid_session_id(session_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || !s_wifi_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (take_audio_send_lock()) {
        reset_audio_batch_for(ctx);
        give_audio_send_lock();
    } else {
        reset_audio_batch_for(ctx);
    }
    char path[160];
    int path_len = snprintf(path, sizeof(path), "%s/%s",
                            ctx->kind == WsKind::Transcribe ? "ws/transcribe" : "ws/host",
                            session_id);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    char uri[URL_MAX];
    if (!make_ws_url(uri, sizeof(uri), path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->events) {
        ctx->events = xEventGroupCreate();
        if (!ctx->events) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    if (ctx->client) {
        give_lock();
        return ESP_ERR_INVALID_STATE;
    }
    ctx->connected = false;
    xEventGroupClearBits(ctx->events, BIT0 | BIT1);
    give_lock();

    esp_websocket_client_config_t config = {};
    config.uri = uri;
    // Buffer must exceed the largest single message (audio JSON ~4.3 KB) so
    // every frame goes out unfragmented — the backend drops fragmented
    // streams.  Matches the proven S3 reference; the old 1 KB value was a C6
    // internal-RAM workaround that forced continuation frames.
    config.buffer_size = 16384;
    config.task_stack = 10240;
    config.task_prio = 10;
    // 5 s network timeout matches the proven S3 reference: the client task's
    // read never blocks longer than that, so PINGs and close handling stay
    // timely (the old 60 s value stalled the whole ws state machine).
    config.network_timeout_ms = 5000;
    config.reconnect_timeout_ms = 10000;
    config.disable_auto_reconnect = true;
    config.skip_cert_common_name_check = false;
#if !CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    config.cert_pem = kClareCaChainPem;
    // esp_websocket_client treats non-zero cert_len as DER. A PEM chain must
    // be NUL terminated and use zero length so the PEM transport API is used.
    config.cert_len = 0;
#endif
    // With CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y no CA may be passed (see
    // fill_http_config): esp-tls only honors VERIFY_NONE when cacert is NULL.
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                                  ws_event_handler, ctx);
    if (err == ESP_OK && take_lock()) {
        ctx->client = client;
        give_lock();
    } else if (err == ESP_OK) {
        err = ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        // TEMP DIAGNOSTIC: heap state before WS start (remove after use)
        ESP_LOGI(TAG, "WS pre-start heap free8=%u largest8=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        err = esp_websocket_client_start(client);
    }
    if (err != ESP_OK) {
        if (take_lock()) {
            if (ctx->client == client) {
                ctx->client = nullptr;
            }
            give_lock();
        }
        (void)esp_websocket_client_destroy(client);
        report_error(err);
        return err;
    }
    // Wait must comfortably exceed config.network_timeout_ms (5 s) plus the
    // TLS handshake: destroying the client mid-handshake leaks its
    // half-built TLS context and the retry then fails with
    // MBEDTLS_ERR_PK_ALLOC_FAILED (logs/clare_sw_aes_20260828.log).
    EventBits_t bits = xEventGroupWaitBits(ctx->events, BIT0 | BIT1, pdFALSE,
                                           pdFALSE, pdMS_TO_TICKS(30000));
    if ((bits & BIT0) == 0) {
        ESP_LOGW(TAG, "WebSocket connect timeout kind=%d bits=0x%lx",
                 static_cast<int>(ctx->kind), static_cast<unsigned long>(bits));
        (void)ws_cleanup(ctx);
        return (bits & BIT1) ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

} // namespace

extern "C" esp_err_t clare_net_format_understanding(const char *json,
                                                      char *out_text,
                                                      size_t out_text_len)
{
    if (!json || !out_text || out_text_len == 0) return ESP_ERR_INVALID_ARG;
    out_text[0] = '\0';
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *overview = cJSON_GetObjectItemCaseSensitive(root, "overview");
    const cJSON *understanding = cJSON_GetObjectItemCaseSensitive(root, "understanding");
    const cJSON *decision_state = cJSON_GetObjectItemCaseSensitive(root, "decisionState");
    const cJSON *summary = cJSON_GetObjectItemCaseSensitive(root, "summary");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    const cJSON *headline = cJSON_IsObject(overview)
                                ? cJSON_GetObjectItemCaseSensitive(overview, "headline")
                                : nullptr;
    const cJSON *meeting_goal = cJSON_IsObject(understanding)
                                    ? cJSON_GetObjectItemCaseSensitive(understanding, "meetingGoal")
                                    : nullptr;
    const cJSON *topics = cJSON_IsObject(understanding)
                              ? cJSON_GetObjectItemCaseSensitive(understanding, "topics")
                              : nullptr;
    const cJSON *threads = cJSON_IsObject(decision_state)
                               ? cJSON_GetObjectItemCaseSensitive(decision_state, "threads")
                               : nullptr;
    const cJSON *highlights = cJSON_GetObjectItemCaseSensitive(root, "highlights");
    if (!highlights) highlights = cJSON_GetObjectItemCaseSensitive(root, "key_points");
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(root, "action_items");
    if (!actions) actions = cJSON_GetObjectItemCaseSensitive(root, "next_steps");
    const cJSON *decisions = cJSON_GetObjectItemCaseSensitive(root, "decisions");
    append_json_text(out_text, out_text_len, "", cJSON_IsString(title) ? title->valuestring : nullptr);
    append_json_text(out_text, out_text_len, "", cJSON_IsString(headline) ? headline->valuestring : nullptr);
    append_json_text(out_text, out_text_len, "Goal: ", cJSON_IsString(meeting_goal) ? meeting_goal->valuestring : nullptr);
    append_json_text(out_text, out_text_len, "Summary: ", cJSON_IsString(summary) ? summary->valuestring : nullptr);
    append_json_array(out_text, out_text_len, "• ", highlights);
    append_json_array(out_text, out_text_len, "• ", topics);
    append_json_array(out_text, out_text_len, "• ", decisions);
    append_json_array(out_text, out_text_len, "• ", actions);
    append_json_array(out_text, out_text_len, "• ", threads);
    if (!out_text[0]) {
        const cJSON *ready = cJSON_GetObjectItemCaseSensitive(root, "ready");
        const cJSON *count = cJSON_GetObjectItemCaseSensitive(root, "transcriptCount");
        append_json_text(out_text, out_text_len, "", cJSON_IsTrue(ready)
                                                    ? "Meeting analysis is ready."
                                                    : "Meeting analysis is still in progress.");
        if (cJSON_IsNumber(count)) {
            size_t used = strlen(out_text);
            snprintf(out_text + used, out_text_len - used, "Transcript entries: %d\n", count->valueint);
        }
    }
    cJSON_Delete(root);
    return out_text[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
}

extern "C" esp_err_t clare_net_init(const clare_net_config_t *config)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_audio_send_lock) {
        s_audio_send_lock = xSemaphoreCreateMutex();
        if (!s_audio_send_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (take_lock()) {
        if (config) {
            s_event_cb = config->event_cb;
            s_event_ctx = config->ctx;
        }
        give_lock();
    }
    if (s_initialized) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    err = ensure_netif();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        if (!s_wifi_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        return err;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, nullptr,
                                              &s_wifi_handler);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, nullptr,
                                              &s_ip_handler);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_initialized = true;
    return ESP_OK;
}

extern "C" esp_err_t clare_net_wifi_start(void)
{
    esp_err_t err = clare_net_init(nullptr);
    if (err != ESP_OK) {
        return err;
    }
    if (CONFIG_CLARE_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID is not configured");
        emit_event(CLARE_NET_EVENT_WIFI_FAILED, nullptr, nullptr, 0, false, 0,
                   ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wifi_started) {
        if (!s_wifi_connected) {
            s_wifi_retries = 0;
            if (s_wifi_events) {
                xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
            }
            (void)esp_wifi_connect();
            emit_event(CLARE_NET_EVENT_WIFI_CONNECTING);
        }
        return ESP_OK;
    }
    wifi_config_t wifi_cfg = {};
    strlcpy(reinterpret_cast<char *>(wifi_cfg.sta.ssid), CONFIG_CLARE_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi_cfg.sta.password), CONFIG_CLARE_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        return err;
    }
    constexpr uint8_t kStaProtocols = WIFI_PROTOCOL_11B |
                                      WIFI_PROTOCOL_11G |
                                      WIFI_PROTOCOL_11N;
    err = esp_wifi_set_protocol(WIFI_IF_STA, kStaProtocols);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Wi-Fi protocols err=%d", static_cast<int>(err));
        return err;
    }
    ESP_LOGI(TAG, "Wi-Fi protocols limited to 11b/g/n mask=0x%02x", kStaProtocols);
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable Wi-Fi power save err=%d", static_cast<int>(err));
    }
    s_wifi_started = true;
    s_wifi_retries = 0;
    s_wifi_connected = false;
    emit_event(CLARE_NET_EVENT_WIFI_CONNECTING);
    // WIFI_EVENT_STA_START owns the first connect call. Calling it here too
    // races the event callback and produces ESP_ERR_WIFI_CONN on boot.
    return ESP_OK;
}

extern "C" esp_err_t clare_net_wifi_connect(uint32_t timeout_ms)
{
    esp_err_t err = clare_net_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    if (s_wifi_connected) {
        return ESP_OK;
    }
    if (!s_wifi_events) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms ? timeout_ms : 30000));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_FAILED_BIT) {
        emit_event(CLARE_NET_EVENT_WIFI_FAILED, nullptr, nullptr, 0, false, 0, ESP_FAIL);
        return ESP_FAIL;
    }
    emit_event(CLARE_NET_EVENT_WIFI_FAILED, nullptr, nullptr, 0, false, 0, ESP_ERR_TIMEOUT);
    return ESP_ERR_TIMEOUT;
}


extern "C" bool clare_net_wifi_is_connected(void)
{
    return s_wifi_connected;
}

extern "C" esp_err_t clare_net_create_session(const char *topic,
                                                char *out_session_id,
                                                size_t out_session_id_len)
{
    if (!out_session_id || out_session_id_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *effective_topic = (topic && topic[0]) ? topic : CONFIG_CLARE_TOPIC;
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (effective_topic && effective_topic[0] &&
        !cJSON_AddStringToObject(root, "topic", effective_topic)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    char url[URL_MAX];
    char response[SESSION_RESPONSE_MAX] = {};
    int status = 0;
    bool truncated = false;
    bool url_ok = make_http_url(url, sizeof(url), "api/session");
    ESP_LOGI(TAG, "Create session URL ready=%d", url_ok ? 1 : 0);
    esp_err_t err = url_ok
                        ? http_perform(url, HTTP_METHOD_POST, "application/json", body,
                                       response, sizeof(response), &status, &truncated)
                        : ESP_ERR_INVALID_ARG;
    cJSON_free(body);
    if (err != ESP_OK) {
        report_error(err, status);
        return err;
    }
    if (truncated) {
        report_error(ESP_ERR_NO_MEM, status);
        return ESP_ERR_NO_MEM;
    }
    cJSON *reply = cJSON_ParseWithLength(response, strlen(response));
    if (!reply) {
        ESP_LOGW(TAG, "Session response JSON parse failed bytes=%u",
                 static_cast<unsigned>(strlen(response)));
        report_error(ESP_ERR_INVALID_RESPONSE, status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(reply, "session_id");
    const char *id = cJSON_IsString(id_item) ? id_item->valuestring : nullptr;
    if (!valid_session_id(id)) {
        ESP_LOGW(TAG, "Session response missing valid session_id");
        cJSON_Delete(reply);
        report_error(ESP_ERR_INVALID_RESPONSE, status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t id_len = strlen(id);
    if (id_len >= out_session_id_len || id_len >= sizeof(s_session_id)) {
        cJSON_Delete(reply);
        report_error(ESP_ERR_NO_MEM, status);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(out_session_id, id, out_session_id_len);
    if (take_lock()) {
        strlcpy(s_session_id, id, sizeof(s_session_id));
        give_lock();
    }
    cJSON_Delete(reply);
    emit_event(CLARE_NET_EVENT_SESSION_CREATED);
    return ESP_OK;
}

extern "C" esp_err_t clare_net_end_session(const char *session_id)
{
    const char *id = session_id;
    if (!id || !id[0]) {
        id = s_session_id;
    }
    if (!valid_session_id(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[160];
    int path_len = snprintf(path, sizeof(path), "api/session/%s/end", id);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[URL_MAX];
    if (!make_http_url(url, sizeof(url), path)) {
        return ESP_ERR_INVALID_ARG;
    }
    char response[END_RESPONSE_MAX] = {};
    int status = 0;
    esp_err_t err = http_perform(url, HTTP_METHOD_POST, nullptr, nullptr,
                                 response, sizeof(response), &status, nullptr);
    if (err != ESP_OK) {
        report_error(err, status);
        return err;
    }
    emit_event(CLARE_NET_EVENT_SESSION_ENDED, nullptr, nullptr, 0, false, status);
    return ESP_OK;
}

extern "C" esp_err_t clare_net_get_understanding(const char *session_id,
                                                   char *out_json,
                                                   size_t out_json_len)
{
    const char *id = session_id;
    if (!id || !id[0]) {
        id = s_session_id;
    }
    if (!valid_session_id(id) || !out_json || out_json_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[192];
    int path_len = snprintf(path, sizeof(path), "api/session/%s/understanding", id);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[URL_MAX];
    if (!make_http_url(url, sizeof(url), path)) {
        return ESP_ERR_INVALID_ARG;
    }
    int status = 0;
    bool truncated = false;
    esp_err_t err = http_perform(url, HTTP_METHOD_GET, nullptr, nullptr,
                                 out_json, out_json_len, &status, &truncated);
    if (err != ESP_OK) {
        report_error(err, status);
        return err;
    }
    if (truncated) {
        report_error(ESP_ERR_NO_MEM, status);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" esp_err_t clare_net_transcribe_connect(const char *session_id)
{
    return ws_connect(&s_transcribe, session_id ? session_id : s_session_id);
}

extern "C" esp_err_t clare_net_transcribe_send_audio(const void *pcm, size_t pcm_len)
{
    if (!pcm || pcm_len == 0 || pcm_len > INT_MAX || (pcm_len & 1U) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_audio_send_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    const uint8_t *input = static_cast<const uint8_t *>(pcm);
    esp_err_t result = ESP_OK;
    size_t remaining = pcm_len;
    while (remaining > 0) {
        size_t room = TRANSCRIBE_AUDIO_BATCH_MAX - s_transcribe_audio_batch_len;
        size_t copy_len = remaining < room ? remaining : room;
        memcpy(s_transcribe_audio_batch + s_transcribe_audio_batch_len, input, copy_len);
        s_transcribe_audio_batch_len += copy_len;
        input += copy_len;
        remaining -= copy_len;
        if (s_transcribe_audio_batch_len == TRANSCRIBE_AUDIO_BATCH_MAX) {
            esp_err_t err = flush_transcribe_audio_batch();
            if (err != ESP_OK) {
                reset_transcribe_audio_batch();
                result = err;
                break;
            }
        }
    }
    give_audio_send_lock();
    return result;
}

extern "C" esp_err_t clare_net_transcribe_send_end(void)
{
    if (!take_audio_send_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = flush_transcribe_audio_batch();
    if (err != ESP_OK) {
        reset_transcribe_audio_batch();
        give_audio_send_lock();
        return err;
    }
    err = ws_send_text(&s_transcribe, "{\"type\":\"end\"}");
    give_audio_send_lock();
    return err;
}

extern "C" esp_err_t clare_net_transcribe_flush(void)
{
    // Flush only the buffered audio tail (a regular audio message, same
    // protocol) WITHOUT the {"type":"end"} signal: on the server, "end" marks
    // the session ended and the host/Q&A channel then rejects it (403), which
    // killed post-meeting questions' meeting context.
    if (!take_audio_send_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = flush_transcribe_audio_batch();
    if (err != ESP_OK) {
        reset_transcribe_audio_batch();
    }
    give_audio_send_lock();
    return err;
}

extern "C" esp_err_t clare_net_transcribe_disconnect(void)
{
    return ws_cleanup(&s_transcribe);
}

extern "C" esp_err_t clare_net_host_connect(const char *session_id)
{
    return ws_connect(&s_host, session_id ? session_id : s_session_id);
}

extern "C" esp_err_t clare_net_host_send_audio(const void *pcm, size_t pcm_len)
{
    if (!pcm || pcm_len == 0 || pcm_len > INT_MAX || (pcm_len & 1U) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_audio_send_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    const uint8_t *input = static_cast<const uint8_t *>(pcm);
    size_t remaining = pcm_len;
    esp_err_t result = ESP_OK;
    while (remaining > 0) {
        size_t room = HOST_AUDIO_BATCH_MAX - s_host_audio_batch_len;
        size_t copy_len = remaining < room ? remaining : room;
        memcpy(s_host_audio_batch + s_host_audio_batch_len, input, copy_len);
        s_host_audio_batch_len += copy_len;
        input += copy_len;
        remaining -= copy_len;
        if (s_host_audio_batch_len == HOST_AUDIO_BATCH_MAX) {
            esp_err_t err = flush_host_audio_batch();
            if (err != ESP_OK) {
                reset_host_audio_batch();
                result = err;
                break;
            }
        }
    }
    give_audio_send_lock();
    return result;
}

extern "C" esp_err_t clare_net_host_send_end_of_speech(void)
{
    if (!take_audio_send_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = flush_host_audio_batch();
    if (err == ESP_OK) {
        err = ws_send_text(&s_host, "{\"type\":\"end_of_speech\"}");
    }
    give_audio_send_lock();
    return err;
}

extern "C" esp_err_t clare_net_host_send_stop(void)
{
    if (!take_audio_send_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    reset_host_audio_batch();
    esp_err_t err = ws_send_text(&s_host, "{\"type\":\"stop\"}");
    give_audio_send_lock();
    return err;
}

extern "C" esp_err_t clare_net_host_disconnect(void)
{
    return ws_cleanup(&s_host);
}
