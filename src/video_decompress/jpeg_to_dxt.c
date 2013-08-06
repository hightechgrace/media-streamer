/*
 * FILE:    video_decompress/jpeg_to_dxt.c
 * AUTHORS: Martin Benes     <martinbenesh@gmail.com>
 *          Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 *          Petr Holub       <hopet@ics.muni.cz>
 *          Milos Liska      <xliska@fi.muni.cz>
 *          Jiri Matela      <matela@ics.muni.cz>
 *          Dalibor Matura   <255899@mail.muni.cz>
 *          Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 *
 * Copyright (c) 2005-2011 CESNET z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *      This product includes software developed by CESNET z.s.p.o.
 *
 * 4. Neither the name of the CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif // HAVE_CONFIG_H

#include "video_decompress/jpeg_to_dxt.h"
#include "cuda_dxt/cuda_dxt.h"

#include <pthread.h>
#include <stdlib.h>

#include "libgpujpeg/gpujpeg_decoder.h"

#include "debug.h"
//#include "host.h"
#include "video.h"
#include "video_decompress.h"
#include "video_decompress/jpeg.h"

volatile bool should_exit_threads = false;

struct state_decompress_jpeg_to_dxt {
        struct gpujpeg_decoder  *jpeg_decoder[MAX_CUDA_DEVICES];
        char                    *dxt_out_buff[MAX_CUDA_DEVICES];

        pthread_t                thread_id[MAX_CUDA_DEVICES];
        int                      src_len[MAX_CUDA_DEVICES];
        int                      threads_running;

        volatile bool            work_ready[MAX_CUDA_DEVICES];
        volatile bool            worker_waiting[MAX_CUDA_DEVICES];
        pthread_cond_t           worker_cv[MAX_CUDA_DEVICES];

        unsigned char           *input[MAX_CUDA_DEVICES];
        unsigned char           *output[MAX_CUDA_DEVICES];

        pthread_mutex_t          lock;
        volatile int             should_reconfigure;
        volatile int             worker_done;

        pthread_cond_t           boss_cv;
        volatile bool            boss_waiting;

        struct video_desc        desc;

        // which card is free to process next image
        int                      free;

        int                      ppb; // pixels per byte
        codec_t                  out_codec;
};

int jpeg_to_dxt_decompress_reconfigure_real(void *state, struct video_desc desc,
                int rshift, int gshift, int bshift, int pitch, codec_t out_codec, int i);

static void *worker_thread(void *arg);

static void *worker_thread(void *arg)
{
        struct state_decompress_jpeg_to_dxt *s = arg;
        int myID = s->threads_running++;

        pthread_mutex_unlock(&s->lock);

        while(!should_exit_threads) {
                pthread_mutex_lock(&s->lock);
                while(!s->work_ready[myID] && !should_exit_threads) {
                        s->worker_waiting[myID] = true;
                        pthread_cond_wait(&s->worker_cv[myID], &s->lock);
                        s->worker_waiting[myID] = false;

                }
                if(should_exit_threads) {
                        pthread_mutex_unlock(&s->lock);
                        break;
                }
                pthread_mutex_unlock(&s->lock);

                if(s->should_reconfigure) {
                        jpeg_to_dxt_decompress_reconfigure_real(s, s->desc, 0, 8, 16, s->desc.width / s->ppb, s->out_codec, myID);
                        pthread_mutex_lock(&s->lock);
                        s->should_reconfigure -= 1;
                        pthread_mutex_unlock(&s->lock);
                } else {
                        //gl_context_make_current(&s->gl_context[myID]);
                        struct gpujpeg_decoder_output decoder_output;

                        gpujpeg_decoder_output_set_cuda_buffer(&decoder_output);
                        gpujpeg_decoder_decode(s->jpeg_decoder[myID], s->input[myID], s->src_len[myID], &decoder_output);

                        if(s->out_codec == DXT1) {
                                cuda_rgb_to_dxt1(decoder_output.data, s->dxt_out_buff[myID], s->desc.width, -s->desc.height, 0);
                        } else {
                                cuda_rgb_to_dxt6(decoder_output.data, s->dxt_out_buff[myID], s->desc.width, -s->desc.height, 0);
                        }
                        if(cudaSuccess != cudaMemcpy((char*) s->output[myID], s->dxt_out_buff[myID], s->desc.width * s->desc.height / s->ppb, cudaMemcpyDeviceToHost)) {
                                fprintf(stderr, "[jpeg_to_dxt] unable to copy from device.");
                        }
                }

                pthread_mutex_lock(&s->lock);
                s->worker_done++;
                s->work_ready[myID] = false;
                if(s->boss_waiting) {
                        pthread_cond_signal(&s->boss_cv);
                }
                pthread_mutex_unlock(&s->lock);
        }

        return NULL;
}


void * jpeg_to_dxt_decompress_init(void)
{
        struct state_decompress_jpeg_to_dxt *s;

        s = (struct state_decompress_jpeg_to_dxt *) malloc(sizeof(struct state_decompress_jpeg_to_dxt));

        s->threads_running = 0;
        s->worker_done = 0;
        s->free = -1;

        pthread_mutex_init(&s->lock, NULL);
        pthread_cond_init(&s->boss_cv, NULL);
        s->boss_waiting = false;
        s->should_reconfigure = 0;

        for(unsigned int i = 0; i < cuda_devices_count; ++i) {
                pthread_cond_init(&s->worker_cv[i], NULL);
                s->jpeg_decoder[i] = NULL;
                s->dxt_out_buff[i] = NULL;
                s->input[i] = NULL;
                s->output[i] = NULL;
                s->work_ready[i] = false;
        }

        for(unsigned int i = 0; i < cuda_devices_count; ++i) {
                pthread_mutex_lock(&s->lock);
                if(pthread_create(&s->thread_id[i], NULL, worker_thread, s) != 0) {
                        fprintf(stderr, "Error creating thread.\n");
                        return NULL;
                }
        }

        return s;
}

/**
 * Reconfigureation function
 *
 * @return 0 to indicate error
 *         otherwise maximal buffer size which ins needed for image of given codec, width, and height
 */
int jpeg_to_dxt_decompress_reconfigure(void *state, struct video_desc desc,
                int rshift, int gshift, int bshift, int pitch, codec_t out_codec)
{
        struct state_decompress_jpeg_to_dxt *s = (struct state_decompress_jpeg_to_dxt *) state;

        UNUSED(rshift);
        UNUSED(gshift);
        UNUSED(bshift);
        assert(out_codec == DXT1 || out_codec == DXT5);

        s->out_codec = out_codec;
        if(out_codec == DXT1) {
                s->ppb = 2;
        } else { // DXT5
                s->ppb = 1;
        }

        assert(pitch == (int) desc.width / s->ppb); // default for DXT1
        pthread_mutex_lock(&s->lock);
        {
                for(int i = 0; i < (int) cuda_devices_count; ++i) {
                        while(s->work_ready[i]) {
                                s->boss_waiting = true;
                                pthread_cond_wait(&s->boss_cv, &s->lock);
                                s->boss_waiting = false;
                        }
                }

                s->worker_done = 0;
                s->desc = desc;
                s->should_reconfigure = cuda_devices_count;

                for(unsigned int i = 0; i < cuda_devices_count; ++i) {
                        s->work_ready[i] = true;

                        if(s->worker_waiting)
                                pthread_cond_signal(&s->worker_cv[i]);
                }
        }
        pthread_mutex_unlock(&s->lock);

        pthread_mutex_lock(&s->lock);
        {
                while(s->worker_done != (int) cuda_devices_count) {
                        s->boss_waiting = true;
                        pthread_cond_wait(&s->boss_cv, &s->lock);
                        s->boss_waiting = false;
                }
        }
        pthread_mutex_unlock(&s->lock);

        return TRUE;
}

/**
 * @return maximal buffer size needed to image with such a properties
 */
int jpeg_to_dxt_decompress_reconfigure_real(void *state, struct video_desc desc,
                int rshift, int gshift, int bshift, int pitch, codec_t out_codec, int i)
{
        UNUSED(rshift);
        UNUSED(gshift);
        UNUSED(bshift);
        struct state_decompress_jpeg_to_dxt *s = (struct state_decompress_jpeg_to_dxt *) state;

        assert(out_codec == DXT1 || out_codec == DXT5);
        assert(pitch == (int) desc.width / s->ppb); // default for DXT1

        free(s->input[i]);
        free(s->output[i]);

        if(s->jpeg_decoder[i] != NULL) {
                gpujpeg_decoder_destroy(s->jpeg_decoder[i]);
        } else {
                gpujpeg_init_device(cuda_devices[i], 0);
        }

        if(s->dxt_out_buff[i] != NULL) {
                cudaFree(s->dxt_out_buff[i]);
        }

        if(cudaSuccess != cudaMallocHost((void **) &s->dxt_out_buff[i], desc.width * desc.height / s->ppb)) {
                fprintf(stderr, "Could not allocate CUDA output buffer.\n");
                return 0;
        }
        //gpujpeg_init_device(cuda_device, GPUJPEG_OPENGL_INTEROPERABILITY);

        s->jpeg_decoder[i] = gpujpeg_decoder_create();
        if(!s->jpeg_decoder[i]) {
                fprintf(stderr, "Creating JPEG decoder failed.\n");
                return 0;
        }

        s->input[i] = malloc(desc.width * desc.height);
        s->output[i] = malloc(desc.width / s->ppb * desc.height);

        return desc.width * desc.height;
}

int jpeg_to_dxt_decompress(void *state, unsigned char *dst, unsigned char *buffer,
                unsigned int src_len, int frame_seq)
{
        struct state_decompress_jpeg_to_dxt *s = (struct state_decompress_jpeg_to_dxt *) state;
        UNUSED(frame_seq);

        if(s->free == -1) {
                s->free = 0;
                memcpy(s->input[s->free], buffer, src_len);
                s->src_len[s->free] = src_len;
                pthread_mutex_lock(&s->lock);
                s->work_ready[s->free] = true;
                if(s->worker_waiting[s->free]) {
                        pthread_cond_signal(&s->worker_cv[s->free]);
                }
                pthread_mutex_unlock(&s->lock);

                s->free = (s->free + 1) % cuda_devices_count;
        } else {
                memcpy(s->input[s->free], buffer, src_len);
                s->src_len[s->free] = src_len;
                pthread_mutex_lock(&s->lock);
                s->work_ready[s->free] = true;
                if(s->worker_waiting[s->free]) {
                        pthread_cond_signal(&s->worker_cv[s->free]);
                }
                pthread_mutex_unlock(&s->lock);

                s->free = (s->free + 1) % cuda_devices_count;

                pthread_mutex_lock(&s->lock);
                // while not done
                while(s->work_ready[s->free]) {
                        s->boss_waiting = true;
                        pthread_cond_wait(&s->boss_cv, &s->lock);
                        s->boss_waiting = false;
                }
                pthread_mutex_unlock(&s->lock);

                memcpy(dst, s->output[s->free], s->desc.width * s->desc.height / s->ppb);
        }

        return TRUE;
}

int jpeg_to_dxt_decompress_get_property(void *state, int property, void *val, size_t *len)
{
        UNUSED(state);
        UNUSED(property);
        UNUSED(val);
        UNUSED(len);

        return FALSE;
}

void jpeg_to_dxt_decompress_done(void *state)
{
        struct state_decompress_jpeg_to_dxt *s = (struct state_decompress_jpeg_to_dxt *) state;

        if(!state) {
                return;
        }

        pthread_mutex_lock(&s->lock);
        {
                should_exit_threads = true;
                for(unsigned int i = 0; i < cuda_devices_count; ++i) {
                        if(s->worker_waiting[i]) {
                                pthread_cond_signal(&s->worker_cv[i]);
                        }
                }
        }
        pthread_mutex_unlock(&s->lock);

        for(unsigned int i = 0; i < cuda_devices_count; ++i) {
                pthread_join(s->thread_id[i], NULL);
        }

        for(unsigned int i = 0; i < cuda_devices_count; ++i) {
                if(s->jpeg_decoder[i]) {
                        gpujpeg_decoder_destroy(s->jpeg_decoder[i]);
                }
                cudaFree(s->dxt_out_buff[i]);
                free(s->input[i]);
                free(s->output[i]);
        }

        free(s);
}

