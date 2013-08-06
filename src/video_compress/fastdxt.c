/*
 * FILE:    video_compress/fastdxt.c
 * AUTHORS: Martin Benes     <martinbenesh@gmail.com>
 *          Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 *          Petr Holub       <hopet@ics.muni.cz>
 *          Milos Liska      <xliska@fi.muni.cz>
 *          Jiri Matela      <matela@ics.muni.cz>
 *          Dalibor Matura   <255899@mail.muni.cz>
 *          Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 *
 * Copyright (c) 2005-2010 CESNET z.s.p.o.
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

//#include "host.h"
#include "debug.h"
#include "fastdxt.h"
#include <pthread.h>
#include "compat/platform_semaphore.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#ifdef HAVE_MACOSX
#include <malloc/malloc.h>
#else                           /* HAVE_MACOSX */
#include <malloc.h>
#endif                          /* HAVE_MACOSX */
#include <string.h>
#include <unistd.h>
#include "module.h"
#include "video.h"
#include "video_compress.h"
#include "libdxt.h"

static volatile int fastdxt_should_exit = FALSE;

#ifndef HAVE_MACOSX
#define uint64_t 	unsigned long
#endif                          /* HAVE_MACOSX */

/* NOTE: These threads busy wait, so at *most* set this to one less than the
 * total number of cores on your system (Also 3 threads will work)! Also, if
 * you see tearing in the rendered image try increasing the number of threads
 * by 1 (For a dual dual-core Opteron 285 3 threads work great). 
 * -- iwsmith <iwsmith@cct.lsu.ed> 9 August 2007
 */
#define MAX_THREADS 16
#define NUM_THREADS_DEFAULT 3

/* Ok, we are going to decompose the problem into 2^n pieces (generally
 * 2, 4, or 8). We will most likely need to do the following:
 * 1. Convert 10-bit -> 8bit
 * 2. Convert YUV 4:2:2 -> 8 bit RGB
 * 3. Compress 8 bit RGB -> DXT
 */

struct state_video_compress_fastdxt {
        struct module module_data;

        int num_threads;
        unsigned char *buffer[MAX_THREADS];
        unsigned char *output_data;
        pthread_mutex_t lock;
        volatile int thread_count;
        pthread_t thread_ids[MAX_THREADS];
        codec_t tx_color_spec;
        sem_t thread_compress[MAX_THREADS];
        sem_t thread_done[MAX_THREADS];
        
        int dxt_height;
        enum interlacing_t interlacing_source;
        int rgb:1;
        
        decoder_t decoder;

        struct video_frame *out[2];
        /* this is just shortcut for (only) tiles from the above frame */
        struct tile *tile[2];

        volatile int buffer_idx;

        int encoder_input_linesize;
};

static void *compress_thread(void *args);
static int reconfigure_compress(struct state_video_compress_fastdxt *compress, int width,
                int height, codec_t codec, enum interlacing_t, double fps);
static void fastdxt_done(struct module *mod);

static int reconfigure_compress(struct state_video_compress_fastdxt *compress, int width,
                int height, codec_t codec, enum interlacing_t interlacing, double fps)
{
        int x;
        int i;

        fprintf(stderr, "Compression reinitialized for %ux%u video.\n", 
                        width, height);
        /* Store original attributes to allow format change detection */
        for(i = 0; i < 2; ++i) {
                compress->tile[i]->width = width;
                compress->tile[i]->height = height;
                compress->out[i]->color_spec = codec;
                compress->out[i]->fps = fps;
                
                if(interlacing == INTERLACED_MERGED) {
                        fprintf(stderr, "[FastDXT] Warning: deinterlacing input prior to compress!\n");
                        compress->out[i]->interlacing = PROGRESSIVE;
                } else {
                        compress->out[i]->interlacing = interlacing;
                }
        }
        compress->tx_color_spec = codec;
        compress->interlacing_source = interlacing;

        compress->dxt_height = (compress->tile[0]->height + 3) / 4 * 4;

        switch (codec) {
                case RGB:
                        compress->decoder = (decoder_t) vc_copylineRGBtoRGBA;
                        compress->rgb = TRUE;
                        break;
                case RGBA:
                        compress->decoder = (decoder_t) memcpy;
                        compress->rgb = TRUE;
                        break;
                case R10k:
                        compress->decoder = (decoder_t) vc_copyliner10k;
                        compress->rgb = TRUE;
                        break;
                case YUYV:
                        compress->decoder = (decoder_t) vc_copylineYUYV;
                        compress->rgb = FALSE;
                        break;
                case UYVY:
                case Vuy2:
                case DVS8:
                        compress->decoder = (decoder_t) memcpy;
                        compress->rgb = FALSE;
                        break;
                case v210:
                        compress->decoder = (decoder_t) vc_copylinev210;
                        compress->rgb = FALSE;
                        break;
                case DVS10:
                        compress->decoder = (decoder_t) vc_copylineDVS10;
                        compress->rgb = FALSE;
                        break;
                case DPX10:        
                        compress->decoder = (decoder_t) vc_copylineDPX10toRGBA;
                        compress->rgb = FALSE;
                        break;
                default:
                        fprintf(stderr, "Unknown codec %d!", codec);
                        //exit_uv(128);
                        return FALSE;
        }
        
        int h_align = 0;

        for(i = 0; codec_info[i].name != NULL; ++i) {
                if(codec == codec_info[i].codec) {
                        h_align = codec_info[i].h_align;
                }
        }
        assert(h_align != 0);

        for(i = 0; i < 2; ++i) {
                compress->encoder_input_linesize = vf_get_tile(compress->out[i], 0)->width * 
                        (compress->rgb ? 4 /*RGBA*/: 2/*YUV 422*/);

                if(compress->rgb) {
                        compress->out[i]->color_spec = DXT1;
                } else {
                        compress->out[i]->color_spec = DXT1_YUV;
                }
        }
        

        for (x = 0; x < compress->num_threads; x++) {
                int my_height = (compress->dxt_height / compress->num_threads) / 4 * 4;
                if(x == compress->num_threads - 1) {
                        my_height = compress->dxt_height - my_height /* "their height" */ * x;
                }
                compress->buffer[x] =
                    (unsigned char *)malloc(width * my_height * 4);
        }

#ifdef HAVE_MACOSX
        compress->output_data = (unsigned char *)malloc(width * compress->dxt_height * 4);
        for(i = 0; i < 2; ++i) {
                compress->tile[i]->data = (char *)malloc(width * compress->dxt_height * 4);
        }
#else
        /*
         *  memalign doesn't exist on Mac OS. malloc always returns 16  
         *  bytes aligned memory
         *
         *  see: http://www.mythtv.org/pipermail/mythtv-dev/2006-January/044309.html
         */
        compress->output_data = (unsigned char *)aligned_malloc(width * compress->dxt_height * 4, 16);
        for(i = 0; i < 2; ++i) {
                compress->tile[i]->data = (char *)aligned_malloc(width * compress->dxt_height * 4, 16);
        }
#endif                          /* HAVE_MACOSX */
        memset(compress->output_data, 0, width * compress->dxt_height * 4);
        for(i = 0; i < 2; ++i) {
                memset(compress->tile[i]->data, 0, width * compress->dxt_height * 4 / 8);
        }

        return TRUE;
}

struct module *fastdxt_init(struct module *parent, char *num_threads_str)
{
        /* This function does the following:
         * 1. Allocate memory for buffers 
         * 2. Spawn compressor threads
         */
        int x;
        int i;
        struct state_video_compress_fastdxt *compress;

        if(num_threads_str && strcmp(num_threads_str, "help") == 0) {
                printf("FastDXT usage:\n");
                printf("\t-FastDXT[:<num_threads>]\n");
                printf("\t\t<num_threads> - count of compress threads (default %d)\n", NUM_THREADS_DEFAULT);
                return &compress_init_noerr;
        }

        compress = calloc(1, sizeof(struct state_video_compress_fastdxt));
        /* initial values */
        compress->num_threads = 0;
        if(num_threads_str == NULL)
                compress->num_threads = NUM_THREADS_DEFAULT;
        else
                compress->num_threads = atoi(num_threads_str);
        assert (compress->num_threads >= 1 && compress->num_threads <= MAX_THREADS);

        for(i = 0; i < 2; ++i) {
                compress->out[i] = vf_alloc(1);
                compress->tile[i] = vf_get_tile(compress->out[i], 0);
        }
        
        for(i = 0; i < 2; ++i) {
                compress->tile[i]->width = 0;
                compress->tile[i]->height = 0;
        }

        compress->thread_count = 0;
        if (pthread_mutex_init(&(compress->lock), NULL)) {
                perror("Error initializing mutex!");
                return NULL;
        }

        for (x = 0; x < compress->num_threads; x++) {
                platform_sem_init(&compress->thread_compress[x], 0, 0);
                platform_sem_init(&compress->thread_done[x], 0, 0);
        }

        pthread_mutex_lock(&(compress->lock));

        for (x = 0; x < compress->num_threads; x++) {
                if (pthread_create
                    (&(compress->thread_ids[x]), NULL, compress_thread,
                     (void *)compress)) {
                        perror("Unable to create compressor thread!");
                        //exit_uv(x);
                        return NULL;
                }
        }
        pthread_mutex_unlock(&(compress->lock));
        
        while(compress->num_threads != compress->thread_count) /* wait for all threads online */
                ;
        fprintf(stderr, "All compression threads are online.\n");
        
        module_init_default(&compress->module_data);
        compress->module_data.cls = MODULE_CLASS_DATA;
        compress->module_data.priv_data = compress;
        compress->module_data.deleter = fastdxt_done;
        module_register(&compress->module_data, parent);

        return &compress->module_data;
}

struct tile * fastdxt_compress_tile(struct module *mod, struct tile *tx, struct video_desc *desc, int buffer_idx)
{
        /* This thread will be called from main.c and handle the compress_threads */
        struct state_video_compress_fastdxt *compress =
                (struct state_video_compress_fastdxt *) mod->priv_data;
        unsigned int x;
        unsigned char *line1, *line2;
        struct video_frame *out = compress->out[buffer_idx];
        struct tile *out_tile = &out->tiles[0];

        assert(tx->width % 4 == 0);
        
        pthread_mutex_lock(&(compress->lock));

        if(tx->width != out_tile->width ||
                        tx->height != out_tile->height ||
                        desc->interlacing != compress->interlacing_source ||
                        desc->color_spec != compress->tx_color_spec)
        {
                int ret;
                ret = reconfigure_compress(compress, tx->width, tx->height, desc->color_spec, desc->interlacing, desc->fps);
                if(!ret)
                        return NULL;
        }

        line1 = tx->data;
        line2 = compress->output_data;

        for (x = 0; x < out_tile->height; ++x) {
                int src_linesize = vc_get_linesize(out_tile->width, compress->tx_color_spec);
                compress->decoder(line2, line1, compress->encoder_input_linesize,
                                0, 8, 16);
                line1 += src_linesize;
                line2 += compress->encoder_input_linesize;
        }

        if(desc->interlacing != INTERLACED_MERGED && desc->interlacing != PROGRESSIVE) {
                fprintf(stderr, "Unsupported interlacing format.\n");
                //exit_uv(1);
        }

        if(desc->interlacing == INTERLACED_MERGED) {
                vc_deinterlace(compress->output_data, compress->encoder_input_linesize,
                                out_tile->height);
        }

        compress->buffer_idx = buffer_idx;

        for (x = 0; x < (unsigned int) compress->num_threads; x++) {
                platform_sem_post(&compress->thread_compress[x]);
        }

        for (x = 0; x < (unsigned int) compress->num_threads; x++) {
                platform_sem_wait(&compress->thread_done[x]);
        }

        out_tile->data_len = tx->width * compress->dxt_height / 2;
        
        pthread_mutex_unlock(&(compress->lock));

        desc->color_spec = out->color_spec;
        return &out->tiles[0];
}

static void *compress_thread(void *args)
{
        struct state_video_compress_fastdxt *compress =
                (struct state_video_compress_fastdxt *)args;
        int myId, range, my_range, x;
        int my_height;
        unsigned char *retv;

        pthread_mutex_lock(&(compress->lock));
        myId = compress->thread_count;
        compress->thread_count++;
        pthread_mutex_unlock(&(compress->lock));

        fprintf(stderr, "Compress thread %d online.\n", myId);

        while (1) {
                platform_sem_wait(&compress->thread_compress[myId]);
                if(fastdxt_should_exit) break;

                my_height = (compress->tile[compress->buffer_idx]->height / compress->num_threads) / 4 * 4;
                range = my_height * compress->tile[compress->buffer_idx]->width; /* for all threads except the last */

                if(myId == compress->num_threads - 1) {
                        my_height = compress->tile[compress->buffer_idx]->height - my_height /* "their height" */ * myId;
                }
                my_range = my_height * compress->tile[compress->buffer_idx]->width;

                if(!compress->rgb)
                {
                        unsigned char *input;
                        input = (compress->output_data) + myId
                                * range * 2;
                        retv = compress->buffer[myId];
                        /* Repack the data to YUV 4:4:4 Format */
                        for (x = 0; x < my_range; x += 2) {
                                retv[4 * x] = input[2 * x + 1]; //Y1
                                retv[4 * x + 1] = input[2 * x]; //U1
                                retv[4 * x + 2] = input[2 * x + 2];     //V1
                                retv[4 * x + 3] = 255;  //Alpha

                                retv[4 * x + 4] = input[2 * x + 3];     //Y2
                                retv[4 * x + 5] = input[2 * x]; //U1
                                retv[4 * x + 6] = input[2 * x + 2];     //V1
                                retv[4 * x + 7] = 255;  //Alpha
                        }
                } else {
                        retv = (compress->output_data) + myId * range * 4;
                }

                DirectDXT1(retv,
                               ((unsigned char *) compress->tile[compress->buffer_idx]->data) + myId * range / 2,
                               compress->tile[compress->buffer_idx]->width, my_height);

                platform_sem_post(&compress->thread_done[myId]);
        }
        
        platform_sem_post(&compress->thread_done[myId]);

        return NULL;
}

static void fastdxt_done(struct module *mod)
{
        struct state_video_compress_fastdxt *compress =
                (struct state_video_compress_fastdxt *) mod->priv_data;
        int x;
        
        pthread_mutex_lock(&(compress->lock)); /* wait for fastdxt_compress if running */
        fastdxt_should_exit = TRUE;
        
        for (x = 0; x < compress->num_threads; x++) {
                platform_sem_post(&compress->thread_compress[x]);
        }

        for (x = 0; x < compress->num_threads; x++) {
                platform_sem_wait(&compress->thread_done[x]);
        }

        pthread_mutex_unlock(&(compress->lock));
        
        pthread_mutex_destroy(&(compress->lock));
        
        for (x = 0; x < compress->num_threads; ++x)
                free(compress->buffer[x]);

        aligned_free(compress->output_data);
        for(x = 0; x < 2; ++x) {
                aligned_free(compress->tile[x]->data);
        }

        free(compress);
}

