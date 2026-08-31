#include "clare_audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "codec_bsp.h"
#include "esp_ae_alc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* MP3 support is optional; the PCM path must build without esp_audio_codec. */
#if defined(__has_include)
#if __has_include("esp_audio_simple_dec.h") && __has_include("esp_mp3_dec.h")
#define CLARE_AUDIO_HAS_MP3 1
#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_types.h"
#endif
#endif
#ifndef CLARE_AUDIO_HAS_MP3
#define CLARE_AUDIO_HAS_MP3 0
#endif

namespace {

constexpr const char *TAG = "clare_audio";
constexpr size_t kMaxFramesPerIo = 1024;
constexpr uint8_t kMixAllChannels = 0xff;

struct AudioState {
    esp_codec_dev_handle_t playback = nullptr;
    esp_codec_dev_handle_t capture = nullptr;
    uint32_t sample_rate = CLARE_AUDIO_SAMPLE_RATE;
    uint8_t channels = CLARE_AUDIO_CODEC_CHANNELS;
    uint8_t bits = CLARE_AUDIO_BITS_PER_SAMPLE;
    uint8_t capture_channel = kMixAllChannels;
    int volume = 60;
    float gain_db = 30.0f;
    bool ready = false;
    bool capture_active = false;
    bool playback_active = false;
    SemaphoreHandle_t capture_lock = nullptr;
    SemaphoreHandle_t playback_lock = nullptr;
    uint8_t *capture_raw = nullptr;
    size_t capture_raw_bytes = 0;
    uint8_t *playback_raw = nullptr;
    size_t playback_raw_bytes = 0;
#if CLARE_AUDIO_HAS_MP3
    esp_audio_simple_dec_handle_t mp3_decoder = nullptr;
    bool mp3_active = false;
    uint8_t *mp3_out = nullptr;
    size_t mp3_out_bytes = 0;
#endif
};

AudioState s_audio;

struct AlcState {
    esp_ae_alc_handle_t handle = nullptr;
    int8_t gain_db = 0;
};
AlcState s_alc;

static esp_err_t alc_result(esp_ae_err_t err, const char *operation)
{
    if (err == ESP_AE_ERR_OK) return ESP_OK;
    ESP_LOGE(TAG, "%s failed: audio effects error %d", operation, static_cast<int>(err));
    if (err == ESP_AE_ERR_MEM_LACK) return ESP_ERR_NO_MEM;
    if (err == ESP_AE_ERR_INVALID_PARAMETER) return ESP_ERR_INVALID_ARG;
    return ESP_FAIL;
}

static size_t bytes_per_sample()
{
    return (s_audio.bits + 7U) / 8U;
}

static bool lock(SemaphoreHandle_t mutex)
{
    return mutex != nullptr && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

static void unlock(SemaphoreHandle_t mutex)
{
    if (mutex != nullptr) xSemaphoreGive(mutex);
}

static int16_t clamp_s16(int64_t value)
{
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return static_cast<int16_t>(value);
}

static int16_t read_sample_s16(const uint8_t *sample)
{
    if (s_audio.bits == 16) {
        int16_t value;
        memcpy(&value, sample, sizeof(value));
        return value;
    }
    if (s_audio.bits == 24) {
        int32_t value = static_cast<int32_t>(sample[0]) |
                        (static_cast<int32_t>(sample[1]) << 8) |
                        (static_cast<int32_t>(sample[2]) << 16);
        if (value & 0x00800000) value |= static_cast<int32_t>(0xff000000);
        return clamp_s16(static_cast<int64_t>(value) >> 8);
    }
    /* 32-bit I2S slots carry useful PCM in the most significant bits. */
    int32_t value;
    memcpy(&value, sample, sizeof(value));
    return clamp_s16(static_cast<int64_t>(value) >> 16);
}

static void write_sample_s16(uint8_t *sample, int16_t value)
{
    if (s_audio.bits == 16) {
        memcpy(sample, &value, sizeof(value));
        return;
    }
    if (s_audio.bits == 24) {
        int32_t expanded = static_cast<int32_t>(value) << 8;
        sample[0] = static_cast<uint8_t>(expanded & 0xff);
        sample[1] = static_cast<uint8_t>((expanded >> 8) & 0xff);
        sample[2] = static_cast<uint8_t>((expanded >> 16) & 0xff);
        return;
    }
    int32_t expanded = static_cast<int32_t>(value) << 16;
    memcpy(sample, &expanded, sizeof(expanded));
}

static esp_codec_dev_sample_info_t sample_info()
{
    esp_codec_dev_sample_info_t info = {};
    info.sample_rate = s_audio.sample_rate;
    info.channel = s_audio.channels;
    info.bits_per_sample = s_audio.bits;
    return info;
}

static esp_err_t codec_result(int result, const char *operation)
{
    if (result == ESP_CODEC_DEV_OK) return ESP_OK;
    ESP_LOGE(TAG, "%s failed: codec error %d", operation, result);
    return (result < 0) ? static_cast<esp_err_t>(result) : ESP_FAIL;
}

static void release_buffers()
{
    if (s_audio.capture_raw) heap_caps_free(s_audio.capture_raw);
    if (s_audio.playback_raw) heap_caps_free(s_audio.playback_raw);
    s_audio.capture_raw = nullptr;
    s_audio.playback_raw = nullptr;
    s_audio.capture_raw_bytes = 0;
    s_audio.playback_raw_bytes = 0;
#if CLARE_AUDIO_HAS_MP3
    if (s_audio.mp3_out) heap_caps_free(s_audio.mp3_out);
    s_audio.mp3_out = nullptr;
    s_audio.mp3_out_bytes = 0;
#endif
}

static esp_err_t allocate_buffers()
{
    const size_t frame_bytes = static_cast<size_t>(s_audio.channels) * bytes_per_sample();
    s_audio.capture_raw_bytes = kMaxFramesPerIo * frame_bytes;
    s_audio.playback_raw_bytes = kMaxFramesPerIo * frame_bytes;
    s_audio.capture_raw = static_cast<uint8_t *>(heap_caps_malloc(
        s_audio.capture_raw_bytes, MALLOC_CAP_8BIT));
    s_audio.playback_raw = static_cast<uint8_t *>(heap_caps_malloc(
        s_audio.playback_raw_bytes, MALLOC_CAP_8BIT));
    if (!s_audio.capture_raw || !s_audio.playback_raw) {
        release_buffers();
        return ESP_ERR_NO_MEM;
    }
#if CLARE_AUDIO_HAS_MP3
    s_audio.mp3_out_bytes = 16U * 1024U;
    s_audio.mp3_out = static_cast<uint8_t *>(heap_caps_malloc(
        s_audio.mp3_out_bytes, MALLOC_CAP_8BIT));
    if (!s_audio.mp3_out) {
        release_buffers();
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

static esp_err_t ensure_capture_open_locked()
{
    if (!s_audio.capture) return ESP_ERR_INVALID_STATE;
    if (s_audio.capture_active) return ESP_OK;
    esp_codec_dev_sample_info_t info = sample_info();
    esp_err_t err = codec_result(esp_codec_dev_open(s_audio.capture, &info), "capture open");
    if (err == ESP_OK) s_audio.capture_active = true;
    return err;
}

static esp_err_t ensure_playback_open_locked()
{
    if (!s_audio.playback) return ESP_ERR_INVALID_STATE;
    if (s_audio.playback_active) return ESP_OK;
    esp_codec_dev_sample_info_t info = sample_info();
    esp_err_t err = codec_result(esp_codec_dev_open(s_audio.playback, &info), "playback open");
    if (err == ESP_OK) s_audio.playback_active = true;
    return err;
}

static void downmix_to_mono(const uint8_t *raw, size_t frames, int16_t *dst)
{
    const size_t sample_bytes = bytes_per_sample();
    for (size_t frame = 0; frame < frames; ++frame) {
        const uint8_t *frame_ptr = raw + frame * s_audio.channels * sample_bytes;
        if (s_audio.capture_channel != kMixAllChannels) {
            const size_t channel = std::min<size_t>(s_audio.capture_channel, s_audio.channels - 1U);
            dst[frame] = read_sample_s16(frame_ptr + channel * sample_bytes);
            continue;
        }
        int64_t sum = 0;
        for (size_t channel = 0; channel < s_audio.channels; ++channel) {
            sum += read_sample_s16(frame_ptr + channel * sample_bytes);
        }
        dst[frame] = clamp_s16(sum / static_cast<int64_t>(s_audio.channels));
    }
}

static esp_err_t write_mono_chunk_locked(const int16_t *pcm, size_t frames)
{
    if (frames == 0) return ESP_OK;
    if (!s_audio.playback_raw) return ESP_ERR_INVALID_STATE;
    const size_t sample_bytes = bytes_per_sample();
    const size_t frame_bytes = static_cast<size_t>(s_audio.channels) * sample_bytes;
    if (frames > kMaxFramesPerIo || frames * frame_bytes > s_audio.playback_raw_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        uint8_t *frame_ptr = s_audio.playback_raw + frame * frame_bytes;
        for (size_t channel = 0; channel < s_audio.channels; ++channel) {
            write_sample_s16(frame_ptr + channel * sample_bytes, pcm[frame]);
        }
    }
    return codec_result(esp_codec_dev_write(
        s_audio.playback, s_audio.playback_raw, static_cast<int>(frames * frame_bytes)),
        "playback write");
}

#if CLARE_AUDIO_HAS_MP3
static int resample_to_16k_mono(const int16_t *input, size_t input_frames,
                                uint32_t input_rate, int16_t *output, size_t output_capacity)
{
    if (!input || !output || input_rate == 0) return -1;
    const size_t output_frames = std::min<size_t>(
        output_capacity,
        (input_frames * CLARE_AUDIO_SAMPLE_RATE + input_rate - 1U) / input_rate);
    for (size_t out = 0; out < output_frames; ++out) {
        const uint64_t source_pos = static_cast<uint64_t>(out) * input_rate;
        size_t index = static_cast<size_t>(source_pos / CLARE_AUDIO_SAMPLE_RATE);
        if (index >= input_frames) index = input_frames - 1U;
        const size_t next = std::min(index + 1U, input_frames - 1U);
        const uint32_t frac = static_cast<uint32_t>(source_pos % CLARE_AUDIO_SAMPLE_RATE);
        output[out] = clamp_s16(static_cast<int64_t>(input[index]) +
                                (static_cast<int64_t>(input[next]) - input[index]) * frac /
                                CLARE_AUDIO_SAMPLE_RATE);
    }
    return static_cast<int>(output_frames);
}
#endif

#if CLARE_AUDIO_HAS_MP3
static esp_err_t map_audio_error(esp_audio_err_t err)
{
    if (err == ESP_AUDIO_ERR_OK || err == ESP_AUDIO_ERR_CONTINUE) return ESP_OK;
    if (err == ESP_AUDIO_ERR_MEM_LACK) return ESP_ERR_NO_MEM;
    if (err == ESP_AUDIO_ERR_INVALID_PARAMETER) return ESP_ERR_INVALID_ARG;
    if (err == ESP_AUDIO_ERR_NOT_SUPPORT) return ESP_ERR_NOT_SUPPORTED;
    if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) return ESP_ERR_INVALID_SIZE;
    return ESP_FAIL;
}

static esp_err_t mp3_write_decoded_locked(const uint8_t *pcm, size_t pcm_bytes,
                                          const esp_audio_simple_dec_info_t &info)
{
    if (info.bits_per_sample != 16 || info.channel == 0 || info.sample_rate == 0) {
        ESP_LOGE(TAG, "unsupported MP3 output format: %lu Hz, %u bit, %u ch",
                 static_cast<unsigned long>(info.sample_rate), info.bits_per_sample, info.channel);
        return ESP_ERR_NOT_SUPPORTED;
    }
    const size_t input_frames = pcm_bytes / (sizeof(int16_t) * info.channel);
    if (input_frames == 0) return ESP_OK;
    int16_t *mono = static_cast<int16_t *>(heap_caps_malloc(
        input_frames * sizeof(int16_t), MALLOC_CAP_8BIT));
    if (!mono) return ESP_ERR_NO_MEM;
    const int16_t *samples = reinterpret_cast<const int16_t *>(pcm);
    for (size_t i = 0; i < input_frames; ++i) {
        int64_t sum = 0;
        for (size_t ch = 0; ch < info.channel; ++ch) sum += samples[i * info.channel + ch];
        mono[i] = clamp_s16(sum / info.channel);
    }

    esp_err_t err = ESP_OK;
    if (info.sample_rate == CLARE_AUDIO_SAMPLE_RATE) {
        for (size_t offset = 0; offset < input_frames && err == ESP_OK; ) {
            const size_t chunk = std::min(kMaxFramesPerIo, input_frames - offset);
            err = write_mono_chunk_locked(mono + offset, chunk);
            offset += chunk;
        }
    } else {
        const size_t output_capacity = (input_frames * CLARE_AUDIO_SAMPLE_RATE + info.sample_rate - 1U) /
                                       info.sample_rate + 2U;
        int16_t *resampled = static_cast<int16_t *>(heap_caps_malloc(
            output_capacity * sizeof(int16_t), MALLOC_CAP_8BIT));
        if (!resampled) {
            heap_caps_free(mono);
            return ESP_ERR_NO_MEM;
        }
        const int out_frames = resample_to_16k_mono(
            mono, input_frames, info.sample_rate, resampled, output_capacity);
        if (out_frames < 0) {
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            for (int offset = 0; offset < out_frames && err == ESP_OK; ) {
                const size_t chunk = std::min(kMaxFramesPerIo, static_cast<size_t>(out_frames - offset));
                err = write_mono_chunk_locked(resampled + offset, chunk);
                offset += static_cast<int>(chunk);
            }
        }
        heap_caps_free(resampled);
    }
    heap_caps_free(mono);
    return err;
}
#endif

} // namespace

extern "C" esp_err_t clare_audio_init(const clare_audio_config_t *config)
{
    if (!config || !config->playback || !config->capture) return ESP_ERR_INVALID_ARG;
    if (s_audio.ready) {
        if (s_audio.playback == config->playback && s_audio.capture == config->capture) return ESP_OK;
        clare_audio_deinit();
    }

    s_audio.playback = config->playback;
    s_audio.capture = config->capture;
    s_audio.sample_rate = config->sample_rate ? config->sample_rate : CLARE_AUDIO_SAMPLE_RATE;
    s_audio.channels = config->codec_channels ? config->codec_channels : CLARE_AUDIO_CODEC_CHANNELS;
    s_audio.bits = config->bits_per_sample ? config->bits_per_sample : CLARE_AUDIO_BITS_PER_SAMPLE;
    s_audio.capture_channel = config->capture_channel;
    if (s_audio.channels == 0 || s_audio.channels > 8 ||
        (s_audio.bits != 16 && s_audio.bits != 24 && s_audio.bits != 32)) {
        s_audio.playback = nullptr;
        s_audio.capture = nullptr;
        return ESP_ERR_INVALID_ARG;
    }
    if (config->output_volume >= 0) s_audio.volume = std::min(100, std::max(0, config->output_volume));
    if (config->input_gain_db >= 0.0f) s_audio.gain_db = config->input_gain_db;

    s_audio.capture_lock = xSemaphoreCreateMutex();
    s_audio.playback_lock = xSemaphoreCreateMutex();
    if (!s_audio.capture_lock || !s_audio.playback_lock) {
        clare_audio_deinit();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = allocate_buffers();
    if (err != ESP_OK) {
        clare_audio_deinit();
        return err;
    }

    if (!lock(s_audio.capture_lock)) {
        clare_audio_deinit();
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t info = sample_info();
    err = codec_result(esp_codec_dev_open(s_audio.capture, &info), "capture open");
    if (err == ESP_OK) {
        s_audio.capture_active = true;
        if (config->input_gain_db >= 0.0f) {
            err = codec_result(esp_codec_dev_set_in_gain(s_audio.capture, s_audio.gain_db), "input gain");
        }
    }
    unlock(s_audio.capture_lock);
    if (err != ESP_OK) {
        clare_audio_deinit();
        return err;
    }

    if (!lock(s_audio.playback_lock)) {
        clare_audio_deinit();
        return ESP_ERR_INVALID_STATE;
    }
    err = codec_result(esp_codec_dev_open(s_audio.playback, &info), "playback open");
    if (err == ESP_OK) {
        s_audio.playback_active = true;
        if (config->output_volume >= 0) {
            err = codec_result(esp_codec_dev_set_out_vol(s_audio.playback, s_audio.volume), "output volume");
        }
    }
    unlock(s_audio.playback_lock);
    if (err != ESP_OK) {
        clare_audio_deinit();
        return err;
    }

    s_audio.ready = true;
    ESP_LOGI(TAG, "ready: %lu Hz, %u-bit, %u codec channels, mono network PCM",
             static_cast<unsigned long>(s_audio.sample_rate), s_audio.bits, s_audio.channels);
    return ESP_OK;
}

extern "C" esp_err_t clare_audio_init_from_codec(void *codec_port)
{
    if (!codec_port) return ESP_ERR_INVALID_ARG;
    auto *codec = static_cast<CodecPort *>(codec_port);
    clare_audio_config_t config = {};
    config.playback = codec->Get_audio_codec_speaker();
    config.capture = codec->Get_audio_codec_microphone();
    config.sample_rate = CLARE_AUDIO_SAMPLE_RATE;
    config.codec_channels = CLARE_AUDIO_CODEC_CHANNELS;
    config.bits_per_sample = CLARE_AUDIO_BITS_PER_SAMPLE;
    config.capture_channel = kMixAllChannels;
    config.output_volume = -1;
    config.input_gain_db = -1.0f;
    return clare_audio_init(&config);
}

extern "C" esp_err_t clare_audio_deinit(void)
{
#if CLARE_AUDIO_HAS_MP3
    clare_audio_mp3_end();
#endif
    if (s_audio.capture_lock && lock(s_audio.capture_lock)) {
        if (s_audio.capture && s_audio.capture_active) esp_codec_dev_close(s_audio.capture);
        s_audio.capture_active = false;
        unlock(s_audio.capture_lock);
    }
    if (s_audio.playback_lock && lock(s_audio.playback_lock)) {
        if (s_audio.playback && s_audio.playback_active) esp_codec_dev_close(s_audio.playback);
        s_audio.playback_active = false;
        unlock(s_audio.playback_lock);
    }
    release_buffers();
    if (s_audio.capture_lock) {
        vSemaphoreDelete(s_audio.capture_lock);
        s_audio.capture_lock = nullptr;
    }
    if (s_audio.playback_lock) {
        vSemaphoreDelete(s_audio.playback_lock);
        s_audio.playback_lock = nullptr;
    }
    s_audio.playback = nullptr;
    s_audio.capture = nullptr;
    s_audio.ready = false;
    return ESP_OK;
}

extern "C" bool clare_audio_is_ready(void)
{
    return s_audio.ready && s_audio.capture && s_audio.playback;
}

extern "C" esp_err_t clare_audio_capture_start(void)
{
    if (!clare_audio_is_ready() || !lock(s_audio.capture_lock)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ensure_capture_open_locked();
    unlock(s_audio.capture_lock);
    return err;
}

extern "C" esp_err_t clare_audio_capture_stop(void)
{
    if (!clare_audio_is_ready() || !lock(s_audio.capture_lock)) return ESP_ERR_INVALID_STATE;
    if (s_audio.capture_active) {
        esp_codec_dev_close(s_audio.capture);
        s_audio.capture_active = false;
    }
    unlock(s_audio.capture_lock);
    return ESP_OK;
}

extern "C" esp_err_t clare_audio_playback_start(void)
{
    if (!clare_audio_is_ready() || !lock(s_audio.playback_lock)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ensure_playback_open_locked();
    unlock(s_audio.playback_lock);
    return err;
}

extern "C" esp_err_t clare_audio_playback_stop(void)
{
    if (!clare_audio_is_ready() || !lock(s_audio.playback_lock)) return ESP_ERR_INVALID_STATE;
    if (s_audio.playback_active) {
        esp_codec_dev_close(s_audio.playback);
        s_audio.playback_active = false;
    }
    unlock(s_audio.playback_lock);
    return ESP_OK;
}

extern "C" int clare_audio_read_mono_pcm16(int16_t *dst, size_t frames)
{
    if (!dst || frames == 0 || !clare_audio_is_ready()) return -1;
    if (!lock(s_audio.capture_lock)) return -1;
    if (ensure_capture_open_locked() != ESP_OK) {
        unlock(s_audio.capture_lock);
        return -1;
    }
    size_t done = 0;
    const size_t frame_bytes = static_cast<size_t>(s_audio.channels) * bytes_per_sample();
    while (done < frames) {
        const size_t chunk_frames = std::min(kMaxFramesPerIo, frames - done);
        const int result = esp_codec_dev_read(s_audio.capture, s_audio.capture_raw,
                                              static_cast<int>(chunk_frames * frame_bytes));
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "capture read failed: codec error %d", result);
            unlock(s_audio.capture_lock);
            return -1;
        }
        downmix_to_mono(s_audio.capture_raw, chunk_frames, dst + done);
        done += chunk_frames;
    }
    unlock(s_audio.capture_lock);
    return static_cast<int>(done);
}

extern "C" int clare_audio_read_pcm16(void *dst, size_t bytes)
{
    if (!dst || bytes == 0 || (bytes % sizeof(int16_t)) != 0) return -1;
    const int frames = clare_audio_read_mono_pcm16(static_cast<int16_t *>(dst), bytes / sizeof(int16_t));
    return frames < 0 ? -1 : frames * static_cast<int>(sizeof(int16_t));
}

extern "C" esp_err_t clare_audio_write_pcm16(const int16_t *pcm, size_t frames)
{
    if (!pcm || frames == 0 || !clare_audio_is_ready()) return ESP_ERR_INVALID_ARG;
    if (!lock(s_audio.playback_lock)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ensure_playback_open_locked();
    for (size_t offset = 0; err == ESP_OK && offset < frames; ) {
        const size_t chunk = std::min(kMaxFramesPerIo, frames - offset);
        err = write_mono_chunk_locked(pcm + offset, chunk);
        offset += chunk;
    }
    unlock(s_audio.playback_lock);
    return err;
}

extern "C" esp_err_t clare_audio_play_pcm16(const int16_t *pcm, size_t frames)
{
    return clare_audio_write_pcm16(pcm, frames);
}

extern "C" esp_err_t clare_audio_set_volume(int volume)
{
    if (!clare_audio_is_ready() || !lock(s_audio.playback_lock)) return ESP_ERR_INVALID_STATE;
    s_audio.volume = std::min(100, std::max(0, volume));
    esp_err_t err = codec_result(esp_codec_dev_set_out_vol(s_audio.playback, s_audio.volume), "output volume");
    unlock(s_audio.playback_lock);
    return err;
}

extern "C" int clare_audio_get_volume(void)
{
    return s_audio.volume;
}

extern "C" esp_err_t clare_audio_set_input_gain(float gain_db)
{
    if (!clare_audio_is_ready() || !std::isfinite(gain_db)) return ESP_ERR_INVALID_STATE;
    if (!lock(s_audio.capture_lock)) return ESP_ERR_INVALID_STATE;
    s_audio.gain_db = gain_db;
    esp_err_t err = codec_result(esp_codec_dev_set_in_gain(s_audio.capture, gain_db), "input gain");
    unlock(s_audio.capture_lock);
    return err;
}

extern "C" esp_err_t clare_audio_alc_init(uint32_t sample_rate, int8_t gain_db)
{
    if (s_alc.handle) return ESP_OK;
    const int8_t clamped_gain = static_cast<int8_t>(
        std::min<int>(63, std::max<int>(-64, static_cast<int>(gain_db))));
    esp_ae_alc_cfg_t cfg = {};
    cfg.sample_rate = sample_rate ? sample_rate : CLARE_AUDIO_SAMPLE_RATE;
    cfg.channel = CLARE_AUDIO_PCM_CHANNELS;
    cfg.bits_per_sample = CLARE_AUDIO_BITS_PER_SAMPLE;
    esp_err_t err = alc_result(esp_ae_alc_open(&cfg, &s_alc.handle), "alc open");
    if (err != ESP_OK) return err;
    /* Smooth gain changes so runtime adjustments do not produce zipper noise. */
    (void)esp_ae_alc_set_transit_time(s_alc.handle, 100);
    s_alc.gain_db = clamped_gain;
    if (clamped_gain != 0) {
        err = alc_result(esp_ae_alc_set_gain(s_alc.handle, 0, clamped_gain), "alc set gain");
        if (err != ESP_OK) {
            esp_ae_alc_close(s_alc.handle);
            s_alc.handle = nullptr;
            return err;
        }
    }
    ESP_LOGI(TAG, "ALC ready: %lu Hz mono PCM16, gain %d dB",
             static_cast<unsigned long>(cfg.sample_rate), static_cast<int>(clamped_gain));
    return ESP_OK;
}

extern "C" esp_err_t clare_audio_alc_set_gain(int8_t gain_db)
{
    if (!s_alc.handle) return ESP_ERR_INVALID_STATE;
    const int8_t clamped_gain = static_cast<int8_t>(
        std::min<int>(63, std::max<int>(-64, static_cast<int>(gain_db))));
    s_alc.gain_db = clamped_gain;
    return alc_result(esp_ae_alc_set_gain(s_alc.handle, 0, clamped_gain), "alc set gain");
}

extern "C" esp_err_t clare_audio_alc_process(int16_t *pcm, size_t frames)
{
    if (!s_alc.handle) return ESP_OK;  /* No-op until ALC init succeeds. */
    if (!pcm || frames == 0) return ESP_ERR_INVALID_ARG;
    return alc_result(esp_ae_alc_process(s_alc.handle, static_cast<uint32_t>(frames),
                                         (esp_ae_sample_t)pcm, (esp_ae_sample_t)pcm),
                      "alc process");
}

extern "C" void clare_audio_alc_deinit(void)
{
    if (s_alc.handle) {
        esp_ae_alc_close(s_alc.handle);
        s_alc.handle = nullptr;
    }
}

#if CLARE_AUDIO_HAS_MP3
// --- TTS jitter buffer + dedicated playback task -----------------------------
// Decoding used to happen synchronously in the websocket client task: the
// codec write blocked the WS read loop, TCP back-pressure made MP3 chunks
// arrive in bursts, and the I2S DMA underran between them ("断断续续").
// Now the WS task only enqueues compressed bytes; this task decodes and
// plays at I2S pace, like the proven vocat reference (mp3_player.c queue).
constexpr size_t kTtsRingBytes = 96 * 1024;   // PSRAM; a full spoken answer fits
constexpr size_t kTtsPrebufferBytes = 8 * 1024; // ~0.5 s @128 kbps before play

struct TtsStream {
    uint8_t *ring = nullptr;
    size_t cap = 0;
    uint32_t rpos = 0;
    uint32_t wpos = 0;
    bool eos = false;
    bool overflow_logged = false;
    SemaphoreHandle_t lock = nullptr;
    TaskHandle_t task = nullptr;
};
TtsStream s_tts;

static size_t tts_avail() { return s_tts.wpos - s_tts.rpos; }

static void tts_ring_reset_locked()
{
    s_tts.rpos = 0;
    s_tts.wpos = 0;
    s_tts.eos = false;
    s_tts.overflow_logged = false;
}

static size_t tts_ring_write(const uint8_t *data, size_t len)
{
    const size_t avail = tts_avail();
    const size_t free_bytes = s_tts.cap - avail;
    if (len > free_bytes) {
        if (!s_tts.overflow_logged) {
            ESP_LOGW(TAG, "tts ring overflow: dropping %u of %u bytes",
                     static_cast<unsigned>(len - free_bytes), static_cast<unsigned>(len));
            s_tts.overflow_logged = true;
        }
        len = free_bytes;
    }
    size_t done = 0;
    while (done < len) {
        const size_t offset = s_tts.wpos % s_tts.cap;
        const size_t slice = std::min(len - done, s_tts.cap - offset);
        memcpy(s_tts.ring + offset, data + done, slice);
        s_tts.wpos += slice;
        done += slice;
    }
    return done;
}

static size_t tts_ring_read(uint8_t *dst, size_t max_len)
{
    const size_t avail = tts_avail();
    if (avail == 0) return 0;
    const size_t offset = s_tts.rpos % s_tts.cap;
    const size_t slice = std::min(std::min(max_len, avail), s_tts.cap - offset);
    memcpy(dst, s_tts.ring + offset, slice);
    s_tts.rpos += slice;
    return slice;
}

static esp_err_t tts_decoder_reopen()
{
    if (s_audio.mp3_decoder) {
        esp_audio_simple_dec_close(s_audio.mp3_decoder);
        s_audio.mp3_decoder = nullptr;
    }
    esp_audio_simple_dec_cfg_t config = {};
    config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    config.use_frame_dec = false;
    esp_audio_err_t err = esp_audio_simple_dec_open(&config, &s_audio.mp3_decoder);
    return (err == ESP_AUDIO_ERR_OK) ? ESP_OK : ESP_FAIL;
}

static void tts_play_task(void *)
{
    // Prebuffer: wait for enough compressed audio (or stream end) so the
    // first playback starts with a cushion against network jitter.
    for (int i = 0; i < 150; ++i) {
        lock(s_tts.lock);
        const bool ready = tts_avail() >= kTtsPrebufferBytes || s_tts.eos;
        unlock(s_tts.lock);
        if (ready) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    uint8_t *feed = static_cast<uint8_t *>(heap_caps_malloc(1024, MALLOC_CAP_SPIRAM));
    if (!feed) {
        ESP_LOGE(TAG, "tts feed buffer alloc failed");
        s_tts.task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "tts playback task started");
    bool stream_open = true;
    size_t pending = 0;          // unconsumed tail carried to the next feed
    int consecutive_errors = 0;  // decode resets without a successful frame
    // DIAG (first-sentence-missing hunt): byte/frame accounting per stream.
    uint32_t stat_ring_bytes = 0;
    uint32_t stat_pcm_bytes = 0;
    uint32_t stat_t0 = 0;
    while (stream_open) {
        lock(s_tts.lock);
        const bool eos = s_tts.eos;
        size_t got = tts_ring_read(feed + pending, 1024 - pending);
        unlock(s_tts.lock);
        stat_ring_bytes += got;

        if (pending + got == 0) {
            if (eos) {
                // Flush the decoder with an empty eos frame, then stop.
                esp_audio_simple_dec_raw_t raw = {};
                raw.buffer = feed;
                raw.len = 0;
                raw.eos = true;
                esp_audio_simple_dec_out_t frame = {};
                frame.buffer = s_audio.mp3_out;
                frame.len = static_cast<uint32_t>(s_audio.mp3_out_bytes);
                (void)esp_audio_simple_dec_process(s_audio.mp3_decoder, &raw, &frame);
                if (frame.decoded_size > 0) {
                    esp_audio_simple_dec_info_t info = {};
                    if (esp_audio_simple_dec_get_info(s_audio.mp3_decoder, &info) == ESP_AUDIO_ERR_OK) {
                        lock(s_audio.playback_lock);
                        (void)mp3_write_decoded_locked(frame.buffer, frame.decoded_size, info);
                        unlock(s_audio.playback_lock);
                    }
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = feed;
        raw.len = static_cast<uint32_t>(pending + got);
        raw.eos = false;
        pending = 0;
        while (raw.len > 0) {
            esp_audio_simple_dec_out_t frame = {};
            frame.buffer = s_audio.mp3_out;
            frame.len = static_cast<uint32_t>(s_audio.mp3_out_bytes);
            esp_audio_err_t err = esp_audio_simple_dec_process(s_audio.mp3_decoder, &raw, &frame);
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > frame.len) {
                uint8_t *new_buf = static_cast<uint8_t *>(heap_caps_realloc(
                    s_audio.mp3_out, frame.needed_size, MALLOC_CAP_8BIT));
                if (!new_buf) { stream_open = false; break; }
                s_audio.mp3_out = new_buf;
                s_audio.mp3_out_bytes = frame.needed_size;
                continue;
            }
            if (err != ESP_AUDIO_ERR_OK && err != ESP_AUDIO_ERR_CONTINUE) {
                // A bad segment (e.g. one sentence's MP3 in a multi-message
                // answer) must NOT kill the stream: reset the decoder in
                // place and keep playing the rest of the buffer.  Only give
                // up after 8 consecutive resets with no decoded frame —
                // previously the task exited and the next TTS_START wiped
                // the ring, which is why a whole sentence was skipped.
                ESP_LOGW(TAG, "tts decode err=%d - decoder reset, stream kept", static_cast<int>(err));
                if (++consecutive_errors >= 8 || tts_decoder_reopen() != ESP_OK) {
                    ESP_LOGE(TAG, "tts decode unrecoverable, stopping stream");
                    stream_open = false;
                }
                break;
            }
            if (frame.decoded_size > 0) {
                consecutive_errors = 0;
                stat_pcm_bytes += frame.decoded_size;
                if (stat_t0 == 0) {
                    stat_t0 = 1;
                    ESP_LOGI(TAG, "tts first audio out (ring consumed %u bytes so far)",
                             static_cast<unsigned>(stat_ring_bytes));
                }
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(s_audio.mp3_decoder, &info) == ESP_AUDIO_ERR_OK) {
                    lock(s_audio.playback_lock);
                    (void)mp3_write_decoded_locked(frame.buffer, frame.decoded_size, info);
                    unlock(s_audio.playback_lock);
                }
            }
            if (raw.consumed > raw.len) { stream_open = false; break; }
            if (raw.consumed == 0) break;  // need more input; carry tail over
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            raw.consumed = 0;
        }
        if (raw.len > 0 && raw.len <= 1024 && stream_open) {
            memmove(feed, raw.buffer, raw.len);
            pending = raw.len;
        } else if (raw.len > 1024) {
            // Decoder made no progress on a full buffer: drop one byte to
            // force re-sync instead of spinning forever.
            memmove(feed, raw.buffer + 1, raw.len - 1);
            pending = raw.len - 1;
        }
    }
    heap_caps_free(feed);
    if (s_audio.mp3_decoder) {
        esp_audio_simple_dec_close(s_audio.mp3_decoder);
        s_audio.mp3_decoder = nullptr;
    }
    s_audio.mp3_active = false;
    ESP_LOGI(TAG, "tts playback task done: ring=%u B pcm=%u B",
             static_cast<unsigned>(stat_ring_bytes), static_cast<unsigned>(stat_pcm_bytes));
    s_tts.task = nullptr;
    vTaskDelete(nullptr);
}

static esp_err_t tts_stream_init()
{
    if (!s_tts.lock) {
        s_tts.lock = xSemaphoreCreateMutex();
        if (!s_tts.lock) return ESP_ERR_NO_MEM;
    }
    if (!s_tts.ring) {
        s_tts.ring = static_cast<uint8_t *>(heap_caps_malloc(kTtsRingBytes, MALLOC_CAP_SPIRAM));
        if (!s_tts.ring) return ESP_ERR_NO_MEM;
        s_tts.cap = kTtsRingBytes;
    }
    return ESP_OK;
}
#endif // CLARE_AUDIO_HAS_MP3

extern "C" esp_err_t clare_audio_mp3_start(void)
{
#if CLARE_AUDIO_HAS_MP3
    if (!clare_audio_is_ready()) return ESP_ERR_INVALID_STATE;
    if (s_audio.mp3_active) {
        // Stream already open (another answer_audio message, or a new answer
        // while the previous one still drains): keep buffered audio and
        // clear a stale end-of-stream flag so playback continues.
        lock(s_tts.lock);
        s_tts.eos = false;
        unlock(s_tts.lock);
        return ESP_OK;
    }
    if (tts_stream_init() != ESP_OK) return ESP_ERR_NO_MEM;
    lock(s_tts.lock);
    tts_ring_reset_locked();
    unlock(s_tts.lock);
    esp_audio_err_t register_err = esp_mp3_dec_register();
    if (register_err != ESP_AUDIO_ERR_OK && register_err != ESP_AUDIO_ERR_ALREADY_EXIST) {
        ESP_LOGE(TAG, "MP3 decoder registration failed: %d", static_cast<int>(register_err));
        return map_audio_error(register_err);
    }
    esp_audio_simple_dec_cfg_t config = {};
    config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    config.use_frame_dec = false;
    esp_audio_err_t err = esp_audio_simple_dec_open(&config, &s_audio.mp3_decoder);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "MP3 decoder open failed: %d", static_cast<int>(err));
        s_audio.mp3_decoder = nullptr;
        return map_audio_error(err);
    }
    s_audio.mp3_active = true;
    if (!s_tts.task) {
        if (xTaskCreate(tts_play_task, "clare_tts", 5120, nullptr, 6, &s_tts.task) != pdPASS) {
            ESP_LOGE(TAG, "tts task create failed");
            esp_audio_simple_dec_close(s_audio.mp3_decoder);
            s_audio.mp3_decoder = nullptr;
            s_audio.mp3_active = false;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

extern "C" esp_err_t clare_audio_mp3_write(const uint8_t *data, size_t len, bool end_of_stream)
{
#if CLARE_AUDIO_HAS_MP3
    if ((!data && len != 0) || !clare_audio_is_ready()) return ESP_ERR_INVALID_ARG;
    if (!s_audio.mp3_active) {
        esp_err_t start_err = clare_audio_mp3_start();
        if (start_err != ESP_OK) return start_err;
    }
    // Fast path only: enqueue compressed bytes; decoding/playback happens in
    // the dedicated task so the caller (websocket client task) never blocks
    // on I2S.
    lock(s_tts.lock);
    if (data && len > 0) {
        (void)tts_ring_write(data, len);
    }
    if (end_of_stream) {
        s_tts.eos = true;
    }
    unlock(s_tts.lock);
    return ESP_OK;
#else
    (void)data;
    (void)len;
    (void)end_of_stream;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

extern "C" esp_err_t clare_audio_mp3_end(void)
{
#if CLARE_AUDIO_HAS_MP3
    // Signal end-of-stream; the playback task drains the ring and closes the
    // decoder ITSELF (force-closing here would race an in-flight decode).
    lock(s_tts.lock);
    s_tts.eos = true;
    unlock(s_tts.lock);
    for (int i = 0; i < 200 && s_tts.task; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_tts.task) {
        ESP_LOGW(TAG, "tts drain still running after 4 s; leaving decoder to the task");
    } else if (s_audio.mp3_decoder) {
        // Safety net for task-creation-failure paths only.
        esp_audio_simple_dec_close(s_audio.mp3_decoder);
        s_audio.mp3_decoder = nullptr;
        s_audio.mp3_active = false;
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

extern "C" esp_err_t clare_audio_play_mp3(const uint8_t *data, size_t len)
{
    esp_err_t err = clare_audio_mp3_start();
    if (err != ESP_OK) return err;
    return clare_audio_mp3_write(data, len, true);
}
