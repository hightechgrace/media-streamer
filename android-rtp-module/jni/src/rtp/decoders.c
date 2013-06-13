/*
 * AUTHOR:   Ladan Gharai/Colin Perkins
 * 
 * Copyright (c) 2003-2004 University of Southern California
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
 *      This product includes software developed by the University of Southern
 *      California Information Sciences Institute.
 * 
 * 4. Neither the name of the University nor of the Institute may be used
 *    to endorse or promote products derived from this software without
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
#include "debug.h"
#include "host.h"
#include "perf.h"
#include "rtp/ll.h"
#include "rtp/rtp.h"
#include "rtp/rtp_callback.h"
#include "rtp/pbuf.h"
#include "rtp/decoders.h"
#include "video.h"
#include "video_codec.h"
#include "video_decompress.h"
#include "video_display.h"
#include "vo_postprocess.h"

void (*decoder_destroy_extrn)(struct state_decoder *decoder) = decoder_destroy;

struct state_decoder;

static struct video_frame * reconfigure_decoder(struct state_decoder * const decoder, struct video_desc desc,
                                struct video_frame *frame);
typedef void (*change_il_t)(char *dst, char *src, int linesize, int height);
static void restrict_returned_codecs(codec_t *display_codecs,
                size_t *display_codecs_count, codec_t *pp_codecs,
                int pp_codecs_count);
static void decoder_set_video_mode(struct state_decoder *decoder, unsigned int video_mode);
static int check_for_mode_change(struct state_decoder *decoder, uint32_t *hdr, struct video_frame **frame);
static int find_best_decompress(codec_t in_codec, codec_t out_codec,
                int prio_min, int prio_max, uint32_t *magic);
static bool try_initialize_decompress(struct state_decoder * decoder, uint32_t magic);
static void wait_for_framebuffer_swap(struct state_decoder *decoder);
static void *ldgm_thread(void *args);
static void *decompress_thread(void *args);
static void cleanup(struct state_decoder *decoder);

enum decoder_type_t {
        UNSET,
        LINE_DECODER,
        EXTERNAL_DECODER
};

struct line_decoder {
        int                  base_offset; /* from the beginning of buffer */
        double               src_bpp;
        double               dst_bpp;
        int                  rshift;
        int                  gshift;
        int                  bshift;
        decoder_t            decode_line;
        unsigned int         dst_linesize; /* framebuffer pitch */
        unsigned int         dst_pitch; /* framebuffer pitch - it can be larger if SDL resolution is larger than data */
        unsigned int         src_linesize; /* display data pitch */
};

struct fec {
        int               k, m, c;
        int               seed;
        void             *state;
};

struct decompress_data;

struct state_decoder {
        pthread_t decompress_thread_id,
                  ldgm_thread_id;
        struct video_desc received_vid_desc;
        struct video_desc display_desc;

        struct video_frame *frame;
        
        /* requested values */
        int               requested_pitch;
        int               rshift, gshift, bshift;
        unsigned int      max_substreams;
        
        struct display   *display;
        codec_t          *native_codecs;
        size_t            native_count;
        enum interlacing_t    *disp_supported_il;
        size_t            disp_supported_il_cnt;
        change_il_t       change_il;

        pthread_mutex_t lock;
        
        /* actual values */
        enum decoder_type_t decoder_type; 
        struct {
                struct line_decoder *line_decoder;
                struct {
                        struct state_decompress **decompress_state;
                        unsigned int accepts_corrupted_frame:1;
                        pthread_cond_t buffer_swapped_cv;
                        pthread_cond_t decompress_boss_cv;
                        pthread_cond_t decompress_worker_cv;
                        bool decompress_thread_has_processed_input;
                        struct decompress_data *decompress_data;
                };
        };
        codec_t           out_codec;
        // display or postprocessor
        int               pitch;
        // display pitch, can differ from previous if we have postprocessor
        int               display_pitch;
        
        struct {
                struct vo_postprocess_state *postprocess;
                struct video_frame *pp_frame;
                int pp_output_frames_count;
        };

        pthread_cond_t    ldgm_worker_cv;
        pthread_cond_t    ldgm_boss_cv;
        struct ldgm_data *ldgm_data;

        unsigned int      video_mode;
        
        unsigned          merged_fb:1;

        // for statistics
        unsigned long int   displayed;
        unsigned long int   dropped;
        unsigned long int   corrupted;

        double              set_fps; // "message" passed to our master
        codec_t             codec;
        bool                slow;
};

// message definitions
struct ldgm_data {
        int *buffer_len;
        int *buffer_num;
        char **recv_buffers;
        struct linked_list **pckt_list;
        int pt;
        int k, m, c, seed;
        int substream_count;
        bool poisoned;
};

struct decompress_data {
        char **decompress_buffer;
        int *buffer_len;
        int *buffer_num;
        char **fec_buffers;
        bool poisoned;
};

static void wait_for_framebuffer_swap(struct state_decoder *decoder) {
        pthread_mutex_lock(&decoder->lock);
        {
                while (!decoder->decompress_thread_has_processed_input) {
                        pthread_cond_wait(&decoder->buffer_swapped_cv, &decoder->lock);
                }
        }
        pthread_mutex_unlock(&decoder->lock);
}

static void *ldgm_thread(void *args) {
        struct state_decoder *decoder = args;

        struct fec fec_state;
        memset(&fec_state, 0, sizeof(fec_state));

        while(1) {
                struct ldgm_data *data = NULL;
                pthread_mutex_lock(&decoder->lock);
                {
                        while (decoder->ldgm_data == NULL) {
                                pthread_cond_wait(&decoder->ldgm_worker_cv, &decoder->lock);
                        }
                        data = decoder->ldgm_data;
                        decoder->ldgm_data = NULL;

                        pthread_cond_signal(&decoder->ldgm_boss_cv);
                }
                pthread_mutex_unlock(&decoder->lock);

                if(data->poisoned) {
                        // signal it also to decompress thread
                        pthread_mutex_lock(&decoder->lock);
                        {
                                while (decoder->decompress_data) {
                                        pthread_cond_wait(&decoder->decompress_boss_cv, &decoder->lock);
                                }

                                decoder->decompress_data = malloc(sizeof(struct decompress_data));
                                decoder->decompress_data->poisoned = true;

                                /*  ...and signal the worker */
                                pthread_cond_signal(&decoder->decompress_worker_cv);
                        }
                        pthread_mutex_unlock(&decoder->lock);
                        free(data);
                        break; // exit from loop
                }

                struct video_frame *frame = decoder->frame;
                struct tile *tile = NULL;
                int ret = TRUE;
                struct decompress_data *decompress_data = malloc(sizeof(struct decompress_data));
                decompress_data->decompress_buffer = calloc(data->substream_count, sizeof(char *));
                decompress_data->fec_buffers = calloc(data->substream_count, sizeof(char *));
                decompress_data->buffer_len = calloc(data->substream_count, sizeof(int));
                decompress_data->buffer_num = calloc(data->substream_count, sizeof(int));
                memcpy(decompress_data->buffer_num, data->buffer_num, data->substream_count * sizeof(int));
                memcpy(decompress_data->fec_buffers, data->recv_buffers, data->substream_count * sizeof(char *));

                /* PT_VIDEO */
                        for(int i = 0; i < (int) decoder->max_substreams; ++i) {
                                bool corrupted_frame_counted = false;
                                decompress_data->buffer_len[i] = data->buffer_len[i];
                                decompress_data->decompress_buffer[i] = data->recv_buffers[i];

                                if(data->buffer_len[i] != (int) ll_count_bytes(data->pckt_list[i])) {
                                        debug_msg("Frame incomplete - substream %d, buffer %d: expected %u bytes, got %u. ", i,
                                                        (unsigned int) data->buffer_num[i],
                                                        data->buffer_len[i],
                                                        (unsigned int) ll_count_bytes(data->pckt_list[i]));
                                        if(!corrupted_frame_counted) {
                                                corrupted_frame_counted = true;
                                                decoder->corrupted++;
                                        }
                                        if(decoder->decoder_type == EXTERNAL_DECODER && !decoder->accepts_corrupted_frame) {
                                                ret = FALSE;
                                                debug_msg("dropped.\n");
                                                goto cleanup;
                                        }
                                        debug_msg("\n");
                                }
                        }
                

                pthread_mutex_lock(&decoder->lock);
                {
                        while (decoder->decompress_data) {
                                pthread_cond_wait(&decoder->decompress_boss_cv, &decoder->lock);
                        }

                        decoder->decompress_data = decompress_data;
                        decoder->decompress_data->poisoned = false;
                        decoder->decompress_thread_has_processed_input = false;

                        /*  ...and signal the worker */
                        pthread_cond_signal(&decoder->decompress_worker_cv);
                }
                pthread_mutex_unlock(&decoder->lock);

cleanup:
                if(ret == FALSE) {
                        for(int i = 0; i < data->substream_count; ++i) {
                                free(data->recv_buffers[i]);
                        }
                        decoder->corrupted++;
                        decoder->dropped++;
                }
                free(data->buffer_len);
                free(data->buffer_num);
                free(data->recv_buffers);
                for(int i = 0; i < data->substream_count; ++i) {
                        ll_destroy(data->pckt_list[i]);
                }
                free(data->pckt_list);
                free(data);
        }

        return NULL;
}

static void *decompress_thread(void *args) {
        struct state_decoder *decoder = args;

        struct tile *tile;

        while(1) {
                struct decompress_data *data = NULL;
                pthread_mutex_lock(&decoder->lock);
                {
                        while (decoder->decompress_data == NULL) {
                                pthread_cond_wait(&decoder->decompress_worker_cv, &decoder->lock);
                        }
                        // we have double buffering so we can signal immediately....
                        if(decoder->decoder_type == EXTERNAL_DECODER) {
                                decoder->decompress_thread_has_processed_input = true;
                                pthread_cond_signal(&decoder->buffer_swapped_cv);
                        }
                        data = decoder->decompress_data;
                        decoder->decompress_data = NULL;
                        pthread_cond_signal(&decoder->decompress_boss_cv);
                }
                pthread_mutex_unlock(&decoder->lock);

                if(data->poisoned) {
                        free(data);
                        break;
                }

                if(decoder->decoder_type == EXTERNAL_DECODER) {
                        int tile_width = decoder->received_vid_desc.width; // get_video_mode_tiles_x(decoder->video_mode);
                        int tile_height = decoder->received_vid_desc.height; // get_video_mode_tiles_y(decoder->video_mode);
                        int x, y;
                        struct video_frame *output;
                        if(decoder->postprocess) {
                                output = decoder->pp_frame;
                        } else {
                                output = decoder->frame;
                        }
                        for (x = 0; x < get_video_mode_tiles_x(decoder->video_mode); ++x) {
                                for (y = 0; y < get_video_mode_tiles_y(decoder->video_mode); ++y) {
                                        int pos = x + get_video_mode_tiles_x(decoder->video_mode) * y;
                                        char *out;
                                        if(decoder->merged_fb) {
                                                tile = vf_get_tile(output, 0);
                                                // TODO: OK when rendering directly to display FB, otherwise, do not reflect pitch (we use PP)
                                                out = tile->data + y * decoder->pitch * tile_height +
                                                        vc_get_linesize(tile_width, decoder->out_codec) * x;
                                        } else {
                                                tile = vf_get_tile(output, x);
                                                out = tile->data;
                                        }
                                        decompress_frame(decoder->decompress_state[pos],
                                                        (unsigned char *) out,
                                                        (unsigned char *) data->decompress_buffer[pos],
                                                        data->buffer_len[pos],
                                                        data->buffer_num[pos]);
                                }
                        }
                }

                if(decoder->change_il) {
                        unsigned int i;
                        for(i = 0; i < decoder->frame->tile_count; ++i) {
                                struct tile *tile = vf_get_tile(decoder->frame, i);
                                decoder->change_il(tile->data, tile->data, vc_get_linesize(tile->width, decoder->out_codec), tile->height);
                        }
                }

                int putf_flags = 0;
                if(is_codec_interframe(decoder->codec)) {
                        putf_flags = PUTF_NONBLOCK;
                }

                int ret = display_put_frame(decoder->display,
                                decoder->frame, putf_flags);
                if(ret == 0) {
//                    printf("DECODERS GET FRAME PETA1\n");

                        decoder->frame =
                                display_get_frame(decoder->display);
//                        printf("DECODERS GET FRAME PETA2\n");

                } else {
                        decoder->dropped++;
                }

skip_frame:

                for(unsigned int i = 0; i < decoder->max_substreams; ++i) {
                        free(data->fec_buffers[i]);
                }
                free(data->fec_buffers);

                free(data->decompress_buffer);
                free(data->buffer_len);
                free(data->buffer_num);
                free(data);

                pthread_mutex_lock(&decoder->lock);
                {
                        // we have put the video frame and requested another one which is
                        // writable so on
                        if(decoder->decoder_type != EXTERNAL_DECODER) {
                                decoder->decompress_thread_has_processed_input = true;
                                pthread_cond_signal(&decoder->buffer_swapped_cv);
                        }
                }
                pthread_mutex_unlock(&decoder->lock);
        }

        return NULL;
}

static void decoder_set_video_mode(struct state_decoder *decoder, unsigned int video_mode)
{
        decoder->video_mode = video_mode;
        decoder->max_substreams = get_video_mode_tiles_x(decoder->video_mode)
                        * get_video_mode_tiles_y(decoder->video_mode);
}

struct state_decoder *decoder_init(const char *requested_mode, const char *postprocess,
                struct display *display)
{
        struct state_decoder *s;
        
        s = (struct state_decoder *) calloc(1, sizeof(struct state_decoder));
        s->native_codecs = NULL;
        s->disp_supported_il = NULL;
        s->postprocess = NULL;
        s->change_il = NULL;
        s->display = display;

        s->displayed = s->dropped = s->corrupted = 0ul;
        pthread_mutex_init(&s->lock, NULL);
        pthread_cond_init(&s->decompress_boss_cv, NULL);
        pthread_cond_init(&s->decompress_worker_cv, NULL);
        pthread_cond_init(&s->buffer_swapped_cv, NULL);

        pthread_cond_init(&s->ldgm_boss_cv, NULL);
        pthread_cond_init(&s->ldgm_worker_cv, NULL);
        s->ldgm_data = NULL;
        s->decompress_thread_has_processed_input = true;
        
        int video_mode = VIDEO_NORMAL;

        if(requested_mode) {
                /* these are data comming from newtork ! */
                if(strcasecmp(requested_mode, "help") == 0) {
                        printf("Video mode options\n\n");
                        printf("-M {tiled-4K | 3D | dual-link }\n");
                        free(s);
                        exit_uv(129);
                        return NULL;
                } else if(strcasecmp(requested_mode, "tiled-4K") == 0) {
                        video_mode = VIDEO_4K;
                } else if(strcasecmp(requested_mode, "3D") == 0) {
                        video_mode = VIDEO_STEREO;
                } else if(strcasecmp(requested_mode, "dual-link") == 0) {
                        video_mode = VIDEO_DUAL;
                } else {
                        fprintf(stderr, "[decoder] Unknown video mode (see -M help)\n");
                        free(s);
                        exit_uv(129);
                        return NULL;
                }
        }

        decoder_set_video_mode(s, video_mode);

        if(!decoder_register_video_display(s, display))
                return NULL;

        return s;
}

static void restrict_returned_codecs(codec_t *display_codecs,
                size_t *display_codecs_count, codec_t *pp_codecs,
                int pp_codecs_count)
{
        int i;

        for (i = 0; i < (int) *display_codecs_count; ++i) {
                int j;

                int found = FALSE;

                for (j = 0; j < pp_codecs_count; ++j) {
                        if(display_codecs[i] == pp_codecs[j]) {
                                found = TRUE;
                        }
                }

                if(!found) {
                        memmove(&display_codecs[i], (const void *) &display_codecs[i + 1],
                                        sizeof(codec_t) * (*display_codecs_count - i - 1));
                        --*display_codecs_count;
                        --i;
                }
        }
}

bool decoder_register_video_display(struct state_decoder *decoder, struct display *display)
{
        int ret, i;
        decoder->display = display;
        
        free(decoder->native_codecs);
        decoder->native_count = 20 * sizeof(codec_t);
        decoder->native_codecs = (codec_t *)
                malloc(decoder->native_count * sizeof(codec_t));
        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_CODECS, decoder->native_codecs, &decoder->native_count);
        decoder->native_count /= sizeof(codec_t);
        if(!ret) {
                fprintf(stderr, "Failed to query codecs from video display.\n");
                return false;
        }
        
        /* next check if we didn't receive alias for UYVY */
        for(i = 0; i < (int) decoder->native_count; ++i) {
                assert(decoder->native_codecs[i] != Vuy2 &&
                                decoder->native_codecs[i] != DVS8);
        }

        free(decoder->disp_supported_il);
        decoder->disp_supported_il_cnt = 20 * sizeof(enum interlacing_t);
        decoder->disp_supported_il = malloc(decoder->disp_supported_il_cnt);
        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_SUPPORTED_IL_MODES, decoder->disp_supported_il, &decoder->disp_supported_il_cnt);
        if(ret) {
                decoder->disp_supported_il_cnt /= sizeof(enum interlacing_t);
        } else {
                enum interlacing_t tmp[] = { PROGRESSIVE, INTERLACED_MERGED, SEGMENTED_FRAME}; /* default if not said othervise */
                memcpy(decoder->disp_supported_il, tmp, sizeof(tmp));
                decoder->disp_supported_il_cnt = sizeof(tmp) / sizeof(enum interlacing_t);
        }

        // Start decompress and ldmg threads
        if(pthread_create(&decoder->decompress_thread_id, NULL, decompress_thread,
                                decoder) != 0) {
                perror("Unable to create thread");
                return false;
        }

        if(pthread_create(&decoder->ldgm_thread_id, NULL, ldgm_thread,
                                decoder) != 0) {
                perror("Unable to create thread");
                return false;
        }

        return true;
}

/**
 * This removes display from current decoder. From now on
 */
void decoder_remove_display(struct state_decoder *decoder)
{
        if(decoder->display) {
                pthread_mutex_lock(&decoder->lock);
                {
                        while (decoder->ldgm_data) {
                                pthread_cond_wait(&decoder->ldgm_boss_cv, &decoder->lock);
                        }

                        decoder->ldgm_data = malloc(sizeof(struct ldgm_data));
                        decoder->ldgm_data->poisoned = true;

                        /*  ...and signal the worker */
                        pthread_cond_signal(&decoder->ldgm_worker_cv);
                }
                pthread_mutex_unlock(&decoder->lock);

                pthread_join(decoder->ldgm_thread_id, NULL);
                pthread_join(decoder->decompress_thread_id, NULL);

                decoder->display = NULL;
        }
}

static void cleanup(struct state_decoder *decoder)
{
        decoder->decoder_type = UNSET;
        if(decoder->decompress_state) {
                for(unsigned int i = 0; i < decoder->max_substreams; ++i) {
                        decompress_done(decoder->decompress_state[i]);
                }
                free(decoder->decompress_state);
                decoder->decompress_state = NULL;
        }
        if(decoder->line_decoder) {
                free(decoder->line_decoder);
                decoder->line_decoder = NULL;
        }
}

void decoder_destroy(struct state_decoder *decoder)
{
        if(!decoder)
                return;

        decoder_remove_display(decoder);

        pthread_mutex_destroy(&decoder->lock);
        pthread_cond_destroy(&decoder->decompress_boss_cv);
        pthread_cond_destroy(&decoder->decompress_worker_cv);

        pthread_cond_destroy(&decoder->ldgm_boss_cv);
        pthread_cond_destroy(&decoder->ldgm_worker_cv);

        cleanup(decoder);

        free(decoder->native_codecs);
        free(decoder->disp_supported_il);

        fprintf(stderr, "Decoder statistics: %lu displayed frames / %lu frames dropped (%lu corrupted)\n",
                        decoder->displayed, decoder->dropped, decoder->corrupted);
        free(decoder);
}

/**
 * Attemps to initialize decompress of given magic
 *
 * @param decoder decoder state
 * @param magic magic of the requested decompressor
 * @return flat if initialization succeeded
 */
static bool try_initialize_decompress(struct state_decoder * decoder, uint32_t magic) {
        decoder->decompress_state = (struct state_decompress **)
                calloc(decoder->max_substreams, sizeof(struct state_decompress *));
        for(unsigned int i = 0; i < decoder->max_substreams; ++i) {
                decoder->decompress_state[i] = decompress_init(magic);

                if(!decoder->decompress_state[i]) {
                        debug_msg("Decompressor with magic %x was not found.\n");
                        for(unsigned int j = 0; j < decoder->max_substreams; ++j) {
                                decompress_done(decoder->decompress_state[i]);
                        }
                        free(decoder->decompress_state);
                        return false;
                }
        }


        int res = 0, ret;
        size_t size = sizeof(res);
        ret = decompress_get_property(decoder->decompress_state[0],
                        DECOMPRESS_PROPERTY_ACCEPTS_CORRUPTED_FRAME,
                        &res,
                        &size);
        if(ret && res) {
                decoder->accepts_corrupted_frame = TRUE;
        } else {
                decoder->accepts_corrupted_frame = FALSE;
        }

        decoder->decoder_type = EXTERNAL_DECODER;
        return true;
}

/**
 * @param[in] in_codec input codec
 * @param[in] out_codec output codec
 * @param[in] prio_min minimal priority that can be probed
 * @param[in] prio_max maximal priority that can be probed
 * @param[out] magic if decompressor was found here is stored its magic
 * @retval -1 if no found
 * @retval priority best decoder's priority
 */
static int find_best_decompress(codec_t in_codec, codec_t out_codec,
                int prio_min, int prio_max, uint32_t *magic) {
        int trans;
        int best_priority = prio_max + 1;
        // first pass - find the one with best priority (least)
        for(trans = 0; trans < decoders_for_codec_count;
                        ++trans) {
                if(in_codec == decoders_for_codec[trans].from &&
                                out_codec == decoders_for_codec[trans].to) {
                        int priority = decoders_for_codec[trans].priority;
                        if(priority <= prio_max &&
                                        priority >= prio_min &&
                                        priority < best_priority) {
                                if(decompress_is_available(
                                                        decoders_for_codec[trans].decompress_index)) {
                                        best_priority = priority;
                                        *magic = decoders_for_codec[trans].decompress_index;
                                }
                        }
                }
        }

        if(best_priority == prio_max + 1)
                return -1;
        return best_priority;
}

static codec_t choose_codec_and_decoder(struct state_decoder * const decoder, struct video_desc desc,
                                codec_t *in_codec, decoder_t *decode_line)
{
        codec_t out_codec = (codec_t) -1;
        *decode_line = NULL;
        *in_codec = desc.color_spec;
        
        /* first deal with aliases */
        if(*in_codec == DVS8 || *in_codec == Vuy2) {
        	printf("if(*in_codec == DVS8 || *in_codec == Vuy2)\n");
                *in_codec = UYVY;
        }
        
        size_t native;
        /* first check if the codec is natively supported */
        for(native = 0u; native < decoder->native_count; ++native)
        {
                out_codec = decoder->native_codecs[native];
                if(out_codec == DVS8 || out_codec == Vuy2){
                	printf("if(out_codec == DVS8 || out_codec == Vuy2)\n");
                        out_codec = UYVY;
                }if(*in_codec == out_codec) {
                        if((out_codec == DXT1 || out_codec == DXT1_YUV ||
                                        out_codec == DXT5)
                                        && decoder->video_mode != VIDEO_NORMAL)
                                continue; /* it is a exception, see NOTES #1 */
                        if(*in_codec == RGBA || /* another exception - we may change shifts */
                                        *in_codec == RGB)
                                continue;

                        *decode_line = (decoder_t) memcpy;
                        decoder->decoder_type = LINE_DECODER;

                        goto after_linedecoder_lookup;
                }
        }
        /* otherwise if we have line decoder */
        int trans;
        for(trans = 0; line_decoders[trans].line_decoder != NULL;
                                ++trans) {
                for(native = 0; native < decoder->native_count; ++native)
                {
                        out_codec = decoder->native_codecs[native];
                        if(out_codec == DVS8 || out_codec == Vuy2)
                                out_codec = UYVY;
                        if(*in_codec == line_decoders[trans].from &&
                                        out_codec == line_decoders[trans].to) {
                                                
                                *decode_line = line_decoders[trans].line_decoder;
                                
                                decoder->decoder_type = LINE_DECODER;
                                goto after_linedecoder_lookup;
                        }
                }
        }
        
after_linedecoder_lookup:

        /* we didn't find line decoder. So try now regular (aka DXT) decoder */
        if(*decode_line == NULL) {
                for(native = 0; native < decoder->native_count; ++native)
                {
                        out_codec = decoder->native_codecs[native];
                        if(out_codec == DVS8 || out_codec == Vuy2)
                                out_codec = UYVY;

                        int prio_max = 1000;
                        int prio_min = 0;
                        int prio_cur;
                        uint32_t decompress_magic = 0u;

                        while(1) {
                                prio_cur = find_best_decompress(*in_codec, out_codec,
                                                prio_min, prio_max, &decompress_magic);
                                // if found, init decoder
                                if(prio_cur != -1) {
                                        if(try_initialize_decompress(decoder, decompress_magic)) {
                                                goto after_decoder_lookup;
                                        } else {
                                                // failed, try to find another one
                                                prio_min = prio_cur + 1;
                                                continue;
                                        }
                                } else {
                                        break;
                                }
                        }
                }
        }
after_decoder_lookup:

        if(decoder->decoder_type == UNSET) {
                fprintf(stderr, "Unable to find decoder for input codec \"%s\"!!!\n", get_codec_name(desc.color_spec));
                exit_uv(128);
                return (codec_t) -1;
        }
        
        decoder->out_codec = out_codec;
        return out_codec;
}

static change_il_t select_il_func(enum interlacing_t in_il, enum interlacing_t *supported, int il_out_cnt, /*out*/ enum interlacing_t *out_il)
{
        struct transcode_t { enum interlacing_t in; enum interlacing_t out; change_il_t func; };

        struct transcode_t transcode[] = {
                {UPPER_FIELD_FIRST, INTERLACED_MERGED, il_upper_to_merged},
                {INTERLACED_MERGED, UPPER_FIELD_FIRST, il_merged_to_upper}
        };

        int i;
        /* first try to check if it can be nativelly displayed */
        for (i = 0; i < il_out_cnt; ++i) {
                if(in_il == supported[i]) {
                        *out_il = in_il;
                        return NULL;
                }
        }

        for (i = 0; i < il_out_cnt; ++i) {
                size_t j;
                for (j = 0; j < sizeof(transcode) / sizeof(struct transcode_t); ++j) {
                        if(in_il == transcode[j].in && supported[i] == transcode[j].out) {
                                *out_il = transcode[j].out;
                                return transcode[j].func;
                        }
                }
        }

        return NULL;
}

static struct video_frame * reconfigure_decoder(struct state_decoder * const decoder,
                struct video_desc desc, struct video_frame *frame_display)
{
        codec_t out_codec, in_codec;
        decoder_t decode_line;
        enum interlacing_t display_il = PROGRESSIVE;
        //struct video_frame *frame;
        int render_mode;

        wait_for_framebuffer_swap(decoder);

        assert(decoder != NULL);
        assert(decoder->native_codecs != NULL);

        cleanup(decoder);

        desc.tile_count = get_video_mode_tiles_x(decoder->video_mode)
                        * get_video_mode_tiles_y(decoder->video_mode);
        
        out_codec = choose_codec_and_decoder(decoder, desc, &in_codec, &decode_line);

        if(out_codec == (codec_t) -1)
                return NULL;
        struct video_desc display_desc = desc;

        int display_mode;
        size_t len = sizeof(int);
        int ret;

        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_VIDEO_MODE,
                        &display_mode, &len);
        if(!ret) {
                debug_msg("Failed to get video display mode.");
                display_mode = DISPLAY_PROPERTY_VIDEO_MERGED;
        }

        bool pp_does_change_tiling_mode = false;

		if(display_mode == DISPLAY_PROPERTY_VIDEO_MERGED) {
				display_desc.width *= get_video_mode_tiles_x(decoder->video_mode);
				display_desc.height *= get_video_mode_tiles_y(decoder->video_mode);
				display_desc.tile_count = 1;
		}


        if(!is_codec_opaque(out_codec)) {
                decoder->change_il = select_il_func(desc.interlacing, decoder->disp_supported_il, decoder->disp_supported_il_cnt, &display_il);
        } else {
                decoder->change_il = NULL;
        }

        if (!decoder->postprocess || !pp_does_change_tiling_mode) { /* otherwise we need postprocessor mode, which we obtained before */
                render_mode = display_mode;
        }

        display_desc.color_spec = out_codec;
        display_desc.interlacing = display_il;

        if(!video_desc_eq(decoder->display_desc, display_desc))
        {
                int ret;
                /*
                 * TODO: put frame should be definitely here. On the other hand, we cannot be sure
                 * that vo driver is initialized so far:(
                 */
                //display_put_frame(decoder->display, frame);
                /* reconfigure VO and give it opportunity to pass us pitch */
                ret = display_reconfigure(decoder->display, display_desc);
                if(!ret) {
                        printf(stderr, "[decoder] Unable to reconfigure display.\n");
                        exit_uv(128);
                        return NULL;
                }
//                printf("DECODERS GET FRAME PETA3\n");

                frame_display = display_get_frame(decoder->display);
//                printf("DECODERS GET FRAME PETA4\n");

                decoder->display_desc = display_desc;
        }
        /*if(decoder->postprocess) {
                frame = decoder->pp_frame;
        } else {
                frame = frame_display;
        }*/

        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_RSHIFT,
                        &decoder->rshift, &len);
        if(!ret) {
                debug_msg("Failed to get rshift property from video driver.\n");
                decoder->rshift = 0;
        }
        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_GSHIFT,
                        &decoder->gshift, &len);
        if(!ret) {
                debug_msg("Failed to get gshift property from video driver.\n");
                decoder->gshift = 8;
        }
        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_BSHIFT,
                        &decoder->bshift, &len);
        if(!ret) {
                debug_msg("Failed to get bshift property from video driver.\n");
                decoder->bshift = 16;
        }
        
        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_BUF_PITCH,
                        &decoder->requested_pitch, &len);
        if(!ret) {
                debug_msg("Failed to get pitch from video driver.\n");
                decoder->requested_pitch = PITCH_DEFAULT;
        }
        
        int linewidth;
        if(render_mode == DISPLAY_PROPERTY_VIDEO_SEPARATE_TILES) {
                linewidth = desc.width; 
        } else {
                linewidth = desc.width * get_video_mode_tiles_x(decoder->video_mode);
        }

        if(!decoder->postprocess) {
                if(decoder->requested_pitch == PITCH_DEFAULT)
                        decoder->pitch = vc_get_linesize(linewidth, out_codec);
                else
                        decoder->pitch = decoder->requested_pitch;
        } else {
                decoder->pitch = vc_get_linesize(linewidth, out_codec);
        }

        if(decoder->requested_pitch == PITCH_DEFAULT) {
                decoder->display_pitch = vc_get_linesize(display_desc.width, out_codec);
        } else {
                decoder->display_pitch = decoder->requested_pitch;
        }

        int src_x_tiles = get_video_mode_tiles_x(decoder->video_mode);
        int src_y_tiles = get_video_mode_tiles_y(decoder->video_mode);
        
        if(decoder->decoder_type == LINE_DECODER) {
                decoder->line_decoder = malloc(src_x_tiles * src_y_tiles *
                                        sizeof(struct line_decoder));                
                if(render_mode == DISPLAY_PROPERTY_VIDEO_MERGED && decoder->video_mode == VIDEO_NORMAL) {
                        struct line_decoder *out = &decoder->line_decoder[0];
                        out->base_offset = 0;
                        out->src_bpp = get_bpp(in_codec);
                        out->dst_bpp = get_bpp(out_codec);
                        out->rshift = decoder->rshift;
                        out->gshift = decoder->gshift;
                        out->bshift = decoder->bshift;
                
                        out->decode_line = decode_line;
                        out->dst_pitch = decoder->pitch;
                        out->src_linesize = vc_get_linesize(desc.width, in_codec);
                        out->dst_linesize = vc_get_linesize(desc.width, out_codec);
                        decoder->merged_fb = TRUE;
                } else if(render_mode == DISPLAY_PROPERTY_VIDEO_MERGED
                                && decoder->video_mode != VIDEO_NORMAL) {
                        int x, y;
                        for(x = 0; x < src_x_tiles; ++x) {
                                for(y = 0; y < src_y_tiles; ++y) {
                                        struct line_decoder *out = &decoder->line_decoder[x + 
                                                        src_x_tiles * y];
                                        out->base_offset = y * (desc.height)
                                                        * decoder->pitch + 
                                                        vc_get_linesize(x * desc.width, out_codec);

                                        out->src_bpp = get_bpp(in_codec);
                                        out->dst_bpp = get_bpp(out_codec);

                                        out->rshift = decoder->rshift;
                                        out->gshift = decoder->gshift;
                                        out->bshift = decoder->bshift;
                
                                        out->decode_line = decode_line;

                                        out->dst_pitch = decoder->pitch;
                                        out->src_linesize =
                                                vc_get_linesize(desc.width, in_codec);
                                        out->dst_linesize =
                                                vc_get_linesize(desc.width, out_codec);
                                }
                        }
                        decoder->merged_fb = TRUE;
                } else if (render_mode == DISPLAY_PROPERTY_VIDEO_SEPARATE_TILES) {
                        int x, y;
                        for(x = 0; x < src_x_tiles; ++x) {
                                for(y = 0; y < src_y_tiles; ++y) {
                                        struct line_decoder *out = &decoder->line_decoder[x + 
                                                        src_x_tiles * y];
                                        out->base_offset = 0;
                                        out->src_bpp = get_bpp(in_codec);
                                        out->dst_bpp = get_bpp(out_codec);
                                        out->rshift = decoder->rshift;
                                        out->gshift = decoder->gshift;
                                        out->bshift = decoder->bshift;
                
                                        out->decode_line = decode_line;
                                        out->src_linesize =
                                                vc_get_linesize(desc.width, in_codec);
                                        out->dst_pitch = 
                                                out->dst_linesize =
                                                vc_get_linesize(desc.width, out_codec);
                                }
                        }
                        decoder->merged_fb = FALSE;
                }
        } else if (decoder->decoder_type == EXTERNAL_DECODER) {
                int buf_size;
                
                for(unsigned int i = 0; i < decoder->max_substreams; ++i) {
                        buf_size = decompress_reconfigure(decoder->decompress_state[i], desc,
                                        decoder->rshift, decoder->gshift, decoder->bshift, decoder->pitch,
                                        out_codec);
                        if(!buf_size) {
                                return NULL;
                        }
                }
                if(render_mode == DISPLAY_PROPERTY_VIDEO_SEPARATE_TILES) {
                        decoder->merged_fb = FALSE;
                } else {
                        decoder->merged_fb = TRUE;
                }
        }
        
        return frame_display;
}

static int check_for_mode_change(struct state_decoder *decoder, uint32_t *hdr, struct video_frame **frame)
{
        uint32_t tmp;
        int ret = FALSE;
        unsigned int width, height;
        codec_t color_spec;
        enum interlacing_t interlacing;
        double fps;
        int fps_pt, fpsd, fd, fi;

        width = ntohl(hdr[3]) >> 16;
        height = ntohl(hdr[3]) & 0xffff;
        color_spec = get_codec_from_fcc(hdr[4]);
        if(color_spec == (codec_t) -1) {
                fprintf(stderr, "Unknown FourCC \"%4s\"!\n", (char *) &hdr[4]);
        }

        tmp = ntohl(hdr[5]);
        interlacing = (enum interlacing_t) (tmp >> 29);
        fps_pt = (tmp >> 19) & 0x3ff;
        fpsd = (tmp >> 15) & 0xf;
        fd = (tmp >> 14) & 0x1;
        fi = (tmp >> 13) & 0x1;

        fps = compute_fps(fps_pt, fpsd, fd, fi);
        if (!(decoder->received_vid_desc.width == width &&
                                decoder->received_vid_desc.height == height &&
                                decoder->received_vid_desc.color_spec == color_spec &&
                                decoder->received_vid_desc.interlacing == interlacing  &&
                                //decoder->received_vid_desc.video_type == video_type &&
                                decoder->received_vid_desc.fps == fps
             )) {
                printf("New incoming video format detected: %dx%d @%.2f%s, codec %s\n",
                                width, height, fps,
                                get_interlacing_suffix(interlacing),
                                get_codec_name(color_spec));
                decoder->received_vid_desc = (struct video_desc) {
                        .width = width, 
                        .height = height,
                        .color_spec = color_spec,
                        .interlacing = interlacing,
                        .fps = fps };

                *frame = reconfigure_decoder(decoder, decoder->received_vid_desc,
                                *frame);
                decoder->frame = *frame;
                ret = TRUE;
                decoder->set_fps = fps;
                decoder->codec = color_spec;
        }
        return ret;
}

int decode_frame(struct coded_data *cdata, void *decode_data)
{
        struct vcodec_state *pbuf_data = (struct vcodec_state *) decode_data;
        struct state_decoder *decoder = pbuf_data->decoder;
        struct video_frame *frame = decoder->frame;

        int ret = TRUE;
        uint32_t offset;
        int len;
        rtp_packet *pckt = NULL;
        unsigned char *source;
        char *data;
        uint32_t data_pos;
        int prints=0;
        struct tile *tile = NULL;
        uint32_t tmp;
        uint32_t substream;

        int i;
        struct linked_list *pckt_list[decoder->max_substreams];
        uint32_t buffer_len[decoder->max_substreams];
        uint32_t buffer_num[decoder->max_substreams];
        // the following is just LDGM related optimalization - normally we fill up
        // allocated buffers when we have compressed data. But in case of LDGM, there
        // is just the LDGM buffer present, so we point to it instead to copying
        char *recv_buffers[decoder->max_substreams]; // for FEC or compressed data
        for (i = 0; i < (int) decoder->max_substreams; ++i) {
                pckt_list[i] = ll_create();
                buffer_len[i] = 0;
                buffer_num[i] = 0;
                recv_buffers[i] = NULL;
        }

        perf_record(UVP_DECODEFRAME, frame);

        // We have no framebuffer assigned, exitting
        if(!decoder->display) {
                return FALSE;
        }
        
        int k = 0, m = 0, c = 0, seed = 0; // LDGM
        int buffer_number, buffer_length;

        // first, dispatch "messages"
        if(decoder->set_fps) {
                struct vcodec_message *msg = malloc(sizeof(struct vcodec_message));
                struct fps_changed_message *data = malloc(sizeof(struct fps_changed_message));
                msg->type = FPS_CHANGED;
                msg->data = data;
                data->val = decoder->set_fps;
                data->interframe_codec = is_codec_interframe(decoder->codec);
                simple_linked_list_append(pbuf_data->messages, msg);
                decoder->set_fps = 0;
        }

        int pt;

        while (cdata != NULL) {
                uint32_t *hdr;
                pckt = cdata->data;

                pt = pckt->pt;
                hdr = (uint32_t *)(void *) pckt->data;
                data_pos = ntohl(hdr[1]);
                tmp = ntohl(hdr[0]);

                substream = tmp >> 22;
                buffer_number = tmp & 0x3ffff;
                buffer_length = ntohl(hdr[2]);

                if(pt == PT_VIDEO) {
                        len = pckt->data_len - sizeof(video_payload_hdr_t);
                        data = (char *) hdr + sizeof(video_payload_hdr_t);
                } else if (pt == PT_VIDEO_LDGM) {
                        len = pckt->data_len - sizeof(ldgm_video_payload_hdr_t);
                        data = (char *) hdr + sizeof(ldgm_video_payload_hdr_t);

                        tmp = ntohl(hdr[3]);
                        k = tmp >> 19;
                        m = 0x1fff & (tmp >> 6);
                        c = 0x3f & tmp;
                        seed = ntohl(hdr[4]);
                } else {
                        fprintf(stderr, "[decoder] Unknown packet type: %d.\n", pckt->pt);
                        exit_uv(1);
                        ret = FALSE;
                        goto cleanup;
                }

                if(substream >= decoder->max_substreams) {
                        fprintf(stderr, "[decoder] received substream ID %d. Expecting at most %d substreams. Did you set -M option?\n",
                                        substream, decoder->max_substreams);
                        // the guess is valid - we start with highest substream number (anytime - since it holds a m-bit)
                        // in next iterations, index is valid
                        if(substream == 1 || substream == 3) {
                                fprintf(stderr, "[decoder] Guessing mode: ");
                                if(substream == 1) {
                                        decoder_set_video_mode(decoder, VIDEO_STEREO);
                                } else {
                                        decoder_set_video_mode(decoder, VIDEO_4K);
                                }
                                decoder->received_vid_desc.width = 0; // just for sure, that we reconfigure in next iteration
                                fprintf(stderr, "%s\n", get_video_mode_description(decoder->video_mode));
                        } else {
                                exit_uv(1);
                        }
                        // we need skip this frame (variables are illegal in this iteration
                        // and in case that we got unrecognized number of substreams - exit
                        ret = FALSE;
                        goto cleanup;
                }

                if(!recv_buffers[substream]) {
                        recv_buffers[substream] = (char *) malloc(buffer_length);
                }

                buffer_num[substream] = buffer_number;
                buffer_len[substream] = buffer_length;

                ll_insert(pckt_list[substream], data_pos, len);
                
                if (pt == PT_VIDEO) {
                        /* Critical section 
                         * each thread *MUST* wait here if this condition is true
                         */
                        struct video_frame *new_frame_buffer;
                        if(check_for_mode_change(decoder, (uint32_t *)(void *)
                                                pckt->data, &new_frame_buffer)) {
                                frame = new_frame_buffer;
                        }
                }

                if(pt == PT_VIDEO && !frame) {
                        ret = FALSE;
                        goto cleanup;
                }

                if (pt == PT_VIDEO && decoder->decoder_type == LINE_DECODER) {
                        wait_for_framebuffer_swap(decoder);

                        if(!decoder->postprocess) {
                        	    if (!decoder->merged_fb) {
                        	    	//printf("!decoder->postprocess   substream\n");
                                        tile = vf_get_tile(frame, substream);
                                } else {
                                	//printf("!decoder->postprocess   0\n");
                                        tile = vf_get_tile(frame, 0);
                                }
                        } else {
                                if (!decoder->merged_fb) {
                                        tile = vf_get_tile(decoder->pp_frame, substream);
                                } else {
                                        tile = vf_get_tile(decoder->pp_frame, 0);
                                }
                        }

                        struct line_decoder *line_decoder = 
                                &decoder->line_decoder[substream];

                        /* End of critical section */

                        /* MAGIC, don't touch it, you definitely break it 
                         *  *source* is data from network, *destination* is frame buffer
                         */

                        /* compute Y pos in source frame and convert it to 
                         * byte offset in the destination frame
                         */
                        int y = (data_pos / line_decoder->src_linesize) * line_decoder->dst_pitch;

                        /* compute X pos in source frame */
                        int s_x = data_pos % line_decoder->src_linesize;

                        /* convert X pos from source frame into the destination frame.
                         * it is byte offset from the beginning of a line. 
                         */
                        int d_x = ((int)((s_x) / line_decoder->src_bpp)) *
                                line_decoder->dst_bpp;

                        /* pointer to data payload in packet */
                        source = (unsigned char*)(data);

                        /* copy whole packet that can span several lines. 
                         * we need to clip data (v210 case) or center data (RGBA, R10k cases)
                         */
                        while (len > 0) {
                                /* len id payload length in source BPP
                                 * decoder needs len in destination BPP, so convert it 
                                 */
                                int l = ((int)(len / line_decoder->src_bpp)) * line_decoder->dst_bpp;

                                /* do not copy multiple lines, we need to 
                                 * copy (& clip, center) line by line 
                                 */
                                if (l + d_x > (int) line_decoder->dst_linesize) {
                                        l = line_decoder->dst_linesize - d_x;
                                }

                                /* compute byte offset in destination frame */
                                offset = y + d_x;

//                                printf("tile->data_len: %d\n",tile->data_len);

                                /* watch the SEGV */
                                if (l + line_decoder->base_offset + offset <= tile->data_len) {
//                                	printf("tile->data_len: %d\n",tile->data_len);
//                                	printf("voy a probar:\n");
//                                	printf("tile->data+0: %d\n",*(tile->data+0));
//                                	printf("success\n");
                                        /*decode frame:
                                         * we have offset for destination
                                         * we update source contiguously
                                         * we pass {r,g,b}shifts */

                                        line_decoder->decode_line((unsigned char*)tile->data + line_decoder->base_offset + offset, source, l,
                                                        line_decoder->rshift, line_decoder->gshift,
                                                        line_decoder->bshift);
                                        /* we decoded one line (or a part of one line) to the end of the line
                                         * so decrease *source* len by 1 line (or that part of the line */
                                        len -= line_decoder->src_linesize - s_x;
                                        /* jump in source by the same amount */
                                        source += line_decoder->src_linesize - s_x;
                                } else {
                                        /* this should not ever happen as we call reconfigure before each packet
                                         * iff reconfigure is needed. But if it still happens, something is terribly wrong
                                         * say it loudly
                                         */
                                        if((prints % 100) == 0) {
                                                fprintf(stderr, "WARNING!! Discarding input data as frame buffer is too small.\n"
                                                                "Well this should not happened. Expect troubles pretty soon.\n");
                                        }
                                        prints++;
                                        len = 0;
                                }
                                /* each new line continues from the beginning */
                                d_x = 0;        /* next line from beginning */
                                s_x = 0;
                                y += line_decoder->dst_pitch;  /* next line */
                        }
                } else { /* PT_VIDEO_LDGM or external decoder */
                        memcpy(recv_buffers[substream] + data_pos, (unsigned char*) data,
                                len);
                }

                cdata = cdata->nxt;
        }

        if(!pckt) {
                ret = FALSE;
                goto cleanup;
        }

        assert(ret == TRUE);

        // format message
        struct ldgm_data *ldgm_data = malloc(sizeof(struct ldgm_data));
        ldgm_data->buffer_len = malloc(sizeof(buffer_len));
        ldgm_data->buffer_num = malloc(sizeof(buffer_num));
        ldgm_data->recv_buffers = malloc(sizeof(recv_buffers));
        ldgm_data->pckt_list = malloc(sizeof(pckt_list));
        ldgm_data->k = k;
        ldgm_data->m = m;
        ldgm_data->c = c;
        ldgm_data->seed = seed;
        ldgm_data->substream_count = decoder->max_substreams;
        ldgm_data->pt = pt;
        ldgm_data->poisoned = false;
        memcpy(ldgm_data->buffer_len, buffer_len, sizeof(buffer_len));
        memcpy(ldgm_data->buffer_num, buffer_num, sizeof(buffer_num));
        memcpy(ldgm_data->recv_buffers, recv_buffers, sizeof(recv_buffers));
        memcpy(ldgm_data->pckt_list, pckt_list, sizeof(pckt_list));

        pthread_mutex_lock(&decoder->lock);
        {
                if(decoder->ldgm_data && !decoder->slow) {
                        fprintf(stderr, "Your computer is too SLOW to play this !!!\n");
                        decoder->slow = true;
                } else {
                        decoder->slow = false;
                }

                while (decoder->ldgm_data) {
                        pthread_cond_wait(&decoder->ldgm_boss_cv, &decoder->lock);
                }

                decoder->ldgm_data = ldgm_data;

                /*  ...and signal the worker */
                pthread_cond_signal(&decoder->ldgm_worker_cv);
        }
        pthread_mutex_unlock(&decoder->lock);

cleanup:
        ;
        unsigned int frame_size = 0;

        for(i = 0; i < (int) (sizeof(pckt_list) / sizeof(struct linked_list *)); ++i) {

                if(ret != TRUE) {
                        free(recv_buffers[i]);
                        ll_destroy(pckt_list[i]);
                }

                frame_size += buffer_len[i];
        }

        pbuf_data->max_frame_size = max(pbuf_data->max_frame_size, frame_size);

        if(ret) {
                decoder->displayed++;
                pbuf_data->decoded++;
        } else {
                decoder->dropped++;
        }

        if(decoder->displayed % 600 == 599) {
                fprintf(stderr, "Decoder statistics: %lu displayed frames / %lu frames dropped (%lu corrupted)\n",
                                decoder->displayed, decoder->dropped, decoder->corrupted);
        }

        return ret;
}
