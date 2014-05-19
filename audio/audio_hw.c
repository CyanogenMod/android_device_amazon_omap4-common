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

#define LOG_TAG "audio_hw_primary"
//#define LOG_NDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>

#include <system/audio.h>

#include <tinyalsa/asoundlib.h>

#include <audio_utils/resampler.h>
#include <audio_utils/echo_reference.h>
#include <hardware/audio_effect.h>
#include <audio_effects/effect_aec.h>

#include "audio_route.h"

#define PCM_CARD 0
#define PCM_CARD_HDMI 1

#ifndef PCM_CARD_DEFAULT
#define PCM_CARD_DEFAULT PCM_CARD
#endif

/* MultiMedia1 LP */
#define PCM_DEVICE_MM_LP	0
#define PCM_DEVICE_MM		1
#define PCM_DEVICE_MM2	 	2
#define PCM_DEVICE_MM_UL 	3
#define PCM_DEVICE_SCO_OUT 	4
#define PCM_DEVICE_SCO_IN 	5

#ifndef PCM_DEVICE_DEFAULT_OUT
#define PCM_DEVICE_DEFAULT_OUT PCM_DEVICE_MM
#endif
#ifndef PCM_DEVICE_DEFAULT_IN
#define PCM_DEVICE_DEFAULT_IN PCM_DEVICE_MM_UL
#endif

#define ABE_BASE_FRAME_COUNT	24

#define SHORT_PERIOD_MULTIPLIER	40  /* 20 ms */
#define SHORT_PERIOD_SIZE	(ABE_BASE_FRAME_COUNT * SHORT_PERIOD_MULTIPLIER)

#define LONG_PERIOD_MULTIPLIER	2 /* 40 ms */
#define LONG_PERIOD_SIZE	(SHORT_PERIOD_SIZE * LONG_PERIOD_MULTIPLIER)

#define PLAYBACK_PERIOD_COUNT	4
#define CAPTURE_PERIOD_COUNT		2

#ifndef OUT_SAMPLING_RATE
#define OUT_SAMPLING_RATE	44100
#endif
#define MM_FULL_POWER_SAMPLING_RATE 48000

#define SCO_PERIOD_SIZE		256
#define SCO_PERIOD_COUNT	4
#define SCO_SAMPLING_RATE	8000

/* minimum sleep time in out_write() when write threshold is not reached */
#define MIN_WRITE_SLEEP_US 5000

#define RESAMPLER_BUFFER_FRAMES (SHORT_PERIOD_SIZE * 2)
#define RESAMPLER_BUFFER_SIZE   (4 * RESAMPLER_BUFFER_FRAMES)

struct pcm_config pcm_config_out = {
    .channels = 2,
    .rate = OUT_SAMPLING_RATE,
    .period_size = SHORT_PERIOD_SIZE,
    .period_count = PLAYBACK_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_out_lp = {
    .channels = 2,
    .rate = OUT_SAMPLING_RATE,
    .period_size = LONG_PERIOD_SIZE,
    .period_count = PLAYBACK_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = OUT_SAMPLING_RATE,
    .period_size = SHORT_PERIOD_SIZE,
    .period_count = CAPTURE_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_sco = {
    .channels = 1,
    .rate = SCO_SAMPLING_RATE,
    .period_size = SCO_PERIOD_SIZE,
    .period_count = SCO_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_hdmi = {
    .channels = 2,
    .rate = 48000,
    .period_size = LONG_PERIOD_SIZE,
    .period_count = PLAYBACK_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = LONG_PERIOD_SIZE * 2,
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    unsigned int out_device;
    unsigned int in_device;
    bool standby;
    bool mic_mute;
    struct audio_route *ar;
    struct mixer *mixer;
    int orientation;
    bool low_power;

    struct stream_out *active_out;
    struct stream_in *active_in;
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    struct pcm_config pcm_config;
    bool standby;

    struct resampler_itfe *resampler;
    int16_t *buffer;
    size_t buffer_frames;

    int write_threshold;
    int low_power;

    struct audio_device *dev;
};

struct stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    struct pcm_config pcm_config;
    bool standby;

    unsigned int requested_rate;
    struct resampler_itfe *resampler;
    struct resampler_buffer_provider buf_provider;
    int16_t *buffer;
    size_t frames_in;
    int read_status;

    struct audio_device *dev;
};

enum {
    ORIENTATION_LANDSCAPE,
    ORIENTATION_PORTRAIT,
    ORIENTATION_SQUARE,
    ORIENTATION_UNDEFINED,
};

static uint32_t out_get_sample_rate(const struct audio_stream *stream);
static size_t out_get_buffer_size(const struct audio_stream *stream);
static audio_format_t out_get_format(const struct audio_stream *stream);
static uint32_t in_get_sample_rate(const struct audio_stream *stream);
static size_t in_get_buffer_size(const struct audio_stream *stream);
static audio_format_t in_get_format(const struct audio_stream *stream);
static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer);
static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer);

static const struct {
    int mask;
    int output_flag;
    const char *name;
} dev_names[] = {
    /* OUTS */
    { AUDIO_DEVICE_OUT_EARPIECE,					 1, "earpiece" },	// 0x00000001
    { AUDIO_DEVICE_OUT_SPEAKER,						 1, "speaker" },	// 0x00000002
    { AUDIO_DEVICE_OUT_WIRED_HEADSET | AUDIO_DEVICE_OUT_WIRED_HEADPHONE, 1, "headphone" },	// 0x00000004 | 0x00000008
    { AUDIO_DEVICE_OUT_WIRED_HEADSET | AUDIO_DEVICE_OUT_WIRED_HEADPHONE, 1, "headphone" },	// 0x00000004 | 0x00000008
    { AUDIO_DEVICE_OUT_AUX_DIGITAL,					 1, "aux-digital-out" },// 0x00000400
    { AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET,				 1, "analog-dock" }, 	// 0x00000800
    { AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET,				 1, "digital-dock" },	// 0x00001000
    /* INS */
    { AUDIO_DEVICE_IN_COMMUNICATION,					 0, "comms" },		// 0x80000001
    { AUDIO_DEVICE_IN_AMBIENT,						 0, "ambient" },	// 0x80000002
    { AUDIO_DEVICE_IN_BUILTIN_MIC,					 0, "builtin-mic" },	// 0x80000004
    { AUDIO_DEVICE_IN_WIRED_HEADSET,					 0, "headset" },	// 0x80000010
    { AUDIO_DEVICE_IN_AUX_DIGITAL,					 0, "aux-digital-in" },	// 0x80000020
    { AUDIO_DEVICE_IN_BACK_MIC,						 0, "back-mic" },	// 0x80000080
};


/*
 * NOTE: when multiple mutexes have to be acquired, always take the
 * audio_device mutex first, followed by the stream_in and/or
 * stream_out mutexes.
 */

/* Helper functions */

static void select_devices(struct audio_device *adev)
{
    unsigned int i;
    int out_devices = 0;
    int in_devices = 0;

    reset_mixer_state(adev->ar);

    for (i = 0; i < (sizeof(dev_names) / sizeof(dev_names[0])); i++)
        if (dev_names[i].output_flag) {
            if (adev->out_device & dev_names[i].mask) {
                ALOGV("[MATCH] out_devices += %s", dev_names[i].name);
                audio_route_apply_path(adev->ar, dev_names[i].name);
                out_devices |= dev_names[i].mask;
            }
        }
        else {
            if ((adev->in_device - AUDIO_DEVICE_BIT_IN) & (dev_names[i].mask - AUDIO_DEVICE_BIT_IN)) {
                ALOGV("[MATCH] in_devices += %s", dev_names[i].name);
                audio_route_apply_path(adev->ar, dev_names[i].name);
                in_devices |= dev_names[i].mask;
            }
        }

    update_mixer_state(adev->ar);

    ALOGV("out_devices == 0x%8x, in_devices == 0x%8x", out_devices, in_devices);
}

/* must be called with hw device and output stream mutexes locked */
static void do_out_standby(struct stream_out *out)
{
    struct audio_device *adev = out->dev;

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        adev->active_out = NULL;
        out->standby = true;
    }
}

/* must be called with hw device and input stream mutexes locked */
static void do_in_standby(struct stream_in *in)
{
    struct audio_device *adev = in->dev;

    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;
        adev->active_in = NULL;
        in->standby = true;
    }
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    unsigned int device = PCM_DEVICE_DEFAULT_OUT;
    unsigned int card = PCM_CARD_DEFAULT;
    int ret;

    /*
     * Due to the lack of sample rate converters in the SoC,
     * it greatly simplifies things to have only the main
     * (speaker/headphone) PCM or the BC SCO PCM open at
     * the same time.
     */
#if 0
    if (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
        device = PCM_DEVICE_SCO_OUT;
        out->pcm_config = pcm_config_sco;
    } else {
#endif
    if (adev->out_device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        card = PCM_CARD_HDMI;
        out->pcm_config = pcm_config_hdmi;
    } else {
        if (out->low_power) {
            out->write_threshold = PLAYBACK_PERIOD_COUNT * LONG_PERIOD_SIZE;
            out->pcm_config.start_threshold = LONG_PERIOD_SIZE * 2;
            out->pcm_config.avail_min = LONG_PERIOD_SIZE;
            device = PCM_DEVICE_MM_LP;
        } else {
            /* default to NOT low power */
            out->write_threshold = PLAYBACK_PERIOD_COUNT * SHORT_PERIOD_SIZE;
            out->pcm_config.start_threshold = SHORT_PERIOD_SIZE * 2;
            out->pcm_config.avail_min = SHORT_PERIOD_SIZE;
        }
    }

    /*
     * All open PCMs can only use a single group of rates at once:
     * Group 1: 11.025, 22.05, 44.1
     * Group 2: 8, 16, 32, 48
     * Group 1 is used for digital audio playback since 44.1 is
     * the most common rate, but group 2 is required for SCO.
     */
    if (adev->active_in) {
        struct stream_in *in = adev->active_in;
        pthread_mutex_lock(&in->lock);
        if (((out->pcm_config.rate % 8000 == 0) &&
                 (in->pcm_config.rate % 8000) != 0) ||
                 ((out->pcm_config.rate % 11025 == 0) &&
                 (in->pcm_config.rate % 11025) != 0))
            do_in_standby(in);
        pthread_mutex_unlock(&in->lock);
    }

    ALOGD("pcm_open(%d, %d, PCM_OUT | PCM_MMAP, %p)\n", card, device, &out->pcm_config);
    out->pcm = pcm_open(card, device, PCM_OUT | PCM_MMAP, &out->pcm_config);

    if (out->pcm && !pcm_is_ready(out->pcm)) {
        ALOGE("pcm_open(out) failed: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        return -ENOMEM;
    }

    adev->active_out = out;

    if (out->resampler)
        out->resampler->reset(out->resampler);

    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct stream_in *in)
{
    struct audio_device *adev = in->dev;
    unsigned int card = PCM_CARD_DEFAULT;
    unsigned int device;
    int ret;

    /*
     * Due to the lack of sample rate converters in the SoC,
     * it greatly simplifies things to have only the main
     * mic PCM or the BC SCO PCM open at the same time.
     */
    if ((adev->in_device - AUDIO_DEVICE_BIT_IN) & (AUDIO_DEVICE_IN_ALL_SCO - AUDIO_DEVICE_BIT_IN)) {
        device = PCM_DEVICE_SCO_IN;
        in->pcm_config = pcm_config_sco;
    } else {
        device = PCM_DEVICE_DEFAULT_IN;
        in->pcm_config = pcm_config_in;
    }

    /*
     * All open PCMs can only use a single group of rates at once:
     * Group 1: 11.025, 22.05, 44.1
     * Group 2: 8, 16, 32, 48
     * Group 1 is used for digital audio playback since 44.1 is
     * the most common rate, but group 2 is required for SCO.
     */
    if (adev->active_out) {
        struct stream_out *out = adev->active_out;
        pthread_mutex_lock(&out->lock);
        if (((in->pcm_config.rate % 8000 == 0) &&
                 (out->pcm_config.rate % 8000) != 0) ||
                 ((in->pcm_config.rate % 11025 == 0) &&
                 (out->pcm_config.rate % 11025) != 0)) {
            do_out_standby(out);
	}
        pthread_mutex_unlock(&out->lock);
    }

    ALOGD("pcm_open(%d, %d, PCM_IN, [channels=%d, rate=%d, period_size=%d, period_count=%d, format=%d, start_threshold=%d, stop_threshold=%d])\n",
        card, device,
        in->pcm_config.channels, in->pcm_config.rate, in->pcm_config.period_size,
        in->pcm_config.period_count, in->pcm_config.format, in->pcm_config.start_threshold,
        in->pcm_config.stop_threshold);
    in->pcm = pcm_open(card, device, PCM_IN, &in->pcm_config);
    if (in->pcm && !pcm_is_ready(in->pcm)) {
        ALOGE("pcm_open(in) failed: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        return -ENOMEM;
    }

    adev->active_in = in;

    /* if no supported sample rate is available, use the resampler */
    if (in->resampler) {
        in->resampler->reset(in->resampler);
        in->frames_in = 0;
    }

    return 0;
}

static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer)
{
    struct stream_in *in;
    size_t hw_frame_size;

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

    hw_frame_size = audio_stream_frame_size(&in->stream.common);
    if (in->frames_in == 0) {
        in->read_status = pcm_read(in->pcm,
                                   (void*)in->buffer,
                                   in->pcm_config.channels * in->pcm_config.period_size * hw_frame_size);
        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }
        in->frames_in = in->pcm_config.period_size;
        if (in->pcm_config.channels == 2) {
            unsigned int i;

            /* Discard right channel */
            for (i = 1; i < in->frames_in; i++)
                in->buffer[i] = in->buffer[i * 2];
        }
    }

    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                                in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer + (in->pcm_config.period_size - in->frames_in);

    ALOGV("%s(read_size=%d, in->frames_in=%d, in->read_status=%d, buffer->frame_count=%d, in->pcm_config.channels=%d)", __FUNCTION__,
        in->pcm_config.channels * in->pcm_config.period_size * hw_frame_size,
        in->frames_in, in->read_status, buffer->frame_count, in->pcm_config.channels);
    return in->read_status;

}

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

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_frames(struct stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;
    size_t frame_size;

    frame_size = audio_stream_frame_size(&in->stream.common);

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
                    frame_count : frames_rd,
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

/* API functions */

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    return OUT_SAMPLING_RATE;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames */

    size_t size = (SHORT_PERIOD_SIZE * OUT_SAMPLING_RATE) / out->pcm_config.rate;
    size = ((size + 15) / 16) * 16;
    size = size * audio_stream_frame_size((struct audio_stream *)stream);
    ALOGV("%s(size=%d)", __FUNCTION__, size);
    return size;
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    do_out_standby(out);
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
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    unsigned int val;

    ALOGD("out_set_parameters::kvpairs == %s", kvpairs);

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    pthread_mutex_lock(&adev->lock);
    if (ret >= 0) {
        val = atoi(value);
        if ((adev->out_device != val) && (val != 0)) {
            /*
             * If SCO is turned on/off, we need to put audio into standby
             * because SCO uses a different PCM.
             */
            if ((val & AUDIO_DEVICE_OUT_ALL_SCO) ^
                    (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO)) {
                pthread_mutex_lock(&out->lock);
                do_out_standby(out);
                pthread_mutex_unlock(&out->lock);
            }

            ALOGV("out_set_parameters::adev->out_device == 0x%8x", val);
            adev->out_device = val;
            select_devices(adev);
        }
    }
    pthread_mutex_unlock(&adev->lock);

    str_parms_destroy(parms);
    return ret;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
//  return (pcm_config_out.period_size * PLAYBACK_PERIOD_COUNT * 1000) / pcm_config_out.rate;
    return (SHORT_PERIOD_SIZE * PLAYBACK_PERIOD_COUNT * 1000) / out->pcm_config.rate;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return 0; //-ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret = 0;
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    size_t frame_size = audio_stream_frame_size(&out->stream.common);
    int16_t *in_buffer = (int16_t *)buffer;
    size_t in_frames = bytes / frame_size;
    size_t out_frames = RESAMPLER_BUFFER_SIZE / frame_size;
    int kernel_frames;
    bool sco_on;

do_over:
    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the output stream mutex - e.g.
     * executing out_set_parameters() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            goto exit;
        }
        out->standby = false;
    }
    sco_on = (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO);
    pthread_mutex_unlock(&adev->lock);

    /* Reduce number of channels, if necessary */
    if (popcount(out_get_channels(&stream->common)) > (int)out->pcm_config.channels) {
        unsigned int i;

        /* Discard right channel */
        for (i = 1; i < in_frames; i++)
            in_buffer[i] = in_buffer[i * 2];

        /* The frame size is now half */
        frame_size /= 2;
    }

    /*
     * If the stream rate differs from the PCM rate, we need to
     * create a resampler.
     */
    if (out->pcm_config.rate != OUT_SAMPLING_RATE) {
        if (!out->resampler) {
            ret = create_resampler(OUT_SAMPLING_RATE,
                    MM_FULL_POWER_SAMPLING_RATE,
                    2,
                    RESAMPLER_QUALITY_DEFAULT,
                    NULL,
                    &out->resampler);
            if (ret != 0)
                goto exit;
            out->buffer = malloc(RESAMPLER_BUFFER_SIZE); /* todo: allow for reallocing */
            if (!out->buffer) {
                ret = -ENOMEM;
                goto exit;
            }
        }

        out->resampler->resample_from_input(out->resampler,
                (int16_t *)in_buffer,
                &in_frames,
                (int16_t *)out->buffer,
                &out_frames);
        in_buffer = out->buffer;
    } else {
        out_frames = in_frames;
    }

    if (!sco_on) {
        int total_sleep_time_us = 0;
        size_t period_size = out->pcm_config.period_size;

        /* do not allow more than out->write_threshold frames in kernel pcm driver buffer */
        do {
            struct timespec time_stamp;

            if (pcm_get_htimestamp(out->pcm, (unsigned int *)&kernel_frames, &time_stamp) < 0)
                break;
            kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;
            if (kernel_frames > out->write_threshold) {
                unsigned long time = (unsigned long)
                    (((int64_t)(kernel_frames - out->write_threshold) * 1000000) /
                            MM_FULL_POWER_SAMPLING_RATE);
                if (time < MIN_WRITE_SLEEP_US)
                    time = MIN_WRITE_SLEEP_US;
                usleep(time);
            }
        } while (kernel_frames > out->write_threshold);
    }

    ret = pcm_mmap_write(out->pcm, buffer, out_frames * frame_size);

exit:

    if (ret != 0) {
	ALOGE("out_write(%p) failed: %d\n", stream, ret);
        unsigned int usecs = bytes * 1000000 / audio_stream_frame_size(&stream->common) /
            out_get_sample_rate(&stream->common);
        ALOGD("usecs delay == %u", usecs);
        if (usecs >= 1000000L)
            usecs = 999999L;
        usleep(usecs);
    }

    pthread_mutex_unlock(&out->lock);

    if (ret == -EPIPE) {
        /* Recover from an underrun */
        ALOGE("XRUN detected\n");
        pthread_mutex_lock(&adev->lock);
        pthread_mutex_lock(&out->lock);
        do_out_standby(out);
        pthread_mutex_unlock(&out->lock);
        pthread_mutex_unlock(&adev->lock);
        goto do_over;
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
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

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    return -EINVAL;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->requested_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (in->pcm_config.period_size * in_get_sample_rate(stream)) /
    in->pcm_config.rate;
    size = ((size + 15) / 16) * 16;
    size = size * audio_stream_frame_size((struct audio_stream *)stream);
    ALOGV("in_get_buffer_size::size == %u", size);
    return size;
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_IN_MONO;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    do_in_standby(in);
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
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    unsigned int val;

    ALOGD("in_set_parameters::kvpairs == %s", kvpairs);

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    pthread_mutex_lock(&adev->lock);
    if (ret >= 0) {
        val = atoi(value) & ~AUDIO_DEVICE_BIT_IN;
        if ((adev->in_device != val) && (val != 0)) {
            /*
             * If SCO is turned on/off, we need to put audio into standby
             * because SCO uses a different PCM.
             */
            if ((val & AUDIO_DEVICE_IN_ALL_SCO) ^
                    (adev->in_device & AUDIO_DEVICE_IN_ALL_SCO)) {
                pthread_mutex_lock(&in->lock);
                do_in_standby(in);
                pthread_mutex_unlock(&in->lock);
            }

            ALOGV("in_set_parameters::adev->in_device == 0x%8x", val);
            adev->in_device = val;
            select_devices(adev);
        }
    }
    pthread_mutex_unlock(&adev->lock);

    str_parms_destroy(parms);
    return ret;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_frame_size(&stream->common);

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the input stream mutex - e.g.
     * executing in_set_parameters() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
        ret = start_input_stream(in);
        if (ret == 0)
            in->standby = 0;
    }
    pthread_mutex_unlock(&adev->lock);

    if (ret < 0)
        goto exit;

    /*if (in->num_preprocessors != 0) {
        ret = process_frames(in, buffer, frames_rq);
    } else */if (in->resampler != NULL) {
        ret = read_frames(in, buffer, frames_rq);
    } else if (in->pcm_config.channels == 2) {
        /*
         * If the PCM is stereo, capture twice as many frames and
         * discard the right channel.
         */
        unsigned int i;
        int16_t *in_buffer = (int16_t *)buffer;

        ret = pcm_read(in->pcm, in->buffer, bytes * 2);

        /* Discard right channel */
        for (i = 0; i < frames_rq; i++)
            in_buffer[i] = in->buffer[i * 2];
    } else {
        ret = pcm_read(in->pcm, buffer, bytes);
    }

    if (ret > 0)
        ret = 0;

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    if (ret < 0)
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               in_get_sample_rate(&stream->common));

    pthread_mutex_unlock(&in->lock);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream,
                               effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream,
                                  effect_handle_t effect)
{
    return 0;
}


static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int ret;

    ALOGV("%s(%p, 0x%04x, 0x%04x, %d, %p)", __FUNCTION__, dev, devices,
                        config->channel_mask, config->sample_rate, stream_out);

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;
    if (devices & AUDIO_DEVICE_OUT_ALL_SCO) {
        ret = create_resampler(OUT_SAMPLING_RATE,
                MM_FULL_POWER_SAMPLING_RATE,
                2,
                RESAMPLER_QUALITY_DEFAULT,
                NULL,
                &out->resampler);
        if (ret != 0)
            goto err_open;
        out->buffer = malloc(RESAMPLER_BUFFER_SIZE); /* todo: allow for reallocing */
    } else
       out->resampler = NULL;

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

    out->dev = adev;

    if (flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) {
        out->pcm_config = pcm_config_out_lp;
        out->low_power = 1;
        ALOGD("opening the low power output at %x", (unsigned int)out);
    } else {
        out->pcm_config = pcm_config_out;
        out->low_power = 0;
        ALOGD("opening the standard (low-latency) output at %x", (unsigned int)out);
    }

    pthread_mutex_lock(&adev->lock);
    adev->out_device &= ~AUDIO_DEVICE_OUT_ALL;
    adev->out_device |= devices;
    select_devices(adev);
    pthread_mutex_unlock(&adev->lock);

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    out->standby = true;

    *stream_out = &out->stream;
    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    out_standby(&stream->common);
    if (out->resampler) {
        release_resampler(out->resampler);
        out->resampler = NULL;
    }
    if (out->buffer) {
        free(out->buffer);
        out->buffer = NULL;
    }
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret;

    ALOGD("adev_set_parameters::kvpairs == %s", kvpairs);

    parms = str_parms_create_str(kvpairs);
    ret = str_parms_get_str(parms, "orientation", value, sizeof(value));
    if (ret >= 0) {
        int orientation;

        if (strcmp(value, "landscape") == 0)
            orientation = ORIENTATION_LANDSCAPE;
        else if (strcmp(value, "portrait") == 0)
            orientation = ORIENTATION_PORTRAIT;
        else if (strcmp(value, "square") == 0)
            orientation = ORIENTATION_SQUARE;
        else
            orientation = ORIENTATION_UNDEFINED;

        pthread_mutex_lock(&adev->lock);
        if (orientation != adev->orientation) {
            adev->orientation = orientation;
            /*
             * Orientation changes can occur with the input device
             * closed so we must call select_devices() here to set
             * up the mixer. This is because select_devices() will
             * not be called when the input device is opened if no
             * other input parameter is changed.
             */
            select_devices(adev);
        }
        pthread_mutex_unlock(&adev->lock);
    }

    ret = str_parms_get_str(parms, "screen_state", value, sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->low_power = false;
        else
            adev->low_power = true;
    }

    str_parms_destroy(parms);
    return ret;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return strdup("");
}

static uint32_t adev_get_supported_devices(const struct audio_hw_device *dev)
{
    struct audio_device *adev = (struct audio_device *)dev;
    uint32_t supported = 0;
    unsigned int i, j;

    for (i = 0; i < adev->ar->num_mixer_paths; i++)
        for (j = 0; j < (sizeof(dev_names) / sizeof(dev_names[0])); j++)
            if (strcmp(adev->ar->mixer_path[i].name, dev_names[j].name) == 0) {
                supported |= dev_names[j].mask;
                break;
            }

    ALOGV("%s(%p) supported == 0x%8x", __FUNCTION__, dev, supported);

    return supported;
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
    // Need this to return a valid # so that the OS sends volume updates
    return 0; //-ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    adev->mic_mute = state;

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (pcm_config_in.period_size * config->sample_rate) / pcm_config_in.rate;
    size = ((size + 15) / 16) * 16;

    return (size * popcount(config->channel_mask) *
                audio_bytes_per_sample(config->format));
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in;
    int ret;
    int channel_count = popcount(config->channel_mask);
    /*audioflinger expects return variable to be NULL incase of failure */
    *stream_in = NULL;

    ALOGV("%s(dev=%p, devices=0x%04x, format=%d, channel_count=%d, sample_rate=%d, stream_in=%p)", __FUNCTION__, dev,
        devices, config->format, channel_count, config->sample_rate, stream_in);

    /* Respond with a request for mono if a different format is given. */
    if (config->channel_mask != AUDIO_CHANNEL_IN_MONO) {
        config->channel_mask = AUDIO_CHANNEL_IN_MONO;
        return -EINVAL;
    }

    in = (struct stream_in *)calloc(1, sizeof(struct stream_in));
    if (!in)
        return -ENOMEM;

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

    in->dev = adev;
    in->standby = true;

    pthread_mutex_lock(&adev->lock);
    adev->in_device &= ~AUDIO_DEVICE_IN_ALL;
    adev->in_device |= devices;
    select_devices(adev);
    pthread_mutex_unlock(&adev->lock);

    in->requested_rate = config->sample_rate;
    in->pcm_config = pcm_config_in;

    in->buffer = malloc(in->pcm_config.channels * in->pcm_config.period_size * audio_stream_frame_size(&in->stream.common));
    ALOGV("%s(buffer_size=%d)", __FUNCTION__,
       in->pcm_config.channels * in->pcm_config.period_size * audio_stream_frame_size(&in->stream.common));
    if (!in->buffer) {
        ret = -ENOMEM;
        goto err;
    }
    in->frames_in = 0;

    if (in->requested_rate != in->pcm_config.rate) {
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;
        ret = create_resampler(in->pcm_config.rate,
                               in->requested_rate,
                               1,
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
        if (ret != 0) {
            ret = -EINVAL;
            goto err;
        }
        ALOGV("%s(create_resampler[pcm_rate=%d, requested_rate=%d])\n",
            __FUNCTION__,
            in->pcm_config.rate, in->requested_rate);
    }

    *stream_in = &in->stream;
    return 0;

err:
    if (in->resampler)
        release_resampler(in->resampler);

    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    in_standby(&stream->common);
    if (in->resampler) {
        release_resampler(in->resampler);
        in->resampler = NULL;
    }
    if (in->buffer) {
        free(in->buffer);
        in->buffer = NULL;
    }
    free(stream);
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    audio_route_free(adev->ar);
    mixer_close(adev->mixer);

    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct audio_device *adev;
    int ret;

    ALOGV("%s(%p, %s, %p)", __FUNCTION__, module, name, device);

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_CURRENT;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.get_supported_devices = adev_get_supported_devices;
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

    adev->mixer = mixer_open(PCM_CARD);
    if (!adev->mixer) {
        free(adev);
	ALOGE("Unable to open the mixer, aborting.");
	return -EINVAL;
    }

    adev->ar = audio_route_init(adev->mixer);
    adev->orientation = ORIENTATION_UNDEFINED;
    adev->out_device = AUDIO_DEVICE_OUT_SPEAKER;
    adev->in_device = AUDIO_DEVICE_IN_BUILTIN_MIC & ~AUDIO_DEVICE_BIT_IN;

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
        .name = "Amazon audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
