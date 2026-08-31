#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <arpa/inet.h>
#include "esp_codec_dev.h"

#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_tls.h"

#include "lvgl_bsp.h"
#include "power_bsp.h"
#include "display_bsp.h"
#include "codec_bsp.h"
#include "clare_audio.h"
#include "clare_net.h"
#include "clare_ui.h"

#ifndef CONFIG_CLARE_BOOT_SELF_TEST
#define CONFIG_CLARE_BOOT_SELF_TEST 0
#endif
#ifndef CONFIG_CLARE_BOOT_SELF_TEST_RETRIES
#define CONFIG_CLARE_BOOT_SELF_TEST_RETRIES 3
#endif
#ifndef CONFIG_CLARE_BOOT_SELF_TEST_RETRY_DELAY_MS
#define CONFIG_CLARE_BOOT_SELF_TEST_RETRY_DELAY_MS 5000
#endif

static const char *TAG = "clare_s3";
static I2cMasterBus s_i2c(14, 15, 0);
static DisplayPort *s_display = nullptr;
static CodecPort *s_codec = nullptr;
static char s_session_id[96] = {};
static volatile bool s_meeting_active = false;
static volatile bool s_transcribe_connected = false;
static volatile bool s_host_connected = false;
static volatile bool s_host_recording = false;
static volatile TickType_t s_host_answer_since = 0;  // !=0 while waiting for an answer
static esp_timer_handle_t s_host_answer_timer = nullptr;
static volatile bool s_audio_task_run = false;
static TaskHandle_t s_audio_task = nullptr;
enum class Action : uint8_t {
    StartMeeting,
    StopMeeting,
    HandleMeetingDisconnect,
    HandleHostRejected,
    ToggleHost,
    RefreshSummary,
    FinishHost,
    CleanupHost,
#if CONFIG_CLARE_BOOT_SELF_TEST
    BootNetworkSelfTest,
#endif
};
static QueueHandle_t s_action_queue = nullptr;
static constexpr size_t kAudioFrameSamples = 320;  // 20 ms at 16 kHz
// ALC digital gain applied to the 16 kHz mono capture stream.  The codec's
// analog mic gain (35 dB) handles the baseline level; ALC adds a clipping-safe
// boost for quiet voices.  Range: (-64, 63] dB.
static constexpr int8_t kAlcGainDb = 6;
static int16_t s_audio_stereo[kAudioFrameSamples * 2] = {};
static int16_t s_audio_mono[kAudioFrameSamples] = {};

static void enqueue_action(Action action);

// If the server never answers (e.g. Send tapped with no speech recorded),
// finish the Q&A automatically after 60 s so the Ask button can never wedge.
static void host_answer_timeout(void *)
{
    if (s_host_answer_since != 0) {
        ESP_LOGW(TAG, "Answer watchdog: no done in 60 s, auto-finishing Q&A");
        enqueue_action(Action::FinishHost);
    }
}

static char s_host_answer_text[2048] = {};

static void ui_status(const char *text) { clare_ui_set_status(text); ESP_LOGI(TAG, "%s", text); }

// Accumulate answer chunks the way the proven reference does
// (vocat ws_session.c): a chunk that starts with everything we already have
// is a full cumulative snapshot -> replace; anything else is a delta ->
// append.  The old code overwrote the visible fragment with every chunk, so
// only a few characters ever showed on screen.
static void host_answer_feed(const char *chunk)
{
    if (!chunk) return;
    const size_t cur = strlen(s_host_answer_text);
    const size_t clen = strlen(chunk);
    if (clen == 0) {
        clare_ui_set_answer(s_host_answer_text);
        return;
    }
    const char *action = "append";
    if (cur > 0 && clen >= cur && strncmp(chunk, s_host_answer_text, cur) == 0) {
        strlcpy(s_host_answer_text, chunk, sizeof(s_host_answer_text));
        action = "snapshot-replace";
    } else if (cur + clen < sizeof(s_host_answer_text)) {
        memcpy(s_host_answer_text + cur, chunk, clen + 1);
    } else {
        strlcpy(s_host_answer_text, chunk, sizeof(s_host_answer_text));
        action = "overflow-replace";
    }
    // TEMP DIAG: sentence-drop hunt - what arrived vs what we did with it.
    ESP_LOGI(TAG, "answer_feed %s cur=%u clen=%u peek=%.48s", action,
             static_cast<unsigned>(cur), static_cast<unsigned>(clen), chunk);
    clare_ui_set_answer(s_host_answer_text);
}

static void net_event(const clare_net_event_t *event, void *)
{
    if (!event) return;
    switch (event->type) {
    case CLARE_NET_EVENT_WIFI_CONNECTING: clare_ui_set_wifi("Wi-Fi: connecting"); break;
    case CLARE_NET_EVENT_WIFI_CONNECTED: clare_ui_set_wifi("Wi-Fi: online"); break;
    case CLARE_NET_EVENT_WIFI_DISCONNECTED: clare_ui_set_wifi("Wi-Fi: offline"); break;
    case CLARE_NET_EVENT_WIFI_FAILED: clare_ui_set_wifi("Wi-Fi: failed"); break;
    case CLARE_NET_EVENT_TRANSCRIBE_CONNECTED:
        s_transcribe_connected = true;
        if (s_meeting_active) ui_status("Listening - recording only");
        break;
    case CLARE_NET_EVENT_TRANSCRIBE_DISCONNECTED:
        s_transcribe_connected = false;
        if (s_meeting_active) enqueue_action(Action::HandleMeetingDisconnect);
        break;
    case CLARE_NET_EVENT_TRANSCRIPT: if (event->text) clare_ui_append_transcript(event->text, event->is_final); break;
    case CLARE_NET_EVENT_HOST_CONNECTED: s_host_connected = true; ui_status("Ask Clare a question"); break;
    case CLARE_NET_EVENT_HOST_DISCONNECTED:
        s_host_connected = false; s_host_recording = false; clare_ui_set_host_active(false);
        s_host_answer_since = 0;
        // The failed/closed client must be torn down (in the main task, not
        // here in the ws task): a stale ctx->client makes every later
        // clare_net_host_connect() fail with INVALID_STATE, which surfaced
        // as "Clare Q&A unavailable" until reboot.
        enqueue_action(Action::CleanupHost);
        break;
    case CLARE_NET_EVENT_HOST_TRANSCRIPTION:
        if (event->text) {
            snprintf(s_host_answer_text, sizeof(s_host_answer_text), "You: %s\nClare: ", event->text);
            clare_ui_set_answer(s_host_answer_text);
            ui_status("Question received");
        }
        break;
    case CLARE_NET_EVENT_HOST_ANSWER_TEXT:
        host_answer_feed(event->text);
        break;
    case CLARE_NET_EVENT_HOST_TTS_START: (void)clare_audio_mp3_start(); break;
    case CLARE_NET_EVENT_HOST_ANSWER_AUDIO:
        if (event->binary && event->binary_len) {
            (void)clare_audio_mp3_write(event->binary, event->binary_len, false);
        }
        break;
    case CLARE_NET_EVENT_HOST_TTS_END:
        // One answer arrives as SEVERAL answer_audio messages; the stream
        // must stay open across them.  Ending here restarted the playback
        // task (and its prebuffer) per message = audible gaps.  The stream
        // is closed on HOST_DONE / stop paths instead.
        break;
    case CLARE_NET_EVENT_HOST_SESSION_REJECTED:
        ui_status("Session expired - recreating");
        enqueue_action(Action::HandleHostRejected);
        break;
    case CLARE_NET_EVENT_HOST_DONE:
        s_host_answer_since = 0;
        if (s_host_answer_timer) esp_timer_stop(s_host_answer_timer);
        enqueue_action(Action::FinishHost);
        break;
    case CLARE_NET_EVENT_ERROR: ui_status("Network error - check Wi-Fi/API"); break;
    default: break;
    }
}

static void audio_task(void *)
{
    bool read_error_logged = false;
    bool send_ok_logged = false;
    bool send_error_logged = false;
    uint32_t send_fail_streak = 0;
    while (s_audio_task_run) {
        if ((!s_meeting_active && !s_host_recording) || !s_codec) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        esp_codec_dev_handle_t mic = s_codec->Get_audio_codec_microphone();
        if (!mic) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        int ret = esp_codec_dev_read(mic, s_audio_stereo, sizeof(s_audio_stereo));
        if (ret != ESP_CODEC_DEV_OK) {
            if (!read_error_logged) {
                ESP_LOGW(TAG, "Microphone read failed ret=%d", ret);
                read_error_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (size_t i = 0; i < kAudioFrameSamples; ++i) {
            s_audio_mono[i] = (int16_t)(((int32_t)s_audio_stereo[i * 2] + s_audio_stereo[i * 2 + 1]) / 2);
        }
        (void)clare_audio_alc_process(s_audio_mono, kAudioFrameSamples);
        esp_err_t err = ESP_OK;
        // Feed exactly one channel at a time, like the vocat reference
        // (transcribe_ws_pause during host Q&A): the backend rejects a
        // second concurrent audio stream on the same session with a 403 on
        // the host WS handshake.  The meeting feed pauses while a question
        // is recorded and resumes when the answer flow ends.
        if (s_host_recording) {
            // Host WS handshake still in progress: the channel cannot accept
            // audio yet.  Drop mic data and idle instead of spinning on
            // failed sends — the spin both starved the TLS handshake of CPU
            // (connect timeout) and tripped the 300-streak disconnect
            // watchdog, killing the meeting channel
            // (logs/clare_s3_round2_regression_20260829.log).
            if (!s_host_connected) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            err = clare_net_host_send_audio(s_audio_mono, sizeof(s_audio_mono));
        } else if (s_meeting_active) {
            // Transcribe channel down or reconnecting: idle (drop frames)
            // instead of spamming failed sends into the watchdog — the
            // supervisor owns recovery (vocat ws_session does the same).
            if (!s_transcribe_connected) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            err = clare_net_transcribe_send_audio(s_audio_mono, sizeof(s_audio_mono));
        }
        if (err == ESP_OK && !send_ok_logged) {
            ESP_LOGI(TAG, "Audio stream started");
            send_ok_logged = true;
            send_fail_streak = 0;
        } else if (err != ESP_OK) {
            if (!send_error_logged) {
                ESP_LOGW(TAG, "Audio stream send failed err=%d", static_cast<int>(err));
                send_error_logged = true;
            }
            // Watchdog: a silently stalled transport must not keep the meeting
            // in a fake "listening" state. After ~6 s of continuous failures,
            // trigger the same recovery path as a disconnect event.
            if (++send_fail_streak >= 300) {
                send_fail_streak = 0;
                if (s_meeting_active) enqueue_action(Action::HandleMeetingDisconnect);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            send_fail_streak = 0;
        }
    }
    s_audio_task = nullptr;
    vTaskDelete(nullptr);
}

static void start_audio_task(void)
{
    if (s_audio_task_run) return;
    s_audio_task_run = true;
    BaseType_t result = xTaskCreate(audio_task, "clare_audio", 6144, nullptr, 5, &s_audio_task);
    if (result != pdPASS) {
        s_audio_task_run = false;
        ui_status("Microphone unavailable");
    }
}

static void stop_audio_task(void)
{
    s_audio_task_run = false;
    for (int i = 0; i < 20 && s_audio_task; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

static void start_meeting_impl(void *)
{
    if (s_meeting_active) return;
    if (s_host_connected) {
        (void)clare_net_host_send_stop();
        (void)clare_net_host_disconnect();
        s_host_connected = false;
        s_host_recording = false;
    }
    if (s_session_id[0]) {
        (void)clare_net_end_session(s_session_id);
        s_session_id[0] = 0;
    }
    if (!clare_net_wifi_is_connected()) {
        ui_status("Connecting Wi-Fi...");
        if (clare_net_wifi_connect(15000) != ESP_OK) { ui_status("Wi-Fi not ready"); return; }
    }
    ui_status("Creating meeting session...");
    if (clare_net_create_session(CONFIG_CLARE_TOPIC, s_session_id, sizeof(s_session_id)) != ESP_OK) { ui_status("Meeting service unavailable"); return; }
    // Mark the user-requested meeting active before the WSS wait. This closes
    // the narrow race where a socket connects and drops before connect() has
    // returned; the disconnect handler can then reliably recover the UI.
    s_meeting_active = true;
    s_transcribe_connected = false;
    clare_ui_set_meeting_active(true);
    ui_status("Connecting transcription...");
    if (clare_net_transcribe_connect(s_session_id) != ESP_OK) {
        s_meeting_active = false;
        clare_ui_set_meeting_active(false);
        ui_status("Transcription connection failed");
        clare_net_end_session(s_session_id);
        s_session_id[0] = 0;
        return;
    }
    ui_status("Listening - recording only");
    clare_ui_reset_transcript();
    clare_ui_reset_answer();
    // Live transcription is disabled (send-only transcribe channel, like the
    // stable vocat reference); the summary is shown after the meeting ends.
    clare_ui_set_transcript("录音中… 转写与总结将在会议结束后生成。");
    start_audio_task();
}

static void stop_meeting_impl(void *)
{
    if (!s_meeting_active) return;
    stop_audio_task();
    s_meeting_active = false;
    s_transcribe_connected = false;
    (void)clare_audio_mp3_end();
    clare_ui_set_meeting_active(false);
    clare_net_transcribe_send_end(); clare_net_transcribe_disconnect();
    if (s_host_recording) {
        (void)clare_net_host_send_end_of_speech();
        s_host_recording = false;
    }
    clare_ui_set_host_active(false);
    ui_status("Meeting ended - Ask Clare or refresh summary");
}

static void handle_meeting_disconnect_impl(void *)
{
    if (!s_meeting_active) return;
    if (s_transcribe_connected) return;  // already recovered by a queued duplicate
    (void)clare_net_transcribe_disconnect();  // clean the dead handle
    // Auto-reconnect supervisor (the missing piece vs the vocat reference):
    // the backend/proxy kills long-lived WSS connections on its own schedule
    // (observed at 20 s/83 s/144 s/343 s of healthy streaming, fatal TLS
    // alert in logs/clare_s3_user_acceptance_20260829.log).  Reconnect with
    // backoff; recreate the session once if the id itself was rejected; only
    // give up after everything failed.
    for (int attempt = 1; attempt <= 3 && s_meeting_active && !s_transcribe_connected; ++attempt) {
        ui_status("Reconnecting transcription...");
        vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
        if (clare_net_transcribe_connect(s_session_id) == ESP_OK) {
            ui_status("Listening - recording only");
            return;
        }
    }
    if (!s_meeting_active || s_transcribe_connected) return;
    ui_status("Recreating meeting session...");
    if (clare_net_create_session(CONFIG_CLARE_TOPIC, s_session_id, sizeof(s_session_id)) == ESP_OK &&
        clare_net_transcribe_connect(s_session_id) == ESP_OK) {
        ui_status("Listening - recording only");
        return;
    }
    if (!s_meeting_active) return;
    s_meeting_active = false;
    if (!s_host_recording) stop_audio_task();
    clare_ui_set_meeting_active(false);
    ui_status("Connection lost - tap Start to retry");
}

/*
 * The server rejected the host channel (403): the session id is no longer
 * valid. Recreate the session transparently and re-enter host mode so the
 * user's question round-trips instead of dying on an expired id.
 */
static void handle_host_rejected_impl(void *)
{
    // toggle_host_impl retries inline on 403; if it already reconnected (or
    // is mid-handshake), this queued event is stale and must not tear the
    // fresh connection down.
    if (s_host_connected || s_host_recording) return;
    (void)clare_audio_mp3_end();
    s_host_recording = false;
    s_host_connected = false;
    (void)clare_net_host_disconnect();
    clare_ui_set_host_active(false);
    if (s_meeting_active) {
        // Never recreate the session from under a running meeting: the
        // transcribe supervisor owns the meeting-channel lifecycle.
        ui_status("Ask failed - tap again to retry");
        return;
    }
    if (s_session_id[0]) {
        (void)clare_net_end_session(s_session_id);
        s_session_id[0] = 0;
    }
    ui_status("Reconnecting...");
    if (clare_net_create_session(CONFIG_CLARE_TOPIC, s_session_id, sizeof(s_session_id)) != ESP_OK) {
        ui_status("Recreate failed - check network");
        return;
    }
    // Pause the meeting feed before the handshake (same 403 reason as in
    // toggle_host_impl).
    s_host_recording = true;
    if (clare_net_host_connect(s_session_id) != ESP_OK) {
        s_host_recording = false;
        ui_status("Reconnect failed - tap Ask Clare to retry");
        return;
    }
    start_audio_task();
    clare_ui_set_host_active(true);
    ui_status("Ask Clare a question");
}

static void toggle_host_impl(void *)
{
    if (!s_session_id[0]) { ui_status("Start a meeting first"); return; }
    if (s_host_recording) {
        (void)clare_net_host_send_end_of_speech();
        s_host_recording = false;
        if (!s_meeting_active) stop_audio_task();
        clare_ui_set_host_active(false); ui_status("Clare is answering...");
        s_host_answer_since = xTaskGetTickCount();
        if (s_host_answer_timer) {
            esp_timer_stop(s_host_answer_timer);
            esp_timer_start_once(s_host_answer_timer, 60 * 1000 * 1000);
        }
        return;
    }
    if (s_host_connected) {
        // The previous question never finished (e.g. user tapped Send without
        // speaking, so the server had no speech to answer and never sent
        // done).  A tap after 30 s of waiting is an explicit abandon: tear
        // the stale Q&A down and fall through to start a fresh one.
        if (xTaskGetTickCount() - s_host_answer_since < pdMS_TO_TICKS(30000)) {
            ui_status("Clare is still answering...");
            return;
        }
        ESP_LOGW(TAG, "Abandoning stale Q&A (no answer for 30 s)");
        (void)clare_net_host_disconnect();
        s_host_connected = false;
        clare_ui_set_host_active(false);
    }
    clare_ui_reset_answer();
    s_host_answer_text[0] = '\0';
    ui_status("Connecting Clare...");  // instant tap feedback; the WS handshake below takes seconds
    // Pause the meeting feed BEFORE the host WS handshake, not after: the
    // backend rejects a second concurrent audio stream with a 403, and the
    // handshake takes seconds during which the audio task is still running.
    s_host_recording = true;
    if (clare_net_host_connect(s_session_id) != ESP_OK) {
        s_host_recording = false;
        // The server asynchronously expires the session after Stop
        // ({"type":"end"}), so the first post-meeting Ask can hit a 403
        // (logs/clare_s3_content_debug_20260831.log).  Recreate the session
        // inline and retry once instead of flashing "Q&A unavailable".
        ui_status("Session expired - recreating");
        if (clare_net_create_session(CONFIG_CLARE_TOPIC, s_session_id, sizeof(s_session_id)) == ESP_OK) {
            s_host_recording = true;
            if (clare_net_host_connect(s_session_id) == ESP_OK) {
                start_audio_task(); clare_ui_set_host_active(true); ui_status("Ask Clare a question");
                return;
            }
            s_host_recording = false;
        }
        ui_status("Clare Q&A unavailable");
        return;
    }
    start_audio_task(); clare_ui_set_host_active(true); ui_status("Ask Clare a question");
}

static void finish_host_impl(void *)
{
    // Let the last sentence finish decoding before tearing the stream down.
    (void)clare_audio_mp3_end();
    s_host_recording = false;
    if (!s_meeting_active) stop_audio_task();
    if (s_host_connected) (void)clare_net_host_disconnect();
    s_host_connected = false;
    clare_ui_set_host_active(false);
    ui_status(s_meeting_active ? "Listening - recording only" : "Answer ready");
}

static void refresh_summary_impl(void *)
{
    if (!s_session_id[0]) { ui_status("No meeting to summarize"); return; }
    // Allocated from PSRAM on demand: internal RAM is scarce (WS task stacks
    // need 10 KB contiguous) and the understanding JSON of a real meeting
    // exceeds 2 KB (logs/clare_s3_e2e_test_20260829.log).
    char *json = static_cast<char *>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    char *summary = static_cast<char *>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    if (!json || !summary) {
        heap_caps_free(json);
        heap_caps_free(summary);
        ui_status("Summary unavailable - low memory");
        return;
    }
    json[0] = '\0';
    summary[0] = '\0';
    if (clare_net_get_understanding(s_session_id, json, 4096) == ESP_OK &&
        clare_net_format_understanding(json, summary, 4096) == ESP_OK) {
        clare_ui_set_answer(summary); ui_status("Summary refreshed");
    }
    else ui_status("Summary not ready");
    heap_caps_free(json);
    heap_caps_free(summary);
}

static void open_clare(void *) { clare_ui_set_page(CLARE_UI_CLARE); }
static void close_clare(void *) { if (!s_meeting_active) clare_ui_set_page(CLARE_UI_HOME); }

#if CONFIG_CLARE_BOOT_SELF_TEST
static void boot_network_self_test(void)
{
    ESP_LOGI(TAG, "Boot network self-test begin");
    if (clare_net_wifi_connect(15000) != ESP_OK) {
        ESP_LOGW(TAG, "Boot network self-test Wi-Fi unavailable");
        return;
    }

    const int max_attempts = CONFIG_CLARE_BOOT_SELF_TEST_RETRIES + 1;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        char session_id[96] = {};
        esp_err_t err = clare_net_create_session(CONFIG_CLARE_TOPIC, session_id,
                                                  sizeof(session_id));
        ESP_LOGI(TAG, "Boot network self-test session err=%d (attempt %d/%d)",
                 static_cast<int>(err), attempt, max_attempts);
        if (err == ESP_OK) {
            err = clare_net_transcribe_connect(session_id);
            ESP_LOGI(TAG, "Boot network self-test transcribe err=%d", static_cast<int>(err));
            if (err == ESP_OK) {
                (void)clare_net_transcribe_disconnect();
            }

            err = clare_net_host_connect(session_id);
            ESP_LOGI(TAG, "Boot network self-test host err=%d", static_cast<int>(err));
            if (err == ESP_OK) {
                (void)clare_net_host_disconnect();
            }
            (void)clare_net_end_session(session_id);
            ESP_LOGI(TAG, "Boot network self-test complete");
            return;
        }

        if (attempt < max_attempts) {
            ESP_LOGW(TAG, "Boot network self-test attempt %d failed, retrying in %d ms",
                     attempt, CONFIG_CLARE_BOOT_SELF_TEST_RETRY_DELAY_MS);
            if (!clare_net_wifi_is_connected()) {
                ESP_LOGW(TAG, "Wi-Fi down after self-test failure, reconnecting...");
                (void)clare_net_wifi_connect(15000);
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_CLARE_BOOT_SELF_TEST_RETRY_DELAY_MS));
        }
    }
    ESP_LOGW(TAG, "Boot network self-test failed after %d attempts", max_attempts);
}
#endif

#if CONFIG_CLARE_BOOT_SELF_TEST
// One Ask round: pause meeting feed, host connect, record `record_ms` of mic
// audio, end_of_speech, then wait `answer_ms` for the answer/TTS events.
static void ask_round(const char *label, uint32_t record_ms, uint32_t answer_ms)
{
    ESP_LOGI(TAG, "Ask E2E[%s]: host connect (403 gate)", label);
    s_host_recording = true;  // pause transcribe feed BEFORE handshake
    if (clare_net_host_connect(s_session_id) != ESP_OK) {
        s_host_recording = false;
        ESP_LOGW(TAG, "Ask E2E[%s]: host connect failed", label);
        return;
    }
    ESP_LOGI(TAG, "Ask E2E[%s]: recording %u ms - play the question NOW",
             label, static_cast<unsigned>(record_ms));
    start_audio_task();
    vTaskDelay(pdMS_TO_TICKS(record_ms));
    s_host_recording = false;
    (void)clare_net_host_send_end_of_speech();
    ESP_LOGI(TAG, "Ask E2E[%s]: end_of_speech sent, waiting %u ms", label,
             static_cast<unsigned>(answer_ms));
    vTaskDelay(pdMS_TO_TICKS(answer_ms));
    (void)clare_net_host_disconnect();
    ESP_LOGI(TAG, "Ask E2E[%s]: round done", label);
}

static void boot_ask_e2e_test(void)
{
    // Hands-free full user-flow E2E: meeting (transcribe) + TWO consecutive
    // Ask rounds on one session.  Round 2 is the regression gate for the
    // "second Ask -> Network error / Connection lost" report.
    ESP_LOGI(TAG, "Ask E2E: create session + transcribe");
    if (clare_net_create_session(CONFIG_CLARE_TOPIC, s_session_id, sizeof(s_session_id)) != ESP_OK) {
        ESP_LOGW(TAG, "Ask E2E: session failed");
        return;
    }
    s_meeting_active = true;
    if (clare_net_transcribe_connect(s_session_id) != ESP_OK) {
        s_meeting_active = false;
        ESP_LOGW(TAG, "Ask E2E: transcribe connect failed");
        (void)clare_net_end_session(s_session_id);
        s_session_id[0] = 0;
        return;
    }
    start_audio_task();
    vTaskDelay(pdMS_TO_TICKS(5000));
    ask_round("round1", 8000, 18000);
    ESP_LOGI(TAG, "Ask E2E: meeting feed resumed between rounds");
    vTaskDelay(pdMS_TO_TICKS(4000));
    ask_round("round2", 8000, 18000);
    stop_audio_task();
    s_meeting_active = false;
    (void)clare_audio_mp3_end();
    (void)clare_net_transcribe_disconnect();
    // Summary fetch gate (4 KB PSRAM buffers).
    char *json = static_cast<char *>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    char *summary = static_cast<char *>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    if (json && summary) {
        json[0] = '\0';
        summary[0] = '\0';
        esp_err_t uerr = clare_net_get_understanding(s_session_id, json, 4096);
        ESP_LOGI(TAG, "Ask E2E: understanding err=%d", static_cast<int>(uerr));
        if (uerr == ESP_OK &&
            clare_net_format_understanding(json, summary, 4096) == ESP_OK) {
            ESP_LOGI(TAG, "Ask E2E: summary formatted ok");
        }
    }
    heap_caps_free(json);
    heap_caps_free(summary);
    (void)clare_net_end_session(s_session_id);
    s_session_id[0] = 0;
    ESP_LOGI(TAG, "Ask E2E: complete");
}
#endif

static void action_task(void *)
{
    Action action;
    while (xQueueReceive(s_action_queue, &action, portMAX_DELAY) == pdTRUE) {
        switch (action) {
        case Action::StartMeeting: start_meeting_impl(nullptr); break;
        case Action::StopMeeting: stop_meeting_impl(nullptr); break;
        case Action::HandleMeetingDisconnect: handle_meeting_disconnect_impl(nullptr); break;
        case Action::HandleHostRejected: handle_host_rejected_impl(nullptr); break;
        case Action::ToggleHost: toggle_host_impl(nullptr); break;
        case Action::RefreshSummary: refresh_summary_impl(nullptr); break;
        case Action::FinishHost: finish_host_impl(nullptr); break;
        case Action::CleanupHost: (void)clare_net_host_disconnect(); break;
#if CONFIG_CLARE_BOOT_SELF_TEST
        case Action::BootNetworkSelfTest:
            vTaskDelay(pdMS_TO_TICKS(8000));
            boot_network_self_test();
            // To run the two-round Ask regression, re-enable boot_ask_e2e_test()
            // here (last run all green: logs/clare_s3_round2_silent_20260829.log).
            // boot_ask_e2e_test() is a hands-free full Q&A pass used during
            // bring-up (logs/clare_s3_vol130_boot_20260829.log: all green).
            // Keep it out of routine boots: it spends a real server session
            // and speaks the answer aloud every time.
            break;
#endif
        }
    }
    vTaskDelete(nullptr);
}

static void enqueue_action(Action action)
{
    if (!s_action_queue) return;
    if (xQueueSend(s_action_queue, &action, 0) != pdTRUE) {
        // Recovery actions pile up while the supervisor is mid-reconnect on
        // a flaky link; dropping duplicates is harmless (one recovery is
        // already queued/running) and must not flash a bogus UI status.
        if (action == Action::HandleMeetingDisconnect || action == Action::CleanupHost) return;
        ui_status("Clare is still working...");
    }
}

static void start_meeting(void *) { enqueue_action(Action::StartMeeting); }
static void stop_meeting(void *) { enqueue_action(Action::StopMeeting); }
static void toggle_host(void *) { enqueue_action(Action::ToggleHost); }
static void refresh_summary(void *) { enqueue_action(Action::RefreshSummary); }

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Clare S3 starting");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init(); }
    ESP_ERROR_CHECK(ret);

    Custom_PmicPortInit(&s_i2c, 0x34);
    // 1.75C defaults: CO5300 466x466, QSPI on the pins from user_config.h.
    s_display = new DisplayPort(s_i2c);
    s_display->DisplayPort_TouchInit();
    Lvgl_PortInit(*s_display);
    s_codec = new CodecPort(s_i2c, "S3_AMOLED_1_75C");
    s_codec->CodecPort_SetInfo("es8311 & es7210", 1, 16000, 2, 16);
    // NOTE: esp_codec_dev maps >100 into extra DAC gain on the ES8311.
    s_codec->CodecPort_SetSpeakerVol(130);
    s_codec->CodecPort_SetMicGain(35.0f);
    // Without this the whole playback path stays offline (clare_audio_is_ready()
    // == false) and every TTS answer is silently dropped — the "no voice"
    // root cause found in the three-pass review.  esp_codec_dev_open() is
    // idempotent so sharing the handles with the audio task is safe.
    if (clare_audio_init_from_codec(s_codec) != ESP_OK) {
        ESP_LOGE(TAG, "Playback path init failed - answers will be text only");
    }
    if (clare_audio_alc_init(16000, kAlcGainDb) != ESP_OK) {
        ESP_LOGW(TAG, "ALC unavailable; capture continues without digital gain");
    }

    const esp_timer_create_args_t answer_timer_args = {
        .callback = host_answer_timeout, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "host_answer",
    };
    if (esp_timer_create(&answer_timer_args, &s_host_answer_timer) != ESP_OK) {
        ESP_LOGW(TAG, "answer watchdog timer unavailable");
    }
    s_action_queue = xQueueCreate(4, sizeof(Action));
    ESP_ERROR_CHECK(s_action_queue ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t action_task_result = xTaskCreate(action_task, "clare_action", 8192,
                                                nullptr, 5, nullptr);
    ESP_ERROR_CHECK(action_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    clare_ui_callbacks_t callbacks = {
        .open_clare = open_clare, .close_clare = close_clare, .start_meeting = start_meeting,
        .stop_meeting = stop_meeting, .toggle_host = toggle_host, .refresh_summary = refresh_summary, .ctx = nullptr,
    };
    clare_ui_init(&callbacks);
    clare_ui_set_wifi("Wi-Fi: starting");
    clare_net_config_t net_config = {.event_cb = net_event, .ctx = nullptr};
    if (clare_net_init(&net_config) == ESP_OK) {
        ret = clare_net_wifi_start();
        if (ret != ESP_OK) clare_ui_set_wifi("Wi-Fi: configure locally");
    } else clare_ui_set_wifi("Wi-Fi: unavailable");
#if CONFIG_CLARE_BOOT_SELF_TEST
    enqueue_action(Action::BootNetworkSelfTest);
#endif
    ESP_LOGI(TAG, "Clare UI ready; touch the Clare tile");
    // Audible smoke test of the whole playback chain (codec -> amp -> speaker
    // -> MP3 decoder).  One short chime per boot; if this is not heard, the
    // speaker path is broken regardless of network state.
    extern const uint8_t selftest_chime_mp3_start[] asm("_binary_selftest_chime_mp3_start");
    extern const uint8_t selftest_chime_mp3_end[] asm("_binary_selftest_chime_mp3_end");
    esp_err_t chime_err = clare_audio_play_mp3(selftest_chime_mp3_start,
                                               selftest_chime_mp3_end - selftest_chime_mp3_start);
    ESP_LOGI(TAG, "Boot chime playback err=%d", static_cast<int>(chime_err));
}
