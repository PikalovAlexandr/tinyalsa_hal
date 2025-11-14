/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file audio_hw.h
 * @brief
 *                 ALSA Audio Git Log
 * - V0.1.0:add alsa audio hal,just support 312x now.
 * - V0.2.0:remove unused variable.
 * - V0.3.0:turn off device when do_standby.
 * - V0.4.0:turn off device before open pcm.
 * - V0.4.1:Need to re-open the control to fix no sound when suspend.
 * - V0.5.0:Merge the mixer operation from legacy_alsa.
 * - V0.6.0:Merge speex denoise from legacy_alsa.
 * - V0.7.0:add copyright.
 * - V0.7.1:add support for box audio
 * - V0.7.2:add support for dircet output
 * - V0.8.0:update the direct output for box, add the DVI mode
 * - V1.0.0:stable version
 *
 * @author  RkAudio
 * @version 1.0.8
 * @date 2015-08-24
 */

#ifndef AUIDO_HW_H
#define AUIDO_HW_H
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>

#include <cutils/log.h>
#include <cutils/list.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>

#include <linux/videodev2.h>

#include <system/audio.h>

#include "asoundlib.h"

#include <audio_utils/resampler.h>
#include <audio_route/audio_route.h>


//#include <speex/speex.h>
#include <speex/speex_preprocess.h>


#include <poll.h>
#include <linux/fb.h>
#include <hardware_legacy/uevent.h>

#include "voice_preprocess.h"
#include "audio_hw_hdmi.h"

#define AUDIO_HAL_VERSION "ALSA Audio Version: V1.1.0"

#define PCM_DEVICE 0
#define PCM_DEVICE_SCO 1
#define PCM_DEVICE_VOICE 2
#define PCM_DEVICE_HDMIIN 2
#define PCM_DEVICE_DEEP 3
/* for bt client call */
#define PCM_DEVICE_HFP 1

#define MIXER_CARD 0

/* duration in ms of volume ramp applied when starting capture to remove plop */
#define CAPTURE_START_RAMP_MS 100

/* default sampling for default output */
#define DEFAULT_PLAYBACK_SAMPLERATE 44100

#define DEFAULT_PLAYBACK_CHANNELS 2

/* default sampling for HDMI multichannel output */
#define HDMI_MULTI_DEFAULT_SAMPLING_RATE  44100
/* maximum number of channel mask configurations supported. Currently the primary
 * output only supports 1 (stereo) and the multi channel HDMI output 2 (5.1 and 7.1) */
#define MAX_SUPPORTED_CHANNEL_MASKS 2
#define MAX_SUPPORTED_SAMPLE_RATES 2

#ifndef BOX_HAL
#define SPEEX_DENOISE_ENABLE
#endif

#define HW_PARAMS_FLAG_LPCM 0
#define HW_PARAMS_FLAG_NLPCM 1

#ifdef BOX_HAL
struct pcm_config pcm_config = {
    .channels = 2,
    .rate = 44100,
    .period_size = 512,
    .period_count = 3,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = 44100,
    .period_size = 1024,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};
#elif defined RK3399_LAPTOP
struct pcm_config pcm_config = {
    .channels = 2,
    .rate = 48000,
    .period_size = 480,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = 48000,
    .period_size = 120,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};
#endif
#if !defined RK3399_LAPTOP
struct pcm_config pcm_config = {
    .channels = 2,
    .rate = 44100,
    .period_size = 512,
    .period_count = 6,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = 44100,
#ifdef SPEEX_DENOISE_ENABLE
    .period_size = 1024,
#else
    .period_size = 256,
#endif
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};
#endif

struct pcm_config pcm_config_in_low_latency = {
    .channels = 2,
    .rate = 44100,
    .period_size = 256,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_sco = {
    .channels = 1,
    .rate = 8000,
    .period_size = 128,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

/* for bt client call*/
struct pcm_config pcm_config_hfp = {
    .channels = 2,
    .rate = 44100,
    .period_size = 256,
    .period_count = 4,
};
#ifdef BT_AP_SCO
struct pcm_config pcm_config_ap_sco = {
    .channels = 2,
    .rate = 8000,
    .period_size = 80,
    .period_count = 4,
};

struct pcm_config pcm_config_in_bt = {
    .channels = 2,
    .rate = 8000,
    .period_size = 120,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};
#endif
struct pcm_config pcm_config_deep = {
    .channels = 2,
    .rate = 44100,
    /* FIXME This is an arbitrary number, may change.
     * Dynamic configuration based on screen on/off is not implemented;
     * let's see what power consumption is first to see if necessary.
     */
    .period_size = 8192,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_hdmi_multi = {
    .channels = 6, /* changed when the stream is opened */
    .rate = HDMI_MULTI_DEFAULT_SAMPLING_RATE,
    .period_size = 1024,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_direct = {
    .channels = 2,
    .rate = 48000,
    .period_size = 1024*4,
    .period_count = 3,
    .format = PCM_FORMAT_S16_LE,
};

#define SIMCOM_MODEM_RATE            8000
#define SIMCOM_MODEM_CHANNELS        1
#define SIMCOM_MODEM_PERIOD_SAMPLES  320
#define SIMCOM_MODEM_PERIOD_BYTES    (SIMCOM_MODEM_PERIOD_SAMPLES * sizeof(int16_t))

struct pcm_config pcm_config_simcom = {
    .channels = SIMCOM_MODEM_CHANNELS,
    .rate = SIMCOM_MODEM_RATE,
    .period_size = SIMCOM_MODEM_PERIOD_SAMPLES,     /* 640 bytes = 320 samples at 16-bit mono */
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_in_simcom = {
    .channels = 1,
    .rate = 8000,
    .period_size = 800,     /* 1600 bytes = 800 samples at 16-bit mono */
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

enum output_type {
    OUTPUT_DEEP_BUF,      // deep PCM buffers output stream
    OUTPUT_LOW_LATENCY,   // low latency output stream
    OUTPUT_HDMI_MULTI,          // HDMI multi channel
    OUTPUT_DIRECT,
    OUTPUT_TOTAL
};

struct direct_mode_t {
    int output_mode;
    char* hbr_Buf;
};

enum snd_out_sound_cards {
    SND_OUT_SOUND_CARD_UNKNOWN = -1,
    SND_OUT_SOUND_CARD_SPEAKER = 0,
    SND_OUT_SOUND_CARD_HDMI,
    SND_OUT_SOUND_CARD_SPDIF,
    SND_OUT_SOUND_CARD_BT,
    SND_OUT_SOUND_CARD_MAX,
};

enum snd_in_sound_cards {
    SND_IN_SOUND_CARD_UNKNOWN = -1,
    SND_IN_SOUND_CARD_MIC = 0,
    SND_IN_SOUND_CARD_BT,
    SND_IN_SOUND_CARD_HDMI,
    SND_IN_SOUND_CARD_MAX,
};

struct dev_proc_info
{
    const char *cid; /* cardX/id match */
    const char *did; /* dai id match */
};

struct dev_info
{
    const char *id;
    int card;
    int device;
};

struct stream_in;
struct stream_out;

typedef enum usecase_type_t {
    USECASE_TYPE_INVALID = -1,
    USECASE_TYPE_PCM_PLAYBACK = 0,
    USECASE_TYPE_PCM_CAPTURE,
    USECASE_TYPE_VOICE_CALL,
    USECASE_TYPE_MAX
} usecase_type_t;

typedef enum audio_usecase_t {
    USECASE_INVALID = -1,
    USECASE_PRIMARY_PLAYBACK = 0,
    USECASE_PRIMARY_CAPTURE,
    USECASE_SIMCOM_VOICE_CALL,
    USECASE_MAX
} audio_usecase_t;

union stream_ptr {
    struct stream_in *in;
    struct stream_out *out;
};

struct audio_usecase {
    struct listnode list;
    audio_usecase_t id;
    usecase_type_t type;
    audio_devices_t devices;
    union stream_ptr stream;
};

struct simcom_capture_stats {
    uint64_t total_samples;
    uint64_t sum_abs;
    int32_t max_abs;
    uint32_t call_count;
    uint32_t zero_batches;
    uint32_t nonzero_batches;
    uint32_t consecutive_zero;
    bool final_reported;
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    audio_devices_t out_device; /* "or" of stream_out.device for all active output streams */
    audio_devices_t in_device;
    bool mic_mute;
    struct audio_route *ar;
    audio_source_t input_source;
    audio_channel_mask_t in_channel_mask;

    struct stream_out *outputs[OUTPUT_TOTAL];
    pthread_mutex_t lock_outputs; /* see note below on mutex acquisition order */
    unsigned int mode;
    bool   screenOff;
#ifdef AUDIO_3A
    rk_process_api* voice_api;
#endif

    /*
     * hh@rock-chips.com
     * this is for HDMI/SPDIF bitstream
     * when HDMI/SPDIF bistream AC3/EAC3/DTS/TRUEHD/DTS-HD, some key tone or other pcm
     * datas may come(play a Ac3 audio and seek the file to play). It is not allow to open sound card
     * as pcm format and not allow to write pcm datas to HDMI/SPDIF sound cards when open it
     * with config.flag = 1.
     */
    int*  owner[3];  /* HDMI, SPDIF, BT/SIMCOM */

    struct dev_info dev_out[SND_OUT_SOUND_CARD_MAX];
    struct dev_info dev_in[SND_IN_SOUND_CARD_MAX];

    struct listnode usecase_list;

    bool simcom_mic_route_active;
    pthread_mutex_t simcom_mic_lock;
    pthread_cond_t simcom_mic_cond;  // Condition variable для синхронизации capture и uplink потоков
    int16_t *simcom_mic_ring;
    size_t simcom_mic_ring_size;
    size_t simcom_mic_ring_read;
    size_t simcom_mic_ring_write;
    bool simcom_mic_ring_full;
    bool simcom_voice_active;
    struct pcm *simcom_voice_pcm;
    pthread_t simcom_voice_thread;
    bool simcom_voice_thread_started;
    bool simcom_voice_thread_stop;
    uint32_t simcom_voice_rate;
    uint32_t simcom_voice_channels;
    size_t simcom_voice_period_size;
    bool simcom_mixer_configured;
    int simcom_mixer_card;
    bool simcom_cpcmreg_state;
    struct pcm *simcom_modem_pcm;
    struct pcm *simcom_downlink_pcm;
    struct pcm *simcom_speaker_pcm;
    bool simcom_direct_mode_enabled;
    bool simcom_direct_path_ready;
    bool simcom_capture_direct_8k;
    bool simcom_downlink_thread_started;
    bool simcom_downlink_thread_stop;
    pthread_t simcom_downlink_thread;
    bool simcom_speaker_needs_resample;
    uint32_t simcom_speaker_rate;
    uint32_t simcom_speaker_channels;
    double simcom_downlink_resample_pos;
    int16_t *simcom_downlink_resample_buf;
    size_t simcom_downlink_resample_capacity;
    size_t simcom_uplink_accum_used;
    int16_t simcom_uplink_accum[SIMCOM_MODEM_PERIOD_SAMPLES];
    struct simcom_capture_stats simcom_stats;
    uint32_t simcom_capture_batches;
    uint32_t simcom_capture_zero_batches;
    uint32_t simcom_capture_nonzero_batches;
    uint32_t simcom_capture_consecutive_zero;
    uint32_t simcom_silence_recoveries;
    uint64_t simcom_last_silence_recover_ms;
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm[SND_OUT_SOUND_CARD_MAX];
    struct pcm_config config;
    struct audio_config aud_config;
    unsigned int pcm_device;
    bool standby; /* true if all PCMs are inactive */
    audio_devices_t device;
    /* FIXME: when HDMI multichannel output is active, other outputs must be disabled as
     * HDMI and WM1811 share the same I2S. This means that notifications and other sounds are
     * silent when watching a 5.1 movie. */
    bool disabled;
    audio_channel_mask_t channel_mask;
    /* Array of supported channel mask configurations. +1 so that the last entry is always 0 */
    audio_channel_mask_t supported_channel_masks[MAX_SUPPORTED_CHANNEL_MASKS + 1];
    uint32_t supported_sample_rates[MAX_SUPPORTED_SAMPLE_RATES + 1];
    bool muted;
    uint64_t written; /* total frames written, not cleared when entering standby */
    uint64_t nframes;

    /*
     * true: current stream take sound card in exclusive Mode, when this stream using this sound card,
     *       other stream can't using this sound card. This happen when current stream
     *       is multi pcm stream or bitstream. Multi channels pcm datas or bitstream datas can't be mixed.
     * false: current stream is 2 channels pcm stream
     */
    bool output_direct;

    /*
     * LPCM:  pcm datas(include multi channels pcm)
     * others: bitstream
     */
    int output_direct_mode;
    audio_usecase_t usecase;
    usecase_type_t usecase_type;
    struct audio_device *dev;
    struct resampler_itfe *resampler;
    // for hdmi bitstream
    char* channel_buffer;
    char* bitstream_buffer;

    struct hdmi_audio_infors hdmi_audio;

    bool   snd_reopen;
    /* SIMCOM buffer accumulation: accumulate data until full period (640 bytes) */
    int16_t *simcom_buffer;
    size_t simcom_buffer_used;
    bool simcom_pcm_started;  /* Track if PCM has been started for SIMCOM */
    int simcom_periods_written;  /* Count of periods written before starting PCM */
};

struct stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    bool standby;

    unsigned int requested_rate;
    struct resampler_itfe *resampler;
    struct resampler_buffer_provider buf_provider;
    int16_t *buffer;
    size_t frames_in;
    int read_status;
    audio_source_t input_source;
    audio_io_handle_t io_handle;
    audio_devices_t device;
    uint16_t ramp_vol;
    uint16_t ramp_step;
    size_t  ramp_frames;
    audio_channel_mask_t channel_mask;
    audio_input_flags_t flags;
    struct pcm_config *config;
    bool simcom_input;
    bool simcom_voice_capture;
    int16_t *simcom_mono_buf;
    size_t simcom_mono_capacity;
    int16_t *simcom_downsample_buf;
    size_t simcom_downsample_capacity;
    double simcom_resample_pos;
    uint32_t simcom_last_rate;
    uint32_t simcom_last_channels;
    audio_usecase_t usecase;
    usecase_type_t usecase_type;

    struct audio_device *dev;
    audio_channel_mask_t supported_channel_masks[MAX_SUPPORTED_CHANNEL_MASKS + 1];
    uint32_t supported_sample_rates[MAX_SUPPORTED_SAMPLE_RATES + 1];
#ifdef SPEEX_DENOISE_ENABLE
    SpeexPreprocessState* mSpeexState;
    int mSpeexFrameSize;
    int16_t *mSpeexPcmIn;
#endif
};

#define STRING_TO_ENUM(string) { #string, string }

struct string_to_enum {
    const char *name;
    uint32_t value;
};

static const struct string_to_enum channels_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_5POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_7POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_FRONT_BACK),
};

enum {
    OUT_DEVICE_SPEAKER,
    OUT_DEVICE_HEADSET,
    OUT_DEVICE_HEADPHONES,
    OUT_DEVICE_BT_SCO,
    OUT_DEVICE_SPEAKER_AND_HEADSET,
    OUT_DEVICE_OFF,
    OUT_DEVICE_TAB_SIZE,           /* number of rows in route_configs[][] */
    OUT_DEVICE_NONE,
    OUT_DEVICE_CNT
};

enum {
    IN_SOURCE_MIC,
    IN_SOURCE_CAMCORDER,
    IN_SOURCE_VOICE_RECOGNITION,
    IN_SOURCE_VOICE_COMMUNICATION,
    IN_SOURCE_OFF,
    IN_SOURCE_TAB_SIZE,            /* number of lines in route_configs[][] */
    IN_SOURCE_NONE,
    IN_SOURCE_CNT
};

enum {
    LPCM = 0,
    NLPCM,
    HBR,
};

struct route_config {
    const char * const output_route;
    const char * const input_route;
    const char * const output_off;
    const char * const input_off;
};

const struct route_config media_speaker = {
    "media-speaker",
    "media-main-mic",
    "playback-off",
    "capture-off",
};

const struct route_config media_headphones = {
    "media-headphones",
    "media-main-mic",
    "playback-off",
    "capture-off",
};

const struct route_config media_headset = {
    "media-headphones",
    "media-headset-mic",
    "playback-off",
    "capture-off",
};

const struct route_config camcorder_speaker = {
    "media-speaker",
    "media-second-mic",
    "playback-off",
    "capture-off",
};

const struct route_config camcorder_headphones = {
    "media-headphones",
    "media-second-mic",
    "playback-off",
    "capture-off",
};

const struct route_config voice_rec_speaker = {
    "voice-rec-speaker",
    "voice-rec-main-mic",
    "incall-off",
    "incall-off",
};

const struct route_config voice_rec_headphones = {
    "voice-rec-headphones",
    "voice-rec-main-mic",
    "incall-off",
    "incall-off",
};

const struct route_config voice_rec_headset = {
    "voice-rec-headphones",
    "voice-rec-headset-mic",
    "incall-off",
    "incall-off",
};

const struct route_config communication_speaker = {
    "communication-speaker",
    "communication-main-mic",
    "voip-off",
    "voip-off",
};

const struct route_config communication_headphones = {
    "communication-headphones",
    "communication-main-mic",
    "voip-off",
    "voip-off",
};

const struct route_config communication_headset = {
    "communication-headphones",
    "communication-headset-mic",
    "voip-off",
    "voip-off",
};

const struct route_config speaker_and_headphones = {
    "speaker-and-headphones",
    "main-mic",
    "playback-off",
    "capture-off",
};

const struct route_config bluetooth_sco = {
    "bt-sco-headset",
    "bt-sco-mic",
    "playback-off",
    "capture-off",
};

const struct route_config * const route_configs[IN_SOURCE_TAB_SIZE]
        [OUT_DEVICE_TAB_SIZE] = {
    {   /* IN_SOURCE_MIC */
        &media_speaker,             /* OUT_DEVICE_SPEAKER */
        &media_headset,             /* OUT_DEVICE_HEADSET */
        &media_headphones,          /* OUT_DEVICE_HEADPHONES */
        &bluetooth_sco,             /* OUT_DEVICE_BT_SCO */
        &speaker_and_headphones     /* OUT_DEVICE_SPEAKER_AND_HEADSET */
    },
    {   /* IN_SOURCE_CAMCORDER */
        &camcorder_speaker,         /* OUT_DEVICE_SPEAKER */
        &camcorder_headphones,      /* OUT_DEVICE_HEADSET */
        &camcorder_headphones,      /* OUT_DEVICE_HEADPHONES */
        &bluetooth_sco,             /* OUT_DEVICE_BT_SCO */
        &speaker_and_headphones     /* OUT_DEVICE_SPEAKER_AND_HEADSET */
    },
    {   /* IN_SOURCE_VOICE_RECOGNITION */
        &voice_rec_speaker,         /* OUT_DEVICE_SPEAKER */
        &voice_rec_headset,         /* OUT_DEVICE_HEADSET */
        &voice_rec_headphones,      /* OUT_DEVICE_HEADPHONES */
        &bluetooth_sco,             /* OUT_DEVICE_BT_SCO */
        &speaker_and_headphones     /* OUT_DEVICE_SPEAKER_AND_HEADSET */
    },
    {   /* IN_SOURCE_VOICE_COMMUNICATION */
        &communication_speaker,     /* OUT_DEVICE_SPEAKER */
        &communication_headset,     /* OUT_DEVICE_HEADSET */
        &communication_headphones,  /* OUT_DEVICE_HEADPHONES */
        &bluetooth_sco,             /* OUT_DEVICE_BT_SCO */
        &speaker_and_headphones     /* OUT_DEVICE_SPEAKER_AND_HEADSET */
    }
};

static void do_out_standby(struct stream_out *out);
#endif

