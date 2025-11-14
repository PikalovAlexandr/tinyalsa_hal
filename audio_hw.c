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
 * @file    audio_hw.c
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
 * @version 1.0.5
 * @date    2015-08-24
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioHardwareTiny"

#include "alsa_audio.h"
#include "audio_hw.h"
#include <system/audio.h>
#include "codec_config/config.h"
#include "audio_bitstream.h"
#include "audio_setting.h"
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>
#include <pthread.h>
#include <log/log.h>
#include <cutils/properties.h>
#include <cutils/list.h>
#include "asound.h"
#define SNDRV_CARDS 8
#define SNDRV_DEVICES 8

#define SIMCOM_MIC_RING_CAPACITY_SAMPLES (8000 * 6) // ~6 seconds of uplink audio
#define SIMCOM_TTY_DEVICE "/dev/ttyUSB3"

static void simcom_ring_reset(struct audio_device *adev);
static void simcom_ring_push(struct audio_device *adev, const int16_t *src, size_t samples);
static size_t simcom_ring_pop(struct audio_device *adev, int16_t *dst, size_t samples);
static int simcom_voice_ensure_ring(struct audio_device *adev);
static int simcom_voice_ensure_buffer(int16_t **buffer, size_t *capacity, size_t required);
static void simcom_voice_process_and_push(struct audio_device *adev,
        const int16_t *src, size_t frames, uint32_t channels, uint32_t rate,
        int16_t **mono_buf, size_t *mono_capacity,
        int16_t **downsample_buf, size_t *downsample_capacity,
        double *resample_pos, uint32_t *last_rate, uint32_t *last_channels);
static int simcom_voice_start_capture(struct audio_device *adev);
static void simcom_voice_stop_capture(struct audio_device *adev);
static void *simcom_voice_capture_thread(void *context);
static void simcom_voice_start_usecase(struct audio_device *adev);
static void simcom_voice_stop_usecase(struct audio_device *adev);
static bool simcom_update_cpcmreg(struct audio_device *adev, bool enable);
static bool simcom_send_at_command(const char *cmd);

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SND_CARDS_NODE          "/proc/asound/cards"

struct SurroundFormat {
    audio_format_t format;
    const char *value;
};


const struct SurroundFormat sSurroundFormat[] = {
    {AUDIO_FORMAT_AC3,"AUDIO_FORMAT_AC3"},
    {AUDIO_FORMAT_E_AC3,"AUDIO_FORMAT_E_AC3"},
    {AUDIO_FORMAT_DTS,"AUDIO_FORMAT_DTS"},
    {AUDIO_FORMAT_DTS_HD,"AUDIO_FORMAT_DTS_HD"},
    {AUDIO_FORMAT_AAC_LC,"AUDIO_FORMAT_AAC_LC"},
    {AUDIO_FORMAT_DOLBY_TRUEHD,"AUDIO_FORMAT_DOLBY_TRUEHD"},
    {AUDIO_FORMAT_AC4,"AUDIO_FORMAT_E_AC3_JOC"}
};

enum SOUND_CARD_OWNER{
    SOUND_CARD_HDMI = 0,
    SOUND_CARD_SPDIF = 1,
    SOUND_CARD_BT = 2,
};

/*
 * mute audio datas when screen off or standby
 * The MediaPlayer no stop/pause when screen off, they may be just play in background,
 * so they still send audio datas to audio hal.
 * HDMI may disconnet and enter stanby status, this means no voice output on HDMI
 * but speaker/av and spdif still work, and voice may output on them.
 * Some customer need to mute the audio datas in this condition.
 * If need mute datas when screen off, define this marco.
 */
//#define MUTE_WHEN_SCREEN_OFF

/*
 * if current audio stream bitstream over hdmi,
 * and hdmi is removed and reconnected later,
 * the driver of hdmi may config it with pcm mode automatically,
 * which is according the implement of hdmi driver.
 * If hdmi driver implement in this way, in order to output audio
 * bitstream stream after hdmi reconnected,
 * we must close sound card of hdmi and reopen/config
 * it in bitstream mode. If need this, define this macro.
 */
#define AUDIO_BITSTREAM_REOPEN_HDMI

//#define ALSA_DEBUG
#ifdef ALSA_IN_DEBUG
FILE *in_debug;
#endif

int in_dump(const struct audio_stream *stream, int fd);
int out_dump(const struct audio_stream *stream, int fd);
static inline bool hasExtCodec();
static void read_in_sound_card(struct stream_in *in);

static bool simcom_debug_audio_enabled(void)
{
    static int initialized = 0;
    static bool enabled = false;
    if (!initialized) {
        enabled = property_get_bool("persist.vendor.simcom.debug_audio", false);
        initialized = 1;
    }
    return enabled;
}

struct simcom_mixer_setting {
    const char *name;
    int target_value;
    bool is_switch;
    bool optional;
};

static const struct simcom_mixer_setting kSimcom_mic_settings[] = {
    {"IN Capture Volume", 40, false, false},
    {"ADC Capture Volume", 55, false, false},
    {"Mono ADC Capture Volume", 55, false, true},
    {"ADC Capture Switch", 1, true, false},
    {"RECMIXL BST3 Switch", 1, true, false},
    {"RECMIXR BST3 Switch", 1, true, false},
    {"MIC1 Boost Capture Volume", 40, false, true},
    {"IN3 Boost", 3, false, true},
    {"Stereo1 ADC MIXL ADC1 Switch", 1, true, true},
    {"Stereo1 ADC MIXR ADC1 Switch", 1, true, true},
    {"Stereo2 ADC MIXL ADC1 Switch", 1, true, true},
    {"Stereo2 ADC MIXR ADC1 Switch", 1, true, true}
};

static void simcom_configure_mic_controls(struct audio_device *adev, int mic_card)
{
    if (!adev || mic_card < 0) {
        return;
    }

    if (adev->simcom_mixer_configured && adev->simcom_mixer_card == mic_card) {
        return;
    }

    struct mixer *mixer = mixer_open_legacy(mic_card);
    if (!mixer) {
        ALOGE("SIMCOM mixer: failed to open mixer for card %d", mic_card);
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(kSimcom_mic_settings); ++i) {
        const struct simcom_mixer_setting *setting = &kSimcom_mic_settings[i];
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, setting->name);
        if (!ctl) {
            if (!setting->optional) {
                ALOGW("SIMCOM mixer: control '%s' not found on card %d", setting->name, mic_card);
            } else if (simcom_debug_audio_enabled()) {
                ALOGD("SIMCOM mixer: optional control '%s' missing on card %d", setting->name, mic_card);
            }
            continue;
        }

        long min = mixer_ctl_get_range_min(ctl);
        long max = mixer_ctl_get_range_max(ctl);
        int value = setting->target_value;
        if (max >= min) {
            if (value > max) value = (int)max;
            if (value < min) value = (int)min;
        }

        int num_values = mixer_ctl_get_num_values(ctl);
        bool changed = false;
        for (int v = 0; v < num_values; ++v) {
            int current = mixer_ctl_get_value(ctl, v);
            if (current == value) {
                continue;
            }
            if (mixer_ctl_set_value(ctl, v, value) == 0) {
                changed = true;
            } else {
                ALOGW("SIMCOM mixer: failed to set '%s'[%d] to %d (card=%d)", setting->name, v, value, mic_card);
            }
        }

        if (changed || simcom_debug_audio_enabled()) {
            ALOGE("SIMCOM mixer: %s set to %d (card=%d)", setting->name, value, mic_card);
        }
    }

    mixer_close_legacy(mixer);
    adev->simcom_mixer_configured = true;
    adev->simcom_mixer_card = mic_card;
}

static void simcom_verify_mic_controls(int mic_card)
{
    if (mic_card < 0) {
        return;
    }

    struct mixer *mixer = mixer_open_legacy(mic_card);
    if (!mixer) {
        ALOGW("SIMCOM mixer verify: failed to open mixer for card %d", mic_card);
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(kSimcom_mic_settings); ++i) {
        const struct simcom_mixer_setting *setting = &kSimcom_mic_settings[i];
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, setting->name);
        if (!ctl) {
            if (!setting->optional) {
                ALOGW("SIMCOM mixer verify: control '%s' missing (card=%d)", setting->name, mic_card);
            }
            continue;
        }

        int current = mixer_ctl_get_value(ctl, 0);
        bool ok = (current == setting->target_value);
        ALOGE("SIMCOM mixer verify: %s [%s] (current=%d target=%d)",
              setting->name,
              ok ? "OK" : "MISMATCH",
              current,
              setting->target_value);
    }

    mixer_close_legacy(mixer);
}

static const char *simcom_classify_signal(int32_t avg_abs)
{
    if (avg_abs < 5) {
        return "SILENCE";
    }
    if (avg_abs < 20) {
        return "LOW";
    }
    if (avg_abs < 120) {
        return "NORMAL";
    }
    return "LOUD";
}

static void simcom_log_capture_summary(struct audio_device *adev, const char *reason)
{
    if (!adev || !simcom_debug_audio_enabled()) {
        return;
    }
    struct simcom_capture_stats *st = &adev->simcom_stats;
    if (st->final_reported) {
        return;
    }
    st->final_reported = true;
    int32_t avg_abs = (st->total_samples > 0)
                      ? (int32_t)(st->sum_abs / (int64_t)st->total_samples)
                      : 0;
    ALOGE("SIMCOM CAPTURE SUMMARY (%s): calls=%u zero=%u nz=%u avg=%d max=%d level=%s",
          reason ? reason : "final",
          st->call_count,
          st->zero_batches,
          st->nonzero_batches,
          avg_abs,
          st->max_abs,
          simcom_classify_signal(avg_abs));
}

static void simcom_trace_capture_preview(struct audio_device *adev,
        const int16_t *src, size_t frames, uint32_t channels)
{
    if (!adev || !simcom_debug_audio_enabled() || !src || frames == 0 || channels == 0) {
        return;
    }

    struct simcom_capture_stats *st = &adev->simcom_stats;
    size_t samples = frames * channels;
    bool all_zero = true;
    int64_t sum_abs = 0;
    int32_t max_abs = 0;
    for (size_t i = 0; i < samples; ++i) {
        int32_t sample = src[i];
        if (sample != 0) {
            all_zero = false;
        }
        if (sample < 0) sample = -sample;
        sum_abs += sample;
        if (sample > max_abs) {
            max_abs = sample;
        }
    }
    int32_t avg_abs = (samples > 0) ? (int32_t)(sum_abs / (int64_t)samples) : 0;

    st->call_count++;
    st->total_samples += samples;
    st->sum_abs += sum_abs;
    if (max_abs > st->max_abs) {
        st->max_abs = max_abs;
    }

    if (all_zero) {
        st->zero_batches++;
        st->consecutive_zero++;
    } else {
        st->nonzero_batches++;
        st->consecutive_zero = 0;
    }

    bool log_initial = st->call_count <= 10;
    bool log_warning = (st->consecutive_zero == 5) ||
                       (st->consecutive_zero > 5 &&
                        (st->consecutive_zero % 5) == 0);
    bool log_periodic = (!all_zero && (st->call_count % 25) == 0);

    if (log_initial || log_warning || log_periodic) {
        char sample_log[160];
        size_t to_log = (samples < 8) ? samples : 8;
        size_t offset = 0;
        for (size_t i = 0; i < to_log && offset < sizeof(sample_log); ++i) {
            offset += snprintf(sample_log + offset,
                               sizeof(sample_log) - offset,
                               "%d%s",
                               src[i],
                               (i + 1 < to_log) ? " " : "");
        }

        ALOGE("SIMCOM CAPTURE RAW: batch=%u zeros=%u nz=%u avg=%d max=%d level=%s first[%zu]=%s%s",
              st->call_count,
              st->zero_batches,
              st->nonzero_batches,
              avg_abs,
              max_abs,
              simcom_classify_signal(avg_abs),
              (samples < 8) ? samples : 8,
              sample_log,
              log_warning ? " [WARNING: consecutive zero batches]" : "");
    }

    if (st->call_count == 10 && !st->final_reported) {
        simcom_log_capture_summary(adev, "initial");
    }
}

static void simcom_ring_reset(struct audio_device *adev)
{
    if (!adev->simcom_mic_ring) {
        return;
    }
    pthread_mutex_lock(&adev->simcom_mic_lock);
    adev->simcom_mic_ring_read = 0;
    adev->simcom_mic_ring_write = 0;
    adev->simcom_mic_ring_full = false;
    pthread_mutex_unlock(&adev->simcom_mic_lock);
}

static void simcom_ring_push(struct audio_device *adev, const int16_t *src, size_t samples)
{
    if (!adev->simcom_mic_ring || samples == 0) {
        return;
    }

    pthread_mutex_lock(&adev->simcom_mic_lock);
    const size_t capacity = adev->simcom_mic_ring_size;
    
    // ИСПРАВЛЕНИЕ: оптимизация записи - блочное копирование вместо sample-by-sample
    // Обрабатываем wrap-around случаи для эффективного копирования
    size_t remaining = samples;
    size_t write_pos = adev->simcom_mic_ring_write;
    
    while (remaining > 0) {
        // Вычисляем сколько samples можно записать до конца буфера
        size_t space_to_end = capacity - write_pos;
        size_t to_write = (remaining < space_to_end) ? remaining : space_to_end;
        
        // Блочное копирование
        memcpy(&adev->simcom_mic_ring[write_pos], &src[samples - remaining], 
               to_write * sizeof(int16_t));
        
        write_pos = (write_pos + to_write) % capacity;
        remaining -= to_write;
    }
    
    adev->simcom_mic_ring_write = write_pos;
    
    // Обновляем флаг full и read указатель при переполнении
    if (adev->simcom_mic_ring_full) {
        adev->simcom_mic_ring_read = adev->simcom_mic_ring_write;
    } else if (adev->simcom_mic_ring_write == adev->simcom_mic_ring_read) {
        adev->simcom_mic_ring_full = true;
    }
    
    // ИСПРАВЛЕНИЕ: детальное логирование переполнения буфера с recovery механизмом
    if (adev->simcom_mic_ring_full) {
        static unsigned int ring_overwrite_counter = 0;
        static unsigned int ring_recovery_counter = 0;
        ring_overwrite_counter++;
        
        // Вычисляем заполненность буфера в samples
        size_t used_samples = 0;
        if (adev->simcom_mic_ring_read < adev->simcom_mic_ring_write) {
            used_samples = adev->simcom_mic_ring_write - adev->simcom_mic_ring_read;
        } else if (adev->simcom_mic_ring_read > adev->simcom_mic_ring_write) {
            used_samples = capacity - adev->simcom_mic_ring_read + adev->simcom_mic_ring_write;
        } else {
            used_samples = capacity;  // Буфер полон
        }
        size_t fill_percent = (capacity > 0) ? (used_samples * 100 / capacity) : 0;
        
        // Логируем переполнение чаще для диагностики
        if ((ring_overwrite_counter & 0x7) == 0) {
            ALOGE("SIMCOM MIC DBG: ring overwrite (write=%zu read=%zu cap=%zu fill=%zu%% used=%zu)",
                  adev->simcom_mic_ring_write,
                  adev->simcom_mic_ring_read,
                  capacity, fill_percent, used_samples);
        }
        
        // ИСПРАВЛЕНИЕ: recovery механизм - при частых переполнениях сбрасываем read указатель
        // Это предотвращает накопление старых данных и освобождает место для новых
        if (ring_overwrite_counter > 100 && (ring_overwrite_counter % 50) == 0) {
            // Если переполнение происходит часто, сбрасываем read на write (теряем старые данные)
            // Это лучше чем полная блокировка буфера
            adev->simcom_mic_ring_read = adev->simcom_mic_ring_write;
            adev->simcom_mic_ring_full = false;
            ring_recovery_counter++;
            if (simcom_debug_audio_enabled()) {
                ALOGE("SIMCOM MIC DBG: ring recovery (overwrites=%u recoveries=%u, reset read to write)",
                      ring_overwrite_counter, ring_recovery_counter);
            }
        }
    }
    
    // ИСПРАВЛЕНИЕ: сигнализируем о новых данных после разблокировки мьютекса
    // Это предотвращает deadlock и обеспечивает пробуждение uplink потока
    pthread_mutex_unlock(&adev->simcom_mic_lock);
    // Сигнализируем ПОСЛЕ разблокировки мьютекса для предотвращения deadlock
    pthread_cond_signal(&adev->simcom_mic_cond);
}

static size_t simcom_ring_pop(struct audio_device *adev, int16_t *dst, size_t samples)
{
    if (!adev->simcom_mic_ring || samples == 0) {
        return 0;
    }

    pthread_mutex_lock(&adev->simcom_mic_lock);
    size_t count = 0;
    const size_t capacity = adev->simcom_mic_ring_size;
    
    // ИСПРАВЛЕНИЕ: добавить детальное логирование для диагностики
    // Убираем условие count == 0, чтобы логирование работало всегда
    static unsigned int pop_log_counter = 0;
    if (simcom_debug_audio_enabled() && samples > 0) {
        pop_log_counter++;
        if (pop_log_counter <= 5 || (pop_log_counter % 100) == 0) {
            ALOGE("SIMCOM RING POP: before read=%zu write=%zu full=%d req=%zu",
                  adev->simcom_mic_ring_read, adev->simcom_mic_ring_write,
                  adev->simcom_mic_ring_full, samples);
        }
    }
    
    // ИСПРАВЛЕНИЕ: вычисляем количество доступных samples один раз и читаем все сразу
    // Это предотвращает чтение по одному sample за раз
    // ВАЖНО: в circular buffer с флагом full, если буфер не полон и read > write,
    // это означает, что мы уже прочитали все данные (wrap-around произошел,
    // но данные были прочитаны, поэтому read "обогнал" write через 0)
    // В этом случае буфер фактически пуст (все данные прочитаны)
    size_t available = 0;
    if (adev->simcom_mic_ring_full) {
        // Буфер полон - доступно capacity samples
        available = capacity;
    } else if (adev->simcom_mic_ring_read < adev->simcom_mic_ring_write) {
        // read < write (без wrap-around) - доступно write - read
        available = adev->simcom_mic_ring_write - adev->simcom_mic_ring_read;
    } else if (adev->simcom_mic_ring_read == adev->simcom_mic_ring_write) {
        // read == write и буфер не полон - буфер пуст
        available = 0;
    } else {
        // read > write и буфер не полон - это означает, что мы прочитали все данные
        // и read "обогнал" write через wrap-around. Буфер фактически пуст.
        // Не читаем из области между read и capacity, так как там старые данные
        // (или нули из calloc), которые еще не были перезаписаны
        available = 0;
        
        if (simcom_debug_audio_enabled()) {
            static unsigned int read_ahead_counter = 0;
            read_ahead_counter++;
            if ((read_ahead_counter & 0x3F) == 0) {
                ALOGE("SIMCOM RING POP: read ahead of write (read=%zu write=%zu) - buffer empty, waiting for new data", 
                      adev->simcom_mic_ring_read, adev->simcom_mic_ring_write);
            }
        }
    }
    
    // Ограничиваем доступные samples запрошенным количеством
    if (available > samples) {
        available = samples;
    }
    
    // ИСПРАВЛЕНИЕ: оптимизация чтения - блочное копирование вместо sample-by-sample
    // Обрабатываем wrap-around случаи для эффективного копирования
    size_t remaining = available;
    size_t read_pos = adev->simcom_mic_ring_read;
    
    while (remaining > 0) {
        // Вычисляем сколько samples можно прочитать до конца буфера
        size_t space_to_end = capacity - read_pos;
        size_t to_read = (remaining < space_to_end) ? remaining : space_to_end;
        
        // Блочное копирование
        memcpy(&dst[count], &adev->simcom_mic_ring[read_pos], 
               to_read * sizeof(int16_t));
        
        read_pos = (read_pos + to_read) % capacity;
        count += to_read;
        remaining -= to_read;
    }
    
    adev->simcom_mic_ring_read = read_pos;
    adev->simcom_mic_ring_full = false;
    
    // ИСПРАВЛЕНИЕ: детальное логирование после чтения
    if (simcom_debug_audio_enabled()) {
        if (count == 0 && samples > 0) {
            static unsigned int ring_empty_counter = 0;
            ring_empty_counter++;
            if ((ring_empty_counter & 0xF) == 0) {
                ALOGE("SIMCOM MIC DBG: ring empty (req=%zu read=%zu write=%zu full=%d)",
                      samples,
                      adev->simcom_mic_ring_read,
                      adev->simcom_mic_ring_write,
                      adev->simcom_mic_ring_full);
            }
        } else if (count > 0) {
            // Логирование статистики считанных данных
            int32_t sum_abs = 0;
            int32_t max_abs = 0;
            for (size_t i = 0; i < count; ++i) {
                int32_t abs_val = (dst[i] < 0) ? -dst[i] : dst[i];
                sum_abs += abs_val;
                if (abs_val > max_abs) max_abs = abs_val;
            }
            int32_t avg_abs = (count > 0) ? (sum_abs / (int32_t)count) : 0;
            ALOGE("SIMCOM RING POP: read=%zu samples avg_abs=%d max_abs=%d first=%d last=%d (read_idx=%zu write_idx=%zu)",
                  count, avg_abs, max_abs, dst[0], dst[count-1],
                  adev->simcom_mic_ring_read, adev->simcom_mic_ring_write);
        }
    }
    
    pthread_mutex_unlock(&adev->simcom_mic_lock);
    return count;
}

static int simcom_voice_ensure_ring(struct audio_device *adev)
{
    if (!adev) {
        return -EINVAL;
    }

    if (!adev->simcom_mic_ring) {
        adev->simcom_mic_ring_size = SIMCOM_MIC_RING_CAPACITY_SAMPLES;
        adev->simcom_mic_ring = (int16_t *)calloc(adev->simcom_mic_ring_size, sizeof(int16_t));
        if (!adev->simcom_mic_ring) {
            adev->simcom_mic_ring_size = 0;
            ALOGE("SIMCOM ring allocation failed");
            return -ENOMEM;
        }
        simcom_ring_reset(adev);
    }
    return 0;
}

static int simcom_voice_ensure_buffer(int16_t **buffer, size_t *capacity, size_t required)
{
    if (!buffer || !capacity) {
        return -EINVAL;
    }
    if (required == 0) {
        return 0;
    }
    if (*capacity >= required) {
        return 0;
    }

    size_t new_capacity = (*capacity == 0) ? required : *capacity;
    while (new_capacity < required) {
        new_capacity *= 2;
    }

    int16_t *tmp = (int16_t *)realloc(*buffer, new_capacity * sizeof(int16_t));
    if (!tmp) {
        return -ENOMEM;
    }
    *buffer = tmp;
    *capacity = new_capacity;
    return 0;
}

static void simcom_voice_process_and_push(struct audio_device *adev,
        const int16_t *src, size_t frames, uint32_t channels, uint32_t rate,
        int16_t **mono_buf, size_t *mono_capacity,
        int16_t **downsample_buf, size_t *downsample_capacity,
        double *resample_pos, uint32_t *last_rate, uint32_t *last_channels)
{
    if (!adev || !adev->simcom_voice_active || !src || frames == 0) {
        return;
    }

    if (channels == 0) {
        channels = 1;
    }
    const uint32_t used_channels = channels;

    uint32_t effective_rate = rate ? rate : 8000;
    if (last_rate && last_channels) {
        if (effective_rate != *last_rate || channels != *last_channels) {
            if (resample_pos) {
                *resample_pos = 0.0;
            }
            *last_rate = effective_rate;
            *last_channels = channels;
        }
    }

    const int16_t *mono_src = src;
    bool used_temp_mono = false;
    simcom_trace_capture_preview(adev, src, frames, used_channels);
    if (channels > 1 && mono_buf && mono_capacity) {
        if (simcom_voice_ensure_buffer(mono_buf, mono_capacity, frames) == 0) {
            for (size_t f = 0; f < frames; ++f) {
                int32_t sum = 0;
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    sum += src[f * channels + ch];
                }
                (*mono_buf)[f] = (int16_t)(sum / (int32_t)channels);
            }
            mono_src = *mono_buf;
            used_temp_mono = true;
        } else {
            ALOGE("SIMCOM uplink: failed to allocate mono buffer (frames=%zu)", frames);
        }
    }

    const int16_t *push_samples = mono_src;
    size_t push_frames = frames;

    if (effective_rate != 8000 && downsample_buf && downsample_capacity && resample_pos) {
        size_t max_out = (size_t)(((uint64_t)frames * 8000 + effective_rate - 1) / effective_rate + 8);
        if (simcom_voice_ensure_buffer(downsample_buf, downsample_capacity, max_out) == 0) {
            const double step = (double)effective_rate / 8000.0;
            double pos = *resample_pos;
            size_t out_count = 0;
            while (pos < frames && out_count < max_out) {
                size_t idx = (size_t)pos;
                double frac = pos - (double)idx;
                int32_t sample0 = mono_src[idx];
                int32_t sample1 = (idx + 1 < frames) ? mono_src[idx + 1] : sample0;
                int32_t interpolated = sample0 + (int32_t)((sample1 - sample0) * frac);
                (*downsample_buf)[out_count++] = (int16_t)interpolated;
                pos += step;
            }
            if (pos >= frames) {
                *resample_pos = pos - frames;
            } else {
                *resample_pos = pos;
            }
            push_samples = *downsample_buf;
            push_frames = out_count;
        } else {
            ALOGE("SIMCOM uplink: failed to allocate resample buffer");
            push_frames = 0;
        }
    } else if (effective_rate != 8000 && resample_pos) {
        // resample requested but buffers not supplied, drop data to avoid corrupting modem
        push_frames = 0;
    }

    if (push_frames > 0 && simcom_voice_ensure_ring(adev) == 0) {
        simcom_ring_push(adev, push_samples, push_frames);
        if (simcom_debug_audio_enabled()) {
            int64_t sum_abs = 0;
            int32_t max_abs = 0;
            int16_t first_sample = push_samples[0];
            int16_t last_sample = push_samples[push_frames - 1];
            for (size_t dbg_idx = 0; dbg_idx < push_frames; ++dbg_idx) {
                int32_t sample = push_samples[dbg_idx];
                int32_t abs_sample = (sample < 0) ? -sample : sample;
                sum_abs += abs_sample;
                if (abs_sample > max_abs) {
                    max_abs = abs_sample;
                }
            }
            int32_t avg_abs = (int32_t)(sum_abs / (int64_t)push_frames);
            ALOGE("SIMCOM DBG CAPTURE: pushed=%zu rate=%u avg_abs=%d max_abs=%d first=%d last=%d temp_mono=%d",
                  push_frames, effective_rate, avg_abs, max_abs, first_sample, last_sample, used_temp_mono);
        }
    }
}

static bool simcom_send_at_command(const char *cmd)
{
    if (!cmd) {
        return false;
    }

    int fd = open(SIMCOM_TTY_DEVICE, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        ALOGE("SIMCOM AT: failed to open %s: %s", SIMCOM_TTY_DEVICE, strerror(errno));
        return false;
    }

    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%s\r", cmd);
    if (len < 0) {
        close(fd);
        return false;
    }
    if (len >= (int)sizeof(buffer)) {
        len = sizeof(buffer) - 1;
        buffer[len] = '\r';
    }

    size_t total = 0;
    while (total < (size_t)len) {
        ssize_t written = write(fd, buffer + total, len - total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            ALOGE("SIMCOM AT: write failed for %s: %s", cmd, strerror(errno));
            close(fd);
            return false;
        }
        total += (size_t)written;
    }

    fsync(fd);
    close(fd);
    ALOGE("SIMCOM AT: command sent: %s", cmd);
    return true;
}

static bool simcom_update_cpcmreg(struct audio_device *adev, bool enable)
{
    if (!adev) {
        return false;
    }

    if (adev->simcom_cpcmreg_state == enable) {
        if (simcom_debug_audio_enabled()) {
            ALOGE("SIMCOM AT: CPCMREG already %d, skipping", enable ? 1 : 0);
        }
        return true;
    }

    const char *cmd = enable ? "AT+CPCMREG=1" : "AT+CPCMREG=0";
    bool ok = simcom_send_at_command(cmd);
    if (ok) {
        adev->simcom_cpcmreg_state = enable;
    } else {
        ALOGE("SIMCOM AT: failed to send %s", cmd);
    }
    return ok;
}

static bool simcom_voice_usecase_present(struct audio_device *adev)
{
    struct listnode *node;
    list_for_each(node, &adev->usecase_list) {
        struct audio_usecase *usecase =
                node_to_item(node, struct audio_usecase, list);
        if (usecase->id == USECASE_SIMCOM_VOICE_CALL) {
            return true;
        }
    }
    return false;
}

static void *simcom_voice_capture_thread(void *context)
{
    struct audio_device *adev = (struct audio_device *)context;
    if (!adev) {
        return NULL;
    }

    struct pcm *pcm = adev->simcom_voice_pcm;
    if (!pcm) {
        ALOGE("SIMCOM voice thread: PCM handle is null");
        return NULL;
    }

    const uint32_t rate = (adev->simcom_voice_rate > 0) ? adev->simcom_voice_rate : pcm_config_in.rate;
    const uint32_t channels = (adev->simcom_voice_channels > 0) ? adev->simcom_voice_channels : pcm_config_in.channels;
    // Use period_size, not total buffer size (period_size * period_count)
    const size_t period_frames = (adev->simcom_voice_period_size > 0) ? adev->simcom_voice_period_size : pcm_config_in.period_size;
    const uint32_t used_channels = (channels == 0) ? 1 : channels;
    const size_t effective_channels = used_channels;
    if (period_frames == 0 || effective_channels == 0) {
        ALOGE("SIMCOM voice thread: invalid PCM parameters (frames=%zu channels=%u)",
              period_frames, channels);
        return NULL;
    }
    const size_t buffer_samples = period_frames * effective_channels;
    int16_t *frame_buf = (int16_t *)calloc(buffer_samples, sizeof(int16_t));
    if (!frame_buf) {
        ALOGE("SIMCOM voice thread: failed to allocate frame buffer");
        return NULL;
    }

    int16_t *mono_buf = NULL;
    size_t mono_capacity = 0;
    int16_t *downsample_buf = NULL;
    size_t downsample_capacity = 0;
    double resample_pos = 0.0;
    uint32_t last_rate = rate;
    uint32_t last_channels = channels;

    uint32_t raw_log_counter = 0;
    const size_t raw_log_limit = 16;

    while (!adev->simcom_voice_thread_stop) {
        int status = pcm_read(pcm, frame_buf, buffer_samples * sizeof(int16_t));
        if (status < 0) {
            if (status == -EPIPE) {
                ALOGW("SIMCOM voice thread: XRUN, preparing PCM");
                pcm_prepare(pcm);
            } else {
                ALOGE("SIMCOM voice thread: pcm_read error %d (%s)", status, pcm_get_error(pcm));
                usleep(20000);
            }
            continue;
        }

        bool pcm_all_zero = true;
        for (size_t sample_idx = 0; sample_idx < buffer_samples; ++sample_idx) {
            if (frame_buf[sample_idx] != 0) {
                pcm_all_zero = false;
                break;
            }
        }
        if (pcm_all_zero) {
            static unsigned int zero_batch_counter = 0;
            zero_batch_counter++;
            if ((zero_batch_counter & 0xF) == 0) {
                const size_t bytes_requested = buffer_samples * sizeof(int16_t);
                ALOGE("SIMCOM voice thread: pcm_read returned %zu bytes of silence (rate=%u channels=%u period=%zu)",
                      bytes_requested, rate, channels, period_frames);
            }
        }

        if (simcom_debug_audio_enabled()) {
            raw_log_counter++;
            if (raw_log_counter >= 50) {
                raw_log_counter = 0;
                const size_t frames_to_log = (period_frames < raw_log_limit) ? period_frames : raw_log_limit;
                const size_t samples_to_log = frames_to_log * effective_channels;
                char sample_log[256];
                size_t offset = 0;
                for (size_t i = 0; i < samples_to_log && offset < sizeof(sample_log); ++i) {
                    offset += snprintf(sample_log + offset,
                                        sizeof(sample_log) - offset,
                                        "%d%s",
                                        frame_buf[i],
                                        (i + 1 < samples_to_log) ? " " : "");
                }
                ALOGE("SIMCOM MIC RAW[%zu/%zu]: %s", frames_to_log, period_frames, sample_log);
            }
        }

        simcom_voice_process_and_push(adev,
                                      frame_buf,
                                      period_frames,
                                      used_channels,
                                      rate,
                                      &mono_buf,
                                      &mono_capacity,
                                      &downsample_buf,
                                      &downsample_capacity,
                                      &resample_pos,
                                      &last_rate,
                                      &last_channels);
    }

    free(downsample_buf);
    free(mono_buf);
    free(frame_buf);
    return NULL;
}

static int simcom_voice_start_capture(struct audio_device *adev)
{
    if (!adev) {
        return -EINVAL;
    }
    if (adev->simcom_voice_thread_started) {
        return 0;
    }

    struct stream_in dummy_in;
    memset(&dummy_in, 0, sizeof(dummy_in));
    dummy_in.dev = adev;
    read_in_sound_card(&dummy_in);

    int mic_card = adev->dev_in[SND_IN_SOUND_CARD_MIC].card;
    int mic_device = adev->dev_in[SND_IN_SOUND_CARD_MIC].device;
    ALOGE("SIMCOM voice: microphone card detection: dev_in[SND_IN_SOUND_CARD_MIC].card=%d device=%d",
          mic_card, mic_device);
    if (mic_card == (int)SND_IN_SOUND_CARD_UNKNOWN) {
        // Принудительно используем карту Realtek RT5651 (card 2), если автодетект не сработал
        mic_card = 2;
        adev->dev_in[SND_IN_SOUND_CARD_MIC].card = mic_card;
        if (mic_device < 0) {
            mic_device = 0;
        }
        adev->dev_in[SND_IN_SOUND_CARD_MIC].device = mic_device;
        ALOGE("SIMCOM voice: forcing microphone to card %d (Realtek RT5651), device=%d",
              mic_card, mic_device);
    }
    if (mic_card == (int)SND_IN_SOUND_CARD_UNKNOWN) {
        ALOGE("SIMCOM voice: microphone sound card still unknown after fallback");
        return -ENODEV;
    }

    if (!adev->simcom_mic_route_active) {
        ALOGE("SIMCOM voice: activating MAIN_MIC_CAPTURE_ROUTE (card=%d)", mic_card);
        route_pcm_card_open(mic_card, MAIN_MIC_CAPTURE_ROUTE);
        adev->simcom_mic_route_active = true;
        ALOGE("SIMCOM voice: MAIN_MIC_CAPTURE_ROUTE activation requested (card=%d)", mic_card);
    } else {
        ALOGE("SIMCOM voice: MAIN_MIC_CAPTURE_ROUTE already active");
    }

    simcom_configure_mic_controls(adev, mic_card);
    simcom_verify_mic_controls(mic_card);

    // Исправление: микрофон realtekrt5651co работает только с 48kHz stereo
    // Принудительно используем 48kHz для захвата микрофона независимо от pcm_config_in
    struct pcm_config capture_config = pcm_config_in;
    capture_config.rate = 48000;      // Микрофон требует 48kHz
    capture_config.channels = 2;      // Стерео (как проверено через tinycap)
    capture_config.period_size = 240; // 240 frames = 5ms при 48kHz (аналогично 120 при 8kHz)
    ALOGE("SIMCOM voice: forcing capture config: rate=48000 channels=2 period=%u",
          capture_config.period_size);

    const int device_candidates[] = {mic_device, 0, 1};
    struct pcm *pcm = NULL;
    int final_device = mic_device;

    for (size_t idx = 0; idx < ARRAY_SIZE(device_candidates); ++idx) {
        int candidate = device_candidates[idx];
        if (candidate < 0) {
            continue;
        }
        if (idx > 0 && candidate == final_device) {
            continue;
        }

        pcm = pcm_open(mic_card, candidate, PCM_IN, &capture_config);
        if (pcm && pcm_is_ready(pcm)) {
            final_device = candidate;
            break;
        }

        ALOGE("SIMCOM voice: pcm_open failed for capture (card=%d device=%d): %s",
              mic_card,
              candidate,
              pcm ? pcm_get_error(pcm) : "no handle");
        if (pcm) {
            pcm_close(pcm);
            pcm = NULL;
        }
    }

    if (!pcm) {
        route_pcm_close(CAPTURE_OFF_ROUTE);
        adev->simcom_mic_route_active = false;
        return -EIO;
    }

    adev->dev_in[SND_IN_SOUND_CARD_MIC].device = final_device;
    adev->simcom_voice_pcm = pcm;
    adev->simcom_voice_rate = (capture_config.rate > 0) ? capture_config.rate : 8000;
    adev->simcom_voice_channels = (capture_config.channels > 0) ? capture_config.channels : 1;
    adev->simcom_voice_period_size = capture_config.period_size;

    ALOGE("SIMCOM voice: capture pcm_open success (card=%d device=%d rate=%u channels=%u period=%u count=%u)",
          mic_card, final_device, adev->simcom_voice_rate, adev->simcom_voice_channels,
          capture_config.period_size, capture_config.period_count);

    simcom_verify_mic_controls(mic_card);

    if (pcm_prepare(pcm) != 0) {
        ALOGE("SIMCOM voice: pcm_prepare failed: %s", pcm_get_error(pcm));
        pcm_close(pcm);
        adev->simcom_voice_pcm = NULL;
        route_pcm_close(CAPTURE_OFF_ROUTE);
        adev->simcom_mic_route_active = false;
        return -EIO;
    }

    ALOGE("SIMCOM voice: pcm_prepare succeeded");

    if (pcm_start(pcm) != 0) {
        ALOGE("SIMCOM voice: pcm_start failed: %s", pcm_get_error(pcm));
        pcm_close(pcm);
        adev->simcom_voice_pcm = NULL;
        route_pcm_close(CAPTURE_OFF_ROUTE);
        adev->simcom_mic_route_active = false;
        return -EIO;
    }

    ALOGE("SIMCOM voice: PCM capture started (card=%d device=%d rate=%u channels=%u)",
          mic_card, mic_device, adev->simcom_voice_rate, adev->simcom_voice_channels);

    adev->simcom_voice_thread_stop = false;
    if (pthread_create(&adev->simcom_voice_thread, NULL,
                       simcom_voice_capture_thread, adev) != 0) {
        ALOGE("SIMCOM voice: failed to create capture thread");
        pcm_stop(adev->simcom_voice_pcm);
        pcm_close(adev->simcom_voice_pcm);
        adev->simcom_voice_pcm = NULL;
        adev->simcom_voice_thread_stop = true;
        return -errno;
    }
    adev->simcom_voice_thread_started = true;
    return 0;
}

static void simcom_voice_stop_capture(struct audio_device *adev)
{
    if (!adev) {
        return;
    }

    if (adev->simcom_voice_thread_started) {
        adev->simcom_voice_thread_stop = true;
        pthread_join(adev->simcom_voice_thread, NULL);
        adev->simcom_voice_thread_started = false;
        adev->simcom_voice_thread_stop = false;
    }

    if (adev->simcom_voice_pcm) {
        pcm_stop(adev->simcom_voice_pcm);
        pcm_close(adev->simcom_voice_pcm);
        adev->simcom_voice_pcm = NULL;
    }

    if (adev->simcom_mic_route_active) {
        route_pcm_close(CAPTURE_OFF_ROUTE);
        adev->simcom_mic_route_active = false;
    }

    adev->simcom_voice_rate = 0;
    adev->simcom_voice_channels = 0;
    adev->simcom_voice_period_size = 0;
}

static void simcom_voice_start_usecase(struct audio_device *adev)
{
    if (!adev) {
        ALOGE("SIMCOM voice: start_usecase called with NULL adev");
        return;
    }
    if (adev->simcom_voice_active) {
        ALOGE("SIMCOM voice: usecase already active, skipping start");
        return;
    }

    ALOGE("SIMCOM voice: starting usecase (thread_started=%d)", adev->simcom_voice_thread_started);

    if (simcom_voice_ensure_ring(adev) != 0) {
        ALOGE("SIMCOM voice: unable to allocate ring buffer");
        return;
    }
    simcom_ring_reset(adev);
    memset(&adev->simcom_stats, 0, sizeof(adev->simcom_stats));
    adev->simcom_mixer_configured = false;
    adev->simcom_mixer_card = -1;
    adev->simcom_voice_active = true;
    ALOGE("SIMCOM voice: usecase started");
    ALOGE("SIMCOM ROUTING STATUS: mic_route_active=%d, voice_active=%d, thread_started=%d",
          adev->simcom_mic_route_active, adev->simcom_voice_active, adev->simcom_voice_thread_started);

    int start_status = simcom_voice_start_capture(adev);
    if (start_status != 0) {
        ALOGE("SIMCOM voice: failed to start capture path (%d)", start_status);
        adev->simcom_voice_active = false;
        simcom_voice_stop_capture(adev);
        return;
    }

    if (!simcom_voice_usecase_present(adev)) {
        struct audio_usecase *usecase = (struct audio_usecase *)calloc(1, sizeof(*usecase));
        if (usecase) {
            usecase->id = USECASE_SIMCOM_VOICE_CALL;
            usecase->type = USECASE_TYPE_VOICE_CALL;
            usecase->devices = AUDIO_DEVICE_OUT_BLUETOOTH_SCO | AUDIO_DEVICE_OUT_TELEPHONY_TX;
            usecase->stream.out = NULL;
            list_add_tail(&adev->usecase_list, &usecase->list);
        } else {
            ALOGE("SIMCOM voice: failed to allocate usecase descriptor");
        }
    }
}

static void simcom_voice_stop_usecase(struct audio_device *adev)
{
    if (!adev) {
        return;
    }
    if (!adev->simcom_voice_active) {
        return;
    }

    // ИСПРАВЛЕНИЕ: устанавливаем флаг и пробуждаем ожидающие потоки
    pthread_mutex_lock(&adev->simcom_mic_lock);
    adev->simcom_voice_active = false;
    pthread_cond_broadcast(&adev->simcom_mic_cond);  // Пробуждаем все ожидающие потоки
    pthread_mutex_unlock(&adev->simcom_mic_lock);
    
    simcom_ring_reset(adev);
    simcom_log_capture_summary(adev, "final");
    ALOGE("SIMCOM voice: usecase stopped (thread_started=%d)", adev->simcom_voice_thread_started);
    adev->simcom_cpcmreg_state = false;

    simcom_voice_stop_capture(adev);

    struct listnode *node, *tmp;
    list_for_each_safe(node, tmp, &adev->usecase_list) {
        struct audio_usecase *usecase =
                node_to_item(node, struct audio_usecase, list);
        if (usecase->id == USECASE_SIMCOM_VOICE_CALL) {
            list_remove(node);
            free(usecase);
            break;
        }
    }
}

/**
 * @brief get_output_device_id
 *
 * @param device
 *
 * @returns
 */
int get_output_device_id(audio_devices_t device)
{
    if (device == AUDIO_DEVICE_NONE)
        return OUT_DEVICE_NONE;

    if (popcount(device) == 2) {
        if ((device == (AUDIO_DEVICE_OUT_SPEAKER |
                        AUDIO_DEVICE_OUT_WIRED_HEADSET)) ||
                (device == (AUDIO_DEVICE_OUT_SPEAKER |
                            AUDIO_DEVICE_OUT_WIRED_HEADPHONE)))
            return OUT_DEVICE_SPEAKER_AND_HEADSET;
        else
            return OUT_DEVICE_NONE;
    }

    if (popcount(device) != 1)
        return OUT_DEVICE_NONE;

    switch (device) {
    case AUDIO_DEVICE_OUT_SPEAKER:
        return OUT_DEVICE_SPEAKER;
    case AUDIO_DEVICE_OUT_WIRED_HEADSET:
        return OUT_DEVICE_HEADSET;
    case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
        return OUT_DEVICE_HEADPHONES;
    case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
    case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
    case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
        return OUT_DEVICE_BT_SCO;
    case AUDIO_DEVICE_OUT_TELEPHONY_TX:
        return OUT_DEVICE_BT_SCO; // Use BT_SCO device for SIMCOM telephony
    default:
        return OUT_DEVICE_NONE;
    }
}

/**
 * @brief get_input_source_id
 *
 * @param source
 *
 * @returns
 */
int get_input_source_id(audio_source_t source)
{
    switch (source) {
    case AUDIO_SOURCE_DEFAULT:
        return IN_SOURCE_NONE;
    case AUDIO_SOURCE_MIC:
        return IN_SOURCE_MIC;
    case AUDIO_SOURCE_CAMCORDER:
        return IN_SOURCE_CAMCORDER;
    case AUDIO_SOURCE_VOICE_RECOGNITION:
        return IN_SOURCE_VOICE_RECOGNITION;
    case AUDIO_SOURCE_VOICE_COMMUNICATION:
        return IN_SOURCE_VOICE_COMMUNICATION;
    default:
        return IN_SOURCE_NONE;
    }
}

/**
 * @brief force_non_hdmi_out_standby
 * must be called with hw device outputs list, all out streams, and hw device mutexes locked
 *
 * @param adev
 */
static void force_non_hdmi_out_standby(struct audio_device *adev)
{
    enum output_type type;
    struct stream_out *out;
    for (type = 0; type < OUTPUT_TOTAL; ++type) {
        out = adev->outputs[type];
        if (type == OUTPUT_HDMI_MULTI|| !out)
            continue;
        /* This will never recurse more than 2 levels deep. */
        do_out_standby(out);
    }
}



/**
 * @brief getOutputRouteFromDevice
 *
 * @param device
 *
 * @returns
 */
unsigned getOutputRouteFromDevice(uint32_t device)
{
    /*if (mMode != AudioSystem::MODE_RINGTONE && mMode != AudioSystem::MODE_NORMAL)
        return PLAYBACK_OFF_ROUTE;
    */
    switch (device) {
    case AUDIO_DEVICE_OUT_SPEAKER:
        return SPEAKER_NORMAL_ROUTE;
    case AUDIO_DEVICE_OUT_WIRED_HEADSET:
        return HEADSET_NORMAL_ROUTE;
    case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
        return HEADPHONE_NORMAL_ROUTE;
    case (AUDIO_DEVICE_OUT_SPEAKER|AUDIO_DEVICE_OUT_WIRED_HEADPHONE):
    case (AUDIO_DEVICE_OUT_SPEAKER|AUDIO_DEVICE_OUT_WIRED_HEADSET):
        return SPEAKER_HEADPHONE_NORMAL_ROUTE;
    case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
    case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
    case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
        return BLUETOOTH_NORMAL_ROUTE;
    case AUDIO_DEVICE_OUT_AUX_DIGITAL:
	return HDMI_NORMAL_ROUTE;
        //case AudioSystem::DEVICE_OUT_EARPIECE:
        //	return EARPIECE_NORMAL_ROUTE;
        //case AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET:
        //case AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET:
        //	return USB_NORMAL_ROUTE;
    default:
        return PLAYBACK_OFF_ROUTE;
    }
}

/**
 * @brief getVoiceRouteFromDevice
 *
 * @param device
 *
 * @returns
 */
uint32_t getVoiceRouteFromDevice(uint32_t device)
{
    ALOGE("not support now");
    return 0;
}

/**
 * @brief getInputRouteFromDevice
 *
 * @param device
 *
 * @returns
 */
uint32_t getInputRouteFromDevice(uint32_t device)
{
    /*if (mMicMute) {
        return CAPTURE_OFF_ROUTE;
    }*/
    ALOGE("%s:device:%x",__FUNCTION__,device);
    if (device & AUDIO_DEVICE_IN_TELEPHONY_RX) {
        return MAIN_MIC_CAPTURE_ROUTE;
    }
    switch (device) {
    case AUDIO_DEVICE_IN_BUILTIN_MIC:
        return MAIN_MIC_CAPTURE_ROUTE;
    case AUDIO_DEVICE_IN_WIRED_HEADSET:
        return HANDS_FREE_MIC_CAPTURE_ROUTE;
    case AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET:
        return BLUETOOTH_SOC_MIC_CAPTURE_ROUTE;
    case AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET:
        return USB_CAPTURE_ROUTE;
    case AUDIO_DEVICE_IN_HDMI:
        return HDMI_IN_CAPTURE_ROUTE;
    default:
        return CAPTURE_OFF_ROUTE;
    }
}

/**
 * @brief getRouteFromDevice
 *
 * @param device
 *
 * @returns
 */
uint32_t getRouteFromDevice(struct audio_device *adev, uint32_t device)
{
    if (device & AUDIO_DEVICE_BIT_IN)
        return getInputRouteFromDevice(device);

    uint32_t route = getOutputRouteFromDevice(device);

    if (adev && (adev->mode == AUDIO_MODE_IN_CALL ||
                 adev->mode == AUDIO_MODE_IN_COMMUNICATION)) {
        switch (route) {
        case SPEAKER_NORMAL_ROUTE:
            route = SPEAKER_INCALL_ROUTE;
            break;
        case EARPIECE_NORMAL_ROUTE:
            route = EARPIECE_INCALL_ROUTE;
            break;
        case HEADPHONE_NORMAL_ROUTE:
            route = HEADPHONE_INCALL_ROUTE;
            break;
        case HEADSET_NORMAL_ROUTE:
            route = HEADSET_INCALL_ROUTE;
            break;
        case BLUETOOTH_NORMAL_ROUTE:
            route = BLUETOOTH_INCALL_ROUTE;
            break;
        case SPEAKER_HEADPHONE_NORMAL_ROUTE:
            route = SPEAKER_INCALL_ROUTE;
            break;
        default:
            break;
        }
    }

    return route;
}

struct dev_proc_info SPEAKER_OUT_NAME[] = /* add codes& dai name here*/
{
    {"realtekrt5616c", NULL,},
    {"realtekrt5651co", "rt5651-aif1",},
    {"realtekrt5670c", NULL,},
    {"realtekrt5672c", NULL,},
    {"realtekrt5678co", NULL,},
    {"rkhdmianalogsnd", NULL,},
    {"rockchipcx2072x", NULL,},
    {"rockchipes8316c", NULL,},
    {"rockchipes8323c", NULL,},
    {"rockchipes8388c", NULL,},
    {"rockchipes8396c", NULL,},
    {"rockchiprk", NULL, },
    {"rockchiprk809co", NULL,},
    {"rockchiprk817co", NULL,},
    {"rockchiprt5640c", "rt5640-aif1",},
    {"rockchiprt5670c", NULL,},
    {"rockchiprt5672c", NULL,},
    {NULL, NULL}, /* Note! Must end with NULL, else will cause crash */
};

struct dev_proc_info HDMI_OUT_NAME[] =
{
    {"realtekrt5651co", "i2s-hifi",},
    {"realtekrt5670co", "i2s-hifi",},
    {"rkhdmidpsound", NULL,},
    {"rockchiphdmi", NULL,},
    {"rockchiprt5640c", "i2s-hifi",},
    {NULL, NULL}, /* Note! Must end with NULL, else will cause crash */
};


struct dev_proc_info SPDIF_OUT_NAME[] =
{
    {"ROCKCHIPSPDIF", "dit-hifi",},
    {"rockchipspdif", NULL,},
    {"rockchipcdndp", NULL,},
    {NULL, NULL}, /* Note! Must end with NULL, else will cause crash */
};

struct dev_proc_info BT_OUT_NAME[] =
{
    {"SIMCOM", NULL,},          /* Map SIMCOM audio to BT card for telephony */
    {"rockchipbt", NULL,},
    {NULL, NULL}, /* Note! Must end with NULL, else will cause crash */
};

struct dev_proc_info MIC_IN_NAME[] =
{
    {"realtekrt5616c", NULL,},
    {"realtekrt5651co", "rt5651-aif1",},
    {"realtekrt5670c", NULL,},
    {"realtekrt5672c", NULL,},
    {"realtekrt5678co", NULL,},
    {"rockchipes8316c", NULL,},
    {"rockchipes8323c", NULL,},
    {"rockchipes8396c", NULL,},
    {"rockchipes7210", NULL,},
    {"rockchipes7243", NULL,},
    {"rockchiprk", NULL, },
    {"rockchiprk809co", NULL,},
    {"rockchiprk817co", NULL,},
    {"rockchiprt5640c", NULL,},
    {"rockchiprt5670c", NULL,},
    {"rockchiprt5672c", NULL,},
    {NULL, NULL}, /* Note! Must end with NULL, else will cause crash */
};


struct dev_proc_info HDMI_IN_NAME[] =
{
    {"realtekrt5651co", "tc358749x-audio"},
    {"hdmiin", NULL},
    {NULL, NULL}, /* Note! Must end with NULL, else will cause crash */
};

struct dev_proc_info BT_IN_NAME[] =
{
    {"SIMCOM", NULL},           /* Map SIMCOM audio to BT card for telephony */
    {"rockchipbt", NULL},
    {NULL, NULL}, /* Note! Must end with NULL, else will cause crash */
};

static int name_match(const char* dst, const char* src)
{
    int score = 0;
    // total equal
    if (!strcmp(dst, src)) {
        score = 100;
    } else  if (strstr(dst, src)) {
        // part equal
        score = 50;
    }

    return score;
}

static bool is_specified_out_sound_card(char *id, struct dev_proc_info *match)
{
    int i = 0;

    if (!match)
        return true; /* match any */

    while (match[i].cid) {
        if (!strcmp(id, match[i].cid)) {
            return true;
    }
        i++;
    }
    return false;
}

static bool dev_id_match(const char *info, const char *did)
{
    const char *deli = "id:";
    char *id;
    int idx = 0;

    if (!did)
        return true;
    if (!info)
        return false;
    /* find str like-> id: ff880000.i2s-rt5651-aif1 rt5651-aif1-0 */
    id = strstr(info, deli);
    if (!id)
        return false;
    id += strlen(deli);
    while(id[idx] != '\0') {
        if (id[idx] == '\r' ||id[idx] == '\n') {
            id[idx] = '\0';
            break;
    }
        idx ++;
    }
    if (strstr(id, did)) {
        ALOGE("match dai!!!: %s %s", id, did);
        return true;
    }
    return false;
}

static bool get_specified_out_dev(struct dev_info *devinfo,
                                  int card,
                                  const char *id,
                                  struct dev_proc_info *match)
{
    int i = 0;
    int device;
    char str_device[32];
    char info[256];
    size_t len;
    FILE* file = NULL;
    int better = 0;
    int index = -1;

    /* parse card id */
    if (!match)
        return true; /* match any */
    while (match[i].cid) {
        int score = name_match(id, match[i].cid);
        if (score > better) {
            better = score;
            index = i;
        }
        i++;
    }

    if (index < 0)
        return false;

    if (!match[index].cid)
        return false;

    if (!match[index].did) { /* no exist dai info, exit */
        devinfo->card = card;
        devinfo->device = 0;
        ALOGD("%s card, got card=%d,device=%d", devinfo->id,
              devinfo->card, devinfo->device);
        return true;
    }

    /* parse device id */
    for (device = 0; device < SNDRV_DEVICES; device++) {
        sprintf(str_device, "proc/asound/card%d/pcm%dp/info", card, device);
        if (access(str_device, 0)) {
            ALOGD("No exist %s, break and finish parsing", str_device);
            break;
        }
        file = fopen(str_device, "r");
        if (!file) {
            ALOGD("Could reading %s property", str_device);
            continue;
        }
        len = fread(info, sizeof(char), sizeof(info)/sizeof(char), file);
        fclose(file);
        if (len == 0 || len > sizeof(info)/sizeof(char))
            continue;
        if (info[len - 1] == '\n') {
            len--;
            info[len] = '\0';
        }
        /* parse device dai */
        if (dev_id_match(info, match[index].did)) {
            devinfo->card = card;
            devinfo->device = device;
            ALOGD("%s card, got card=%d,device=%d", devinfo->id,
                  devinfo->card, devinfo->device);
        return true;
    }
    }
    return false;
}

static bool get_specified_in_dev(struct dev_info *devinfo,
                                 int card,
                                 const char *id,
                                 struct dev_proc_info *match)
{
    int i = 0;
    int device;
    char str_device[32];
    char info[256];
    size_t len;
    FILE* file = NULL;
    int better = 0;
    int index = -1;

    /* parse card id */
    if (!match)
        return true; /* match any */
    while (match[i].cid) {
        int score = name_match(id, match[i].cid);
        if (score > better) {
            better = score;
            index = i;
        }
        i++;
    }

    if (index < 0)
        return false;

    if (!match[index].cid)
        return false;

    if (!match[index].did) { /* no exist dai info, exit */
        devinfo->card = card;
        devinfo->device = 0;
        ALOGD("%s card, got card=%d,device=%d", devinfo->id,
              devinfo->card, devinfo->device);
        return true;
    }

    /* parse device id */
    for (device = 0; device < SNDRV_DEVICES; device++) {
        sprintf(str_device, "proc/asound/card%d/pcm%dc/info", card, device);
        if (access(str_device, 0)) {
            ALOGD("No exist %s, break and finish parsing", str_device);
            break;
        }
        file = fopen(str_device, "r");
        if (!file) {
            ALOGD("Could reading %s property", str_device);
            continue;
        }
        len = fread(info, sizeof(char), sizeof(info)/sizeof(char), file);
        fclose(file);
        if (len == 0 || len > sizeof(info)/sizeof(char))
            continue;
        if (info[len - 1] == '\n') {
            len--;
            info[len] = '\0';
        }
        /* parse device dai */
        if (dev_id_match(info, match[i].did)) {
            devinfo->card = card;
            devinfo->device = device;
            ALOGD("%s card, got card=%d,device=%d", devinfo->id,
                  devinfo->card, devinfo->device);
            return true;
        }
    }
    return false;
}

static bool is_specified_in_sound_card(char *id, struct dev_proc_info *match)
{
    int i = 0;

    /*
     * mic: diffrent product may have diffrent card name,modify codes here
     * for example: 0 [rockchiprk3328 ]: rockchip-rk3328 - rockchip-rk3328
     */
    if (!match)
        return true;/* match any */
    while (match[i].cid) {
        if (!strcmp(id, match[i].cid)) {
            return true;
  }
        i++;
    }
    return false;
}

static void set_default_dev_info( struct dev_info *info, int size, int rid)
{
    for(int i =0; i < size; i++) {
        if (rid) {
            info[i].id = NULL;
        }
        info[i].card = (int)SND_OUT_SOUND_CARD_UNKNOWN;
    }
}

static void dumpdev_info(const char *tag, struct dev_info  *devinfo, int count)
{
    ALOGD("dump %s device info", tag);
    for(int i = 0; i < count; i++) {
        if (devinfo[i].id && devinfo[i].card != SND_OUT_SOUND_CARD_UNKNOWN)
            ALOGD("dev_info %s  card=%d, device:%d", devinfo[i].id,
                  devinfo[i].card,
                  devinfo[i].device);
    }
}

/*
 * get sound card infor by parser node: /proc/asound/cards
 * the sound card number is not always the same value
 */
static void read_out_sound_card(struct stream_out *out)
{

    struct audio_device *device = NULL;
    int card = 0;
    char str[32];
    char id[20];
    size_t len;
    FILE* file = NULL;

    if((out == NULL) || (out->dev == NULL)) {
        return ;
    }
    device = out->dev;
    set_default_dev_info(device->dev_out, SND_OUT_SOUND_CARD_UNKNOWN, 0);
    for (card = 0; card < SNDRV_CARDS; card++) {
        sprintf(str, "proc/asound/card%d/id", card);
        if (access(str, 0)) {
            ALOGD("No exist %s, break and finish parsing", str);
            break;
        }
        file = fopen(str, "r");
        if (!file) {
            ALOGD("Could reading %s property", str);
            continue;
        }
        len = fread(id, sizeof(char), sizeof(id)/sizeof(char), file);
        fclose(file);
        if (len == 0 || len > sizeof(id)/sizeof(char))
            continue;
        if (id[len - 1] == '\n') {
            len--;
            id[len] = '\0';
        }
        ALOGD("card%d id:%s", card, id);
        get_specified_out_dev(&device->dev_out[SND_OUT_SOUND_CARD_SPEAKER], card, id, SPEAKER_OUT_NAME);
        get_specified_out_dev(&device->dev_out[SND_OUT_SOUND_CARD_HDMI], card, id, HDMI_OUT_NAME);
        get_specified_out_dev(&device->dev_out[SND_OUT_SOUND_CARD_SPDIF], card, id, SPDIF_OUT_NAME);
        get_specified_out_dev(&device->dev_out[SND_OUT_SOUND_CARD_BT], card, id, BT_OUT_NAME);
    }
    dumpdev_info("out", device->dev_out, SND_OUT_SOUND_CARD_MAX);
    return ;
}

/*
 * get sound card infor by parser node: /proc/asound/cards
 * the sound card number is not always the same value
 */
static void read_in_sound_card(struct stream_in *in)
{
    struct audio_device *device = NULL;
    int card = 0;
    char str[32];
    char id[20];
    size_t len;
    FILE* file = NULL;

    if((in == NULL) || (in->dev == NULL)){
        return ;
    }
    device = in->dev;
    set_default_dev_info(device->dev_in, SND_IN_SOUND_CARD_UNKNOWN, 0);
    for (card = 0; card < SNDRV_CARDS; card++) {
        sprintf(str, "proc/asound/card%d/id", card);
        if(access(str, 0)) {
            ALOGD("No exist %s, break and finish parsing", str);
                break;
        }
        file = fopen(str, "r");
        if (!file) {
            ALOGD("Could reading %s property", str);
            continue;
        }
        len = fread(id, sizeof(char), sizeof(id)/sizeof(char), file);
        fclose(file);
        if (len == 0 || len > sizeof(id)/sizeof(char))
            continue;
        if (id[len - 1] == '\n') {
            len--;
           id[len] = '\0';
        }
        get_specified_in_dev(&device->dev_in[SND_IN_SOUND_CARD_MIC], card, id, MIC_IN_NAME);
        /* set HDMI audio input info if need hdmi audio input */
        get_specified_in_dev(&device->dev_in[SND_IN_SOUND_CARD_HDMI], card, id, HDMI_IN_NAME);
        get_specified_in_dev(&device->dev_in[SND_IN_SOUND_CARD_BT], card, id, BT_IN_NAME);
    }
    dumpdev_info("in", device->dev_in, SND_IN_SOUND_CARD_MAX);
    return ;
}

static inline bool hasExtCodec()
{
    char line[80];
    bool ret = false;
    FILE *fd = fopen("proc/asound/cards","r");
    if(NULL != fd){
      memset(line, 0, 80);
      while((fgets(line,80,fd))!= NULL){
          line[80-1]='\0';
          if(strstr(line,"realtekrt5651co")){
              ret = true;
              break;
          }
      }
      fclose(fd);
    }
    return ret;
}

static bool is_bitstream(struct stream_out *out)
{
    if (out == NULL) {
        return false;
    }

    bool bitstream = false;
    if (out->output_direct) {
        switch(out->output_direct_mode) {
            case HBR:
            case NLPCM:
                bitstream = true;
                break;
            case LPCM:
            default:
                bitstream = false;
                break;
        }
    } else {
        if(out->output_direct_mode != LPCM) {
            ALOGD("%s: %d: error output_direct = false, but output_direct_mode != LPCM, this is error config",__FUNCTION__,__LINE__);
        }
    }

    return bitstream;
}

static bool is_multi_pcm(struct stream_out *out)
{
    if (out == NULL) {
        return false;
    }

    bool multi = false;
    if (out->output_direct && (out->output_direct_mode == LPCM) && (out->config.channels > 2)) {
        multi = true;
    }

    return multi;
}

/**
 * @brief mixer_mode_set
 * for rk3399 audio output mixer mode set
 * @param out
 *
 * @return
 */
static int mixer_mode_set(struct stream_out *out)
{
    int ret = 0;
    struct mixer *pMixer = NULL;
    struct mixer_ctl *pctl;
    struct audio_device *adev = out->dev;

    /*
     * set audio mode for hdmi
     * The driver of hdmi read the audio mode to identify
     * the type of audio stream according to audio mode.
     * 1) LPCM: the stream is pcm format
     * 2) NLPCM: the stream is bitstream format, AC3/EAC3/DTS use this format
     * 3) HDR: the stream is bitstream format, TrueHD/Atoms/DTS-HD/DTS-X use this format.
     */
    if (out->device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        pMixer = mixer_open_legacy(adev->dev_out[SND_OUT_SOUND_CARD_HDMI].card);
        if (!pMixer) {
            ALOGE("mMixer is a null point %s %d,CARD = %d",__func__, __LINE__,adev->dev_out[SND_OUT_SOUND_CARD_HDMI].card);
            return ret;
        }
        pctl = mixer_get_control(pMixer,"AUDIO MODE",0 );
        if (pctl != NULL) {
            ALOGD("Now set mixer audio_mode is %d for drm",out->output_direct_mode);
            switch (out->output_direct_mode) {
            case HBR:
                ret = mixer_ctl_set_val(pctl , out->output_direct_mode);
                break;
            case NLPCM:
                ret = mixer_ctl_set_val(pctl , out->output_direct_mode);
                break;
            default:
                ret = mixer_ctl_set_val(pctl , out->output_direct_mode);
                break;
            }

            if (ret != 0) {
                ALOGE("set_controls() can not set ctl!");
                mixer_close_legacy(pMixer);
                return -EINVAL;
            }
        }
        mixer_close_legacy(pMixer);
    }

    return ret;
}


static void open_sound_card_policy(struct stream_out *out)
{
    if (out == NULL) {
        return ;
    }

    if (is_bitstream(out) || (is_multi_pcm(out))) {
        return ;
    }

    /*
     * In Box Product, ouput 2 channles pcm datas over hdmi,speaker and spdif simultaneous.
     * speaker can only support 44.1k or 48k
     */
    bool support = ((out->config.rate == 44100) || (out->config.rate == 48000));
    struct audio_device *adev = out->dev;
    if (support) {
        if(adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].card != SND_OUT_SOUND_CARD_UNKNOWN) {
            out->device |= AUDIO_DEVICE_OUT_SPEAKER;
        }

        if(adev->dev_out[SND_OUT_SOUND_CARD_HDMI].card != SND_OUT_SOUND_CARD_UNKNOWN) {
            /*
             * hdmi is taken by direct/mulit pcm output
             */
            if(adev->outputs[OUTPUT_HDMI_MULTI] != NULL) {
                out->device &= ~AUDIO_DEVICE_OUT_AUX_DIGITAL;
            } else {
                out->device |= AUDIO_DEVICE_OUT_AUX_DIGITAL;
            }
        }

        if(adev->dev_out[SND_OUT_SOUND_CARD_SPDIF].card != SND_OUT_SOUND_CARD_UNKNOWN){
           out->device |= AUDIO_DEVICE_OUT_SPDIF;
        }
    }

    // some specail config for chips
#ifdef RK3288
    /*3288's hdmi & codec use the same i2s,so only config the codec card*/
    audio_devices_t devices = (AUDIO_DEVICE_OUT_AUX_DIGITAL|AUDIO_DEVICE_OUT_SPEAKER);
    if ((out->device & devices) == devices) {
        out->device &= ~AUDIO_DEVICE_OUT_AUX_DIGITAL;
    }
#endif
}

/**
 * @brief start_output_stream
 * must be called with hw device outputs list, output stream, and hw device mutexes locked
 *
 * @param out
 *
 * @returns
 */
static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int ret = 0;
    int card = (int)SND_OUT_SOUND_CARD_UNKNOWN;
    int device = 0;
    // set defualt value to true for compatible with mid project
    bool disable = true;
    
    ALOGE("start_output_stream: mode=%d, device=0x%x", adev->mode, out->device);

if (!hasExtCodec()){
    /*
     * In Box Project, if output stream is 2 channels pcm,
     * the 2 channels pcm can simultaneous output over speaker,hdmi and spdif.
     * If hdmi is already opened in multi pcm mode or bitstream mode,
     * and new output stream want open it in
     * multi pcm mode or bitstream mode, disable it.
     * Otherwise, if new output stream is 2 channels pcm,
     * we can let output over speaker and spdif, this it
     * better than usleep in out_write function
     */
    disable = is_multi_pcm(out) || is_bitstream(out);
}

    ALOGD("%s:%d out = %p,device = 0x%x,outputs[OUTPUT_HDMI_MULTI] = %p",__FUNCTION__,__LINE__,out,out->device,adev->outputs[OUTPUT_HDMI_MULTI]);
    if (out == adev->outputs[OUTPUT_HDMI_MULTI]) {
        force_non_hdmi_out_standby(adev);
    } else if (adev->outputs[OUTPUT_HDMI_MULTI] &&
            !adev->outputs[OUTPUT_HDMI_MULTI]->standby) {
        out->disabled = true;
        return 0;
    }

    out->disabled = false;
    read_out_sound_card(out);

if (!hasExtCodec()){
    open_sound_card_policy(out);
}

    out_dump(out, 0);
    route_pcm_card_open(adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].card,
                        getRouteFromDevice(adev, out->device));

    // Исправление: после активации output route повторно активировать MAIN_MIC_CAPTURE_ROUTE
    // если capture активен, так как route_pcm_card_open для output может вызвать
    // route_pcm_close(CAPTURE_OFF_ROUTE), что сбрасывает mixer настройки для capture
    // ВАЖНО: повторная активация должна происходить при ЛЮБОМ mode, если capture активен
    // Проверяем: либо thread активен, либо simcom_voice_active=true, либо mic_route_active=true,
    // либо capture PCM открыт (adev->simcom_voice_pcm != NULL) - это самый надежный способ
    // определить, активен ли capture, даже при mode=0
    int mic_card = adev->dev_in[SND_IN_SOUND_CARD_MIC].card;
    bool should_reactivate = false;
    if (mic_card != (int)SND_IN_SOUND_CARD_UNKNOWN) {
        // Активируем, если:
        // 1. Capture thread активен, ИЛИ
        // 2. simcom_voice_active=true (usecase активен), ИЛИ
        // 3. simcom_mic_route_active=true (mic route был активирован ранее), ИЛИ
        // 4. capture PCM открыт (simcom_voice_pcm != NULL) - самый надежный способ
        //    определить активный capture, даже когда флаги сброшены при mode=0
        bool capture_pcm_open = (adev->simcom_voice_pcm != NULL);
        should_reactivate = adev->simcom_voice_thread_started || 
                           adev->simcom_voice_active || 
                           adev->simcom_mic_route_active ||
                           capture_pcm_open;
        
        // Добавляем детальное логирование для диагностики
        ALOGE("SIMCOM voice: checking re-activation: mode=%d, mic_card=%d, thread=%d, voice_active=%d, mic_route_active=%d, pcm_open=%d, should=%d",
              adev->mode, mic_card, adev->simcom_voice_thread_started, 
              adev->simcom_voice_active, adev->simcom_mic_route_active, capture_pcm_open ? 1 : 0, should_reactivate);
        
        if (should_reactivate) {
            ALOGE("SIMCOM voice: re-activating MAIN_MIC_CAPTURE_ROUTE after output route activation (card=%d, mode=%d, thread=%d, voice_active=%d, mic_route_active=%d, pcm_open=%d)",
                  mic_card, adev->mode, adev->simcom_voice_thread_started, 
                  adev->simcom_voice_active, adev->simcom_mic_route_active, capture_pcm_open ? 1 : 0);
            route_pcm_card_open(mic_card, MAIN_MIC_CAPTURE_ROUTE);
            adev->simcom_mic_route_active = true;  // Обновляем флаг после активации
            ALOGE("SIMCOM voice: MAIN_MIC_CAPTURE_ROUTE re-activated");
        } else {
            ALOGE("SIMCOM voice: skipping re-activation: all flags are false and PCM is closed");
        }
    } else {
        ALOGE("SIMCOM voice: skipping re-activation: mic_card is UNKNOWN");
    }

    if (out->device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        if (adev->owner[SOUND_CARD_HDMI] == NULL) {
            card = adev->dev_out[SND_OUT_SOUND_CARD_HDMI].card;
            device =adev->dev_out[SND_OUT_SOUND_CARD_HDMI].device;
            if (card != (int)SND_OUT_SOUND_CARD_UNKNOWN) {
if (!hasExtCodec()){			
#ifdef USE_DRM
            ret = mixer_mode_set(out);

            if (ret!=0) {
                ALOGE("mixer mode set error,ret=%d!",ret);
            }
#endif
}
                out->pcm[SND_OUT_SOUND_CARD_HDMI] = pcm_open(card, device,
                                                    PCM_OUT | PCM_MONOTONIC, &out->config);
                if (out->pcm[SND_OUT_SOUND_CARD_HDMI] &&
                        !pcm_is_ready(out->pcm[SND_OUT_SOUND_CARD_HDMI])) {
                    ALOGE("pcm_open(PCM_CARD_HDMI) failed: %s, card number = %d",
                          pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_HDMI]),card);
                    pcm_close(out->pcm[SND_OUT_SOUND_CARD_HDMI]);
                    return -ENOMEM;
                }
if (!hasExtCodec()){
                if(is_multi_pcm(out) || is_bitstream(out)){
                    adev->owner[SOUND_CARD_HDMI] = (int*)out;
                }
}
            }
        } else {
            ALOGD("The current HDMI is DVI mode");
            out->device |= AUDIO_DEVICE_OUT_SPEAKER;
        }
    }

    if (out->device & (AUDIO_DEVICE_OUT_SPEAKER |
                       AUDIO_DEVICE_OUT_WIRED_HEADSET |
                       AUDIO_DEVICE_OUT_WIRED_HEADPHONE)) {
        card = adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].card;
        device = adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].device;
        if(card != (int)SND_OUT_SOUND_CARD_UNKNOWN) {
			if (out->device & (AUDIO_DEVICE_OUT_SPEAKER | AUDIO_DEVICE_OUT_WIRED_HEADSET |AUDIO_DEVICE_OUT_WIRED_HEADPHONE)) {	
            out->pcm[SND_OUT_SOUND_CARD_SPEAKER] = pcm_open(card, device,
                                          PCM_OUT | PCM_MONOTONIC, &out->config);
            if (out->pcm[SND_OUT_SOUND_CARD_SPEAKER] && !pcm_is_ready(out->pcm[SND_OUT_SOUND_CARD_SPEAKER])) {
                ALOGE("pcm_open(PCM_CARD) failed: %s,card number = %d",
                      pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_SPEAKER]),card);
                pcm_close(out->pcm[SND_OUT_SOUND_CARD_SPEAKER]);
                return -ENOMEM;
            }
        }
			else{
				card = adev->dev_out[SND_OUT_SOUND_CARD_HDMI].card;
				out->pcm[SND_OUT_SOUND_CARD_HDMI] = pcm_open(card, device,
												PCM_OUT | PCM_MONOTONIC, &out->config);
				if (out->pcm[SND_OUT_SOUND_CARD_HDMI] &&
						!pcm_is_ready(out->pcm[SND_OUT_SOUND_CARD_HDMI])) {
					ALOGE("pcm_open(PCM_CARD_HDMI) failed: %s, card number = %d",
						  pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_HDMI]),card);
					pcm_close(out->pcm[SND_OUT_SOUND_CARD_HDMI]);
					return -ENOMEM;
				}
			}
        }

    }
    
	// For voice calls, also duplicate to SIMCOM if available and not already opened
	// This works for ANY device type during voice calls
	// Check if the BT card is SIMCOM by reading /proc/asound/cardX/id
	// SIMCOM can be on any card (e.g., card 3), not just card 0
	char simcom_id[20] = {0};
	bool is_simcom = false;
	int bt_card = adev->dev_out[SND_OUT_SOUND_CARD_BT].card;
	ALOGE("SIMCOM detection: bt_card=%d, SNDRV_CARDS=%d", bt_card, SNDRV_CARDS);
	if (bt_card >= 0 && bt_card < SNDRV_CARDS) {
		char card_path[64];
		snprintf(card_path, sizeof(card_path), "/proc/asound/card%d/id", bt_card);
		ALOGE("SIMCOM detection: Trying to open %s", card_path);
		FILE *id_file = fopen(card_path, "r");
		if (id_file) {
			size_t len = fread(simcom_id, 1, sizeof(simcom_id) - 1, id_file);
			fclose(id_file);
			if (len > 0 && simcom_id[len - 1] == '\n') {
				simcom_id[len - 1] = '\0';
			}
			ALOGE("SIMCOM detection: Read card_id='%s' (len=%zu)", simcom_id, len);
			if (strstr(simcom_id, "SIMCOM") || strstr(simcom_id, "simcom")) {
				is_simcom = true;
				ALOGE("SIMCOM detection: SIMCOM found! is_simcom=1");
			} else {
				ALOGE("SIMCOM detection: 'SIMCOM' not found in '%s'", simcom_id);
			}
		} else {
			ALOGE("SIMCOM detection: Failed to open %s: %s", card_path, strerror(errno));
		}
	} else {
		ALOGE("SIMCOM detection: Invalid bt_card=%d (must be 0-%d)", bt_card, SNDRV_CARDS - 1);
	}
	ALOGE("Checking SIMCOM for voice call: mode=%d, device=0x%x, bt_card=%d, card_id=%s, is_simcom=%d, pcm_exists=%d",
	      adev->mode, out->device, bt_card,
	      simcom_id[0] ? simcom_id : "unknown",
	      is_simcom, out->pcm[SND_OUT_SOUND_CARD_BT] != NULL);
	ALOGE("SIMCOM output check: mode=%d, bt_card=%d, is_simcom=%d, voice_active=%d, owner=%p",
	      adev->mode, bt_card, is_simcom, adev->simcom_voice_active, adev->owner[SOUND_CARD_BT]);
	// Check for voice call mode (AUDIO_MODE_IN_CALL = 2) - mode is now set correctly
	// IMPORTANT: Only open SIMCOM PCM if mode is already IN_CALL
	// This prevents opening PCM before VOICE CALL: BEGIN is logged
	// Also check for telephony device (0x8) as fallback
	// Use owner mechanism to prevent double-open from different streams
	// NOTE: This code runs under lock_all_outputs, so owner check/assign is atomic
	// Set owner BEFORE opening PCM to prevent race condition
	if (is_simcom && 
	    adev->mode == AUDIO_MODE_IN_CALL) {
		ALOGE("SIMCOM: mode is IN_CALL, proceeding with PCM open");
		// Check owner first to handle both new opens and reopens
		ALOGE("SIMCOM: Checking owner, owner=%p, out=%p", adev->owner[SOUND_CARD_BT], out);
		if (adev->owner[SOUND_CARD_BT] == NULL) {
			// Claim ownership BEFORE opening to prevent double-open
			adev->owner[SOUND_CARD_BT] = (int*)out;
			ALOGE("Opening SIMCOM for voice call parallel output (mode=%d, device=0x%x, out=%p, owner claimed)", 
			      adev->mode, out->device, out);
		// simcom_call_monitor отправляет AT+CPCMREG=1 при переходе в режим вызова
		// Задержка уже отработана перед открытием PCM, поэтому просто открываем устройство
		ALOGE("SIMCOM: AT+CPCMREG=1 already requested before PCM open (card=%d)", bt_card);
			out->pcm[SND_OUT_SOUND_CARD_BT] = pcm_open(bt_card, 0,
										PCM_OUT | PCM_MONOTONIC, &pcm_config_simcom);
			if (out->pcm[SND_OUT_SOUND_CARD_BT] && 
			    !pcm_is_ready(out->pcm[SND_OUT_SOUND_CARD_BT])) {
				ALOGE("pcm_open(SIMCOM voice call) failed: %s, releasing owner",
				      pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_BT]));
				pcm_close(out->pcm[SND_OUT_SOUND_CARD_BT]);
				out->pcm[SND_OUT_SOUND_CARD_BT] = NULL;
				adev->owner[SOUND_CARD_BT] = NULL; // Release owner on failure
			} else if (out->pcm[SND_OUT_SOUND_CARD_BT]) {
				// Prepare PCM - this creates URBs that copy from runtime->dma_area
				// URBs will be created with empty buffer data initially
				// We'll start PCM when first real data arrives
				int prep_ret = pcm_prepare(out->pcm[SND_OUT_SOUND_CARD_BT]);
				if (prep_ret != 0) {
					ALOGE("pcm_prepare(SIMCOM) failed: %s", pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_BT]));
					pcm_close(out->pcm[SND_OUT_SOUND_CARD_BT]);
					out->pcm[SND_OUT_SOUND_CARD_BT] = NULL;
					adev->owner[SOUND_CARD_BT] = NULL;
				} else {
					ALOGE("SIMCOM opened and prepared successfully (URBs created, will start on first data write)");
				}
			}
		} else if (adev->owner[SOUND_CARD_BT] == (int*)out) {
			// Already owned by this stream - check if PCM needs to be reopened
			if (!out->pcm[SND_OUT_SOUND_CARD_BT]) {
				ALOGE("SIMCOM owner is this stream but PCM not open, reopening (out=%p)", out);
			// SIMCOM already activated through simcom_call_monitor
			ALOGE("SIMCOM: Opening PCM for reopen (card=%d)", bt_card);
				out->pcm[SND_OUT_SOUND_CARD_BT] = pcm_open(bt_card, 0,
											PCM_OUT | PCM_MONOTONIC, &pcm_config_simcom);
				if (out->pcm[SND_OUT_SOUND_CARD_BT] && 
				    !pcm_is_ready(out->pcm[SND_OUT_SOUND_CARD_BT])) {
					ALOGE("pcm_open(SIMCOM voice call) failed on reopen: %s",
					      pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_BT]));
					pcm_close(out->pcm[SND_OUT_SOUND_CARD_BT]);
					out->pcm[SND_OUT_SOUND_CARD_BT] = NULL;
					adev->owner[SOUND_CARD_BT] = NULL;
				} else if (out->pcm[SND_OUT_SOUND_CARD_BT]) {
					// Prepare PCM first
					int prep_ret = pcm_prepare(out->pcm[SND_OUT_SOUND_CARD_BT]);
					if (prep_ret != 0) {
						ALOGE("pcm_prepare(SIMCOM) failed on reopen: %s", pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_BT]));
						pcm_close(out->pcm[SND_OUT_SOUND_CARD_BT]);
						out->pcm[SND_OUT_SOUND_CARD_BT] = NULL;
						adev->owner[SOUND_CARD_BT] = NULL;
					} else {
						// Prepare PCM - URBs will be created with empty buffer data initially
						ALOGE("SIMCOM reopened and prepared successfully (URBs created, will start on first data write)");
					}
				}
			} else {
				// PCM already open, check if it's ready
				if (out->pcm[SND_OUT_SOUND_CARD_BT] && !pcm_is_ready(out->pcm[SND_OUT_SOUND_CARD_BT])) {
					ALOGE("SIMCOM PCM not ready, closing and reopening");
					pcm_close(out->pcm[SND_OUT_SOUND_CARD_BT]);
					out->pcm[SND_OUT_SOUND_CARD_BT] = NULL;
					// Will reopen in next iteration
				} else {
					ALOGE("SIMCOM already opened in this stream (out=%p, owner=%p, pcm=%p), reusing existing PCM", 
					      out, adev->owner[SOUND_CARD_BT], out->pcm[SND_OUT_SOUND_CARD_BT]);
				}
			}
		} else {
			ALOGE("SIMCOM already owned by another stream (owner=%p, current out=%p), skipping open", 
			      adev->owner[SOUND_CARD_BT], out);
		}
    }

    if (out->device & AUDIO_DEVICE_OUT_SPDIF) {
        if (adev->owner[SOUND_CARD_SPDIF] == NULL){
            card = adev->dev_out[SND_OUT_SOUND_CARD_SPDIF].card;
            device = adev->dev_out[SND_OUT_SOUND_CARD_SPDIF].device;
            if(card != (int)SND_OUT_SOUND_CARD_UNKNOWN) {
                out->pcm[SND_OUT_SOUND_CARD_SPDIF] = pcm_open(card, device,
                                                    PCM_OUT | PCM_MONOTONIC, &out->config);

                if (out->pcm[SND_OUT_SOUND_CARD_SPDIF] &&
                        !pcm_is_ready(out->pcm[SND_OUT_SOUND_CARD_SPDIF])) {
                    ALOGE("pcm_open(PCM_CARD_SPDIF) failed: %s,card number = %d",
                          pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_SPDIF]),card);
                    pcm_close(out->pcm[SND_OUT_SOUND_CARD_SPDIF]);
                    return -ENOMEM;
                }

if (!hasExtCodec()){
                if(is_multi_pcm(out) || is_bitstream(out)){
                    adev->owner[SOUND_CARD_SPDIF] = (int*)out;
                }
}
            }
        }
    }
		if (out->device & AUDIO_DEVICE_OUT_ALL_SCO) {
	#ifdef BT_AP_SCO // HARD CODE FIXME
			card = adev->dev_out[SND_OUT_SOUND_CARD_BT].card;
			device = adev->dev_out[SND_OUT_SOUND_CARD_BT].device;
			ALOGD("pcm_open bt/simcom card number = %d",card);
			if(card != (int)SND_OUT_SOUND_CARD_UNKNOWN) {
				// If SIMCOM already opened for voice call above, skip opening here to avoid conflict
				if (out->pcm[SND_OUT_SOUND_CARD_BT]) {
					ALOGD("SIMCOM/BT PCM already opened, reusing existing handle");
				} else if (adev->owner[SOUND_CARD_BT] != NULL) {
					ALOGD("SIMCOM/BT already owned by another stream, skipping open");
				} else {
					// Check if this is SIMCOM card (card 0) and use SIMCOM config
					// Use pcm_config_sco as default (mono 8kHz) or BT config if BT_AP_SCO is defined
#ifdef BT_AP_SCO
					struct pcm_config *config_to_use = &pcm_config_ap_sco;
#else
					struct pcm_config *config_to_use = &pcm_config_sco;
#endif
					if (card == 0 && adev->dev_out[SND_OUT_SOUND_CARD_BT].id && 
					    strstr(adev->dev_out[SND_OUT_SOUND_CARD_BT].id, "SIMCOM")) {
						ALOGD("Using SIMCOM PCM config for card 0");
						config_to_use = &pcm_config_simcom;
					}
				out->pcm[SND_OUT_SOUND_CARD_BT] = pcm_open(card, 0,
												PCM_OUT | PCM_MONOTONIC, config_to_use);
					if (out->pcm[SND_OUT_SOUND_CARD_BT] && 
					    !pcm_is_ready(out->pcm[SND_OUT_SOUND_CARD_BT])) {
						ALOGE("pcm_open(SIMCOM/BT SCO) failed: %s",
						      pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_BT]));
						pcm_close(out->pcm[SND_OUT_SOUND_CARD_BT]);
						out->pcm[SND_OUT_SOUND_CARD_BT] = NULL;
					} else if (out->pcm[SND_OUT_SOUND_CARD_BT]) {
						// Исправление: очистка буфера PCM от старых данных после tinyplay
						// Если перед вызовом был запущен tinyplay, в буфере могут остаться данные
						// pcm_prepare сбросит буфер, но лучше явно очистить через pcm_drop
						if (pcm_prepare(out->pcm[SND_OUT_SOUND_CARD_BT]) != 0) {
							ALOGE("pcm_prepare(SIMCOM/BT SCO) failed: %s",
							      pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_BT]));
						} else {
							// pcm_prepare сбрасывает буфер, что решает проблему с остаточными данными
							ALOGD("SIMCOM/BT PCM buffer cleared (pcm_prepare)");
						}
						adev->owner[SOUND_CARD_BT] = (int*)out;
					}
					// Only create resampler if not using SIMCOM (which is 8kHz mono already)
					if (out->pcm[SND_OUT_SOUND_CARD_BT] && config_to_use != &pcm_config_simcom) {
				ret = create_resampler(out->config.rate,
											   config_to_use->rate,
									   2,
									   RESAMPLER_QUALITY_DEFAULT,
									   NULL,
									   &out->resampler);
				if (ret != 0) {
					ret = -EINVAL;
				}
			}
				}
			}
			// ИСПРАВЛЕНИЕ: Отключен local audio monitoring для SIMCOM telephony
			// Проблема: открытие speaker для мониторинга создавало обратную связь (эхо)
			// Микрофон абонента попадал в динамик через этот monitoring
			// Решение: отключаем speaker monitoring для SIMCOM telephony
			// Теперь аудио идет только на SIMCOM, без локального мониторинга
			// int bt_card = adev->dev_out[SND_OUT_SOUND_CARD_BT].card;
			// if (bt_card == 0 && adev->dev_out[SND_OUT_SOUND_CARD_BT].id && 
			//     strstr(adev->dev_out[SND_OUT_SOUND_CARD_BT].id, "SIMCOM") &&
			//     adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].card != SND_OUT_SOUND_CARD_UNKNOWN) {
			// 	ALOGD("Also opening speaker for SIMCOM telephony monitoring");
			// 	card = adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].card;
			// 	device = adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].device;
			// 	out->pcm[SND_OUT_SOUND_CARD_SPEAKER] = pcm_open(card, device,
			// 								PCM_OUT | PCM_MONOTONIC, &out->config);
			// 	if (out->pcm[SND_OUT_SOUND_CARD_SPEAKER] && 
			// 	    !pcm_is_ready(out->pcm[SND_OUT_SOUND_CARD_SPEAKER])) {
			// 		ALOGE("pcm_open(SIMCOM speaker) failed: %s, card %d",
			// 		      pcm_get_error(out->pcm[SND_OUT_SOUND_CARD_SPEAKER]), card);
			// 		pcm_close(out->pcm[SND_OUT_SOUND_CARD_SPEAKER]);
			// 		out->pcm[SND_OUT_SOUND_CARD_SPEAKER] = NULL;
			// 	}
			// }
			ALOGD("SIMCOM telephony: speaker monitoring disabled to prevent feedback loop");
	#endif
		}
	

    adev->out_device |= out->device;
    ALOGD("%s:%d, out = %p",__FUNCTION__,__LINE__,out);
    return 0;
}

/**
 * @brief get_next_buffer
 *
 * @param buffer_provider
 * @param buffer
 *
 * @returns
 */
static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                           struct resampler_buffer* buffer)
{
    struct stream_in *in;
    size_t i,size;

    if (buffer_provider == NULL || buffer == NULL)
        return -EINVAL;

    in = (struct stream_in *)((char *)buffer_provider -
                              offsetof(struct stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->frames_in == 0) {
        size = pcm_frames_to_bytes(in->pcm,pcm_get_buffer_size(in->pcm));
        in->read_status = pcm_read(in->pcm,
                                   (void*)in->buffer,pcm_frames_to_bytes(in->pcm, in->config->period_size));
        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }

        //fwrite(in->buffer,pcm_frames_to_bytes(in->pcm,pcm_get_buffer_size(in->pcm)),1,in_debug);
        in->frames_in = in->config->period_size;

        /* Do stereo to mono conversion in place by discarding right channel */
        if ((in->channel_mask == AUDIO_CHANNEL_IN_MONO)
                &&(in->config->channels == 2)) {
            //ALOGE("channel_mask = AUDIO_CHANNEL_IN_MONO");
            for (i = 0; i < in->frames_in; i++)
                in->buffer[i] = in->buffer[i * 2];
        }
    }

    //ALOGV("pcm_frames_to_bytes(in->pcm,pcm_get_buffer_size(in->pcm)):%d",size);
    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                          in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer +
                  (in->config->period_size - in->frames_in) *
                  audio_channel_count_from_in_mask(in->channel_mask);

    return in->read_status;

}

/**
 * @brief release_buffer
 *
 * @param buffer_provider
 * @param buffer
 */
static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                           struct resampler_buffer* buffer)
{
    struct stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct stream_in *)((char *)buffer_provider -
                              offsetof(struct stream_in, buf_provider));

    in->frames_in -= buffer->frame_count;
}

#define STR_32KHZ "32KHZ"
#define STR_44_1KHZ "44.1KHZ"
#define STR_48KHZ "48KHZ"
/**
 * @brief get_hdmiin_audio_rate
 * @param
 * @return hdmiin audio rate
 */
static int get_hdmiin_audio_rate(struct audio_device *adev)
{
    int rate = 44100;
    char value[PROPERTY_VALUE_MAX] = "";
    property_get("vendor.hdmiin.audiorate", value, STR_44_1KHZ);

    if ( 0 == strncmp(value, STR_32KHZ, strlen(STR_32KHZ)) ){
        rate = 32000;
    } else if ( 0 == strncmp(value, STR_44_1KHZ, strlen(STR_44_1KHZ)) ){
        rate = 44100;
    } else if ( 0 == strncmp(value, STR_48KHZ, strlen(STR_48KHZ)) ){
        rate = 48000;
    } else {
        rate = atoi(value);
        if (rate <= 0)
            rate = 44100;
    }

    // if hdmiin connect to codec, use 44100 sample rate
    if (adev->dev_out[SND_IN_SOUND_CARD_HDMI].card
            == adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].card)
        rate = 44100;

    return rate;
}

int create_resampler_helper(struct stream_in *in, uint32_t in_rate)
{
    int ret = 0;
    if (in->resampler) {
        release_resampler(in->resampler);
        in->resampler = NULL;
    }

    in->buf_provider.get_next_buffer = get_next_buffer;
    in->buf_provider.release_buffer = release_buffer;
    ALOGD("create resampler, channel %d, rate %d => %d",
                    audio_channel_count_from_in_mask(in->channel_mask),
                    in_rate, in->requested_rate);
    ret = create_resampler(in_rate,
                    in->requested_rate,
                    audio_channel_count_from_in_mask(in->channel_mask),
                    RESAMPLER_QUALITY_DEFAULT,
                    &in->buf_provider,
                    &in->resampler);
    if (ret != 0) {
        ret = -EINVAL;
    }

    return ret;
}

/**
 * @brief start_input_stream
 * must be called with input stream and hw device mutexes locked
 *
 * @param in
 *
 * @returns
 */
static int start_input_stream(struct stream_in *in)
{
    struct audio_device *adev = in->dev;
    int  ret = 0;
    int card = 0;
    int device = 0;
    (void)device;

    in->usecase = USECASE_PRIMARY_CAPTURE;
    in->usecase_type = USECASE_TYPE_PCM_CAPTURE;
    in->simcom_input = false;

    in_dump(in, 0);
    read_in_sound_card(in);
        uint32_t route = getRouteFromDevice(in->dev, in->device | AUDIO_DEVICE_BIT_IN);
        ALOGE("start_input_stream: using capture route %u for device mask 0x%x", route, in->device);
        route_pcm_card_open(adev->dev_in[SND_IN_SOUND_CARD_MIC].card, route);
#ifdef RK3399_LAPTOP //HARD CODE FIXME
    bool request_bt_sco = (in->device & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) != 0;
    bool request_telephony = (in->device & AUDIO_DEVICE_IN_TELEPHONY_RX) != 0;
    if (request_bt_sco && !request_telephony) {
        card = adev->dev_in[SND_IN_SOUND_CARD_BT].card;
        device =  adev->dev_in[SND_IN_SOUND_CARD_BT].device;
        if(card != SND_IN_SOUND_CARD_UNKNOWN){
            // Check if this is SIMCOM card by reading /proc/asound/cardX/id
            // SIMCOM can be on any card (e.g., card 3), not just card 0
            char simcom_id[20] = {0};
            bool is_simcom = false;
            int bt_card = adev->dev_in[SND_IN_SOUND_CARD_BT].card;
            ALOGE("SIMCOM input detection: bt_card=%d, SNDRV_CARDS=%d", bt_card, SNDRV_CARDS);
            if (bt_card >= 0 && bt_card < SNDRV_CARDS) {
                char card_path[64];
                snprintf(card_path, sizeof(card_path), "/proc/asound/card%d/id", bt_card);
                ALOGE("SIMCOM input detection: Trying to open %s", card_path);
                FILE *id_file = fopen(card_path, "r");
                if (id_file) {
                    size_t len = fread(simcom_id, 1, sizeof(simcom_id) - 1, id_file);
                    fclose(id_file);
                    if (len > 0 && simcom_id[len - 1] == '\n') {
                        simcom_id[len - 1] = '\0';
                    }
                    ALOGE("SIMCOM input detection: Read card_id='%s' (len=%zu)", simcom_id, len);
                    if (strstr(simcom_id, "SIMCOM") || strstr(simcom_id, "simcom")) {
                        is_simcom = true;
                        ALOGE("SIMCOM input detection: SIMCOM found! is_simcom=1");
                    } else {
                        ALOGE("SIMCOM input detection: 'SIMCOM' not found in '%s'", simcom_id);
                    }
                } else {
                    ALOGE("SIMCOM input detection: Failed to open %s: %s", card_path, strerror(errno));
                }
            } else {
                ALOGE("SIMCOM input detection: Invalid bt_card=%d (must be 0-%d)", bt_card, SNDRV_CARDS - 1);
            }
            
            // Use SIMCOM config if SIMCOM detected, otherwise use BT config
            struct pcm_config *config_to_use = &pcm_config_in_bt;
            if (is_simcom) {
                ALOGE("Using SIMCOM PCM config for input (card=%d, device=%d)", card, device);
                config_to_use = &pcm_config_in_simcom;
                in->simcom_input = true;
            } else {
                ALOGE("Using BT PCM config for input (card=%d, device=%d)", card, device);
            }
            in->config = config_to_use;
            ALOGE("Opening SIMCOM input PCM: card=%d, device=%d, rate=%d, channels=%d, period_size=%d", 
                  card, device, config_to_use->rate, config_to_use->channels, config_to_use->period_size);
            in->pcm = pcm_open(card, device, PCM_IN, in->config);
            if (in->pcm && !pcm_is_ready(in->pcm)) {
                ALOGE("pcm_open(SIMCOM input) failed: %s", pcm_get_error(in->pcm));
                pcm_close(in->pcm);
                in->pcm = NULL;
                return -EIO;
            } else if (in->pcm) {
                ALOGE("SIMCOM input PCM opened successfully (card=%d, device=%d)", card, device);
            }
            if (in->resampler) {
                release_resampler(in->resampler);

                in->buf_provider.get_next_buffer = get_next_buffer;
                in->buf_provider.release_buffer = release_buffer;

                // Only create resampler if not using SIMCOM (which is 8kHz mono already)
                if (config_to_use != &pcm_config_in_simcom) {
                ret = create_resampler(in->config->rate,
                                       in->requested_rate,
                                       audio_channel_count_from_in_mask(in->channel_mask),
                                       RESAMPLER_QUALITY_DEFAULT,
                                       &in->buf_provider,
                                       &in->resampler);
                if (ret != 0) {
                    ret = -EINVAL;
                    }
                }
            }
        } else {
            ALOGE("%s: %d,the card number of bt is = %d",__FUNCTION__,__LINE__,card);
            return -EINVAL;
        }
    } else { /* use built-in mic (card2) for telephony and default cases */
        in->config = &pcm_config_in;
        card = adev->dev_in[SND_IN_SOUND_CARD_MIC].card;
        device =  adev->dev_in[SND_IN_SOUND_CARD_MIC].device;
        route_pcm_card_open(card, MAIN_MIC_CAPTURE_ROUTE);
        in->simcom_input = false;
        if (card != SND_IN_SOUND_CARD_UNKNOWN) {
            in->pcm = pcm_open(card, device, PCM_IN, in->config);

            if (in->resampler) {
                release_resampler(in->resampler);

                in->buf_provider.get_next_buffer = get_next_buffer;
                in->buf_provider.release_buffer = release_buffer;

                ret = create_resampler(in->config->rate,
                                       in->requested_rate,
                                       audio_channel_count_from_in_mask(in->channel_mask),
                                       RESAMPLER_QUALITY_DEFAULT,
                                       &in->buf_provider,
                                       &in->resampler);
                if (ret != 0) {
                    ret = -EINVAL;
                }
            }
        } else {
            ALOGE("%s: %d,the card number of mic is %d",__FUNCTION__,__LINE__,card);
            return -EINVAL;
        }
    }
#else
    card = (int)adev->dev_in[SND_IN_SOUND_CARD_HDMI].card;
    if (in->device & AUDIO_DEVICE_IN_HDMI && (card != (int)SND_OUT_SOUND_CARD_UNKNOWN)) {
        in->config->rate = get_hdmiin_audio_rate(adev);
        in->pcm = pcm_open(card, PCM_DEVICE, PCM_IN, in->config);
        ALOGD("open HDMIIN %d", card);
        if (in->resampler) {
            release_resampler(in->resampler);
            in->resampler = NULL;
        }

        // if hdmiin connect to codec, don't resample
        if (in->config->rate != in->requested_rate) {
            ret = create_resampler_helper(in, in->config->rate);
        }
    } else if (in->device & AUDIO_DEVICE_IN_BUILTIN_MIC ||
               in->device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
        card = adev->dev_in[SND_IN_SOUND_CARD_MIC].card;
        device =  adev->dev_in[SND_IN_SOUND_CARD_MIC].device;
        in->pcm = pcm_open(card, device, PCM_IN, in->config);
    } else {
        card = adev->dev_in[SND_IN_SOUND_CARD_BT].card;
        device = adev->dev_in[SND_IN_SOUND_CARD_BT].device;
        in->pcm = pcm_open(card, device, PCM_IN, in->config);
    }
#endif
    if (in->pcm && !pcm_is_ready(in->pcm)) {
        ALOGE("pcm_open() failed: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        return -ENOMEM;
    }

    /* if no supported sample rate is available, use the resampler */
    if (in->resampler)
        in->resampler->reset(in->resampler);

    in->frames_in = 0;
    adev->input_source = in->input_source;
    adev->in_device = in->device;
    adev->in_channel_mask = in->channel_mask;

    in->simcom_voice_capture = false;
    in->usecase = USECASE_PRIMARY_CAPTURE;
    in->usecase_type = USECASE_TYPE_PCM_CAPTURE;
    in->simcom_resample_pos = 0.0;
    in->simcom_last_rate = 0;
    in->simcom_last_channels = 0;
    if (adev->simcom_voice_active) {
        bool voice_source = (in->input_source == AUDIO_SOURCE_VOICE_COMMUNICATION);
        bool telephony_device = (in->device & AUDIO_DEVICE_IN_TELEPHONY_RX) != 0;
        bool builtin_mic = (in->device & AUDIO_DEVICE_IN_BUILTIN_MIC) != 0;
        if (voice_source || telephony_device || builtin_mic) {
            uint32_t channels = audio_channel_count_from_in_mask(in->channel_mask);
            if (channels == 0 && in->config) {
                channels = in->config->channels;
            }
            in->simcom_last_channels = channels;
            in->simcom_last_rate = in->requested_rate ? in->requested_rate :
                                      (in->config ? in->config->rate : 8000);
                in->simcom_voice_capture = true;
                in->usecase = USECASE_SIMCOM_VOICE_CALL;
                in->usecase_type = USECASE_TYPE_VOICE_CALL;
            if (simcom_voice_ensure_ring(adev) != 0) {
                ALOGE("SIMCOM voice capture: failed to ensure ring buffer");
            }
        }
    }

    /* initialize volume ramp */
    in->ramp_frames = (CAPTURE_START_RAMP_MS * in->requested_rate) / 1000;
    in->ramp_step = (uint16_t)(USHRT_MAX / in->ramp_frames);
    in->ramp_vol = 0;;


    return 0;
}

/**
 * @brief get_input_buffer_size
 *
 * @param sample_rate
 * @param format
 * @param channel_count
 * @param is_low_latency
 *
 * @returns
 */
static size_t get_input_buffer_size(unsigned int sample_rate,
                                    audio_format_t format,
                                    unsigned int channel_count,
                                    bool is_low_latency)
{
    const struct pcm_config *config = is_low_latency ?
                                              &pcm_config_in_low_latency : &pcm_config_in;
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (config->period_size * sample_rate) / config->rate;
    size = ((size + 15) / 16) * 16;

    return size * channel_count * audio_bytes_per_sample(format);
}


/**
 * @brief read_frames
 * read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified
 *
 * @param in
 * @param buffer
 * @param frames
 *
 * @returns
 */
static ssize_t read_frames(struct stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;
    size_t frame_size = audio_stream_in_frame_size(&in->stream);

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;
        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                                                  (int16_t *)((char *)buffer +
                                                          frames_wr * frame_size),
                                                  &frames_rd);
        } else {
            struct resampler_buffer buf = {
                { raw : NULL, },
frame_count :
                frames_rd,
            };
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer +
                       frames_wr * frame_size,
                       buf.raw,
                       buf.frame_count * frame_size);
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }
    return frames_wr;
}

/**
 * @brief out_get_sample_rate
 *
 * @param stream
 *
 * @returns
 */
static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    char value[PROPERTY_VALUE_MAX];
    property_get("vendor.vts_test", value, NULL);

    if (strcmp(value, "true") == 0) {
        return out->aud_config.sample_rate;
    } else {
        return out->config.rate;
    }

}

/**
 * @brief out_set_sample_rate
 *
 * @param stream
 * @param rate
 *
 * @returns
 */
static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return -ENOSYS;
}

/**
 * @brief out_get_buffer_size
 *
 * @param stream
 *
 * @returns
 */
static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->config.period_size *
           audio_stream_out_frame_size((const struct audio_stream_out *)stream);
}

/**
 * @brief out_get_channels
 *
 * @param stream
 *
 * @returns
 */
static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    char value[PROPERTY_VALUE_MAX];
    property_get("vendor.vts_test", value, NULL);

    if (strcmp(value, "true") == 0){
        return out->aud_config.channel_mask;
    } else {
        return out->channel_mask;
    }

}

/**
 * @brief out_get_format
 *
 * @param stream
 *
 * @returns
 */
static audio_format_t out_get_format(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    char value[PROPERTY_VALUE_MAX];
    property_get("vendor.vts_test", value, NULL);

    if (strcmp(value, "true") == 0){
        return out->aud_config.format;
    } else {
        return AUDIO_FORMAT_PCM_16_BIT;
    }

}

/**
 * @brief out_set_format
 *
 * @param stream
 * @param format
 *
 * @returns
 */
static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

/**
 * @brief output_devices
 * Return the set of output devices associated with active streams
 * other than out.  Assumes out is non-NULL and out->dev is locked.
 *
 * @param out
 *
 * @returns
 */
static audio_devices_t output_devices(struct stream_out *out)
{
    struct audio_device *dev = out->dev;
    enum output_type type;
    audio_devices_t devices = AUDIO_DEVICE_NONE;

    for (type = 0; type < OUTPUT_TOTAL; ++type) {
        struct stream_out *other = dev->outputs[type];
        if (other && (other != out) && !other->standby) {
            // TODO no longer accurate
            /* safe to access other stream without a mutex,
             * because we hold the dev lock,
             * which prevents the other stream from being closed
             */
            devices |= other->device;
        }
    }

    return devices;
}

/**
 * @brief do_out_standby
 * must be called with hw device outputs list, all out streams, and hw device mutex locked
 *
 * @param out
 */
static void do_out_standby(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int i;
    ALOGD("%s,out = %p,device = 0x%x",__FUNCTION__,out,out->device);
    if (!out->standby) {
        for (i = 0; i < SND_OUT_SOUND_CARD_MAX; i++) {
            if (out->pcm[i]) {
                pcm_close(out->pcm[i]);
                out->pcm[i] = NULL;
            }
        }
        // Free SIMCOM accumulation buffer
        if (out->simcom_buffer) {
            free(out->simcom_buffer);
            out->simcom_buffer = NULL;
            out->simcom_buffer_used = 0;
        }
        out->simcom_pcm_started = false;
        out->simcom_periods_written = 0;
        out->standby = true;
        out->nframes = 0;
        if (out == adev->outputs[OUTPUT_HDMI_MULTI]) {
            /* force standby on low latency output stream so that it can reuse HDMI driver if
             * necessary when restarted */
            force_non_hdmi_out_standby(adev);
        }
if (!hasExtCodec()){
#ifdef USE_DRM
        mixer_mode_set(out);
#endif
}
        /* re-calculate the set of active devices from other streams */
        adev->out_device = output_devices(out);

#ifdef AUDIO_3A
        if (adev->voice_api != NULL) {
            adev->voice_api->flush();
        }
#endif
        route_pcm_close(PLAYBACK_OFF_ROUTE);
        ALOGD("close device");

        /* Skip resetting the mixer if no output device is active */
        if (adev->out_device) {
            route_pcm_open(getRouteFromDevice(adev, adev->out_device));
            ALOGD("change device");
        }
if (!hasExtCodec()){
        if(adev->owner[SOUND_CARD_HDMI] == (int*)out){
            adev->owner[SOUND_CARD_HDMI] = NULL;
        }

        if(adev->owner[SOUND_CARD_SPDIF] == (int*)out){
            adev->owner[SOUND_CARD_SPDIF] = NULL;
        }
}
        if(adev->owner[SOUND_CARD_BT] == (int*)out){
            adev->owner[SOUND_CARD_BT] = NULL;
}
    }
}

/**
 * @brief lock_all_outputs
 * lock outputs list, all output streams, and device
 *
 * @param adev
 */
static void lock_all_outputs(struct audio_device *adev)
{
    enum output_type type;
    pthread_mutex_lock(&adev->lock_outputs);
    for (type = 0; type < OUTPUT_TOTAL; ++type) {
        struct stream_out *out = adev->outputs[type];
        if (out)
            pthread_mutex_lock(&out->lock);
    }
    pthread_mutex_lock(&adev->lock);
}

/**
 * @brief unlock_all_outputs
 * unlock device, all output streams (except specified stream), and outputs list
 *
 * @param adev
 * @param except
 */
static void unlock_all_outputs(struct audio_device *adev, struct stream_out *except)
{
    /* unlock order is irrelevant, but for cleanliness we unlock in reverse order */
    pthread_mutex_unlock(&adev->lock);
    enum output_type type = OUTPUT_TOTAL;
    do {
        struct stream_out *out = adev->outputs[--type];
        if (out && out != except)
            pthread_mutex_unlock(&out->lock);
    } while (type != (enum output_type) 0);
    pthread_mutex_unlock(&adev->lock_outputs);
}

/**
 * @brief out_standby
 *
 * @param stream
 *
 * @returns
 */
static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;

    lock_all_outputs(adev);

    do_out_standby(out);

    unlock_all_outputs(adev, NULL);

    return 0;
}

/**
 * @brief out_dump
 *
 * @param stream
 * @param fd
 *
 * @returns
 */
int out_dump(const struct audio_stream *stream, int fd)
{
    struct stream_out *out = (struct stream_out *)stream;

    ALOGD("out->Device     : 0x%x", out->device);
    ALOGD("out->SampleRate : %d", out->config.rate);
    ALOGD("out->Channels   : %d", out->config.channels);
    ALOGD("out->Formate    : %d", out->config.format);
    ALOGD("out->PreiodSize : %d", out->config.period_size);
    return 0;
}
/**
 * @brief out_set_parameters
 *
 * @param stream
 * @param kvpairs
 *
 * @returns
 */
static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    int status = 0;
    unsigned int val;

    ALOGD("%s: kvpairs = %s", __func__, kvpairs);

    parms = str_parms_create_str(kvpairs);

    //set channel_mask
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_CHANNELS,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        out->aud_config.channel_mask = val;
    }
    // set sample rate
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_SAMPLING_RATE,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        out->aud_config.sample_rate = val;
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    lock_all_outputs(adev);
    if (ret >= 0) {
        val = atoi(value);
        /* Don't switch HDMI audio in box products */
        if ((val != 0) && ((out->device & val) != val) ||
            (val != 0) && !(out->device & AUDIO_DEVICE_OUT_HDMI)) {
            /* Force standby if moving to/from SPDIF or if the output
             * device changes when in SPDIF mode */
            if (((val & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) ^
                    (adev->out_device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET)) ||
                    (adev->out_device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET)) {
                do_out_standby(out);
            }

            /* force output standby to start or stop SCO pcm stream if needed */
            if ((val & AUDIO_DEVICE_OUT_ALL_SCO) ^
                    (out->device & AUDIO_DEVICE_OUT_ALL_SCO)) {
                do_out_standby(out);
            }

            if (!out->standby && (out == adev->outputs[OUTPUT_HDMI_MULTI] ||
                                  !adev->outputs[OUTPUT_HDMI_MULTI] ||
                                  adev->outputs[OUTPUT_HDMI_MULTI]->standby)) {
                adev->out_device = output_devices(out) | val;
#ifndef RK3228
                do_out_standby(out);
#endif
            }
            out->device = val;
        }
    }
    unlock_all_outputs(adev, NULL);

    str_parms_destroy(parms);

    ALOGV("%s: exit: status(%d)", __func__, status);
    return status;

}

/*
 * function: get support formats
 * Query supported formats. The response is a '|' separated list of strings from audio_format_t enum
 *  e.g: "sup_formats=AUDIO_FORMAT_PCM_16_BIT"
 */

static int stream_get_parameter_formats(const struct audio_stream *stream,
                                    struct str_parms *query,
                                    struct str_parms *reply)
{
    struct stream_out *out = (struct stream_out *)stream;
    int avail = 1024;
    char value[avail];
    if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_FORMATS)) {
        memset(value,0,avail);
        // set support pcm 16 bit default
        strcat(value, "AUDIO_FORMAT_PCM_16_BIT");
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_FORMATS, value);
        return 0;
    }

    return -1;
}


/*
 * function: get support channels
 * Query supported channel masks. The response is a '|' separated list of strings from
 * audio_channel_mask_t enum
 * e.g: "sup_channels=AUDIO_CHANNEL_OUT_STEREO|AUDIO_CHANNEL_OUT_MONO"
 */

static int stream_get_parameter_channels(struct str_parms *query,
                                    struct str_parms *reply,
                                    audio_channel_mask_t *supported_channel_masks)
{
    char value[1024];
    size_t i, j;
    bool first = true;

    if(str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS)){
        value[0] = '\0';
        i = 0;
        /* the last entry in supported_channel_masks[] is always 0 */
        while (supported_channel_masks[i] != 0) {
            for (j = 0; j < ARRAY_SIZE(channels_name_to_enum_table); j++) {
                if (channels_name_to_enum_table[j].value == supported_channel_masks[i]) {
                    if (!first) {
                        strcat(value, "|");
                    }

                    strcat(value, channels_name_to_enum_table[j].name);
                    first = false;
                    break;
                }
            }
            i++;
        }
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value);
        return 0;
    }

    return -1;
}

/*
 * function: get support sample_rates
 * Query supported sampling rates. The response is a '|' separated list of integer values
 * e.g: ""sup_sampling_rates=44100|48000"
 */

static int stream_get_parameter_rates(struct str_parms *query,
                                struct str_parms *reply,
                                uint32_t *supported_sample_rates)
{
    char value[256];
    int ret = -1;

    if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES)) {
        value[0] = '\0';
        int cursor = 0;
        int i = 0;
        while(supported_sample_rates[i]){
            int avail = sizeof(value) - cursor;
            ret = snprintf(value + cursor, avail, "%s%d",
                           cursor > 0 ? "|" : "",
                           supported_sample_rates[i]);

            if (ret < 0 || ret > avail){
                value[cursor] = '\0';
                break;
            }

            cursor += ret;
            ++i;
        }
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES, value);
        return 0;
    }
    return -1;
}

/**
 * @brief out_get_parameters
 *
 * @param stream
 * @param keys
 *
 * @returns
 */
static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    ALOGD("%s: keys = %s", __func__, keys);

    struct stream_out *out = (struct stream_out *)stream;
    struct str_parms *query = str_parms_create_str(keys);
    char *str = NULL;
    struct str_parms *reply = str_parms_create();

    if (stream_get_parameter_formats(stream,query,reply) == 0) {
        str = str_parms_to_str(reply);
    } else if (stream_get_parameter_channels(query, reply, &out->supported_channel_masks[0]) == 0) {
        str = str_parms_to_str(reply);
    } else if (stream_get_parameter_rates(query, reply, &out->supported_sample_rates[0]) == 0) {
        str = str_parms_to_str(reply);
    } else {
        ALOGD("%s,str_parms_get_str failed !",__func__);
        str = strdup("");
    }

    str_parms_destroy(query);
    str_parms_destroy(reply);

    ALOGV("%s,exit -- str = %s",__func__,str);
    return str;
}

/**
 * @brief out_get_latency
 *
 * @param stream
 *
 * @returns
 */
static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return (out->config.period_size * out->config.period_count * 1000) /
           out->config.rate;
}

/**
 * @brief out_set_volume
 *
 * @param stream
 * @param left
 * @param right
 *
 * @returns
 */
static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;

    /* The mutex lock is not needed, because the client
     * is not allowed to close the stream concurrently with this API
     *  pthread_mutex_lock(&adev->lock_outputs);
     */
    bool is_HDMI = out == adev->outputs[OUTPUT_HDMI_MULTI];
    /*  pthread_mutex_unlock(&adev->lock_outputs); */
    if (is_HDMI) {
        /* only take left channel into account: the API is for stereo anyway */
        out->muted = (left == 0.0f);
        return 0;
    }
    return -ENOSYS;
}
/**
 * @brief dump_out_data
 *
 * @param buffer bytes
 */
static void dump_out_data(const void* buffer,size_t bytes)
{
    char value[PROPERTY_VALUE_MAX];
    property_get("vendor.audio.record", value, "0");
    int size = atoi(value);
    if (size <= 0)
        return ;

    ALOGD("dump pcm file.");
    static FILE* fd = NULL;
    static int offset = 0;
    if (fd == NULL) {
        fd=fopen("/data/misc/audioserver/debug.pcm","wb+");
        if(fd == NULL) {
            ALOGD("DEBUG open /data/debug.pcm ,errno = %s",strerror(errno));
            offset = 0;
        }
    }

    if (fd != NULL) {
        fwrite(buffer,bytes,1,fd);
        offset += bytes;
        fflush(fd);
        if(offset >= size*1024*1024) {
            fclose(fd);
            fd = NULL;
            offset = 0;
            property_set("vendor.audio.record", "0");
            ALOGD("TEST playback pcmfile end");
        }
    }
}

static void dump_in_data(const void* buffer, size_t bytes)
{
    static int offset = 0;
    static FILE* fd = NULL;
    char value[PROPERTY_VALUE_MAX];
    property_get("vendor.audio.record.in", value, "0");
    int size = atoi(value);
    if (size > 0) {
        if(fd == NULL) {
            fd=fopen("/data/misc/audioserver/debug_in.pcm","wb+");
            if(fd == NULL) {
                ALOGD("DEBUG open /data/misc/audioserver/debug_in.pcm ,errno = %s",strerror(errno));
            } else {
                ALOGD("dump pcm to file /data/misc/audioserver/debug_in.pcm");
            }
            offset = 0;
        }
    }

    if (fd != NULL) {
        ALOGD("dump in pcm %zu bytes", bytes);
        fwrite(buffer,bytes,1,fd);
        offset += bytes;
        fflush(fd);
        if (offset >= size*1024*1024) {
            fclose(fd);
            fd = NULL;
            offset = 0;
            property_set("vendor.audio.record.in", "0");
            ALOGD("TEST record pcmfile end");
        }
    }
}

/**
 * @brief reset_bitstream_buf
 *
 * @param out
 */
static void reset_bitstream_buf(struct stream_out *out)
{
    if (is_bitstream(out)) {
        if(out->config.format == PCM_FORMAT_S24_LE) {
            if (out->bitstream_buffer) {
                free (out->bitstream_buffer);
                out->bitstream_buffer = NULL;
            }
        }
    }
}

static void check_hdmi_reconnect(struct stream_out *out)
{
    if (out == NULL) {
        return ;
    }

    struct audio_device *adev = out->dev;
    lock_all_outputs(adev);
    /*
     * if snd_reopen is set to true, this means we need to reopen sound card.
     * There are a situation, we need to do this:
     *   current stream is bistream over hdmi, and hdmi is unpluged and plug later,
     *   the driver of hdmi may init the hdmi in pcm mode automatically, according the
     *   implement of driver of hdmi. If we contiune send bitstream to hdmi open in pcm mode,
     *   hdmi may make noies or mute.
     */
    if (out->snd_reopen && !out->standby)
    {
        /*
         * standby sound cards
         * the driver of hdmi will auto init with last configurations,
         * so, we don't need close and reopen sound card of hdmi here.
         * If driver of hdmi not config the hdmi with last output configurations,
         * please open this codes to close and reopen sound card of hdmi.
         */
  //      do_out_standby(out);
  //      reset_bitstream_buf(out);
    }
    unlock_all_outputs(adev,NULL);
    /*
     * audio hal recived the msg of hdmi plugin, and other part of sdk will reviced it too.
     * Other part(maybe hwc) will config hdmi after it reviced the msg.
     * Audio must wait other part(maybe hwc) codes config hdmi finish, before send bitstream datas to hdmi
     */
    if (out->snd_reopen && is_bitstream(out) && (out->device == AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
#ifdef USE_DRM
        const char* PATH = "/sys/class/drm/card0-HDMI-A-1/enabled";
#else
        const char* PATH = "/sys/class/display/HDMI/enabled";
#endif
        if (access(PATH, R_OK) != 0) {
            /*
             * in most test, the time is 700~800ms between received msg of hdmi plug in
             * and hdmi init finish, so we sleep 1 sec here if no way to get the status of hdmi.
             */
            usleep(1000000);
        } else {
            /*
             * read this node to judge the status of hdmi is config finish?
             */
            char buffer[1024];
            int counter  = 200;
            FILE* file = NULL;
            while (counter >= 0 && ((file = fopen(PATH,"r")) != NULL)) {
                int size = fread(buffer,1,sizeof(buffer),file);
                if(size >= 0) {
                    if(strstr(buffer,"enabled")) {
                        fclose(file);
                        usleep(10000);
                        break;
                    }
                }
                usleep(10000);
                counter --;
                fclose(file);
            }
        }
        ALOGD("%s: out = %p",__FUNCTION__,out);
        out->snd_reopen = false;
    }
}

static void out_mute_data(struct stream_out *out,void* buffer,size_t bytes)
{
    struct audio_device *adev = out->dev;
    bool mute = false;

#ifdef MUTE_WHEN_SCREEN_OFF
    mute = adev->screenOff;
#endif
    // for some special customer
    char value[PROPERTY_VALUE_MAX];
    property_get("vendor.audio.mute", value, "false");
    if (!strcasecmp(value,"true")) {
        mute = true;
    }

    if (out->muted || mute){
        memset((void *)buffer, 0, bytes);
    }
}

static int fill_hdmi_bistream(struct stream_out *out,void* buffer,size_t insize)
{
    int size = 2*insize;
    if (out->bitstream_buffer == NULL) {
        out->bitstream_buffer = (char *)malloc(size);
        ALOGD("new bitstream buffer!");
    }
    memset(out->bitstream_buffer, 0x00, size);
    fill_hdmi_bitstream_buf((void *)buffer, (void *)out->bitstream_buffer,(void*)out->channel_buffer, (int)insize);
    return size;
}

static int bitstream_write_data(struct stream_out *out,void* buffer,size_t bytes)
{
    if ((out == NULL) || (buffer == NULL) || (bytes <= 0)) {
        ALOGD("%s: %d, input parameter is invalid",__FUNCTION__,__LINE__);
        return -1;
    }

    struct audio_device *adev = out->dev;
    int ret = 0;
    if ((out->device & AUDIO_DEVICE_OUT_AUX_DIGITAL) && (is_multi_pcm(out) || is_bitstream(out))) {
        int card = adev->dev_out[SND_OUT_SOUND_CARD_HDMI].card;
        if ((card != SND_OUT_SOUND_CARD_UNKNOWN) && (out->pcm[SND_OUT_SOUND_CARD_HDMI] != NULL)) {
            if(out->config.format == PCM_FORMAT_S16_LE){
                out_mute_data(out,buffer,bytes);
                dump_out_data(buffer, bytes);
                ret = pcm_write(out->pcm[SND_OUT_SOUND_CARD_HDMI], (void *)buffer, bytes);
            }else if(out->config.format == PCM_FORMAT_S24_LE){
                int size = fill_hdmi_bistream(out,buffer,bytes);
                out_mute_data(out,(void*)out->bitstream_buffer,size);
                dump_out_data((void*)out->bitstream_buffer, size);
                ret = pcm_write(out->pcm[SND_OUT_SOUND_CARD_HDMI], (void *)out->bitstream_buffer, size);
            }
        } else {
            ALOGD("%s: %d: HDMI sound card not open",__FUNCTION__,__LINE__);
            ret = -1;
        }
    }

    return ret;
}

/**
 * @brief out_write
 *
 * @param stream
 * @param buffer
 * @param bytes
 *
 * @returns
 */

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret = 0;
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    size_t newbytes = bytes * 2;
    int i,card;
    /* FIXME This comment is no longer correct
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the output stream mutex - e.g.
     * executing out_set_parameters() while holding the hw device
     * mutex
     */

	if (!hasExtCodec()){
	    check_hdmi_reconnect(out);
	}

    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        pthread_mutex_unlock(&out->lock);
        lock_all_outputs(adev);
        if (!out->standby) {
            unlock_all_outputs(adev, out);
            goto false_alarm;
        }
        ret = start_output_stream(out);
        if (ret < 0) {
            unlock_all_outputs(adev, NULL);
            goto final_exit;
        }
        out->standby = false;
        unlock_all_outputs(adev, out);
    }
false_alarm:

    if (out->disabled) {
        ret = -EPIPE;
        ALOGD("%s: %d: error out = %p",__FUNCTION__,__LINE__,out);
        goto exit;
    }


#ifdef AUDIO_3A
    if (adev->voice_api != NULL) {
        int ret = 0;
        adev->voice_api->queuePlaybackBuffer(buffer, bytes);
        ret = adev->voice_api->getPlaybackBuffer(buffer, bytes);
        if (ret < 0) {
            memset((char *)buffer, 0x00, bytes);
        }
    }
#endif

    /* Write to all active PCMs */
    if ((out->device & AUDIO_DEVICE_OUT_AUX_DIGITAL) && is_bitstream(out)) {
if (!hasExtCodec()){
        ret = bitstream_write_data(out,(void*)buffer,bytes);
        if(ret < 0) {
            goto exit;
        }
}
    } else {
        out_mute_data(out,(void*)buffer,bytes);
        dump_out_data(buffer, bytes);
        ret = -1;
        for (i = 0; i < SND_OUT_SOUND_CARD_MAX; i++)
            if (out->pcm[i]) {
                if (i == SND_OUT_SOUND_CARD_BT) {
                    // Check if this is SIMCOM - read card ID to determine
                    int bt_card = adev->dev_out[SND_OUT_SOUND_CARD_BT].card;
                    char simcom_id_check[20] = {0};
                    bool is_simcom_write = false;
                    if (bt_card >= 0 && bt_card < SNDRV_CARDS) {
                        char card_path[64];
                        snprintf(card_path, sizeof(card_path), "/proc/asound/card%d/id", bt_card);
                        FILE *id_file = fopen(card_path, "r");
                        if (id_file) {
                            size_t len = fread(simcom_id_check, 1, sizeof(simcom_id_check) - 1, id_file);
                            fclose(id_file);
                            if (len > 0 && simcom_id_check[len - 1] == '\n') {
                                simcom_id_check[len - 1] = '\0';
                            }
                            if (strstr(simcom_id_check, "SIMCOM") || strstr(simcom_id_check, "simcom")) {
                                is_simcom_write = true;
                            }
                        }
                    }
                    
                    if (is_simcom_write) {
                        if (adev->simcom_voice_active) {
                            out->usecase = USECASE_SIMCOM_VOICE_CALL;
                            out->usecase_type = USECASE_TYPE_VOICE_CALL;
                        } else {
                            out->usecase = USECASE_PRIMARY_PLAYBACK;
                            out->usecase_type = USECASE_TYPE_PCM_PLAYBACK;
                        }
                        // Verify PCM is actually open and owned by this stream
                        if (!out->pcm[i] || adev->owner[SOUND_CARD_BT] != (int*)out) {
                            ALOGE("SIMCOM PCM not open or not owned by this stream (pcm=%p, owner=%p, out=%p), skipping write",
                                  out->pcm[i], adev->owner[SOUND_CARD_BT], out);
                            continue; // Skip writing to SIMCOM if not owned by this stream
                        }

                        ALOGE("SIMCOM UPLINK: voice_active=%d ring=%p ring_size=%zu pcm=%p pcm_ready=%d owner=%p",
                              adev->simcom_voice_active,
                              adev->simcom_mic_ring,
                              adev->simcom_mic_ring_size,
                              out->pcm[i],
                              out->pcm[i] ? pcm_is_ready(out->pcm[i]) : 0,
                              adev->owner[SOUND_CARD_BT]);
                        
                        // SIMCOM requires strict 640-byte periods (320 samples at 8kHz mono 16-bit)
                        // Accumulate data in buffer until we have a full period
                        const size_t period_bytes = 640;  // 320 samples * 2 bytes
                        const size_t period_samples = 320;
                        // ИСПРАВЛЕНИЕ: увеличенный буфер для обработки большего объема данных за итерацию
                        const size_t max_uplink_buffer_samples = 640;  // Удвоенный размер для обработки большего объема
                        int16_t uplink_buffer[max_uplink_buffer_samples];

                        if (adev->simcom_voice_active) {
                            if (simcom_voice_ensure_ring(adev) == 0) {
                                // ИСПРАВЛЕНИЕ: внутренний цикл для обработки всех доступных данных за итерацию
                                // Обрабатываем данные пока ring buffer не опустеет ниже threshold
                                // Ограничиваем максимальную обработку чтобы не блокировать надолго
                                const size_t min_ring_threshold = 160;  // Минимальный порог для продолжения обработки
                                const size_t max_iterations_per_call = 4;  // Максимум 4 периода за один вызов out_write
                                size_t total_processed = 0;
                                size_t iteration = 0;
                                
                                while (iteration < max_iterations_per_call && adev->simcom_voice_active) {
                                    // Проверяем доступность данных в ring buffer БЕЗ блокировки
                                    size_t available_samples = 0;
                                    pthread_mutex_lock(&adev->simcom_mic_lock);
                                    if (adev->simcom_mic_ring) {
                                        if (adev->simcom_mic_ring_full) {
                                            available_samples = adev->simcom_mic_ring_size;
                                        } else if (adev->simcom_mic_ring_read < adev->simcom_mic_ring_write) {
                                            available_samples = adev->simcom_mic_ring_write - adev->simcom_mic_ring_read;
                                        } else if (adev->simcom_mic_ring_read > adev->simcom_mic_ring_write) {
                                            available_samples = adev->simcom_mic_ring_size - adev->simcom_mic_ring_read + adev->simcom_mic_ring_write;
                                        }
                                    }
                                    pthread_mutex_unlock(&adev->simcom_mic_lock);
                                    
                                    // Если данных меньше порога - выходим из цикла
                                    if (available_samples < min_ring_threshold && iteration > 0) {
                                        break;
                                    }
                                    
                                    // Читаем один период данных
                                    size_t fetched_total = 0;
                                    
                                    // КРИТИЧНО: сначала пытаемся прочитать доступные данные БЕЗ ожидания
                                    size_t first_fetch = simcom_ring_pop(adev,
                                                                         uplink_buffer,
                                                                         period_samples);
                                    fetched_total += first_fetch;
                                    
                                    // ИСПРАВЛЕНИЕ: минимальное ожидание только если данных нет вообще
                                    // Если есть хотя бы часть данных - отправляем их, не ждем
                                    if (fetched_total == 0 && adev->simcom_voice_active) {
                                        // Только если данных нет вообще - делаем одну короткую попытку
                                        const int max_wait_iterations = 1;  // Максимум 1 попытка (10 мс)
                                        int wait_iterations = 0;
                                        
                                        while (fetched_total == 0 && wait_iterations < max_wait_iterations) {
                                            // Проверяем состояние БЕЗ блокировки мьютекса
                                            if (!adev->simcom_voice_active) {
                                                break;
                                            }
                                            
                                            // КРИТИЧНО: используем короткий timeout (10 мс) для предотвращения deadlock
                                            struct timespec timeout;
                                            clock_gettime(CLOCK_REALTIME, &timeout);
                                            timeout.tv_nsec += 10 * 1000000;  // 10 мс timeout
                                            if (timeout.tv_nsec >= 1000000000) {
                                                timeout.tv_sec++;
                                                timeout.tv_nsec -= 1000000000;
                                            }
                                            
                                            // КРИТИЧНО: минимальное время блокировки мьютекса
                                            pthread_mutex_lock(&adev->simcom_mic_lock);
                                            if (!adev->simcom_voice_active) {
                                                pthread_mutex_unlock(&adev->simcom_mic_lock);
                                                break;
                                            }
                                            
                                            // Ожидание с timeout - всегда выходит через timeout или по сигналу
                                            int cond_ret = pthread_cond_timedwait(&adev->simcom_mic_cond,
                                                                                  &adev->simcom_mic_lock,
                                                                                  &timeout);
                                            bool still_active = adev->simcom_voice_active;
                                            pthread_mutex_unlock(&adev->simcom_mic_lock);
                                            
                                            if (!still_active) {
                                                break;
                                            }
                                            
                                            wait_iterations++;
                                            
                                            // ИСПРАВЛЕНИЕ: обработка spurious wakeup - всегда проверяем данные после пробуждения
                                            size_t fetched = simcom_ring_pop(adev,
                                                                             uplink_buffer,
                                                                             period_samples);
                                            if (fetched > 0) {
                                                fetched_total += fetched;
                                                break;  // Данные получены - выходим из ожидания
                                            }
                                        }
                                        
                                        if (simcom_debug_audio_enabled() && fetched_total == 0 && wait_iterations >= max_wait_iterations) {
                                            static unsigned int no_data_counter = 0;
                                            no_data_counter++;
                                            if ((no_data_counter & 0x1F) == 0) {  // Каждые 32 раза
                                                ALOGE("SIMCOM uplink: no data after %d waits, padding silence", wait_iterations);
                                            }
                                        }
                                    }
                                    
                                    // ИСПРАВЛЕНИЕ: валидация аудио данных - проверка на нули и уровень сигнала
                                    bool has_valid_data = false;
                                    int32_t signal_level = 0;
                                    int32_t max_abs_sample = 0;
                                    if (fetched_total > 0) {
                                        for (size_t v = 0; v < fetched_total; ++v) {
                                            int32_t sample = uplink_buffer[v];
                                            if (sample < 0) sample = -sample;
                                            signal_level += sample;
                                            if (sample > max_abs_sample) max_abs_sample = sample;
                                            if (sample > 100) {  // Порог для определения валидных данных
                                                has_valid_data = true;
                                            }
                                        }
                                        signal_level = (fetched_total > 0) ? (signal_level / (int32_t)fetched_total) : 0;
                                    }
                                    
                                    // ИСПРАВЛЕНИЕ: заполняем нулями только если данных нет
                                    if (fetched_total < period_samples) {
                                        memset(uplink_buffer + fetched_total, 0,
                                               (period_samples - fetched_total) * sizeof(int16_t));
                                        if (simcom_debug_audio_enabled() && fetched_total == 0) {
                                            static unsigned int silence_counter = 0;
                                            silence_counter++;
                                            if ((silence_counter & 0x1F) == 0) {  // Каждые 32 раза
                                                ALOGE("SIMCOM uplink: no data available, padding silence (iter=%u)", silence_counter);
                                            }
                                        }
                                    } else if (simcom_debug_audio_enabled() && !has_valid_data && max_abs_sample < 100) {
                                        static unsigned int low_signal_counter = 0;
                                        low_signal_counter++;
                                        if ((low_signal_counter & 0x1F) == 0) {  // Каждые 32 раза
                                            ALOGE("SIMCOM uplink: low signal level detected (avg=%d max=%d, fetched=%zu)",
                                                  signal_level, max_abs_sample, fetched_total);
                                        }
                                    }
                                    
                                    // ИСПРАВЛЕНИЕ: проверка и подготовка PCM перед записью
                                    if (!pcm_is_ready(out->pcm[i])) {
                                        if (simcom_debug_audio_enabled()) {
                                            ALOGE("SIMCOM PCM not ready before uplink write, preparing");
                                        }
                                        if (pcm_prepare(out->pcm[i]) != 0) {
                                            ALOGE("SIMCOM PCM prepare failed: %s", pcm_get_error(out->pcm[i]));
                                            // Пропускаем запись, если PCM не готов
                                            break;
                                        }
                                    }
                                    
                                    // ИСПРАВЛЕНИЕ: для SIMCOM драйвер запускает PCM автоматически при первой записи
                                    // ИСПРАВЛЕНИЕ: используем правильный размер в байтах (fetched_total * sizeof(int16_t))
                                    // Но для PCM записи всегда используем полный период (period_bytes)
                                    // так как драйвер ожидает строго 640 байт
                                    size_t bytes_to_write = period_bytes;  // Всегда полный период для драйвера
                                    ret = pcm_write(out->pcm[i], uplink_buffer, bytes_to_write);
                                    
                                    // Обработка результата записи
                                    if (ret == 0) {
                                        // Успешная запись
                                        if (!out->simcom_pcm_started) {
                                            out->simcom_pcm_started = true;
                                            if (simcom_debug_audio_enabled()) {
                                                ALOGE("SIMCOM PCM write succeeded, driver should have started PCM automatically");
                                            }
                                        }
                                        total_processed += fetched_total;
                                    } else {
                                        // ИСПРАВЛЕНИЕ: улучшенная retry логика при ошибках PCM
                                        const char *error_msg = pcm_get_error(out->pcm[i]);
                                        static unsigned int pcm_error_counter = 0;
                                        static unsigned int pcm_recovery_counter = 0;
                                        pcm_error_counter++;
                                        
                                        // Пытаемся переподготовить PCM при ошибках
                                        if (pcm_prepare(out->pcm[i]) == 0) {
                                            // Повторная попытка записи после переподготовки
                                            ret = pcm_write(out->pcm[i], uplink_buffer, bytes_to_write);
                                            if (ret == 0) {
                                                pcm_recovery_counter++;
                                                total_processed += fetched_total;
                                                if (simcom_debug_audio_enabled() && (pcm_recovery_counter % 10) == 0) {
                                                    ALOGE("SIMCOM uplink pcm_write recovered after prepare retry (recovered=%u, original_error=%s)", 
                                                          pcm_recovery_counter, error_msg);
                                                }
                                            } else {
                                                // Логируем ошибку периодически, чтобы не засорять логи
                                                if (simcom_debug_audio_enabled() && (pcm_error_counter % 50) == 0) {
                                                    ALOGE("SIMCOM uplink pcm_write failed: %s -> %s (bytes=%zu, errors=%u)", 
                                                          error_msg, pcm_get_error(out->pcm[i]), bytes_to_write, pcm_error_counter);
                                                }
                                                // При ошибке выходим из цикла
                                                break;
                                            }
                                        } else {
                                            // Логируем ошибку периодически
                                            if (simcom_debug_audio_enabled() && (pcm_error_counter % 50) == 0) {
                                                ALOGE("SIMCOM uplink pcm_write failed: %s (bytes=%zu, prepare failed, errors=%u)", 
                                                      error_msg, bytes_to_write, pcm_error_counter);
                                            }
                                            // При ошибке выходим из цикла
                                            break;
                                        }
                                    }
                                    
                                    // ИСПРАВЛЕНИЕ: финальная диагностика pipeline с согласованными типами (samples)
                                    if (ret == 0 && simcom_debug_audio_enabled()) {
                                        // Вычисляем заполненность ring buffer в samples
                                        size_t ring_used = 0;
                                        size_t ring_capacity = adev->simcom_mic_ring_size;
                                        if (adev->simcom_mic_ring) {
                                            pthread_mutex_lock(&adev->simcom_mic_lock);
                                            if (adev->simcom_mic_ring_full) {
                                                ring_used = ring_capacity;
                                            } else if (adev->simcom_mic_ring_read < adev->simcom_mic_ring_write) {
                                                ring_used = adev->simcom_mic_ring_write - adev->simcom_mic_ring_read;
                                            } else if (adev->simcom_mic_ring_read > adev->simcom_mic_ring_write) {
                                                ring_used = ring_capacity - adev->simcom_mic_ring_read + adev->simcom_mic_ring_write;
                                            }
                                            size_t ring_usage_percent = (ring_capacity > 0) ? (ring_used * 100 / ring_capacity) : 0;
                                            pthread_mutex_unlock(&adev->simcom_mic_lock);
                                            
                                            static unsigned int uplink_diag_counter = 0;
                                            uplink_diag_counter++;
                                            if ((uplink_diag_counter & 0x1F) == 0) {  // Каждые 32 раза
                                                ALOGE("SIMCOM DBG UPLINK: iter=%zu processed=%zu fetched=%zu signal_avg=%d signal_max=%d valid=%d ring=%zu/%zu (%zu%%)",
                                                      iteration, total_processed, fetched_total, signal_level, max_abs_sample,
                                                      has_valid_data ? 1 : 0, ring_used, ring_capacity, ring_usage_percent);
                                            }
                                        }
                                    }
                                    
                                    if (ret != 0) {
                                        ALOGE("SIMCOM uplink pcm_write error: %s (bytes=%zu, owner=%p)",
                                              pcm_get_error(out->pcm[i]), bytes_to_write, adev->owner[SOUND_CARD_BT]);
                                        break;  // При ошибке выходим из цикла
                                    }
                                    
                                    iteration++;
                                }  // Конец while цикла обработки данных
                                
                                if (simcom_debug_audio_enabled() && total_processed > 0) {
                                    static unsigned int batch_counter = 0;
                                    batch_counter++;
                                    if ((batch_counter & 0x1F) == 0) {  // Каждые 32 раза
                                        ALOGE("SIMCOM DBG UPLINK: batch processed %zu samples in %zu iterations", 
                                              total_processed, iteration);
                                    }
                                }
                            }  // Конец if (simcom_voice_ensure_ring)
                            continue;  // Пропускаем обычную обработку для SIMCOM voice
                        }
                        
                        // Allocate accumulation buffer if needed
                        if (!out->simcom_buffer) {
                            out->simcom_buffer = (int16_t *)malloc(period_samples * sizeof(int16_t));
                            if (!out->simcom_buffer) {
                                ALOGE("SIMCOM: failed to allocate accumulation buffer");
                                ret = -ENOMEM;
                                break;
                            }
                            out->simcom_buffer_used = 0;
                        }

                        const uint32_t in_rate = out->config.rate;
                        const uint32_t in_channels = out->config.channels;
                        const int16_t *in16 = (const int16_t *)buffer;

                        if (simcom_debug_audio_enabled()) {
                            size_t dbg_frames = bytes / (sizeof(int16_t) * (in_channels ? in_channels : 1));
                            if (dbg_frames > 0 && in_channels > 0) {
                                int64_t sum_abs = 0;
                                int32_t max_abs = 0;
                                int16_t first_sample = 0;
                                int16_t last_sample = 0;
                                for (size_t dbg_idx = 0; dbg_idx < dbg_frames; ++dbg_idx) {
                                    int32_t sample_acc = 0;
                                    for (uint32_t ch = 0; ch < in_channels; ++ch) {
                                        sample_acc += in16[dbg_idx * in_channels + ch];
                                    }
                                    int32_t sample = sample_acc / (int32_t)in_channels;
                                    if (dbg_idx == 0) {
                                        first_sample = (int16_t)sample;
                                    }
                                    if (dbg_idx == dbg_frames - 1) {
                                        last_sample = (int16_t)sample;
                                    }
                                    int32_t abs_sample = (sample < 0) ? -sample : sample;
                                    sum_abs += abs_sample;
                                    if (abs_sample > max_abs) {
                                        max_abs = abs_sample;
                                    }
                                }
                                int32_t avg_abs = (int32_t)(sum_abs / (int64_t)dbg_frames);
                                ALOGE("SIMCOM DBG IN: frames=%zu rate=%u ch=%u avg_abs=%d max_abs=%d first=%d last=%d",
                                      dbg_frames, in_rate, in_channels, avg_abs, max_abs, first_sample, last_sample);
                            } else {
                                ALOGE("SIMCOM DBG IN: empty buffer rate=%u ch=%u bytes=%zu", in_rate, in_channels, bytes);
                            }
                        }

                        // Convert input to 8kHz mono only if required
                        bool needs_resample = !(in_rate == 8000 && in_channels == 1);
                        size_t in_frames_total = bytes / (sizeof(int16_t) * in_channels);
                        int16_t *conv = NULL;
                        const int16_t *write_samples = NULL;
                        size_t write_frames = 0;

                        if (needs_resample) {
                            size_t max_out_frames = (size_t)((uint64_t)in_frames_total * 8000 / (in_rate ? in_rate : 8000) + 16);
                            conv = (int16_t *)malloc(max_out_frames * sizeof(int16_t));
                            if (!conv) {
                                ALOGE("SIMCOM convert: malloc failed, dropping write");
                                ret = -ENOMEM;
                                break;
                            }
                            double pos = 0.0;
                            const double step = (double)in_rate / 8000.0;
                            size_t out_count = 0;
                            for (; out_count < max_out_frames && (size_t)pos < in_frames_total; ++out_count) {
                                size_t idx = (size_t)pos;
                                if (idx >= in_frames_total) break;
                                if (in_channels == 1) {
                                    conv[out_count] = in16[idx];
                                } else {
                                    int32_t l = in16[idx * in_channels + 0];
                                    int32_t r = in16[idx * in_channels + 1];
                                    conv[out_count] = (int16_t)((l + r) / 2);
                                }
                                pos += step;
                            }
                            write_samples = conv;
                            write_frames = out_count;
                        } else {
                            // Already 8kHz mono - use input buffer directly
                            write_samples = in16;
                            write_frames = in_frames_total;
                        }

                        size_t frames_appended = 0;
                        while (frames_appended < write_frames) {
                            size_t space_available = period_samples - out->simcom_buffer_used;
                            size_t frames_to_copy = write_frames - frames_appended;
                            if (frames_to_copy > space_available) {
                                frames_to_copy = space_available;
                            }

                            memcpy(out->simcom_buffer + out->simcom_buffer_used,
                                   write_samples + frames_appended,
                                   frames_to_copy * sizeof(int16_t));
                            out->simcom_buffer_used += frames_to_copy;
                            frames_appended += frames_to_copy;

                            if (out->simcom_buffer_used < period_samples) {
                                continue;
                            }

                            // Write full period
                            if (simcom_debug_audio_enabled()) {
                                int32_t sum_abs = 0;
                                int32_t max_abs = 0;
                                for (size_t dbg_idx = 0; dbg_idx < period_samples; ++dbg_idx) {
                                    int32_t sample = out->simcom_buffer[dbg_idx];
                                    if (sample < 0) {
                                        sample = -sample;
                                    }
                                    sum_abs += sample;
                                    if (sample > max_abs) {
                                        max_abs = sample;
                                    }
                                }
                                int32_t avg_abs = sum_abs / (int32_t)period_samples;
                                int16_t first_sample = out->simcom_buffer[0];
                                int16_t last_sample = out->simcom_buffer[period_samples - 1];
                                ALOGE("SIMCOM DBG: period stats avg_abs=%d max_abs=%d first=%d last=%d",
                                      avg_abs, max_abs, first_sample, last_sample);
                            }
                            // Verify PCM is still valid before writing
                            if (!out->pcm[i]) {
                                ALOGE("SIMCOM PCM is NULL during write (owner=%p, out=%p)", 
                                      adev->owner[SOUND_CARD_BT], out);
                                ret = -EIO;
                                break;
                            }
                            if (!pcm_is_ready(out->pcm[i])) {
                                ALOGE("SIMCOM PCM not ready, attempting prepare (pcm=%p)", out->pcm[i]);
                                pcm_prepare(out->pcm[i]);
                                // Don't call pcm_start here - SIMCOM driver starts automatically on write
                                if (!pcm_is_ready(out->pcm[i])) {
                                    ALOGE("SIMCOM PCM still not ready after prepare: %s", 
                                          pcm_get_error(out->pcm[i]));
                                    ret = -EIO;
                                    break;
                                }
                            }
                            
                            // For SIMCOM: write data first, then start PCM
                            // The driver expects data in runtime->dma_area before URB submission
                            ALOGE("SIMCOM: About to write %zu bytes to PCM (pcm=%p, ready=%d)", 
                                  period_bytes, out->pcm[i], pcm_is_ready(out->pcm[i]));
                            ret = pcm_write(out->pcm[i], (const void *)out->simcom_buffer, period_bytes);
                            if (ret == 0) {
                                ALOGE("SIMCOM: pcm_write succeeded (%zu bytes)", period_bytes);
                            }
                            if (ret != 0) {
                                ALOGE("SIMCOM pcm_write error: %s (bytes=%zu, period=%zu, owner=%p)",
                                      pcm_get_error(out->pcm[i]), period_bytes, period_bytes, adev->owner[SOUND_CARD_BT]);
                                
                                // If first write fails, try to start PCM first (some drivers need this)
                                if (!out->simcom_pcm_started && ret < 0 && (errno == EIO || errno == EBUSY || errno == EAGAIN || ret == -EPIPE)) {
                                    ALOGE("SIMCOM: first write failed, attempting to start PCM first");
                                    pcm_ioctl(out->pcm[i], SNDRV_PCM_IOCTL_START);
                                    out->simcom_pcm_started = true;
                                    // Retry write after starting
                                    ret = pcm_write(out->pcm[i], (const void *)out->simcom_buffer, period_bytes);
                                    if (ret == 0) {
                                        ALOGE("SIMCOM: write succeeded after start");
                                        // Success, shift buffer
                                        memmove(out->simcom_buffer, out->simcom_buffer + period_samples, 
                                                (out->simcom_buffer_used - period_samples) * sizeof(int16_t));
                                        out->simcom_buffer_used -= period_samples;
                                        continue;
                                    }
                                }
                                
                                if (ret == -EPIPE) {
                                    pcm_prepare(out->pcm[i]);
                                    pcm_ioctl(out->pcm[i], SNDRV_PCM_IOCTL_START);
                                    out->simcom_pcm_started = true;
                                    continue; // retry same period
                                }
                                // If I/O error, try to prepare and start PCM, then retry
                                if (ret < 0 && errno == EIO) {
                                    ALOGE("SIMCOM I/O error, attempting pcm_prepare/start and retry");
                                    pcm_prepare(out->pcm[i]);
                                    pcm_ioctl(out->pcm[i], SNDRV_PCM_IOCTL_START);
                                    out->simcom_pcm_started = true;
                                    ret = pcm_write(out->pcm[i], (const void *)out->simcom_buffer, period_bytes);
                                    if (ret == 0) {
                                        // Success, shift buffer
                                        memmove(out->simcom_buffer, out->simcom_buffer + period_samples, 
                                                (out->simcom_buffer_used - period_samples) * sizeof(int16_t));
                                        out->simcom_buffer_used -= period_samples;
                                        continue;
                                    }
                                }
                                // If device busy, PCM might be in wrong state - try close/reopen
                                if (ret < 0 && (errno == EBUSY || errno == EAGAIN)) {
                                    ALOGE("SIMCOM device busy, closing PCM");
                                    pcm_close(out->pcm[i]);
                                    out->pcm[i] = NULL;
                                    adev->owner[SOUND_CARD_BT] = NULL;
                                    out->simcom_pcm_started = false;
                                    out->simcom_periods_written = 0;
                                }
                                break;
                            }
                            // After successful write, start PCM if not already started
                            // Write several periods first to fill the buffer, then start PCM
                            if (!out->simcom_pcm_started) {
                                out->simcom_pcm_started = true;
                            }
                            // Successfully wrote period, shift buffer
                            memmove(out->simcom_buffer, out->simcom_buffer + period_samples, 
                                    (out->simcom_buffer_used - period_samples) * sizeof(int16_t));
                            out->simcom_buffer_used -= period_samples;
                        }
                        if (conv) {
                            free(conv);
                        }
                        
                        // Copy remaining frames (already handled inside loop), nothing left to do here
                        if (ret != 0) break;
                    } else if (out->resampler) {
                        // BT with resampler (legacy BT code)
                    size_t inFrameCount = bytes/2/2;
                    // Use 8000 Hz as standard SCO rate (pcm_config_ap_sco not defined)
                    const unsigned int sco_rate = 8000;
                    size_t outFrameCount = inFrameCount/(out->config.rate/sco_rate);
                    int16_t out_buffer[outFrameCount*2];
                    memset(out_buffer, 0x00, outFrameCount*2);

                    out->resampler->resample_from_input(out->resampler,
                                                        (const int16_t *)buffer,
                                                        &inFrameCount,
                                                        out_buffer,
                                                        &outFrameCount);

                    ret = pcm_write(out->pcm[i], (void *)out_buffer, outFrameCount*2*2);
                    if (ret != 0)
                        break;
                    } else {
                        // BT without resampler - write directly
                        ret = pcm_write(out->pcm[i], (void *)buffer, bytes);
                        if (ret != 0)
                            break;
                    }
                } else {
if (!hasExtCodec()){
                    /*
                     * do not write hdmi/spdif snd sound if they are taken by other bitstream/multi channle pcm stream
                     */
                    if(((i == SND_OUT_SOUND_CARD_HDMI) && (adev->owner[SOUND_CARD_HDMI] != (int*)out) && (adev->owner[SOUND_CARD_HDMI] != NULL)) ||
                        ((i == SND_OUT_SOUND_CARD_SPDIF) && (adev->owner[SOUND_CARD_SPDIF] != (int*)out) && (adev->owner[SOUND_CARD_SPDIF] != NULL))){
                        continue;
                    }
}
                    ret = pcm_write(out->pcm[i], (void *)buffer, bytes);
                    if (ret != 0)
                        break;
                }
            }
    }
exit:
    pthread_mutex_unlock(&out->lock);
final_exit:
    {
        // For PCM we always consume the buffer and return #bytes regardless of ret.
        out->written += bytes / (out->config.channels * sizeof(short));
        out->nframes = out->written;
    }
    if (ret != 0) {
        ALOGD("AudioData write  error , keep slience! ret = %d", ret);
        usleep(bytes * 1000000 / audio_stream_out_frame_size(stream) /
               out_get_sample_rate(&stream->common));
    }

    return bytes;
}

/**
 * @brief out_get_render_position
 *
 * @param stream
 * @param dsp_frames
 *
 * @returns
 */
static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    struct stream_out *out = (struct stream_out *)stream;

    *dsp_frames = out->nframes;
    return 0;
}

/**
 * @brief out_add_audio_effect
 *
 * @param stream
 * @param effect
 *
 * @returns
 */
static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

/**
 * @brief out_remove_audio_effect
 *
 * @param stream
 * @param effect
 *
 * @returns
 */
static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

/**
 * @brief out_get_next_write_timestamp
 *
 * @param stream
 * @param timestamp
 *
 * @returns
 */
static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    ALOGV("%s: %d Entered", __FUNCTION__, __LINE__);
    return -ENOSYS;
}

/**
 * @brief out_get_presentation_position
 *
 * @param stream
 * @param frames
 * @param timestamp
 *
 * @returns
 */
static int out_get_presentation_position(const struct audio_stream_out *stream,
        uint64_t *frames, struct timespec *timestamp)
{
    struct stream_out *out = (struct stream_out *)stream;
    int ret = -1;

    pthread_mutex_lock(&out->lock);

    int i;
    // There is a question how to implement this correctly when there is more than one PCM stream.
    // We are just interested in the frames pending for playback in the kernel buffer here,
    // not the total played since start.  The current behavior should be safe because the
    // cases where both cards are active are marginal.
    for (i = 0; i < SND_OUT_SOUND_CARD_MAX; i++)
        if (out->pcm[i]) {
            size_t avail;
            //ALOGD("===============%s,%d==============",__FUNCTION__,__LINE__);
            if (pcm_get_htimestamp(out->pcm[i], &avail, timestamp) == 0) {
                size_t kernel_buffer_size = out->config.period_size * out->config.period_count;
                //ALOGD("===============%s,%d==============",__FUNCTION__,__LINE__);
                // FIXME This calculation is incorrect if there is buffering after app processor
                int64_t signed_frames = out->written - kernel_buffer_size + avail;
                //signed_frames -= 17;
                //ALOGV("============singed_frames:%lld=======",signed_frames);
                //ALOGV("============timestamp:%lld==========",timestamp);
                // It would be unusual for this value to be negative, but check just in case ...
                if (signed_frames >= 0) {
                    *frames = signed_frames;
                    ret = 0;
                }
                break;
            }
        }
    pthread_mutex_unlock(&out->lock);

    return ret;
}

/**
 * @brief in_get_sample_rate
 * audio_stream_in implementation
 *
 * @param stream
 *
 * @returns
 */
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    //ALOGV("%s:get requested_rate : %d ",__FUNCTION__,in->requested_rate);
    return in->requested_rate;
}

/**
 * @brief in_set_sample_rate
 *
 * @param stream
 * @param rate
 *
 * @returns
 */
static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

/**
 * @brief in_get_channels
 *
 * @param stream
 *
 * @returns
 */
static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    //ALOGV("%s:get channel_mask : %d ",__FUNCTION__,in->channel_mask);
    return in->channel_mask;
}


/**
 * @brief in_get_buffer_size
 *
 * @param stream
 *
 * @returns
 */
static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return get_input_buffer_size(in->requested_rate,
                                 AUDIO_FORMAT_PCM_16_BIT,
                                 audio_channel_count_from_in_mask(in_get_channels(stream)),
                                 (in->flags & AUDIO_INPUT_FLAG_FAST) != 0);
}

/**
 * @brief in_get_format
 *
 * @param stream
 *
 * @returns
 */
static audio_format_t in_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

/**
 * @brief in_set_format
 *
 * @param stream
 * @param format
 *
 * @returns
 */
static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

/**
 * @brief do_in_standby
 * must be called with in stream and hw device mutex locked
 *
 * @param in
 */
static void do_in_standby(struct stream_in *in)
{
    struct audio_device *adev = in->dev;

    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;

        if (in->device & AUDIO_DEVICE_IN_HDMI) {
            route_pcm_close(HDMI_IN_CAPTURE_OFF_ROUTE);
        }

        in->dev->input_source = AUDIO_SOURCE_DEFAULT;
        in->dev->in_device = AUDIO_DEVICE_NONE;
        in->dev->in_channel_mask = 0;
        in->standby = true;
        route_pcm_close(CAPTURE_OFF_ROUTE);
    }

}

/**
 * @brief in_standby
 *
 * @param stream
 *
 * @returns
 */
static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    pthread_mutex_lock(&in->lock);
    pthread_mutex_lock(&in->dev->lock);

    do_in_standby(in);

    pthread_mutex_unlock(&in->dev->lock);
    pthread_mutex_unlock(&in->lock);

    return 0;
}

/**
 * @brief in_dump
 *
 * @param stream
 * @param fd
 *
 * @returns
 */
int in_dump(const struct audio_stream *stream, int fd)
{
    struct stream_in *in = (struct stream_in *)stream;

    ALOGD("in->Device     : 0x%x", in->device);
    ALOGD("in->SampleRate : %d", in->config->rate);
    ALOGD("in->Channels   : %d", in->config->channels);
    ALOGD("in->Formate    : %d", in->config->format);
    ALOGD("in->PreiodSize : %d", in->config->period_size);

    return 0;
}

/**
 * @brief in_set_parameters
 *
 * @param stream
 * @param kvpairs
 *
 * @returns
 */
static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    int status = 0;
    unsigned int val;
    bool apply_now = false;

    ALOGV("%s: kvpairs = %s", __func__, kvpairs);
    parms = str_parms_create_str(kvpairs);

    //set channel_mask
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_CHANNELS,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        in->channel_mask = val;
    }
     // set sample rate
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_SAMPLING_RATE,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        in->requested_rate = val;
    }

    pthread_mutex_lock(&in->lock);
    pthread_mutex_lock(&adev->lock);
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        /* no audio source uses val == 0 */
        if ((in->input_source != val) && (val != 0)) {
            in->input_source = val;
            apply_now = !in->standby;
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    if (ret >= 0) {
        /* strip AUDIO_DEVICE_BIT_IN to allow bitwise comparisons */
        val = atoi(value) & ~AUDIO_DEVICE_BIT_IN;
        /* no audio device uses val == 0 */
        if ((in->device != val) && (val != 0)) {
            /* force output standby to start or stop SCO pcm stream if needed */
            if ((val & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) ^
                    (in->device & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET)) {
                do_in_standby(in);
            }
            in->device = val;
            apply_now = !in->standby;
        }
    }

    if (apply_now) {
        adev->input_source = in->input_source;
        adev->in_device = in->device;
    route_pcm_open(getRouteFromDevice(in->dev, in->device | AUDIO_DEVICE_BIT_IN));
    }

    pthread_mutex_unlock(&adev->lock);
    pthread_mutex_unlock(&in->lock);

    str_parms_destroy(parms);

    ALOGV("%s: exit: status(%d)", __func__, status);
    return status;

}

/**
 * @brief in_get_parameters
 *
 * @param stream
 * @param keys
 *
 * @returns
 */
static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    ALOGD("%s: keys = %s", __func__, keys);

    struct stream_in *in = (struct stream_in *)stream;
    struct str_parms *query = str_parms_create_str(keys);
    char *str = NULL;
    struct str_parms *reply = str_parms_create();

    if (stream_get_parameter_formats(stream,query,reply) == 0) {
        str = str_parms_to_str(reply);
    } else if (stream_get_parameter_channels(query, reply, &in->supported_channel_masks[0]) == 0) {
        str = str_parms_to_str(reply);
    } else if (stream_get_parameter_rates(query, reply, &in->supported_sample_rates[0]) == 0) {
        str = str_parms_to_str(reply);
    } else {
        ALOGD("%s,str_parms_get_str failed !",__func__);
        str = strdup("");
    }

    str_parms_destroy(query);
    str_parms_destroy(reply);

    ALOGV("%s,exit -- str = %s",__func__,str);
    return str;
}

/**
 * @brief in_set_gain
 *
 * @param stream
 * @param gain
 *
 * @returns
 */
static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

/**
 * @brief in_apply_ramp
 *
 * @param in
 * @param buffer
 * @param frames
 */
static void in_apply_ramp(struct stream_in *in, int16_t *buffer, size_t frames)
{
    size_t i;
    uint16_t vol = in->ramp_vol;
    uint16_t step = in->ramp_step;

    frames = (frames < in->ramp_frames) ? frames : in->ramp_frames;

    if (in->channel_mask == AUDIO_CHANNEL_IN_MONO)
        for (i = 0; i < frames; i++) {
            buffer[i] = (int16_t)((buffer[i] * vol) >> 16);
            vol += step;
        }
    else
        for (i = 0; i < frames; i++) {
            buffer[2*i] = (int16_t)((buffer[2*i] * vol) >> 16);
            buffer[2*i + 1] = (int16_t)((buffer[2*i + 1] * vol) >> 16);
            vol += step;
        }


    in->ramp_vol = vol;
    in->ramp_frames -= frames;
}

/**
 * @brief in_read
 *
 * @param stream
 * @param buffer
 * @param bytes
 *
 * @returns
 */
static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_in_frame_size(stream);

    if (in->device & AUDIO_DEVICE_IN_HDMI) {
        unsigned int rate = get_hdmiin_audio_rate(adev);
        if(rate != in->config->rate){
            ALOGD("HDMI-In: rate is changed: %d -> %d, restart input stream",
                    in->config->rate, rate);
            do_in_standby(in);
        }
    }
    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the input stream mutex - e.g.
     * executing in_set_parameters() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
        pthread_mutex_lock(&adev->lock);
        ret = start_input_stream(in);
        pthread_mutex_unlock(&adev->lock);
        if (ret < 0)
            goto exit;
        in->standby = false;
#ifdef AUDIO_3A
        if (adev->voice_api != NULL) {
            adev->voice_api->start();
        }
#endif
    }

    /*if (in->num_preprocessors != 0)
        ret = process_frames(in, buffer, frames_rq);
      else */
    //ALOGV("%s:frames_rq:%d",__FUNCTION__,frames_rq);
    ret = read_frames(in, buffer, frames_rq);
    if (ret > 0)
        ret = 0;

    if (adev->simcom_voice_active && in->simcom_voice_capture && bytes > 0) {
        uint32_t channels = audio_channel_count_from_in_mask(in->channel_mask);
        if (channels == 0 && in->config) {
            channels = in->config->channels;
        }
        uint32_t in_rate = in->requested_rate ? in->requested_rate :
                           (in->config ? in->config->rate : 8000);
        size_t frames = 0;
        if (channels != 0) {
            frames = bytes / (sizeof(int16_t) * channels);
        }

        simcom_voice_process_and_push(adev,
                                      (const int16_t *)buffer,
                                      frames,
                                      channels,
                                      in_rate,
                                      &in->simcom_mono_buf,
                                      &in->simcom_mono_capacity,
                                      &in->simcom_downsample_buf,
                                      &in->simcom_downsample_capacity,
                                      &in->simcom_resample_pos,
                                      &in->simcom_last_rate,
                                      &in->simcom_last_channels);
    }

    dump_in_data(buffer, bytes);

#ifdef AUDIO_3A
    do {
        if (adev->voice_api != NULL) {
            int ret  = 0;
            ret = adev->voice_api->quueCaputureBuffer(buffer, bytes);
            if (ret < 0) break;
            ret = adev->voice_api->getCapureBuffer(buffer, bytes);
            if (ret < 0) memset(buffer, 0x00, bytes);
        }
    } while (0);
#endif

    //if (in->ramp_frames > 0)
    //    in_apply_ramp(in, buffer, frames_rq);

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    //if (ret == 0 && adev->mic_mute)
    //    memset(buffer, 0, bytes);

    if (in->device & AUDIO_DEVICE_IN_HDMI) {
        goto exit;
    }

#ifdef SPEEX_DENOISE_ENABLE
    if(!adev->mic_mute && ret== 0) {
        int index = 0;
        int startPos = 0;
        spx_int16_t* data = (spx_int16_t*) buffer;

        int channel_count = audio_channel_count_from_out_mask(in->channel_mask);
        int curFrameSize = bytes/(channel_count*sizeof(int16_t));
        long ch;
        ALOGV("channel_count:%d",channel_count);
        if(curFrameSize != in->mSpeexFrameSize)
            ALOGD("the current request have some error mSpeexFrameSize %d bytes %d ",in->mSpeexFrameSize, bytes);

        while(curFrameSize >= startPos+in->mSpeexFrameSize) {
            if( 2 == channel_count) {
                for(index = startPos; index< startPos +in->mSpeexFrameSize ; index++ )
                    in->mSpeexPcmIn[index-startPos] = data[index*channel_count]/2 + data[index*channel_count+1]/2;
            } else {
                for(index = startPos; index< startPos +in->mSpeexFrameSize ; index++ )
                    in->mSpeexPcmIn[index-startPos] = data[index*channel_count];
            }
            speex_preprocess_run(in->mSpeexState,in->mSpeexPcmIn);
#ifndef TARGET_RK2928
            for(ch = 0 ; ch < channel_count; ch++)
                for(index = startPos; index< startPos + in->mSpeexFrameSize ; index++ ) {
                    data[index*channel_count+ch] = in->mSpeexPcmIn[index-startPos];
                }
#else
            for(index = startPos; index< startPos + in->mSpeexFrameSize ; index++ ) {
                int tmp = (int)in->mSpeexPcmIn[index-startPos]+ in->mSpeexPcmIn[index-startPos]/2;
                data[index*channel_count+0] = tmp > 32767 ? 32767 : (tmp < -32768 ? -32768 : tmp);
            }
            for(int ch = 1 ; ch < channel_count; ch++)
                for(index = startPos; index< startPos + in->mSpeexFrameSize ; index++ ) {
                    data[index*channel_count+ch] = data[index*channel_count+0];
                }
#endif
            startPos += in->mSpeexFrameSize;
        }
    }
#endif

#ifdef ALSA_IN_DEBUG
        fwrite(buffer, bytes, 1, in_debug);
#endif
exit:
    if (ret < 0) {
        usleep(bytes * 1000000 / audio_stream_in_frame_size(stream) /
               in_get_sample_rate(&stream->common));
        do_in_standby(in);
    }

    pthread_mutex_unlock(&in->lock);
    return bytes;
}

/**
 * @brief in_get_input_frames_lost
 *
 * @param stream
 *
 * @returns
 */
static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

/**
 * @brief in_add_audio_effect
 *
 * @param stream
 * @param effect
 *
 * @returns
 */
static int in_add_audio_effect(const struct audio_stream *stream,
                               effect_handle_t effect)
{
    struct stream_in *in = (struct stream_in *)stream;
    effect_descriptor_t descr;
    if ((*effect)->get_descriptor(effect, &descr) == 0) {

        pthread_mutex_lock(&in->lock);
        pthread_mutex_lock(&in->dev->lock);


        pthread_mutex_unlock(&in->dev->lock);
        pthread_mutex_unlock(&in->lock);
    }

    return 0;
}

/**
 * @brief in_remove_audio_effect
 *
 * @param stream
 * @param effect
 *
 * @returns
 */
static int in_remove_audio_effect(const struct audio_stream *stream,
                                  effect_handle_t effect)
{
    struct stream_in *in = (struct stream_in *)stream;
    effect_descriptor_t descr;
    if ((*effect)->get_descriptor(effect, &descr) == 0) {

        pthread_mutex_lock(&in->lock);
        pthread_mutex_lock(&in->dev->lock);


        pthread_mutex_unlock(&in->dev->lock);
        pthread_mutex_unlock(&in->lock);
    }

    return 0;
}

static int adev_get_microphones(const struct audio_hw_device *dev,
                         struct audio_microphone_characteristic_t *mic_array,
                         size_t *mic_count)
{
    struct audio_device *adev = (struct audio_device *)dev;
    size_t actual_mic_count = 0;

    int card_no = 0;

    char snd_card_node_id[100]={0};
    char snd_card_node_cap[100]={0};
    char address[32] = "bottom";

    do{
        sprintf(snd_card_node_id, "/proc/asound/card%d/id", card_no);
        if (access(snd_card_node_id,F_OK) == -1) break;

        sprintf(snd_card_node_cap, "/proc/asound/card%d/pcm0c/info", card_no);
        if (access(snd_card_node_cap,F_OK) == -1) continue;

        actual_mic_count++;
    }while(++card_no);

    mic_array->device = -2147483644;
    strcpy(mic_array->address,address);

    ALOGD("%s,get capture mic actual_mic_count =%d",__func__,actual_mic_count);
    *mic_count = actual_mic_count;
    return 0;
}

static int in_get_active_microphones(const struct audio_stream_in *stream,
                         struct audio_microphone_characteristic_t *mic_array,
                         size_t *mic_count)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    pthread_mutex_lock(&in->lock);
    pthread_mutex_lock(&adev->lock);

    size_t actual_mic_count = 0;
    int card_no = 0;

    char snd_card_node_id[100]={0};
    char snd_card_node_cap[100]={0};
    char snd_card_info[100]={0};
    char snd_card_state[255]={0};

    do{
        sprintf(snd_card_node_id, "/proc/asound/card%d/id", card_no);
        if (access(snd_card_node_id,F_OK) == -1) break;

        sprintf(snd_card_node_cap, "/proc/asound/card%d/pcm0c/info", card_no);
        if (access(snd_card_node_cap,F_OK) == -1) {
            continue;
        } else {
            sprintf(snd_card_info, "/proc/asound/card%d/pcm0c/sub0/status", card_no);
            int fd;
            fd = open(snd_card_info, O_RDONLY);
            if (fd < 0) {
                ALOGE("%s,failed to open node: %s", __func__, snd_card_info);
            } else {
                int length = read(fd, snd_card_state, sizeof(snd_card_state) -1);
                snd_card_state[length] = 0;
                if (strcmp(snd_card_state, "closed") != 0) actual_mic_count++;
            }
            close(fd);
        }
    } while(++card_no);

    pthread_mutex_unlock(&adev->lock);
    pthread_mutex_unlock(&in->lock);

    ALOGD("%s,get active mic actual_mic_count =%d", __func__, actual_mic_count);
    *mic_count = actual_mic_count;
    return 0;
}

/*
 * get support channels mask of hdmi from parsing edid of hdmi
 */
static void get_hdmi_support_channels_masks(struct stream_out *out)
{
    if(out == NULL)
        return ;

    int channels = get_hdmi_audio_speaker_allocation(&out->hdmi_audio);
    switch (channels) {
    case AUDIO_CHANNEL_OUT_5POINT1:
        ALOGD("%s: HDMI Support 5.1 channels pcm",__FUNCTION__);
        out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_5POINT1;
        out->supported_channel_masks[1] = AUDIO_CHANNEL_OUT_STEREO;
        break;
    case AUDIO_CHANNEL_OUT_7POINT1:
        ALOGD("%s: HDMI Support 7.1 channels pcm",__FUNCTION__);
        out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_5POINT1;
        out->supported_channel_masks[1] = AUDIO_CHANNEL_OUT_7POINT1;
        break;
    case AUDIO_CHANNEL_OUT_STEREO:
    default:
        ALOGD("%s: HDMI Support 2 channels pcm",__FUNCTION__);
        out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_STEREO;
        out->supported_channel_masks[1] = AUDIO_CHANNEL_OUT_MONO;
        break;
    }
}

/**
 * @brief adev_open_output_stream
 *
 * @param dev
 * @param handle
 * @param devices
 * @param flags
 * @param config
 * @param stream_out
 * @param __unused
 *
 * @returns
 */
static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address __unused)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int ret;
    enum output_type type = OUTPUT_LOW_LATENCY;
    bool isPcm = audio_is_linear_pcm(config->format);

    ALOGD("audio hal adev_open_output_stream devices = 0x%x, flags = %d, samplerate = %d,format = 0x%x",
          devices, flags, config->sample_rate,config->format);
    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;

    out->usecase = USECASE_PRIMARY_PLAYBACK;
    out->usecase_type = USECASE_TYPE_PCM_PLAYBACK;
    /*get default supported channel_mask*/
    memset(out->supported_channel_masks, 0, sizeof(out->supported_channel_masks));
    out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_STEREO;
    out->supported_channel_masks[1] = AUDIO_CHANNEL_OUT_MONO;
    /*get default supported sample_rate*/
    memset(out->supported_sample_rates, 0, sizeof(out->supported_sample_rates));
    out->supported_sample_rates[0] = 44100;
    out->supported_sample_rates[1] = 48000;

    if(config != NULL)
        memcpy(&(out->aud_config),config,sizeof(struct audio_config));
    out->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    if (devices == AUDIO_DEVICE_NONE)
        devices = AUDIO_DEVICE_OUT_SPEAKER;
    out->device = devices;
    /*
     * set output_direct_mode to LPCM, means data is not multi pcm or bitstream datas.
     * set output_direct to false, means data is 2 channels pcm
     */
    out->output_direct_mode = LPCM;
    out->output_direct = false;
    out->snd_reopen = false;
    out->channel_buffer = NULL;
    out->bitstream_buffer = NULL;

    init_hdmi_audio(&out->hdmi_audio);
    if(devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        parse_hdmi_audio(&out->hdmi_audio);
        get_hdmi_support_channels_masks(out);
    }

    if (flags & AUDIO_OUTPUT_FLAG_DIRECT) {
        if (devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
            if (config->format == AUDIO_FORMAT_IEC61937) {
                ALOGD("%s:out = %p HDMI Bitstream",__FUNCTION__,out);
                out->channel_mask = config->channel_mask;
                if (isValidSamplerate(config->sample_rate)) {
                    out->config = pcm_config_direct;
                    out->config.rate = config->sample_rate;
                    out->output_direct = true;
                    int channel = audio_channel_count_from_out_mask(config->channel_mask);

                    if (channel == 8 && config->sample_rate == 192000) {
                        out->output_direct_mode = HBR;
                    } else {
                        out->output_direct_mode = NLPCM;
                    }

                    if (channel == 8) {
                        out->config = pcm_config_direct;
                        out->config.rate = config->sample_rate;
                    } else if (config->sample_rate >= 176400) {
                        out->config.period_size = 1024 * 4;
                    } else {
                        out->config.period_size = 2048;
                    }
                    type = OUTPUT_HDMI_MULTI;
                } else {
                    out->config = pcm_config;
                    out->config.rate = 44100;
                    ALOGE("hdmi bitstream samplerate %d unsupport", config->sample_rate);
                }
                out->config.channels = audio_channel_count_from_out_mask(config->channel_mask);
                if (out->config.channels < 2)
                    out->config.channels = 2;
                out->pcm_device = PCM_DEVICE;
                out->device = AUDIO_DEVICE_OUT_AUX_DIGITAL;
            } else if (isPcm){ // multi pcm
                if (config->sample_rate == 0)
                    config->sample_rate = HDMI_MULTI_DEFAULT_SAMPLING_RATE;
                if (config->channel_mask == 0)
                    config->channel_mask = AUDIO_CHANNEL_OUT_5POINT1;

                int layout = get_hdmi_audio_speaker_allocation(&out->hdmi_audio);
                unsigned int mask = (layout&config->channel_mask);
                ALOGD("%s:out = %p HDMI multi pcm: layout = 0x%x,mask = 0x%x",
                    __FUNCTION__,out,layout,mask);
                // current hdmi allocation(speaker) only support MONO or STEREO
                if(mask <= (int)AUDIO_CHANNEL_OUT_STEREO) {
                    ALOGD("%s:out = %p input stream is multi pcm,channle mask = 0x%x,but hdmi not support,mixer it to stereo output",
                        __FUNCTION__,out,config->channel_mask);
                    out->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
                    out->config = pcm_config;
                    out->pcm_device = PCM_DEVICE;
                    type = OUTPUT_LOW_LATENCY;
                    out->device = AUDIO_DEVICE_OUT_AUX_DIGITAL;
                    out->output_direct = false;
                } else {
                    /*
                     * maybe input audio stream is 7.1 channels,
                     * but hdmi only support 5.1, we also output 7.1 for default.
                     * Is better than output 2 channels after mixer?
                     * If customer like output 2 channles data after mixer,
                     * modify codes here
                     */
                    out->channel_mask = config->channel_mask;
                    out->config = pcm_config_hdmi_multi;
                    out->config.rate = config->sample_rate;
                    out->config.channels = audio_channel_count_from_out_mask(config->channel_mask);
                    out->pcm_device = PCM_DEVICE;
                    type = OUTPUT_HDMI_MULTI;
                    out->device = AUDIO_DEVICE_OUT_AUX_DIGITAL;
                    out->output_direct = true;
                }
            } else {
                ALOGD("Not any bitstream mode!");
            }
        } else if ((devices & AUDIO_DEVICE_OUT_SPDIF)
                      && (config->format == AUDIO_FORMAT_IEC61937)) {
            ALOGD("%s:out = %p Spdif Bitstream",__FUNCTION__,out);
            out->channel_mask = config->channel_mask;
            out->config = pcm_config_direct;
            if ((config->sample_rate == 48000) ||
                    (config->sample_rate == 32000) ||
                    (config->sample_rate == 44100)) {
                out->config.rate = config->sample_rate;
                out->config.format = PCM_FORMAT_S16_LE;
                out->config.period_size = 2048;
            } else {
                out->config.rate = 44100;
                ALOGE("spdif passthrough samplerate %d is unsupport",config->sample_rate);
            }
            out->config.channels = audio_channel_count_from_out_mask(config->channel_mask);
            devices = AUDIO_DEVICE_OUT_SPDIF;
            out->pcm_device = PCM_DEVICE;
            out->output_direct = true;
            type = OUTPUT_HDMI_MULTI;
            out->device = AUDIO_DEVICE_OUT_SPDIF;
            out->output_direct_mode = NLPCM;
        } else {
            out->config = pcm_config;
            out->pcm_device = PCM_DEVICE;
            type = OUTPUT_LOW_LATENCY;
        }
    } else if (flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) {
        out->config = pcm_config_deep;
        out->pcm_device = PCM_DEVICE_DEEP;
        type = OUTPUT_DEEP_BUF;
    } else {
        out->config = pcm_config;
        out->pcm_device = PCM_DEVICE;
        type = OUTPUT_LOW_LATENCY;
    }

    /*
      * the ip of hdmi need convert 16 bits to 21 bits(except rk3128's ip) if bitstream over hdmi
      */
    if (is_bitstream(out) && (devices & AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
        /*
         * the ip of hdmi need convert 16 bits to 21 bits(except rk3128's ip)
         */
        out->config.format = PCM_FORMAT_S24_LE;
#ifdef RK3128
        out->config.format = PCM_FORMAT_S16_LE;
#endif
        if(out->config.format == PCM_FORMAT_S24_LE){
            out->channel_buffer = malloc(CHASTA_SUB_NUM);
            initchnsta(out->channel_buffer);
            setChanSta(out->channel_buffer,out->config.rate, out->config.channels);
        }
    } else {
        out->config.format = PCM_FORMAT_S16_LE;
    }

    ALOGD("out->config.rate = %d, out->config.channels = %d out->config.format = %d",
          out->config.rate, out->config.channels, out->config.format);

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->stream.get_presentation_position = out_get_presentation_position;

    out->dev = adev;

    out->standby = true;
    out->nframes = 0;

    pthread_mutex_lock(&adev->lock_outputs);
    if (adev->outputs[type]) {
        pthread_mutex_unlock(&adev->lock_outputs);
        ret = -EBUSY;
        goto err_open;
    }
    adev->outputs[type] = out;
    pthread_mutex_unlock(&adev->lock_outputs);

    *stream_out = &out->stream;

    return 0;

err_open:
    if (out != NULL) {
        destory_hdmi_audio(&out->hdmi_audio);
        free(out);
    }
    *stream_out = NULL;
    return ret;
}

/**
 * @brief adev_close_output_stream
 *
 * @param dev
 * @param stream
 */
static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct audio_device *adev;
    enum output_type type;

    ALOGD("adev_close_output_stream!");
    out_standby(&stream->common);
    adev = (struct audio_device *)dev;
    pthread_mutex_lock(&adev->lock_outputs);
    for (type = 0; type < OUTPUT_TOTAL; ++type) {
        if (adev->outputs[type] == (struct stream_out *) stream) {
            adev->outputs[type] = NULL;
            break;
        }
    }
    {
        struct stream_out *out = (struct stream_out *)stream;
        if (out->bitstream_buffer != NULL) {
            free(out->bitstream_buffer);
            out->bitstream_buffer = NULL;
        }

        if (out->channel_buffer != NULL) {
            free(out->channel_buffer);
            out->channel_buffer = NULL;
        }

        destory_hdmi_audio(&out->hdmi_audio);
    }
    pthread_mutex_unlock(&adev->lock_outputs);
    free(stream);
}

/**
 * @brief adev_set_parameters
 *
 * @param dev
 * @param kvpairs
 *
 * @returns
 */
static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *parms = NULL;
    char value[32] = "";
    /*
     * ret is the result of str_parms_get_str,
     * if no paramter which str_parms_get_str to get, it will return result < 0 always.
     * For example: kvpairs = connect=1024 is coming
     *              str_parms_get_str(parms, AUDIO_PARAMETER_KEY_SCREEN_STATE,value, sizeof(value))
     *              will return result < 0,this means no screen_state in parms
     */
    int ret = 0;

    /*
     * status is the result of one process,
     * For example: kvpairs = screen_state=on is coming,
     *              str_parms_get_str(parms, AUDIO_PARAMETER_KEY_SCREEN_STATE,value, sizeof(value))
     *              will return result >= 0,this means screen is on, we can do something,
     *              if the things we do is correct, we set status = 0, or status < 0 means fail.
     */
    int status = 0;

    ALOGD("%s: kvpairs = %s", __func__, kvpairs);
    parms = str_parms_create_str(kvpairs);
    pthread_mutex_lock(&adev->lock);

    // screen state off/on
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_SCREEN_STATE, // screen_state
                            value, sizeof(value));
    if (ret >= 0) {
        if(strcmp(value,"on") == 0){
            adev->screenOff = false;
        } else if(strcmp(value,"off") == 0){
            adev->screenOff = true;
        }
    }

#ifdef AUDIO_BITSTREAM_REOPEN_HDMI
	if (!hasExtCodec()){
    // hdmi reconnect
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_DEVICE_CONNECT, // hdmi reconnect
                            value, sizeof(value));
    if (ret >= 0) {
        int device = atoi(value);
        if(device == (int)AUDIO_DEVICE_OUT_AUX_DIGITAL){
            struct stream_out *out = adev->outputs[OUTPUT_HDMI_MULTI];
            if((out != NULL) && is_bitstream(out) && (out->device == AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
                ALOGD("%s: hdmi connect when audio stream is output over hdmi, do something,out = %p",__FUNCTION__,out);
                out->snd_reopen = true;
            }
	        }
        }
    }
#endif

    pthread_mutex_unlock(&adev->lock);
    str_parms_destroy(parms);
    return status;
}

/*
 * get support formats for bitstream
 * There is no stand interface in andorid to get the formats can be bistream,
 * so we extend get parameter to report formats
 */
static int get_support_bitstream_formats(struct str_parms *query,
                                    struct str_parms *reply)
{
    int avail = 1024;
    char value[avail];

    struct hdmi_audio_infors hdmi_edid;
    init_hdmi_audio(&hdmi_edid);
    const char* AUDIO_PARAMETER_STREAM_SUP_BITSTREAM_FORMAT = "sup_bitstream_formats";
    if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_BITSTREAM_FORMAT)) {
        memset(value,0,avail);

        // get the format can be bistream?
        if(parse_hdmi_audio(&hdmi_edid) >= 0){
            int cursor = 0;
            for(int i = 0; i < ARRAY_SIZE(sSurroundFormat); i++){
                if(is_support_format(&hdmi_edid,sSurroundFormat[i].format)){
                    avail -= cursor;
                    int length = snprintf(value + cursor, avail, "%s%s",
                                   cursor > 0 ? "|" : "",
                                   sSurroundFormat[i].value);
                    if (length < 0 || length >= avail) {
                        break;
                    }
                    cursor += length;
                }
            }
        }

        destory_hdmi_audio(&hdmi_edid);
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_BITSTREAM_FORMAT, value);
        return 0;
    }

    return -1;
}

/**
 * @brief adev_get_parameters
 *
 * @param dev
 * @param keys
 *
 * @returns
 */
static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *parms = str_parms_create_str(keys);
    struct str_parms *reply = str_parms_create();
    char *str = NULL;
    ALOGD("%s: keys = %s",__FUNCTION__,keys);
    if (str_parms_has_key(parms, "ec_supported")) {
        str_parms_destroy(parms);
        parms = str_parms_create_str("ec_supported=yes");
        str = str_parms_to_str(parms);
    } else if (get_support_bitstream_formats(parms,reply) == 0) {
        str = str_parms_to_str(reply);
    } else {
        str = strdup("");
    }

    str_parms_destroy(parms);
    str_parms_destroy(reply);

    return str;
}

/**
 * @brief adev_init_check
 *
 * @param dev
 *
 * @returns
 */
static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

/**
 * @brief adev_set_voice_volume
 *
 * @param dev
 * @param volume
 *
 * @returns
 */
static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    int ret = 0;
    struct audio_device *adev = (struct audio_device *)dev;
    if(adev->mode == AUDIO_MODE_IN_CALL) {
        if (volume < 0.0) {
            volume = 0.0;
        } else if (volume > 1.0) {
            volume = 1.0;
        }

        const char *mixer_ctl_name = "Speaker Playback Volume";
        ret = route_set_voice_volume(mixer_ctl_name,volume);
    }

    return ret;
}

/**
 * @brief adev_set_master_volume
 *
 * @param dev
 * @param volume
 *
 * @returns
 */
static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

/**
 * @brief adev_set_mode
 *
 * @param dev
 * @param mode
 *
 * @returns
 */
// Helper function to send AT command to modem

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    struct audio_device *adev = (struct audio_device *)dev;
    ALOGD("%s: set_mode = %d", __func__, mode);

    bool target_call = (mode == AUDIO_MODE_IN_CALL || mode == AUDIO_MODE_IN_COMMUNICATION);
    bool current_call = (adev->mode == AUDIO_MODE_IN_CALL || adev->mode == AUDIO_MODE_IN_COMMUNICATION);

    ALOGE("adev_set_mode: mode=%d, current=%d, call_active=%d", mode, adev->mode, adev->simcom_voice_active);

    if (target_call && !current_call) {
        ALOGE("VOICE CALL: BEGIN (transitioning from mode=%d to mode=%d)", adev->mode, mode);
        if (!simcom_update_cpcmreg(adev, true)) {
            ALOGE("adev_set_mode: failed to send AT+CPCMREG=1 before call start");
        }
        usleep(200000);
        ALOGE("adev_set_mode: SIMCOM activation delay complete");
        simcom_voice_start_usecase(adev);
    } else if (!target_call && current_call) {
        if (adev->simcom_voice_active || adev->simcom_voice_thread_started ||
            adev->simcom_mic_route_active || adev->simcom_voice_pcm) {
            ALOGW("adev_set_mode: ignoring MODE_IN_CALL -> NORMAL drop while SIMCOM pipeline active "
                  "(voice_active=%d thread=%d route=%d pcm=%p)",
                  adev->simcom_voice_active, adev->simcom_voice_thread_started,
                  adev->simcom_mic_route_active, adev->simcom_voice_pcm);
            return 0;
        }
        ALOGE("VOICE CALL: END (transitioning from mode=%d to mode=%d)", adev->mode, mode);
        if (!simcom_update_cpcmreg(adev, false)) {
            ALOGE("adev_set_mode: failed to send AT+CPCMREG=0 on call end");
        }
        simcom_voice_stop_usecase(adev);
    }

    adev->mode = mode;

    return 0;
}

/**
 * @brief adev_set_mic_mute
 *
 * @param dev
 * @param state
 *
 * @returns
 */
static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    adev->mic_mute = state;

    return 0;
}

/**
 * @brief adev_get_mic_mute
 *
 * @param dev
 * @param state
 *
 * @returns
 */
static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

/**
 * @brief adev_get_input_buffer_size
 *
 * @param dev
 * @param config
 *
 * @returns
 */
static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
        const struct audio_config *config)
{

    return get_input_buffer_size(config->sample_rate, config->format,
                                 audio_channel_count_from_in_mask(config->channel_mask),
                                 false /* is_low_latency: since we don't know, be conservative */);
}

/**
 * @brief adev_open_input_stream
 *
 * @param dev
 * @param handle
 * @param devices
 * @param config
 * @param stream_in
 * @param flags
 * @param __unused
 * @param __unused
 *
 * @returns
 */
static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags,
                                  const char *address __unused,
                                  audio_source_t source __unused)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in;
    int ret;

    ALOGD("audio hal adev_open_input_stream devices = 0x%x, flags = %d, config->samplerate = %d,config->channel_mask = %x",
           devices, flags, config->sample_rate,config->channel_mask);

    *stream_in = NULL;
#ifdef ALSA_IN_DEBUG
    in_debug = fopen("/data/debug.pcm","wb");//please touch /data/debug.pcm first
#endif
    /* Respond with a request for mono if a different format is given. */
    //ALOGV("%s:config->channel_mask %d",__FUNCTION__,config->channel_mask);
    if (/*config->channel_mask != AUDIO_CHANNEL_IN_MONO &&
            config->channel_mask != AUDIO_CHANNEL_IN_FRONT_BACK*/
        config->channel_mask != AUDIO_CHANNEL_IN_STEREO) {
        config->channel_mask = AUDIO_CHANNEL_IN_STEREO;
        ALOGE("%s:channel is not support",__FUNCTION__);
        return -EINVAL;
    }

    in = (struct stream_in *)calloc(1, sizeof(struct stream_in));
    if (!in)
        return -ENOMEM;

    in->simcom_input = false;
    /*get default supported channel_mask*/
    memset(in->supported_channel_masks, 0, sizeof(in->supported_channel_masks));
    in->supported_channel_masks[0] = AUDIO_CHANNEL_IN_STEREO;
    in->supported_channel_masks[1] = AUDIO_CHANNEL_IN_MONO;
    /*get default supported sample_rate*/
    memset(in->supported_sample_rates, 0, sizeof(in->supported_sample_rates));
    in->supported_sample_rates[0] = 44100;
    in->supported_sample_rates[1] = 48000;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;
    in->stream.get_active_microphones = in_get_active_microphones;

    in->dev = adev;
    in->standby = true;
    in->requested_rate = config->sample_rate;
    in->input_source = AUDIO_SOURCE_DEFAULT;
    /* strip AUDIO_DEVICE_BIT_IN to allow bitwise comparisons */
    in->device = devices & ~AUDIO_DEVICE_BIT_IN;
    in->io_handle = handle;
    in->channel_mask = config->channel_mask;
    if (in->device & AUDIO_DEVICE_IN_HDMI) {
        ALOGD("HDMI-In: use low latency");
        flags |= AUDIO_INPUT_FLAG_FAST;
    }
    in->flags = flags;
    struct pcm_config *pcm_config = flags & AUDIO_INPUT_FLAG_FAST ?
                                            &pcm_config_in_low_latency : &pcm_config_in;
#ifdef BT_AP_SCO
    if (/*adev->mode == AUDIO_MODE_IN_COMMUNICATION && */in->device & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
        pcm_config = &pcm_config_in_bt;
    }
#endif
    // Check for SIMCOM telephony input - use SIMCOM config if device is TELEPHONY_RX
    if (in->device & AUDIO_DEVICE_IN_TELEPHONY_RX) {
        ALOGE("adev_open_input_stream: TELEPHONY_RX device detected, using SIMCOM config");
        pcm_config = &pcm_config_in_simcom;
    }

    in->config = pcm_config;

    in->buffer = malloc(pcm_config->period_size * pcm_config->channels
                        * audio_stream_in_frame_size(&in->stream));
#ifdef SPEEX_DENOISE_ENABLE
    in->mSpeexState = NULL;
    in->mSpeexFrameSize = 0;
    in->mSpeexPcmIn = NULL;
#endif

    if (!in->buffer) {
        ret = -ENOMEM;
        goto err_malloc;
    }

    if (in->requested_rate != pcm_config->rate) {
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;

        ALOGD("pcm_config->rate:%d,in->requested_rate:%d,in->channel_mask:%d",
              pcm_config->rate,in->requested_rate,audio_channel_count_from_in_mask(in->channel_mask));
        ret = create_resampler(pcm_config->rate,
                               in->requested_rate,
                               audio_channel_count_from_in_mask(in->channel_mask),
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
        if (ret != 0) {
            ret = -EINVAL;
            goto err_resampler;
        }
    }

    if (in->device&AUDIO_DEVICE_IN_HDMI) {
        goto out;
    }

#ifdef AUDIO_3A
    ALOGD("voice process has opened, try to create voice process!");
    adev->voice_api = rk_voiceprocess_create(DEFAULT_PLAYBACK_SAMPLERATE,
                                             DEFAULT_PLAYBACK_CHANNELS,
                                             in->requested_rate,
                                             audio_channel_count_from_in_mask(in->channel_mask));
    if (adev->voice_api == NULL) {
        ALOGE("crate voice process failed!");
    }
#endif

#ifdef SPEEX_DENOISE_ENABLE
    uint32_t size;
    int denoise = 1;
    int noiseSuppress = -24;
    int channel_count = audio_channel_count_from_out_mask(config->channel_mask);

    size = in_get_buffer_size(&in->stream.common);
    in->mSpeexFrameSize = size/(channel_count * sizeof(int16_t));
    ALOGD("in->mSpeexFrameSize:%d in->requested_rate:%d",in->mSpeexFrameSize, in->requested_rate);
    in->mSpeexPcmIn = malloc(sizeof(int16_t)*in->mSpeexFrameSize);
    if(!in->mSpeexPcmIn) {
        ALOGE("speexPcmIn malloc failed");
        goto err_speex_malloc;
    }
    in->mSpeexState = speex_preprocess_state_init(in->mSpeexFrameSize, in->requested_rate);
    if(in->mSpeexState == NULL) {
        ALOGE("speex error");
        goto err_speex_malloc;
    }

    speex_preprocess_ctl(in->mSpeexState, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
    speex_preprocess_ctl(in->mSpeexState, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noiseSuppress);

#endif

out:
    *stream_in = &in->stream;
    return 0;

err_speex_malloc:
#ifdef SPEEX_DENOISE_ENABLE
    free(in->mSpeexPcmIn);
#endif
err_resampler:
    free(in->buffer);
err_malloc:
    free(in);
    return ret;
}

/**
 * @brief adev_close_input_stream
 *
 * @param dev
 * @param stream
 */
static void adev_close_input_stream(struct audio_hw_device *dev,
                                    struct audio_stream_in *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = (struct audio_device *)dev;

    ALOGD("%s",__FUNCTION__);

    in_standby(&stream->common);
    if (in->resampler) {
        release_resampler(in->resampler);
        in->resampler = NULL;
    }

#ifdef ALSA_IN_DEBUG
    fclose(in_debug);
#endif
#ifdef AUDIO_3A
    if (adev->voice_api != NULL) {
        rk_voiceprocess_destory();
        adev->voice_api = NULL;
    }
#endif

#ifdef SPEEX_DENOISE_ENABLE
    if (in->mSpeexState) {
        speex_preprocess_state_destroy(in->mSpeexState);
    }
    if(in->mSpeexPcmIn) {
        free(in->mSpeexPcmIn);
    }
#endif
    free(in->simcom_mono_buf);
    free(in->simcom_downsample_buf);
    free(in->buffer);
    free(stream);
}

/**
 * @brief adev_dump
 *
 * @param device
 * @param fd
 *
 * @returns
 */
static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

/**
 * @brief adev_close
 *
 * @param device
 *
 * @returns
 */
static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    //audio_route_free(adev->ar);

    simcom_voice_stop_usecase(adev);

    struct listnode *node, *tmp;
    list_for_each_safe(node, tmp, &adev->usecase_list) {
        struct audio_usecase *usecase =
                node_to_item(node, struct audio_usecase, list);
        list_remove(node);
        free(usecase);
    }

    pthread_cond_destroy(&adev->simcom_mic_cond);
    pthread_mutex_destroy(&adev->simcom_mic_lock);
    free(adev->simcom_mic_ring);
    adev->simcom_mic_ring = NULL;
    adev->simcom_mic_ring_size = 0;
    adev->simcom_mic_ring_read = 0;
    adev->simcom_mic_ring_write = 0;
    adev->simcom_mic_ring_full = false;
    adev->simcom_voice_pcm = NULL;
    adev->simcom_voice_thread_started = false;
    adev->simcom_voice_thread_stop = false;
    adev->simcom_cpcmreg_state = false;
    adev->simcom_voice_rate = 0;
    adev->simcom_voice_channels = 0;


    route_uninit();

    free(device);
    return 0;
}

static void adev_open_init(struct audio_device *adev)
{
    ALOGD("%s",__func__);
    int i = 0;
    adev->mic_mute = false;
    adev->screenOff = false;

#ifdef AUDIO_3A
    adev->voice_api = NULL;
#endif

    adev->input_source = AUDIO_SOURCE_DEFAULT;
    adev->simcom_voice_active = false;
    adev->simcom_mic_route_active = false;
    list_init(&adev->usecase_list);
    pthread_mutex_init(&adev->simcom_mic_lock, NULL);
    pthread_cond_init(&adev->simcom_mic_cond, NULL);
    adev->simcom_mic_ring = NULL;
    adev->simcom_mic_ring_size = 0;
    adev->simcom_mic_ring_read = 0;
    adev->simcom_mic_ring_write = 0;
    adev->simcom_mic_ring_full = false;
    adev->simcom_voice_pcm = NULL;
    adev->simcom_voice_thread_started = false;
    adev->simcom_voice_thread_stop = false;

    for(i =0; i < OUTPUT_TOTAL; i++){
        adev->outputs[i] = NULL;
    }
    set_default_dev_info(adev->dev_out, SND_OUT_SOUND_CARD_MAX, 1);
    set_default_dev_info(adev->dev_in, SND_IN_SOUND_CARD_MAX, 1);
    adev->dev_out[SND_OUT_SOUND_CARD_SPEAKER].id = "SPEAKER";
    adev->dev_out[SND_OUT_SOUND_CARD_HDMI].id = "HDMI";
    adev->dev_out[SND_OUT_SOUND_CARD_SPDIF].id = "SPDIF";
    adev->dev_out[SND_OUT_SOUND_CARD_BT].id = "BT";
    adev->dev_in[SND_IN_SOUND_CARD_MIC].id = "MIC";
    adev->dev_in[SND_IN_SOUND_CARD_BT].id = "BT";
    adev->owner[0] = NULL;
    adev->owner[1] = NULL;

    char value[PROPERTY_VALUE_MAX];
    if (property_get("vendor.audio.period_size", value, NULL) > 0) {
        pcm_config.period_size = atoi(value);
        pcm_config_in.period_size = pcm_config.period_size;
    }
    if (property_get("vendor.audio.in_period_size", value, NULL) > 0)
        pcm_config_in.period_size = atoi(value);
}

/**
 * @brief adev_open
 *
 * @param module
 * @param name
 * @param device
 *
 * @returns
 */
static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct audio_device *adev;
    int ret;

    ALOGD(AUDIO_HAL_VERSION);

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;
    adev->hw_device.get_microphones = adev_get_microphones;
    //adev->ar = audio_route_init(MIXER_CARD, NULL);
    //route_init();
    /* adev->cur_route_id initial value is 0 and such that first device
     * selection is always applied by select_devices() */
    *device = &adev->hw_device.common;

    adev_open_init(adev);
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Manta audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};