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

#define LOG_TAG "usb_audio_hw"
/*#define LOG_NDEBUG 0*/

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

#include <log/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/audio.h>
#include <hardware/audio_alsaops.h>
#include <hardware/hardware.h>

#include <system/audio.h>

#include <tinyalsa/asoundlib.h>

#include <audio_utils/channels.h>

#include "alsa_device_profile.h"
#include "alsa_device_proxy.h"
#include "logging.h"

#define DEFAULT_INPUT_BUFFER_SIZE_MS 20

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */

    /* output */
    alsa_device_profile out_profile;

    /* input */
    alsa_device_profile in_profile;

    bool standby;
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock;               /* see note below on mutex acquisition order */
    bool standby;

    struct audio_device *dev;           /* hardware information - only using this for the lock */

    alsa_device_profile * profile;
    alsa_device_proxy proxy;            /* state of the stream */

    void * conversion_buffer;           /* any conversions are put into here
                                         * they could come from here too if
                                         * there was a previous conversion */
    size_t conversion_buffer_size;      /* in bytes */
};

struct stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    bool standby;

    struct audio_device *dev;           /* hardware information - only using this for the lock */

    alsa_device_profile * profile;
    alsa_device_proxy proxy;            /* state of the stream */

    // not used?
    // struct audio_config hal_pcm_config;

    /* We may need to read more data from the device in order to data reduce to 16bit, 4chan */
    void * conversion_buffer;           /* any conversions are put into here
                                         * they could come from here too if
                                         * there was a previous conversion */
    size_t conversion_buffer_size;      /* in bytes */
};

/*
 * Data Conversions
 */
/*
 * Convert a buffer of packed (3-byte) PCM24LE samples to PCM16LE samples.
 *   in_buff points to the buffer of PCM24LE samples
 *   num_in_samples size of input buffer in SAMPLES
 *   out_buff points to the buffer to receive converted PCM16LE LE samples.
 * returns
 *   the number of BYTES of output data.
 * We are doing this since we *always* present to The Framework as A PCM16LE device, but need to
 * support PCM24_3LE (24-bit, packed).
 * NOTE:
 *   This conversion is safe to do in-place (in_buff == out_buff).
 * TODO Move this to a utilities module.
 */
static size_t convert_24_3_to_16(const unsigned char * in_buff, size_t num_in_samples,
                                 short * out_buff)
{
    /*
     * Move from front to back so that the conversion can be done in-place
     * i.e. in_buff == out_buff
     */
    /* we need 2 bytes in the output for every 3 bytes in the input */
    unsigned char* dst_ptr = (unsigned char*)out_buff;
    const unsigned char* src_ptr = in_buff;
    size_t src_smpl_index;
    for (src_smpl_index = 0; src_smpl_index < num_in_samples; src_smpl_index++) {
        src_ptr++;               /* lowest-(skip)-byte */
        *dst_ptr++ = *src_ptr++; /* low-byte */
        *dst_ptr++ = *src_ptr++; /* high-byte */
    }

    /* return number of *bytes* generated: */
    return num_in_samples * 2;
}

/*
 * Convert a buffer of packed (3-byte) PCM32 samples to PCM16LE samples.
 *   in_buff points to the buffer of PCM32 samples
 *   num_in_samples size of input buffer in SAMPLES
 *   out_buff points to the buffer to receive converted PCM16LE LE samples.
 * returns
 *   the number of BYTES of output data.
 * We are doing this since we *always* present to The Framework as A PCM16LE device, but need to
 * support PCM_FORMAT_S32_LE (32-bit).
 * NOTE:
 *   This conversion is safe to do in-place (in_buff == out_buff).
 * TODO Move this to a utilities module.
 */
static size_t convert_32_to_16(const int32_t * in_buff, size_t num_in_samples, short * out_buff)
{
    /*
     * Move from front to back so that the conversion can be done in-place
     * i.e. in_buff == out_buff
     */

    short * dst_ptr = out_buff;
    const int32_t* src_ptr = in_buff;
    size_t src_smpl_index;
    for (src_smpl_index = 0; src_smpl_index < num_in_samples; src_smpl_index++) {
        *dst_ptr++ = *src_ptr++ >> 16;
    }

    /* return number of *bytes* generated: */
    return num_in_samples * 2;
}

static char * device_get_parameters(alsa_device_profile * profile, const char * keys)
{
    ALOGV("usb:audio_hw::device_get_parameters() keys:%s", keys);

    if (profile->card < 0 || profile->device < 0) {
        return strdup("");
    }

    struct str_parms *query = str_parms_create_str(keys);
    struct str_parms *result = str_parms_create();

    /* These keys are from hardware/libhardware/include/audio.h */
    /* supported sample rates */
    if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES)) {
        char* rates_list = profile_get_sample_rate_strs(profile);
        str_parms_add_str(result, AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES,
                          rates_list);
        free(rates_list);
    }

    /* supported channel counts */
    if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS)) {
        char* channels_list = profile_get_channel_count_strs(profile);
        str_parms_add_str(result, AUDIO_PARAMETER_STREAM_SUP_CHANNELS,
                          channels_list);
        free(channels_list);
    }

    /* supported sample formats */
    if (str_parms_has_key(query, AUDIO_PARAMETER_STREAM_SUP_FORMATS)) {
        char * format_params = profile_get_format_strs(profile);
        str_parms_add_str(result, AUDIO_PARAMETER_STREAM_SUP_FORMATS,
                          format_params);
        free(format_params);
    }
    str_parms_destroy(query);

    char* result_str = str_parms_to_str(result);
    str_parms_destroy(result);

    ALOGV("usb:audio_hw::device_get_parameters = %s", result_str);

    return result_str;
}

/*
 * HAl Functions
 */
/**
 * NOTE: when multiple mutexes have to be acquired, always respect the
 * following order: hw device > out stream
 */

/*
 * OUT functions
 */
static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    uint32_t rate = proxy_get_sample_rate(&((struct stream_out*)stream)->proxy);
    ALOGV("out_get_sample_rate() = %d", rate);
    return rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    const struct stream_out* out = (const struct stream_out*)stream;
    size_t buffer_size =
        proxy_get_period_size(&out->proxy) * audio_stream_out_frame_size(&(out->stream));
    ALOGV("out_get_buffer_size()  = %zu", buffer_size);
    return buffer_size;
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    /*
     * alsa_device_profile * profile = ((struct stream_out*)stream)->profile;
     * unsigned channel_count = profile_get_channel_count(profile);
     * uint32_t channel_mask = audio_channel_out_mask_from_count(channel_count);
     * ALOGV("out_get_channels() = 0x%X count:%d", channel_mask, channel_count);
     * return channel_mask;
     */

    /* Always Stereo for now. We will do *some* conversions in this HAL.
     * TODO When AudioPolicyManager & AudioFlinger supports arbitrary channels
     * rewrite this to return the ACTUAL channel format */
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    /* Note: The HAL doesn't do any FORMAT conversion at this time. It
     * Relies on the framework to provide data in the specified format.
     * This could change in the future.
     */
    alsa_device_proxy * proxy = &((struct stream_out*)stream)->proxy;
    audio_format_t format = audio_format_from_pcm_format(proxy_get_format(proxy));
    ALOGV("out_get_format() = %d", format);
    return format;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);

    if (!out->standby) {
        proxy_close(&out->proxy);
        out->standby = true;
    }

    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    ALOGV("usb:audio_hw::out out_set_parameters() keys:%s", kvpairs);

    struct stream_out *out = (struct stream_out *)stream;

    char value[32];
    int param_val;
    int routing = 0;
    int ret_value = 0;

    struct str_parms * parms = str_parms_create_str(kvpairs);
    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);

    bool recache_device_params = false;
    param_val = str_parms_get_str(parms, "card", value, sizeof(value));
    if (param_val >= 0) {
        out->profile->card = atoi(value);
        recache_device_params = true;
    }

    param_val = str_parms_get_str(parms, "device", value, sizeof(value));
    if (param_val >= 0) {
        out->profile->device = atoi(value);
        recache_device_params = true;
    }

    if (recache_device_params && out->profile->card >= 0 && out->profile->device >= 0) {
        ret_value = profile_read_device_info(out->profile) ? 0 : -EINVAL;
    }

    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    str_parms_destroy(parms);

    return ret_value;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    struct stream_out *out = (struct stream_out *)stream;
    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);

    char * params_str =  device_get_parameters(out->profile, keys);

    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    return params_str;
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    alsa_device_proxy * proxy = &((struct stream_out*)stream)->proxy;
    return proxy_get_latency(proxy);
}

static int out_set_volume(struct audio_stream_out *stream, float left, float right)
{
    return -ENOSYS;
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct stream_out *out)
{
    ALOGV("usb:audio_hw::out start_output_stream(card:%d device:%d)",
          out->profile->card, out->profile->device);

    return proxy_open(&out->proxy);
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer, size_t bytes)
{
    int ret;
    struct stream_out *out = (struct stream_out *)stream;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            goto err;
        }
        out->standby = false;
    }

    alsa_device_profile* profile = out->profile;
    alsa_device_proxy* proxy = &out->proxy;

    /*
     *  Setup conversion buffer
     * compute maximum potential buffer size.
     * * 2 for stereo -> quad conversion
     * * 3/2 for 16bit -> 24 bit conversion
     */
    size_t required_conversion_buffer_size = (bytes * 3 * 2) / 2;
    if (required_conversion_buffer_size > out->conversion_buffer_size) {
        /* TODO Remove this when AudioPolicyManger/AudioFlinger support arbitrary formats
           (and do these conversions themselves) */
        out->conversion_buffer_size = required_conversion_buffer_size;
        out->conversion_buffer = realloc(out->conversion_buffer, out->conversion_buffer_size);
    }

    const void * write_buff = buffer;
    int num_write_buff_bytes = bytes;

    /*
     * Num Channels conversion
     */
    int num_device_channels = proxy_get_channel_count(proxy);
    int num_req_channels = 2; /* always for now */

    if (num_device_channels != num_req_channels) {
        audio_format_t audio_format = out_get_format(&(out->stream.common));
        unsigned sample_size_in_bytes = audio_bytes_per_sample(audio_format);
        num_write_buff_bytes =
             adjust_channels(write_buff, num_req_channels,
                             out->conversion_buffer, num_device_channels,
                             sample_size_in_bytes, num_write_buff_bytes);
        write_buff = out->conversion_buffer;
    }

    if (write_buff != NULL && num_write_buff_bytes != 0) {
        proxy_write(&out->proxy, write_buff, num_write_buff_bytes);
    }

    pthread_mutex_unlock(&out->lock);

    return bytes;

err:
    pthread_mutex_unlock(&out->lock);
    if (ret != 0) {
        usleep(bytes * 1000000 / audio_stream_out_frame_size(stream) /
               out_get_sample_rate(&stream->common));
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream, uint32_t *dsp_frames)
{
    return -EINVAL;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                         uint64_t *frames, struct timespec *timestamp)
{
    /* FIXME - This needs to be implemented */
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream, int64_t *timestamp)
{
    return -EINVAL;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    ALOGV("usb:audio_hw::out adev_open_output_stream() handle:0x%X, device:0x%X, flags:0x%X",
          handle, devices, flags);

    struct audio_device *adev = (struct audio_device *)dev;

    struct stream_out *out;

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;

    /* setup function pointers */
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
    out->stream.get_presentation_position = out_get_presentation_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    out->dev = adev;

    out->profile = &adev->out_profile;

    // build this to hand to the alsa_device_proxy
    struct pcm_config proxy_config;

    int ret = 0;

    /* Rate */
    if (config->sample_rate == 0) {
        proxy_config.rate = config->sample_rate = profile_get_default_sample_rate(out->profile);
    } else if (profile_is_sample_rate_valid(out->profile, config->sample_rate)) {
        proxy_config.rate = config->sample_rate;
    } else {
        proxy_config.rate = config->sample_rate = profile_get_default_sample_rate(out->profile);
        ret = -EINVAL;
    }

    /* Format */
    if (config->format == AUDIO_FORMAT_DEFAULT) {
        proxy_config.format = profile_get_default_format(out->profile);
        config->format = audio_format_from_pcm_format(proxy_config.format);
    } else {
        enum pcm_format fmt = pcm_format_from_audio_format(config->format);
        if (profile_is_format_valid(out->profile, fmt)) {
            proxy_config.format = fmt;
        } else {
            proxy_config.format = profile_get_default_format(out->profile);
            config->format = audio_format_from_pcm_format(proxy_config.format);
            ret = -EINVAL;
        }
    }

    /* Channels */
    if (config->channel_mask == AUDIO_CHANNEL_NONE) {
        /* This will be needed when the framework supports non-stereo output */
        /* config->channel_mask =
         *        audio_channel_out_mask_from_count(profile_get_default_channel_count(out->profile));
         */
        proxy_config.channels = profile_get_default_channel_count(out->profile);
        config->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    } else {
        /* This will be needed when the framework supports non-stereo output */
        /*
         * unsigned channel_count = audio_channel_count_from_out_mask(config->channel_mask);
         * if (profile_is_channel_count_valid(out->profile, channel_count)) {
         *     proxy_set_channel_count(out->proxy, channel_count);
         * } else {
         *     config->channel_mask =
         *             audio_channel_out_mask_from_count(proxy_get_channel_count(out->proxy));
         *     ret = -EINVAL;
         * }
         */
        if (config->channel_mask != AUDIO_CHANNEL_OUT_STEREO) {
            proxy_config.channels = profile_get_default_channel_count(out->profile);
            config->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
            ret = -EINVAL;
        }  else {
            proxy_config.channels = profile_get_default_channel_count(out->profile);
        }
    }

    proxy_prepare(&out->proxy, out->profile, &proxy_config);

    /* TODO The retry mechanism isn't implemented in AudioPolicyManager/AudioFlinger. */
    ret = 0;

    out->conversion_buffer = NULL;
    out->conversion_buffer_size = 0;

    out->standby = true;

    *stream_out = &out->stream;

    return ret;

err_open:
    free(out);
    *stream_out = NULL;
    return -ENOSYS;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    ALOGV("usb:audio_hw::out adev_close_output_stream()");
    struct stream_out *out = (struct stream_out *)stream;

    /* Close the pcm device */
    out_standby(&stream->common);

    free(out->conversion_buffer);

    out->conversion_buffer = NULL;
    out->conversion_buffer_size = 0;

    free(stream);
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    /* TODO This needs to be calculated based on format/channels/rate */
    return 320;
}

/*
 * IN functions
 */
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    uint32_t rate = proxy_get_sample_rate(&((const struct stream_in *)stream)->proxy);
    ALOGV("in_get_sample_rate() = %d", rate);
    return rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    ALOGV("in_set_sample_rate(%d) - NOPE", rate);
    return -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    const struct stream_in * in = ((const struct stream_in*)stream);
    size_t buffer_size =
        proxy_get_period_size(&in->proxy) * audio_stream_in_frame_size(&(in->stream));
    ALOGV("in_get_buffer_size() = %zd", buffer_size);

    return buffer_size;
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    /* TODO Here is the code we need when we support arbitrary channel counts
     * alsa_device_proxy * proxy = ((struct stream_in*)stream)->proxy;
     * unsigned channel_count = proxy_get_channel_count(proxy);
     * uint32_t channel_mask = audio_channel_in_mask_from_count(channel_count);
     * ALOGV("in_get_channels() = 0x%X count:%d", channel_mask, channel_count);
     * return channel_mask;
     */
    /* TODO When AudioPolicyManager & AudioFlinger supports arbitrary channels
     rewrite this to return the ACTUAL channel format */
    return AUDIO_CHANNEL_IN_STEREO;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    /* TODO Here is the code we need when we support arbitrary input formats
     * alsa_device_proxy * proxy = ((struct stream_in*)stream)->proxy;
     * audio_format_t format = audio_format_from_pcm_format(proxy_get_format(proxy));
     * ALOGV("in_get_format() = %d", format);
     * return format;
     */
    /* Input only supports PCM16 */
    /* TODO When AudioPolicyManager & AudioFlinger supports arbitrary input formats
       rewrite this to return the ACTUAL channel format (above) */
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    ALOGV("in_set_format(%d) - NOPE", format);

    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);

    if (!in->standby) {
        proxy_close(&in->proxy);
        in->standby = true;
    }

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);

    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    ALOGV("usb: audio_hw::in in_set_parameters() keys:%s", kvpairs);

    struct stream_in *in = (struct stream_in *)stream;

    char value[32];
    int param_val;
    int routing = 0;
    int ret_value = 0;

    struct str_parms * parms = str_parms_create_str(kvpairs);

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);

    bool recache_device_params = false;

    /* Card/Device */
    param_val = str_parms_get_str(parms, "card", value, sizeof(value));
    if (param_val >= 0) {
        in->profile->card = atoi(value);
        recache_device_params = true;
    }

    param_val = str_parms_get_str(parms, "device", value, sizeof(value));
    if (param_val >= 0) {
        in->profile->device = atoi(value);
        recache_device_params = true;
    }

    if (recache_device_params && in->profile->card >= 0 && in->profile->device >= 0) {
        ret_value = profile_read_device_info(in->profile) ? 0 : -EINVAL;
     }

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);

    str_parms_destroy(parms);

    return ret_value;
}

static char * in_get_parameters(const struct audio_stream *stream, const char *keys)
{
    struct stream_in *in = (struct stream_in *)stream;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);

    char * params_str =  device_get_parameters(in->profile, keys);

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);

    return params_str;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

/* must be called with hw device and output stream mutexes locked */
static int start_input_stream(struct stream_in *in)
{
    ALOGV("usb:audio_hw::start_input_stream(card:%d device:%d)",
          in->profile->card, in->profile->device);

    return proxy_open(&in->proxy);
}

/* TODO mutex stuff here (see out_write) */
static ssize_t in_read(struct audio_stream_in *stream, void* buffer, size_t bytes)
{
    size_t num_read_buff_bytes = 0;
    void * read_buff = buffer;
    void * out_buff = buffer;

    struct stream_in * in = (struct stream_in *)stream;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);

    if (in->standby) {
        if (start_input_stream(in) != 0) {
            goto err;
        }
        in->standby = false;
    }

    alsa_device_profile * profile = in->profile;

    /*
     * OK, we need to figure out how much data to read to be able to output the requested
     * number of bytes in the HAL format (16-bit, stereo).
     */
    num_read_buff_bytes = bytes;
    int num_device_channels = proxy_get_channel_count(&in->proxy);
    int num_req_channels = 2; /* always, for now */

    if (num_device_channels != num_req_channels) {
        num_read_buff_bytes = (num_device_channels * num_read_buff_bytes) / num_req_channels;
    }

    enum pcm_format format = proxy_get_format(&in->proxy);
    if (format == PCM_FORMAT_S24_3LE) {
        /* 24-bit USB device */
        num_read_buff_bytes = (3 * num_read_buff_bytes) / 2;
    } else if (format == PCM_FORMAT_S32_LE) {
        /* 32-bit USB device */
        num_read_buff_bytes = num_read_buff_bytes * 2;
    }

    /* Setup/Realloc the conversion buffer (if necessary). */
    if (num_read_buff_bytes != bytes) {
        if (num_read_buff_bytes > in->conversion_buffer_size) {
            /*TODO Remove this when AudioPolicyManger/AudioFlinger support arbitrary formats
              (and do these conversions themselves) */
            in->conversion_buffer_size = num_read_buff_bytes;
            in->conversion_buffer = realloc(in->conversion_buffer, in->conversion_buffer_size);
        }
        read_buff = in->conversion_buffer;
    }

    if (proxy_read(&in->proxy, read_buff, num_read_buff_bytes) == 0) {
        /*
         * Do any conversions necessary to send the data in the format specified to/by the HAL
         * (but different from the ALSA format), such as 24bit ->16bit, or 4chan -> 2chan.
         */
        if (format != PCM_FORMAT_S16_LE) {
            /* we need to convert */
            if (num_device_channels != num_req_channels) {
                out_buff = read_buff;
            }

            if (format == PCM_FORMAT_S24_3LE) {
                num_read_buff_bytes =
                    convert_24_3_to_16(read_buff, num_read_buff_bytes / 3, out_buff);
            } else if (format == PCM_FORMAT_S32_LE) {
                num_read_buff_bytes =
                    convert_32_to_16(read_buff, num_read_buff_bytes / 4, out_buff);
            } else {
                goto err;
            }
        }

        if (num_device_channels != num_req_channels) {
            // ALOGV("chans dev:%d req:%d", num_device_channels, num_req_channels);

            out_buff = buffer;
            /* Num Channels conversion */
            if (num_device_channels != num_req_channels) {
                audio_format_t audio_format = in_get_format(&(in->stream.common));
                unsigned sample_size_in_bytes = audio_bytes_per_sample(audio_format);

                num_read_buff_bytes =
                    adjust_channels(read_buff, num_device_channels,
                                    out_buff, num_req_channels,
                                    sample_size_in_bytes, num_read_buff_bytes);
            }
        }
    }

err:
    pthread_mutex_unlock(&in->lock);

    return num_read_buff_bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags __unused)
{
    ALOGV("usb: in adev_open_input_stream() rate:%" PRIu32 ", chanMask:0x%" PRIX32 ", fmt:%" PRIu8,
          config->sample_rate, config->channel_mask, config->format);

    struct stream_in *in = (struct stream_in *)calloc(1, sizeof(struct stream_in));
    int ret = 0;

    if (in == NULL)
        return -ENOMEM;

    /* setup function pointers */
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

    in->dev = (struct audio_device *)dev;

    in->profile = &in->dev->in_profile;

    struct pcm_config proxy_config;

    /* Rate */
    if (config->sample_rate == 0) {
        proxy_config.rate = config->sample_rate = profile_get_default_sample_rate(in->profile);
    } else if (profile_is_sample_rate_valid(in->profile, config->sample_rate)) {
        proxy_config.rate = config->sample_rate;
    } else {
        proxy_config.rate = config->sample_rate = profile_get_default_sample_rate(in->profile);
        ret = -EINVAL;
    }

    /* Format */
    /* until the framework supports format conversion, just take what it asks for
     * i.e. AUDIO_FORMAT_PCM_16_BIT */
    if (config->format == AUDIO_FORMAT_DEFAULT) {
        /* just return AUDIO_FORMAT_PCM_16_BIT until the framework supports other input
         * formats */
        config->format = AUDIO_FORMAT_PCM_16_BIT;
        proxy_config.format = PCM_FORMAT_S16_LE;
    } else if (config->format == AUDIO_FORMAT_PCM_16_BIT) {
        /* Always accept AUDIO_FORMAT_PCM_16_BIT until the framework supports other input
         * formats */
        proxy_config.format = PCM_FORMAT_S16_LE;
    } else {
        /* When the framework support other formats, validate here */
        config->format = AUDIO_FORMAT_PCM_16_BIT;
        proxy_config.format = PCM_FORMAT_S16_LE;
        ret = -EINVAL;
    }

    if (config->channel_mask == AUDIO_CHANNEL_NONE) {
        /* just return AUDIO_CHANNEL_IN_STEREO until the framework supports other input
         * formats */
        config->channel_mask = AUDIO_CHANNEL_IN_STEREO;

    } else if (config->channel_mask != AUDIO_CHANNEL_IN_STEREO) {
        /* allow only stereo capture for now */
        config->channel_mask = AUDIO_CHANNEL_IN_STEREO;
        ret = -EINVAL;
    }
    // proxy_config.channels = 0;  /* don't change */
    proxy_config.channels = profile_get_default_channel_count(in->profile);

    proxy_prepare(&in->proxy, in->profile, &proxy_config);

    in->standby = true;

    in->conversion_buffer = NULL;
    in->conversion_buffer_size = 0;

    *stream_in = &in->stream;

    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev, struct audio_stream_in *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    /* Close the pcm device */
    in_standby(&stream->common);

    free(in->conversion_buffer);

    free(stream);
}

/*
 * ADEV Functions
 */
static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    return 0;
}

static char * adev_get_parameters(const struct audio_hw_device *dev, const char *keys)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    return -ENOSYS;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    return -ENOSYS;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;
    free(device);

    return 0;
}

static int adev_open(const hw_module_t* module, const char* name, hw_device_t** device)
{
    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    struct audio_device *adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    profile_init(&adev->out_profile, PCM_OUT);
    profile_init(&adev->in_profile, PCM_IN);

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *)module;
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

    *device = &adev->hw_device.common;

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
        .name = "USB audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
