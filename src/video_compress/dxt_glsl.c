/*
 * FILE:    dxt_glsl_compress.c
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

#include <pthread.h>
#include <stdlib.h>

#include "gl_context.h"

#include "compat/platform_semaphore.h"
#include "debug.h"
#include "dxt_compress/dxt_encoder.h"
#include "dxt_compress/dxt_util.h"
//#include "host.h"
#include "module.h"
#include "video.h"
#include "video_compress.h"
#include "video_compress/dxt_glsl.h"

struct state_video_compress_rtdxt {
        struct module module_data;

        struct dxt_encoder **encoder;

        struct video_frame *out[2];
        decoder_t decoder;
        char *decoded;
        unsigned int configured:1;
        unsigned int interlaced_input:1;
        codec_t color_spec;

        int encoder_input_linesize;
        
        struct gl_context gl_context;
};

static int configure_with(struct state_video_compress_rtdxt *s, struct video_frame *frame);
static void dxt_glsl_compress_done(struct module *mod);

static int configure_with(struct state_video_compress_rtdxt *s, struct video_frame *frame)
{
        unsigned int x;
        enum dxt_format format;
        int i;
        
        for (i = 0; i < 2; ++i) {
                s->out[i] = vf_alloc(frame->tile_count);
        }
        
        for (x = 0; x < frame->tile_count; ++x) {
                if (vf_get_tile(frame, x)->width != vf_get_tile(frame, 0)->width ||
                                vf_get_tile(frame, x)->width != vf_get_tile(frame, 0)->width) {

                        fprintf(stderr,"[RTDXT] Requested to compress tiles of different size!");
                        //exit_uv(128);
                        return FALSE;
                }
        }
        
        for (i = 0; i < 2; ++i) {
                s->out[i]->fps = frame->fps;
                s->out[i]->color_spec = s->color_spec;

                for (x = 0; x < frame->tile_count; ++x) {
                        vf_get_tile(s->out[i], x)->width = vf_get_tile(frame, 0)->width;
                        vf_get_tile(s->out[i], x)->height = vf_get_tile(frame, 0)->height;
                }
        }

        switch (frame->color_spec) {
                case RGB:
                        s->decoder = (decoder_t) memcpy;
                        format = DXT_FORMAT_RGB;
                        break;
                case RGBA:
                        s->decoder = (decoder_t) memcpy;
                        format = DXT_FORMAT_RGBA;
                        break;
                case R10k:
                        s->decoder = (decoder_t) vc_copyliner10k;
                        format = DXT_FORMAT_RGBA;
                        break;
                case YUYV:
                        s->decoder = (decoder_t) vc_copylineYUYV;
                        format = DXT_FORMAT_YUV422;
                        break;
                case UYVY:
                case Vuy2:
                case DVS8:
                        s->decoder = (decoder_t) memcpy;
                        format = DXT_FORMAT_YUV422;
                        break;
                case v210:
                        s->decoder = (decoder_t) vc_copylinev210;
                        format = DXT_FORMAT_YUV422;
                        break;
                case DVS10:
                        s->decoder = (decoder_t) vc_copylineDVS10;
                        format = DXT_FORMAT_YUV422;
                        break;
                case DPX10:        
                        s->decoder = (decoder_t) vc_copylineDPX10toRGBA;
                        format = DXT_FORMAT_RGBA;
                        break;
                default:
                        fprintf(stderr, "[RTDXT] Unknown codec: %d\n", frame->color_spec);
                        //exit_uv(128);
                        return FALSE;
        }

        /* We will deinterlace the output frame */
        if(frame->interlacing  == INTERLACED_MERGED) {
                for (i = 0; i < 2; ++i) {
                        s->out[i]->interlacing = PROGRESSIVE;
                }
                s->interlaced_input = TRUE;
                fprintf(stderr, "[DXT compress] Enabling automatic deinterlacing.\n");
        } else {
                for (i = 0; i < 2; ++i) {
                        s->out[i]->interlacing = frame->interlacing;
                }
                s->interlaced_input = FALSE;
        }
        
        int data_len = 0;

        s->encoder = malloc(frame->tile_count * sizeof(struct dxt_encoder *));
        if(s->out[0]->color_spec == DXT1) {
                for(int i = 0; i < (int) frame->tile_count; ++i) {
                        s->encoder[i] =
                                dxt_encoder_create(DXT_TYPE_DXT1, s->out[0]->tiles[0].width, s->out[0]->tiles[0].height, format,
                                                s->gl_context.legacy);
                }
                data_len = dxt_get_size(s->out[0]->tiles[0].width, s->out[0]->tiles[0].height, DXT_TYPE_DXT1);
        } else if(s->out[0]->color_spec == DXT5){
                for(int i = 0; i < (int) frame->tile_count; ++i) {
                        s->encoder[i] =
                                dxt_encoder_create(DXT_TYPE_DXT5_YCOCG, s->out[0]->tiles[0].width, s->out[0]->tiles[0].height, format,
                                                s->gl_context.legacy);
                }
                data_len = dxt_get_size(s->out[0]->tiles[0].width, s->out[0]->tiles[0].height, DXT_TYPE_DXT5_YCOCG);
        }

        for(int i = 0; i < (int) frame->tile_count; ++i) {
                if(s->encoder[i] == NULL) {
                        fprintf(stderr, "[RTDXT] Unable to create decoder.\n");
                        //exit_uv(128);
                        return FALSE;
                }
        }

        s->encoder_input_linesize = s->out[0]->tiles[0].width;
        switch(format) { 
                case DXT_FORMAT_RGBA:
                        s->encoder_input_linesize *= 4;
                        break;
                case DXT_FORMAT_RGB:
                        s->encoder_input_linesize *= 3;
                        break;
                case DXT_FORMAT_YUV422:
                        s->encoder_input_linesize *= 2;
                        break;
                case DXT_FORMAT_YUV:
                        /* not used - just not compilator to complain */
                        abort();
                        break;
        }

        assert(data_len > 0);
        assert(s->encoder_input_linesize > 0);

        for (i = 0; i < 2; ++i) {
                for (x = 0; x < frame->tile_count; ++x) {
                        vf_get_tile(s->out[i], x)->data_len = data_len;
                        vf_get_tile(s->out[i], x)->data = (char *) malloc(data_len);
                }
        }
        
        s->decoded = malloc(4 * s->out[0]->tiles[0].width * s->out[0]->tiles[0].height);
        
        s->configured = TRUE;
        return TRUE;
}

struct module *dxt_glsl_compress_init(struct module *parent, char * opts)
{
        struct state_video_compress_rtdxt *s;
        int i;
        
        s = (struct state_video_compress_rtdxt *) malloc(sizeof(struct state_video_compress_rtdxt));

        for (i = 0; i < 2; ++i) {
                s->out[i] = NULL;
        }
        s->decoded = NULL;

        if(!init_gl_context(&s->gl_context, GL_CONTEXT_ANY)) {
                fprintf(stderr, "[RTDXT] Error initializing GL context");
                return NULL;
        }

        if(strcmp(opts, "help") == 0) {
                printf("DXT GLSL comperssion usage:\n");
                printf("\t-c RTDXT:DXT1\n");
                printf("\t\tcompress with DXT1\n");
                printf("\t-c RTDXT:DXT5\n");
                printf("\t\tcompress with DXT5 YCoCg\n");
                return &compress_init_noerr;
        }
        
        if (strcasecmp(opts, "DXT5") == 0) {
                s->color_spec = DXT5;
        } else if (strcasecmp(opts, "DXT1") == 0) {
                s->color_spec = DXT1;
        } else if (opts[0] == '\0') {
                s->color_spec = DXT1;
        } else {
                fprintf(stderr, "Unknown compression: %s\n", opts);
                return NULL;
        }
                
        s->configured = FALSE;

        gl_context_make_current(NULL);

        module_init_default(&s->module_data);
        s->module_data.cls = MODULE_CLASS_DATA;
        s->module_data.priv_data = s;
        s->module_data.deleter = dxt_glsl_compress_done;
        module_register(&s->module_data, parent);

        return &s->module_data;
}

struct video_frame * dxt_glsl_compress(struct module *mod, struct video_frame * tx, int buffer_idx)
{
        struct state_video_compress_rtdxt *s = (struct state_video_compress_rtdxt *) mod->priv_data;
        int i;
        unsigned char *line1, *line2;

        assert(buffer_idx >= 0 && buffer_idx < 2);
        
        unsigned int x;

        gl_context_make_current(&s->gl_context);
        
        if(!s->configured) {
                int ret;
                ret = configure_with(s, tx);
                if(!ret)
                        return NULL;
        }


        for (x = 0; x < tx->tile_count;  ++x) {
                struct tile *in_tile = vf_get_tile(tx, x);
                struct tile *out_tile = vf_get_tile(s->out[buffer_idx], x);
                
                line1 = (unsigned char *) in_tile->data;
                line2 = (unsigned char *) s->decoded;
                
                for (i = 0; i < (int) in_tile->height; ++i) {
                        s->decoder(line2, line1, s->encoder_input_linesize,
                                        0, 8, 16);
                        line1 += vc_get_linesize(in_tile->width, tx->color_spec);
                        line2 += s->encoder_input_linesize;
                }
                
                if(s->interlaced_input)
                        vc_deinterlace((unsigned char *) s->decoded, s->encoder_input_linesize,
                                        in_tile->height);
                
                dxt_encoder_compress(s->encoder[x],
                                (unsigned char *) s->decoded,
                                (unsigned char *) out_tile->data);
        }

        gl_context_make_current(NULL);
        
        return s->out[buffer_idx];
}

static void dxt_glsl_compress_done(struct module *mod)
{
        struct state_video_compress_rtdxt *s = (struct state_video_compress_rtdxt *) mod->priv_data;
        int i, x;
        
        if(s->out[0]) {
                for(int i = 0; i < (int) s->out[0]->tile_count; ++i) {
                        if(s->encoder[i])
                                dxt_encoder_destroy(s->encoder[i]);
                }
        }

        for (i = 0; i < 2; ++i) {
                if(s->out[i]) {
                        for (x = 0; x < (int) s->out[i]->tile_count; ++x) {
                                free(s->out[i]->tiles[x].data);
                        }
                }
                vf_free(s->out[i]);
        }

        free(s->decoded);

        destroy_gl_context(&s->gl_context);

        free(s);
}

