/*
 *  Copyright (C) 2017, Zhang YanMing <yanmin.zhang@ingenic.com, jamincheung@126.com>
 *
 *  Ingenic Linux plarform SDK project
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under  the terms of the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <utils/log.h>
#include <utils/assert.h>
#include <utils/common.h>
#include <audio/alsa/wave_player.h>
#include "wave_pcm_common.h"

#define LOG_TAG "wave_player"

static WaveContainer wave_container;
static struct snd_pcm_container pcm_container;

static uint32_t safe_read(int fd, void* buf, uint32_t count) {
    uint32_t read_sofar = 0;
    uint32_t readed;

    while (count > 0) {
        if ((readed = read(fd, buf, count)) == 0)
            break;

        if (readed < 0)
            return read_sofar > 0 ? read_sofar : readed;

        count -= readed;
        read_sofar += readed;
        buf = (char *)buf + readed;
    }

    return read_sofar;
}

static int do_play_wave(struct snd_pcm_container* pcm_container,
        WaveContainer* wave_container, int fd) {
    uint64_t writed = 0;
    uint64_t count = LE_INT(wave_container->chunk_header.length);
    uint64_t c;
    int load = 0;
    int ret;

    while (writed < count) {
        do {
            c = count - writed;
            if (c > pcm_container->chunk_bytes)
                c = pcm_container->chunk_bytes;
            c -= load;

            if (c == 0)
                break;

            ret = safe_read(fd, pcm_container->data_buf + load, c);
            if (ret < 0) {
                LOGE("Failed to safe read\n");
                return -1;
            }

            if (ret == 0)
                break;

            load += ret;

        } while (load < pcm_container->chunk_bytes);

        load = load * 8 / pcm_container->bits_per_frame;
        ret = pcm_write(pcm_container, load);
        if (ret != load)
            break;

        ret = ret * pcm_container->bits_per_frame / 8;
        writed += ret;
        load = 0;
    }

    return 0;
}

static int play_wave(int fd) {
    assert_die_if(fd < 0, "Invaild fd\n");

    int error = 0;
    snd_pcm_format_t format;

    lseek(fd, 0, SEEK_SET);
    error = wave_read_header(fd, &wave_container);
    if (error < 0) {
        LOGE("Failed to read wave header\n");
        goto error;
    }

    error = get_wave_format(&wave_container, &format);
    if (error < 0) {
        LOGE("Failed to get snd format\n");
        goto error;
    }

    error = pcm_set_params(&pcm_container, format, wave_container.format.channels,
            wave_container.format.sample_fq);
    if (error < 0) {
        LOGE("Failed to set hw params\n");
        goto error;
    }

#ifdef LOCAL_DEBUG
    snd_pcm_dump(pcm_container.handle, pcm_container.out_log);
#endif

    error = do_play_wave(&pcm_container, &wave_container, fd);
    if (error < 0) {
        LOGE("Failed to do play wave file\n");
        goto error;
    }

    snd_pcm_drain(pcm_container.handle);

    free(pcm_container.data_buf);

    return 0;

error:
    if (pcm_container.data_buf)
        free(pcm_container.data_buf);

    return -1;
}

static int play_stream(const char* snd_device, char* buffer, int size) {
    return 0;
}

static int pause_play(void) {
    return pcm_pause(&pcm_container);
}

static int resume_play(void) {
    return pcm_resume(&pcm_container);
}

static int cancel_play(void) {
    return pcm_cancel(&pcm_container);
}

static int init(const char* snd_device) {
    assert_die_if(snd_device == NULL, "snd_device is NULL\n");

    int error = 0;

    snd_output_stdio_attach(&pcm_container.out_log, stderr, 0);

    error = snd_pcm_open(&pcm_container.handle, snd_device,
            SND_PCM_STREAM_PLAYBACK, 0);
    if (error < 0) {
        LOGE("Failed to snd_pcm_open %s: %s\n", snd_device, snd_strerror(error));
        goto error;
    }

    return 0;

error:
    if (pcm_container.out_log)
        snd_output_close(pcm_container.out_log);
    if (pcm_container.handle)
        snd_pcm_close(pcm_container.handle);

    return -1;
}

static int deinit(void) {
    snd_output_close(pcm_container.out_log);
    snd_pcm_close(pcm_container.handle);

    return 0;
}

static struct wave_player this = {
        .init = init,
        .deinit = deinit,
        .play_wave = play_wave,
        .play_stream = play_stream,
        .pause_play = pause_play,
        .resume_play = resume_play,
        .cancel_play = cancel_play,
};

struct wave_player* get_wave_player(void) {
    return &this;
}
