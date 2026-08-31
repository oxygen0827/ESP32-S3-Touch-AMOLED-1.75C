#pragma once

/*
 * Clare audio adapter for the ESP32-C6 AMOLED board.
 *
 * The network protocol uses signed little-endian, 16-bit, mono PCM at 16 kHz.
 * The board codec is configured as a 16-bit stereo device by CodecPort. This
 * module performs the format conversion at the hardware boundary so callers
 * never need to know the I2S slot layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLARE_AUDIO_SAMPLE_RATE       (16000U)
#define CLARE_AUDIO_PCM_CHANNELS      (1U)
#define CLARE_AUDIO_CODEC_CHANNELS   (2U)
#define CLARE_AUDIO_BITS_PER_SAMPLE  (16U)
#define CLARE_AUDIO_FRAME_SAMPLES    (320U)  /* 20 ms at 16 kHz */
#define CLARE_AUDIO_FRAME_BYTES      (CLARE_AUDIO_FRAME_SAMPLES * sizeof(int16_t))

/** Format and device handles supplied by the board layer. */
typedef struct {
    esp_codec_dev_handle_t playback;
    esp_codec_dev_handle_t capture;
    uint32_t sample_rate;       /* Defaults to CLARE_AUDIO_SAMPLE_RATE. */
    uint8_t codec_channels;     /* Defaults to CLARE_AUDIO_CODEC_CHANNELS. */
    uint8_t bits_per_sample;    /* Defaults to CLARE_AUDIO_BITS_PER_SAMPLE. */
    uint8_t capture_channel;    /* 0..N-1; 0xff mixes all input channels. */
    int output_volume;          /* 0..100; negative keeps codec default. */
    float input_gain_db;        /* Negative keeps codec default. */
} clare_audio_config_t;

/** Initialise from already-created esp_codec_dev handles. */
esp_err_t clare_audio_init(const clare_audio_config_t *config);

/** Convenience C ABI entry point for a C++ CodecPort object (opaque pointer). */
esp_err_t clare_audio_init_from_codec(void *codec_port);

/** Close the devices and release adapter-owned buffers. Handles remain owned by CodecPort. */
esp_err_t clare_audio_deinit(void);
bool clare_audio_is_ready(void);

/** Start/stop logical capture and playback streams. Init starts both streams. */
esp_err_t clare_audio_capture_start(void);
esp_err_t clare_audio_capture_stop(void);
esp_err_t clare_audio_playback_start(void);
esp_err_t clare_audio_playback_stop(void);

/**
 * Read exactly `frames` mono PCM16 samples.
 *
 * This call is deliberately blocking: esp_codec_dev_read waits for the I2S DMA
 * frame and has no timeout parameter. It returns `frames` on success and a
 * negative value on a codec/argument/state error.
 */
int clare_audio_read_mono_pcm16(int16_t *dst, size_t frames);

/** Byte-oriented alias for WebSocket feed code. Returns bytes read or -1. */
int clare_audio_read_pcm16(void *dst, size_t bytes);

/** Write 16 kHz mono PCM16. The call blocks until the codec accepts the data. */
esp_err_t clare_audio_write_pcm16(const int16_t *pcm, size_t frames);
esp_err_t clare_audio_play_pcm16(const int16_t *pcm, size_t frames);

esp_err_t clare_audio_set_volume(int volume);
int clare_audio_get_volume(void);
esp_err_t clare_audio_set_input_gain(float gain_db);

/*
 * Automatic Level Control (ALC) for the 16 kHz mono network path, provided by
 * Espressif's esp_audio_effects component.  The handle is independent of the
 * codec handles above: call clare_audio_alc_init() once at startup, then feed
 * mono PCM16 frames through clare_audio_alc_process() before handing them to
 * the network layer.  ALC applies a fixed digital gain with built-in clipping
 * protection and smooth gain transitions.
 *
 * clare_audio_alc_process() is a no-op until clare_audio_alc_init() succeeds,
 * so the capture path keeps working (ungained) if ALC allocation fails.
 * clare_audio_alc_process() must only be called from a single task.
 */
esp_err_t clare_audio_alc_init(uint32_t sample_rate, int8_t gain_db);
esp_err_t clare_audio_alc_set_gain(int8_t gain_db);
esp_err_t clare_audio_alc_process(int16_t *pcm, size_t frames);
void clare_audio_alc_deinit(void);

/*
 * Optional streaming MP3 support. These functions are implemented when the
 * project links Espressif's `esp_audio_codec` component; otherwise they return
 * ESP_ERR_NOT_SUPPORTED without touching the codec. Chunks may split MP3
 * frames, so callers should use start/write/end for a response stream.
 */
esp_err_t clare_audio_mp3_start(void);
esp_err_t clare_audio_mp3_write(const uint8_t *data, size_t len, bool end_of_stream);
esp_err_t clare_audio_mp3_end(void);
esp_err_t clare_audio_play_mp3(const uint8_t *data, size_t len);

/* Compatibility names used by the original ws_meeting_demo pipeline. */
static inline int clare_audio_recorder_read(void *dst, size_t bytes)
{
    return clare_audio_read_pcm16(dst, bytes);
}
static inline esp_err_t clare_audio_player_write_pcm(const int16_t *pcm, int frames)
{
    return (frames < 0) ? ESP_ERR_INVALID_ARG : clare_audio_write_pcm16(pcm, (size_t)frames);
}

#ifdef __cplusplus
}
#endif

