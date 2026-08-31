#pragma once

// Private Clare transport for the C6 application.
//
// The API deliberately keeps credentials and meeting contents out of log
// messages.  Event payload pointers are only valid for the duration of the
// callback; callers that need them after returning must copy them.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLARE_NET_EVENT_WIFI_CONNECTING = 0,
    CLARE_NET_EVENT_WIFI_CONNECTED,
    CLARE_NET_EVENT_WIFI_DISCONNECTED,
    CLARE_NET_EVENT_WIFI_FAILED,
    CLARE_NET_EVENT_SESSION_CREATED,
    CLARE_NET_EVENT_SESSION_ENDED,
    CLARE_NET_EVENT_TRANSCRIBE_CONNECTED,
    CLARE_NET_EVENT_TRANSCRIBE_DISCONNECTED,
    CLARE_NET_EVENT_TRANSCRIPT,
    CLARE_NET_EVENT_HOST_CONNECTED,
    CLARE_NET_EVENT_HOST_DISCONNECTED,
    CLARE_NET_EVENT_HOST_TRANSCRIPTION,
    CLARE_NET_EVENT_HOST_ANSWER_TEXT,
    CLARE_NET_EVENT_HOST_ANSWER_AUDIO,
    CLARE_NET_EVENT_HOST_DONE,
    // Streaming TTS demarcation for answer_audio sentences. binary payloads on
    // HOST_ANSWER_AUDIO are decoded MP3 bytes between START and END.
    CLARE_NET_EVENT_HOST_TTS_START,
    CLARE_NET_EVENT_HOST_TTS_END,
    // The server rejected the host channel (HTTP 403): the session is invalid
    // and the application must recreate it.
    CLARE_NET_EVENT_HOST_SESSION_REJECTED,
    CLARE_NET_EVENT_ERROR,
} clare_net_event_type_t;

typedef struct {
    clare_net_event_type_t type;
    // Text is UTF-8 and is NUL terminated for text events.  It is NULL for
    // events that only carry binary data.
    const char *text;
    // Binary is used for decoded host answer audio (MP3).  It is NULL for
    // other events.  The buffer is callback-scoped.
    const uint8_t *binary;
    size_t binary_len;
    bool is_final;
    // HTTP status or WebSocket close status when applicable; zero otherwise.
    int status_code;
    esp_err_t error;
    // True when text is a stream delta that must be appended rather than a
    // cumulative partial value that replaces the previous partial value.
    bool is_delta;
} clare_net_event_t;

typedef void (*clare_net_event_cb_t)(const clare_net_event_t *event, void *ctx);

// Callbacks run on the Wi-Fi/WebSocket/HTTP worker that produced the event;
// they are not guaranteed to run on the LVGL task.  UI callbacks should
// marshal updates through the board's LVGL lock/async mechanism.

typedef struct {
    clare_net_event_cb_t event_cb;
    void *ctx;
} clare_net_config_t;

// Initialise the Wi-Fi/netif event plumbing.  This is idempotent.
esp_err_t clare_net_init(const clare_net_config_t *config);

// Start STA mode and connect using CONFIG_CLARE_WIFI_SSID/PASSWORD.  The
// password is never logged.  The wait call returns ESP_OK once an IPv4
// address has been assigned.
esp_err_t clare_net_wifi_start(void);
esp_err_t clare_net_wifi_connect(uint32_t timeout_ms);
bool clare_net_wifi_is_connected(void);

// Session HTTP API.  On success, out_session_id contains the ID and the
// module also stores it for subsequent WebSocket calls.
esp_err_t clare_net_create_session(const char *topic,
                                   char *out_session_id,
                                   size_t out_session_id_len);
esp_err_t clare_net_end_session(const char *session_id);
esp_err_t clare_net_get_understanding(const char *session_id,
                                      char *out_json,
                                      size_t out_json_len);
// Convert the understanding JSON into compact human-readable text. The
// output never contains the raw JSON envelope when known fields are present.
esp_err_t clare_net_format_understanding(const char *json,
                                         char *out_text,
                                         size_t out_text_len);

// Transcription WebSocket. Audio is 16 kHz, signed little-endian, mono PCM.
// The transport batches codec frames and sends the server's JSON/Base64 audio
// envelope so the backend is not flooded with 20 ms binary messages.
esp_err_t clare_net_transcribe_connect(const char *session_id);
esp_err_t clare_net_transcribe_send_audio(const void *pcm, size_t pcm_len);
esp_err_t clare_net_transcribe_send_end(void);
esp_err_t clare_net_transcribe_disconnect(void);

// Optional host/Q&A WebSocket.  This channel uses the documented JSON
// Base64 protocol because the host service expects typed messages.
esp_err_t clare_net_host_connect(const char *session_id);
esp_err_t clare_net_host_send_audio(const void *pcm, size_t pcm_len);
esp_err_t clare_net_host_send_end_of_speech(void);
esp_err_t clare_net_host_send_stop(void);
esp_err_t clare_net_host_disconnect(void);

// Convenience aliases used by older Clare integration code.
static inline esp_err_t clare_net_session_create(const char *topic,
                                                 char *id, size_t id_len)
{
    return clare_net_create_session(topic, id, id_len);
}
static inline esp_err_t clare_net_session_end(const char *id)
{
    return clare_net_end_session(id);
}
static inline esp_err_t clare_net_wifi_init(const clare_net_config_t *config)
{
    return clare_net_init(config);
}
static inline esp_err_t clare_net_wifi_wait(uint32_t timeout_ms)
{
    return clare_net_wifi_connect(timeout_ms);
}
static inline esp_err_t clare_net_transcribe_send_pcm(const void *pcm, size_t len)
{
    return clare_net_transcribe_send_audio(pcm, len);
}
static inline esp_err_t clare_net_transcribe_end(void)
{
    return clare_net_transcribe_send_end();
}
static inline esp_err_t clare_net_host_send_pcm(const void *pcm, size_t len)
{
    return clare_net_host_send_audio(pcm, len);
}

#ifdef __cplusplus
}
#endif
