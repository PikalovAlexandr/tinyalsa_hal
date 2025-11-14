#define LOG_TAG "audio_hw_usb"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <termios.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <system/audio.h>
#include <hardware/audio.h>
#include <hardware/hardware.h>

#define USB_AUDIO_SAMPLE_RATE 8000
#define USB_AUDIO_CHANNEL_COUNT 1
#define USB_AUDIO_FORMAT AUDIO_FORMAT_PCM_16_BIT
#define USB_AUDIO_FRAME_SIZE 2

#define USB_IN_BUFFER_SIZE 1600
#define USB_OUT_BUFFER_SIZE 640

struct usb_audio_device {
    struct audio_hw_device device;
    int usb_fd;
    int thread_running;
    
    struct audio_stream_in *active_input;
    struct audio_stream_out *active_output;
};

struct usb_audio_stream_in {
    struct audio_stream_in stream;
    struct usb_audio_device *dev;
    uint32_t frames_read;
};

struct usb_audio_stream_out {
    struct audio_stream_out stream;
    struct usb_audio_device *dev;
    uint32_t frames_written;
};

// Конфигурация USB порта (из вашего оригинального кода)
static int configure_usb_port(int fd) {
    struct termios tio;
    
    if (tcgetattr(fd, &tio) != 0) {
        ALOGE("tcgetattr failed: %s", strerror(errno));
        return -1;
    }
    
    cfmakeraw(&tio);
    cfsetispeed(&tio, B4000000);
    cfsetospeed(&tio, B4000000);
    
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag |= CLOCAL | CREAD;
    
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    
    tcflush(fd, TCIFLUSH);
    tcflush(fd, TCOFLUSH);
    
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        ALOGE("tcsetattr failed: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

// Функции audio_stream_in
static uint32_t in_get_sample_rate(const struct audio_stream *stream) {
    return USB_AUDIO_SAMPLE_RATE;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate) {
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream) {
    return USB_IN_BUFFER_SIZE;
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream) {
    return AUDIO_CHANNEL_IN_MONO;
}

static audio_format_t in_get_format(const struct audio_stream *stream) {
    return USB_AUDIO_FORMAT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format) {
    return 0;
}

static int in_standby(struct audio_stream *stream) {
    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd) {
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs) {
    return 0;
}

static char* in_get_parameters(const struct audio_stream *stream, const char *keys) {
    return strdup("");
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect) {
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect) {
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer, size_t bytes) {
    struct usb_audio_stream_in *usb_stream = (struct usb_audio_stream_in *)stream;
    
    if (usb_stream->dev->usb_fd < 0) {
        return -1;
    }
    
    ssize_t bytes_read = read(usb_stream->dev->usb_fd, buffer, bytes);
    if (bytes_read > 0) {
        usb_stream->frames_read += bytes_read / USB_AUDIO_FRAME_SIZE;
        ALOGV("USB Audio read %zd bytes", bytes_read);
    }
    
    return bytes_read;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream) {
    return 0;
}

// Функции audio_stream_out
static uint32_t out_get_sample_rate(const struct audio_stream *stream) {
    return USB_AUDIO_SAMPLE_RATE;
}

static size_t out_get_buffer_size(const struct audio_stream *stream) {
    return USB_OUT_BUFFER_SIZE;
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream) {
    return AUDIO_CHANNEL_OUT_MONO;
}

static audio_format_t out_get_format(const struct audio_stream *stream) {
    return USB_AUDIO_FORMAT;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer, size_t bytes) {
    struct usb_audio_stream_out *usb_stream = (struct usb_audio_stream_out *)stream;
    
    if (usb_stream->dev->usb_fd < 0) {
        return -1;
    }
    
    ssize_t bytes_written = write(usb_stream->dev->usb_fd, buffer, bytes);
    if (bytes_written > 0) {
        usb_stream->frames_written += bytes_written / USB_AUDIO_FRAME_SIZE;
        ALOGV("USB Audio wrote %zd bytes", bytes_written);
    }
    
    return bytes_written;
}

static int out_get_render_position(const struct audio_stream_out *stream, uint32_t *dsp_frames) {
    struct usb_audio_stream_out *usb_stream = (struct usb_audio_stream_out *)stream;
    *dsp_frames = usb_stream->frames_written;
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs) {
    return 0;
}

static char* out_get_parameters(const struct audio_stream *stream, const char *keys) {
    return strdup("");
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate) {
    return 0;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format) {
    return 0;
}

static int out_standby(struct audio_stream *stream) {
    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd) {
    return 0;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect) {
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect) {
    return 0;
}

// Vtable для потоков
static struct audio_stream_in usb_audio_stream_in = {
    .common = {
        .get_sample_rate = in_get_sample_rate,
        .set_sample_rate = in_set_sample_rate,
        .get_buffer_size = in_get_buffer_size,
        .get_channels = in_get_channels,
        .get_format = in_get_format,
        .set_format = in_set_format,
        .standby = in_standby,
        .dump = in_dump,
        .set_parameters = in_set_parameters,
        .get_parameters = in_get_parameters,
        .add_audio_effect = in_add_audio_effect,
        .remove_audio_effect = in_remove_audio_effect,
    },
    .read = in_read,
    .get_input_frames_lost = in_get_input_frames_lost,
};

static struct audio_stream_out usb_audio_stream_out = {
    .common = {
        .get_sample_rate = out_get_sample_rate,
        .set_sample_rate = out_set_sample_rate,
        .get_buffer_size = out_get_buffer_size,
        .get_channels = out_get_channels,
        .get_format = out_get_format,
        .set_format = out_set_format,
        .standby = out_standby,
        .dump = out_dump,
        .set_parameters = out_set_parameters,
        .get_parameters = out_get_parameters,
        .add_audio_effect = out_add_audio_effect,
        .remove_audio_effect = out_remove_audio_effect,
    },
    .write = out_write,
    .get_render_position = out_get_render_position,
};

// Основные функции устройства
static int usb_adev_open_output_stream(struct audio_hw_device *dev,
                                      audio_io_handle_t handle,
                                      audio_devices_t devices,
                                      audio_output_flags_t flags,
                                      struct audio_config *config,
                                      struct audio_stream_out **stream_out,
                                      const char *address) {
    struct usb_audio_device *usb_dev = (struct usb_audio_device *)dev;
    
    if (usb_dev->active_output != NULL) {
        ALOGE("Output stream already active");
        return -EBUSY;
    }
    
    struct usb_audio_stream_out *out_stream = 
        calloc(1, sizeof(struct usb_audio_stream_out));
    if (!out_stream) {
        return -ENOMEM;
    }
    
    memcpy(&out_stream->stream, &usb_audio_stream_out, sizeof(usb_audio_stream_out));
    out_stream->dev = usb_dev;
    
    // Настройка конфигурации
    config->sample_rate = USB_AUDIO_SAMPLE_RATE;
    config->channel_mask = AUDIO_CHANNEL_OUT_MONO;
    config->format = USB_AUDIO_FORMAT;
    
    usb_dev->active_output = &out_stream->stream;
    *stream_out = &out_stream->stream;
    
    ALOGI("USB Audio output stream opened");
    return 0;
}

static void usb_adev_close_output_stream(struct audio_hw_device *dev,
                                        struct audio_stream_out *stream) {
    struct usb_audio_device *usb_dev = (struct usb_audio_device *)dev;
    
    if (usb_dev->active_output == stream) {
        free(usb_dev->active_output);
        usb_dev->active_output = NULL;
    }
    
    ALOGI("USB Audio output stream closed");
}

static int usb_adev_open_input_stream(struct audio_hw_device *dev,
                                     audio_io_handle_t handle,
                                     audio_devices_t devices,
                                     audio_input_flags_t flags,
                                     struct audio_config *config,
                                     struct audio_stream_in **stream_in,
                                     audio_source_t source) {
    struct usb_audio_device *usb_dev = (struct usb_audio_device *)dev;
    
    if (usb_dev->active_input != NULL) {
        ALOGE("Input stream already active");
        return -EBUSY;
    }
    
    struct usb_audio_stream_in *in_stream = 
        calloc(1, sizeof(struct usb_audio_stream_in));
    if (!in_stream) {
        return -ENOMEM;
    }
    
    memcpy(&in_stream->stream, &usb_audio_stream_in, sizeof(usb_audio_stream_in));
    in_stream->dev = usb_dev;
    
    // Настройка конфигурации
    config->sample_rate = USB_AUDIO_SAMPLE_RATE;
    config->channel_mask = AUDIO_CHANNEL_IN_MONO;
    config->format = USB_AUDIO_FORMAT;
    
    usb_dev->active_input = &in_stream->stream;
    *stream_in = &in_stream->stream;
    
    ALOGI("USB Audio input stream opened");
    return 0;
}

static void usb_adev_close_input_stream(struct audio_hw_device *dev,
                                       struct audio_stream_in *stream) {
    struct usb_audio_device *usb_dev = (struct usb_audio_device *)dev;
    
    if (usb_dev->active_input == stream) {
        free(usb_dev->active_input);
        usb_dev->active_input = NULL;
    }
    
    ALOGI("USB Audio input stream closed");
}

static int usb_adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs) {
    return 0;
}

static char* usb_adev_get_parameters(const struct audio_hw_device *dev, const char *keys) {
    return strdup("");
}

static int usb_adev_init_check(const struct audio_hw_device *dev) {
    return 0;
}

static int usb_adev_set_voice_volume(struct audio_hw_device *dev, float volume) {
    return 0;
}

static int usb_adev_set_master_volume(struct audio_hw_device *dev, float volume) {
    return -ENOSYS;
}

static int usb_adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode) {
    return 0;
}

static int usb_adev_set_mic_mute(struct audio_hw_device *dev, bool state) {
    return 0;
}

static int usb_adev_get_mic_mute(const struct audio_hw_device *dev, bool *state) {
    *state = false;
    return 0;
}

static size_t usb_adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                            const struct audio_config *config) {
    return USB_IN_BUFFER_SIZE;
}

static int usb_adev_close(hw_device_t *device) {
    struct usb_audio_device *dev = (struct usb_audio_device *)device;
    
    dev->thread_running = 0;
    
    if (dev->usb_fd >= 0) {
        close(dev->usb_fd);
    }
    
    free(dev);
    
    ALOGI("USB Audio device closed");
    return 0;
}

static struct audio_hw_device usb_audio_hw_device = {
    .common = {
        .tag = HARDWARE_DEVICE_TAG,
        .version = AUDIO_DEVICE_API_VERSION_2_0,
        .module = NULL,
        .close = usb_adev_close,
    },
    .init_check = usb_adev_init_check,
    .set_voice_volume = usb_adev_set_voice_volume,
    .set_master_volume = usb_adev_set_master_volume,
    .set_mode = usb_adev_set_mode,
    .set_mic_mute = usb_adev_set_mic_mute,
    .get_mic_mute = usb_adev_get_mic_mute,
    .set_parameters = usb_adev_set_parameters,
    .get_parameters = usb_adev_get_parameters,
    .get_input_buffer_size = usb_adev_get_input_buffer_size,
    .open_output_stream = usb_adev_open_output_stream,
    .close_output_stream = usb_adev_close_output_stream,
    .open_input_stream = usb_adev_open_input_stream,
    .close_input_stream = usb_adev_close_input_stream,
};

static int usb_adev_open(const hw_module_t* module, const char* name,
                        hw_device_t** device) {
    ALOGI("USB Audio HAL: open called for %s", name);
    
    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) {
        return -EINVAL;
    }
    
    struct usb_audio_device *dev = calloc(1, sizeof(struct usb_audio_device));
    if (!dev) {
        return -ENOMEM;
    }
    
    memcpy(&dev->device, &usb_audio_hw_device, sizeof(usb_audio_hw_device));
    
    // Открываем USB порт
    dev->usb_fd = open("/dev/ttyUSB4", O_RDWR | O_NONBLOCK);
    if (dev->usb_fd < 0) {
        ALOGE("Failed to open /dev/ttyUSB4: %s", strerror(errno));
        free(dev);
        return -ENODEV;
    }
    
    // Конфигурируем порт
    if (configure_usb_port(dev->usb_fd) != 0) {
        ALOGE("Failed to configure USB port");
        close(dev->usb_fd);
        free(dev);
        return -ENODEV;
    }
    
    dev->thread_running = 1;
    dev->active_input = NULL;
    dev->active_output = NULL;
    
    *device = &dev->device.common;
    
    ALOGI("USB Audio device opened successfully");
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = usb_adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "USB Audio HW HAL",
        .author = "Rockchip",
        .methods = &hal_module_methods,
    },
};