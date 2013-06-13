/*
 * FILE:    main.c
 * AUTHORS: Colin Perkins    <csp@csperkins.org>
 *          Ladan Gharai     <ladan@isi.edu>
 *          Martin Benes     <martinbenesh@gmail.com>
 *          Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 *          Petr Holub       <hopet@ics.muni.cz>
 *          Milos Liska      <xliska@fi.muni.cz>
 *          Jiri Matela      <matela@ics.muni.cz>
 *          Dalibor Matura   <255899@mail.muni.cz>
 *          Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 *
 * Copyright (c) 2005-2010 CESNET z.s.p.o.
 * Copyright (c) 2001-2004 University of Southern California
 * Copyright (c) 2003-2004 University of Glasgow
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
 *      California Information Sciences Institute. This product also includes
 *      software developed by CESNET z.s.p.o.
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

#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <pthread.h>
#include "capture_filter.h"
#include "debug.h"
#include "host.h"
#include "perf.h"
#include "rtp/decoders.h"
#include "rtp/rtp.h"
#include "rtp/rtp_callback.h"
#include "rtp/pbuf.h"
#include "video_codec.h"
#include "video_capture.h"
#include "video_display.h"
#include "video_compress.h"
#include "video_decompress.h"
#include "video_export.h"
#include "pdb.h"
#include "tv.h"
#include "transmit.h"
#include "tfrc.h"
#include "lib_common.h"
#include "compat/platform_semaphore.h"
#include "audio/audio.h"
#include "audio/codec.h"
#include "audio/utils.h"
#include <unistd.h>

#if defined DEBUG && defined HAVE_LINUX
#include <mcheck.h>
#endif

#define EXIT_FAIL_USAGE		1
#define EXIT_FAIL_UI   		2
#define EXIT_FAIL_DISPLAY	3
#define EXIT_FAIL_CAPTURE	4
#define EXIT_FAIL_NETWORK	5
#define EXIT_FAIL_TRANSMIT	6
#define EXIT_FAIL_COMPRESS	7
#define EXIT_FAIL_DECODER	8

#define PORT_BASE               5004
#define PORT_AUDIO              5006

/* please see comments before transmit.c:audio_tx_send() */
/* also note that this actually differs from video */
#define DEFAULT_AUDIO_FEC       "mult:3"

#define OPT_AUDIO_CHANNEL_MAP (('a' << 8) | 'm')
#define OPT_AUDIO_CAPTURE_CHANNELS (('a' << 8) | 'c')
#define OPT_AUDIO_SCALE (('a' << 8) | 's')
#define OPT_ECHO_CANCELLATION (('E' << 8) | 'C')
#define OPT_CUDA_DEVICE (('C' << 8) | 'D')
#define OPT_MCAST_IF (('M' << 8) | 'I')
#define OPT_EXPORT (('E' << 8) | 'X')
#define OPT_IMPORT (('I' << 8) | 'M')
#define OPT_AUDIO_CODEC (('A' << 8) | 'C')
#define OPT_CAPTURE_FILTER (('O' << 8) | 'F')

#ifdef HAVE_MACOSX
#define INITIAL_VIDEO_RECV_BUFFER_SIZE  5944320
#else
#define INITIAL_VIDEO_RECV_BUFFER_SIZE  ((4*1920*1080)*110/100)
#endif

enum tx_protocol {
        ULTRAGRID_RTP
};

struct state_uv {
        int recv_port_number;
        int send_port_number;
        union {
                struct rtp **network_devices; // ULTRAGRID_RTP
        };
        unsigned int connections_count;
        
        struct vidcap *capture_device;
        struct capture_filter *capture_filter;
        struct timeval start_time, curr_time;
        struct pdb *participants;
        
        char *decoder_mode;
        char *postprocess;
        
        uint32_t ts;
        struct tx *tx;
        struct display *display_device;
        char *requested_compression;
        const char *requested_display;
        const char *requested_capture;
        unsigned requested_mtu;
        
        enum tx_protocol tx_protocol;

        struct state_audio *audio;

        /* used mainly to serialize initialization */
        pthread_mutex_t master_lock;

        volatile unsigned int has_item_to_send:1;
        volatile unsigned int sender_waiting:1;
        volatile unsigned int compress_thread_waiting:1;
        volatile unsigned int should_exit_sender:1;
        pthread_mutex_t sender_lock;
        pthread_cond_t compress_thread_cv;
        pthread_cond_t sender_cv;

        struct video_frame * volatile tx_frame;
        struct video_export *video_exporter;
};

static volatile int wait_to_finish = FALSE;
static volatile int threads_joined = FALSE;
static int exit_status = EXIT_SUCCESS;
static bool should_exit_receiver = false;
static bool should_exit_sender = false;

static struct state_uv *uv_state;
bool net_frame_decoded = false;
struct video_frame *net_video_frame;
//char* net_audio_frame;

//
// prototypes
//
static struct rtp **initialize_network(char *addrs, int recv_port_base,
                int send_port_base, struct pdb *participants, bool use_ipv6,
                char *mcast_if);

static void list_video_display_devices(void);
static void list_video_capture_devices(void);
static void sender_finish(struct state_uv *uv);
static void display_buf_increase_warning(int size);
static bool enable_export(char *dir);
static void remove_display_from_decoders(struct state_uv *uv);

static void signal_handler(int signal)
{
        debug_msg("Caught signal %d\n", signal);
        exit_uv(0);
        return;
}

static void _exit_uv(int status);

static void _exit_uv(int status) {
        exit_status = status;
        wait_to_finish = TRUE;
        if(!threads_joined) {
                if(uv_state->capture_device) {
                        should_exit_sender = true;
                }
                if(uv_state->display_device) {
                        should_exit_receiver = true;
                }
                if(uv_state->audio)
                        audio_finish(uv_state->audio);
        }
        wait_to_finish = FALSE;
}

void (*exit_uv)(int status) = _exit_uv;

static void usage(void)
{
}

static void list_video_display_devices()
{
        int i;
        display_type_t *dt;

        printf("Available display devices:\n");
        display_init_devices();
        for (i = 0; i < display_get_device_count(); i++) {
                dt = display_get_device_details(i);
                printf("\t%s\n", dt->name);
        }
        display_free_devices();
}

static void list_video_capture_devices()
{
        int i;
        struct vidcap_type *vt;

        printf("Available capture devices:\n");
        vidcap_init_devices();
        for (i = 0; i < vidcap_get_device_count(); i++) {
                vt = vidcap_get_device_details(i);
                printf("\t%s\n", vt->name);
        }
        vidcap_free_devices();
}

static void display_buf_increase_warning(int size)
{
        fprintf(stderr, "\n***\n"
                        "Unable to set buffer size to %d B.\n"
                        "Please set net.core.rmem_max value to %d or greater. (see also\n"
                        "https://www.sitola.cz/igrid/index.php/Setup_UltraGrid)\n"
#ifdef HAVE_MACOSX
                        "\tsysctl -w kern.ipc.maxsockbuf=%d\n"
                        "\tsysctl -w net.inet.udp.recvspace=%d\n"
#else
                        "\tsysctl -w net.core.rmem_max=%d\n"
#endif
                        "To make this persistent, add these options (key=value) to /etc/sysctl.conf\n"
                        "\n***\n\n",
                        size, size,
#ifdef HAVE_MACOSX
                        size * 4,
#endif /* HAVE_MACOSX */
                        size);

}

static struct rtp **initialize_network(char *addrs, int recv_port_base,
                int send_port_base, struct pdb *participants, bool use_ipv6,
                char *mcast_if)
{
	struct rtp **devices = NULL;
        double rtcp_bw = 5 * 1024 * 1024;       /* FIXME */
	int ttl = 255;
	char *saveptr = NULL;
	char *addr;
	char *tmp;
	int required_connections, index;
        int recv_port = recv_port_base;
        int send_port = send_port_base;

	tmp = strdup(addrs);
	if(strtok_r(tmp, ",", &saveptr) == NULL) {
		free(tmp);
		return NULL;
	}
	else required_connections = 1;
	while(strtok_r(NULL, ",", &saveptr) != NULL)
		++required_connections;

	free(tmp);
	tmp = strdup(addrs);

	devices = (struct rtp **) 
		malloc((required_connections + 1) * sizeof(struct rtp *));

	for(index = 0, addr = strtok_r(addrs, ",", &saveptr); 
		index < required_connections;
		++index, addr = strtok_r(NULL, ",", &saveptr), recv_port += 2, send_port += 2)
	{
                /* port + 2 is reserved for audio */
                if (recv_port == recv_port_base + 2)
                        recv_port += 2;
                if (send_port == send_port_base + 2)
                        send_port += 2;

		devices[index] = rtp_init_if(addr, mcast_if, recv_port,
                                send_port, ttl, rtcp_bw, FALSE,
                                rtp_recv_callback, (void *)participants,
                                use_ipv6);
		if (devices[index] != NULL) {
			rtp_set_option(devices[index], RTP_OPT_WEAK_VALIDATION, 
				TRUE);
			rtp_set_sdes(devices[index], rtp_my_ssrc(devices[index]),
				RTCP_SDES_TOOL,
				PACKAGE_STRING, strlen(PACKAGE_STRING));

                        int size = INITIAL_VIDEO_RECV_BUFFER_SIZE;
                        int ret = rtp_set_recv_buf(devices[index], INITIAL_VIDEO_RECV_BUFFER_SIZE);
                        if(!ret) {
                                display_buf_increase_warning(size);
                        }

                        rtp_set_send_buf(devices[index], 1024 * 56);

			pdb_add(participants, rtp_my_ssrc(devices[index]));
		}
		else {
			int index_nest;
			for(index_nest = 0; index_nest < index; ++index_nest) {
				rtp_done(devices[index_nest]);
			}
			free(devices);
			devices = NULL;
		}
	}
	if(devices != NULL) devices[index] = NULL;
	free(tmp);
        
        return devices;
}

static void destroy_devices(struct rtp ** network_devices)
{
	struct rtp ** current = network_devices;
        if(!network_devices)
                return;
	while(*current != NULL) {
		rtp_done(*current++);
	}
	free(network_devices);
}

static struct tx *initialize_transmit(unsigned requested_mtu, char *fec)
{
        /* Currently this is trivial. It'll get more complex once we */
        /* have multiple codecs and/or error correction.             */
        return tx_init(requested_mtu, fec);
}

static struct vcodec_state *new_decoder(struct state_uv *uv) {
        struct vcodec_state *state = calloc(1, sizeof(struct vcodec_state));

        if(state) {
                state->messages = simple_linked_list_init();
                state->decoder = decoder_init(uv->decoder_mode, uv->postprocess, uv->display_device);

                if(!state->decoder) {
                        fprintf(stderr, "Error initializing decoder (incorrect '-M' or '-p' option?).\n");
                        free(state);
                        exit_uv(1);
                        return NULL;
                } else {
                        //decoder_register_video_display(state->decoder, uv->display_device);
                }
        }

        return state;
}

/**
 * Removes display from decoders and effectively kills them. They cannot be used
 * until new display assigned.
 */
static void remove_display_from_decoders(struct state_uv *uv) {
        if (uv->participants != NULL) {
                pdb_iter_t it;
                struct pdb_e *cp = pdb_iter_init(uv->participants, &it);
                while (cp != NULL) {
                        if(cp->video_decoder_state)
                                decoder_remove_display(cp->video_decoder_state->decoder);
                        cp = pdb_iter_next(&it);
                }
                pdb_iter_done(&it);
        }
}

static void *receiver_thread(void *arg)
{
        struct state_uv *uv = (struct state_uv *)arg;




        struct pdb_e *cp;
        struct timeval timeout;
        int fr;
        int ret;
        unsigned int tiles_post = 0;
        struct timeval last_tile_received = {0, 0};
        int last_buf_size = INITIAL_VIDEO_RECV_BUFFER_SIZE;
#ifdef SHARED_DECODER
        struct vcodec_state *shared_decoder = new_decoder(uv);
        if(shared_decoder == NULL) {
                fprintf(stderr, "Unable to create decoder!\n");
                exit_uv(1);
                return NULL;
        }
#endif // SHARED_DECODER


        initialize_video_decompress();

        pthread_mutex_unlock(&uv->master_lock);

        fr = 1;

        while (!should_exit_receiver) {
                /* Housekeeping and RTCP... */
                gettimeofday(&uv->curr_time, NULL);
                uv->ts = tv_diff(uv->curr_time, uv->start_time) * 90000;
                rtp_update(uv->network_devices[0], uv->curr_time);
                rtp_send_ctrl(uv->network_devices[0], uv->ts, 0, uv->curr_time);

                /* Receive packets from the network... The timeout is adjusted */
                /* to match the video capture rate, so the transmitter works.  */
                if (fr) {
                        gettimeofday(&uv->curr_time, NULL);
                        fr = 0;
                }

                timeout.tv_sec = 0;
                //timeout.tv_usec = 999999 / 59.94;
                timeout.tv_usec = 10000;
                ret = rtp_recv_poll_r(uv->network_devices, &timeout, uv->ts);

		/*
                   if (ret == FALSE) {
                   printf("Failed to receive data\n");
                   }
                 */
                UNUSED(ret);

                /* Decode and render for each participant in the conference... */
                pdb_iter_t it;
                cp = pdb_iter_init(uv->participants, &it);
                while (cp != NULL) {
                        if (tfrc_feedback_is_due(cp->tfrc_state, uv->curr_time)) {
                                debug_msg("tfrc rate %f\n",
                                          tfrc_feedback_txrate(cp->tfrc_state,
                                                               uv->curr_time));
                        }

                        if(cp->video_decoder_state == NULL) {
#ifdef SHARED_DECODER
                                cp->video_decoder_state = shared_decoder;
#else
                                cp->video_decoder_state = new_decoder(uv);
#endif // SHARED_DECODER
                                if(cp->video_decoder_state == NULL) {
                                        fprintf(stderr, "Fatal: unable to find decoder state for "
                                                        "participant %u.\n", cp->ssrc);
                                        exit_uv(1);
                                        break;
                                }
                                cp->video_decoder_state->display = uv->display_device;
                        }

                        /* Decode and render video... */
                        if (pbuf_decode
                            (cp->playout_buffer, uv->curr_time, decode_frame, cp->video_decoder_state)) {
                                tiles_post++;
                                /* we have data from all connections we need */
                                if(tiles_post == uv->connections_count) 
                                {
                                        tiles_post = 0;
                                        gettimeofday(&uv->curr_time, NULL);
                                        fr = 1;
#if 0
                                        print("PORQUE1?\n");
                                        display_put_frame(uv->display_device,
                                                          cp->video_decoder_state->frame_buffer);
                                        print("PORQUE2?\n");

                                        cp->video_decoder_state->frame_buffer =
                                            display_get_frame(uv->display_device);
                                        print("PORQUE3?\n");

#endif
                                }
                                last_tile_received = uv->curr_time;
                        }

                        /* dual-link TIMEOUT - we won't wait for next tiles */
                        if(tiles_post > 1 && tv_diff(uv->curr_time, last_tile_received) > 
                                        999999 / 59.94 / uv->connections_count) {
                                tiles_post = 0;
                                gettimeofday(&uv->curr_time, NULL);
                                fr = 1;
#if 0
                                display_put_frame(uv->display_device,
                                                cp->video_decoder_state->frame_buffer);
                                cp->video_decoder_state->frame_buffer =
                                        display_get_frame(uv->display_device);
#endif
                                last_tile_received = uv->curr_time;
                        }

                        if(cp->video_decoder_state->decoded % 100 == 99) {
                                int new_size = cp->video_decoder_state->max_frame_size * 110ull / 100;
                                if(new_size > last_buf_size) {
                                        struct rtp **device = uv->network_devices;
                                        while(*device) {
                                                int ret = rtp_set_recv_buf(*device, new_size);
                                                if(!ret) {
                                                        display_buf_increase_warning(new_size);
                                                }
                                                debug_msg("Recv buffer adjusted to %d\n", new_size);
                                                device++;
                                        }
                                }
                                last_buf_size = new_size;
                        }

                        while(simple_linked_list_size(cp->video_decoder_state->messages) > 0) {
                                struct vcodec_message *msg =
                                        simple_linked_list_pop(cp->video_decoder_state->messages);

                                assert(msg->type == FPS_CHANGED);
                                struct fps_changed_message *data = msg->data;

                                pbuf_set_playout_delay(cp->playout_buffer,
                                                1.0 / data->val,
                                                1.0 / data->val * (data->interframe_codec ? 2.2 : 1.2)
                                                );
                                free(data);
                                free(msg);
                        }


                        pbuf_remove(cp->playout_buffer, uv->curr_time);
                        cp = pdb_iter_next(&it);
                }
                pdb_iter_done(&it);
        }
        
#ifdef SHARED_DECODER
        destroy_decoder(shared_decoder);
#else
        /* Because decoders work asynchronously we need to make sure
         * that display won't be called */
        remove_display_from_decoders(uv);
#endif //  SHARED_DECODER

        display_finish(uv_state->display_device);

        return 0;
}

static void sender_finish(struct state_uv *uv) {
        pthread_mutex_lock(&uv->sender_lock);

        uv->should_exit_sender = TRUE;

        if(uv->sender_waiting) {
                uv->has_item_to_send = TRUE;
                pthread_cond_signal(&uv->sender_cv);
        }

        pthread_mutex_unlock(&uv->sender_lock);

}

static void *sender_thread(void *arg) {
        struct state_uv *uv = (struct state_uv *)arg;
        struct video_frame *splitted_frames = NULL;
        int tile_y_count;
        struct video_desc saved_vid_desc;

        tile_y_count = uv->connections_count;
        memset(&saved_vid_desc, 0, sizeof(saved_vid_desc));

        /* we have more than one connection */
        if(tile_y_count > 1) {
                /* it is simply stripping frame */
                splitted_frames = vf_alloc(tile_y_count);
        }

        while(!uv->should_exit_sender) {
                pthread_mutex_lock(&uv->sender_lock);

                while(!uv->has_item_to_send && !uv->should_exit_sender) {
                        uv->sender_waiting = TRUE;
                        pthread_cond_wait(&uv->sender_cv, &uv->sender_lock);
                        uv->sender_waiting = FALSE;
                }
                struct video_frame *tx_frame = uv->tx_frame;

                if(uv->should_exit_sender) {
                        uv->has_item_to_send = FALSE;
                        pthread_mutex_unlock(&uv->sender_lock);
                        goto exit;
                }

                pthread_mutex_unlock(&uv->sender_lock);

                if(uv->tx_protocol == ULTRAGRID_RTP) {
                        if(uv->connections_count == 1) { /* normal case - only one connection */
                                tx_send(uv->tx, tx_frame,
                                                uv->network_devices[0]);
                        } else { /* split */
                                int i;

                                //assert(frame_count == 1);
                                vf_split_horizontal(splitted_frames, tx_frame,
                                                tile_y_count);
                                for (i = 0; i < tile_y_count; ++i) {
                                        tx_send_tile(uv->tx, splitted_frames, i,
                                                        uv->network_devices[i]);
                                }
                        }
                }


                pthread_mutex_lock(&uv->sender_lock);

                uv->has_item_to_send = FALSE;

                if(uv->compress_thread_waiting) {
                        pthread_cond_signal(&uv->compress_thread_cv);
                }
                pthread_mutex_unlock(&uv->sender_lock);
        }

exit:
        vf_free(splitted_frames);



        return NULL;
}

static void *compress_thread(void *arg)
{
        struct state_uv *uv = (struct state_uv *)arg;

        struct video_frame *tx_frame;
        struct audio_frame *audio;
        //struct video_frame *splitted_frames = NULL;
        pthread_t sender_thread_id;
        int i = 0;

        struct compress_state *compression;
        int ret = compress_init(uv->requested_compression, &compression);

        pthread_mutex_unlock(&uv->master_lock);
        /* NOTE: unlock before propagating possible error */
        if(ret < 0) {
                fprintf(stderr, "Error initializing compression.\n");
                exit_uv(0);
                goto compress_done;
        }
        if(ret > 0) {
                exit_uv(0);
                goto compress_done;
        }

        if (pthread_create
            (&sender_thread_id, NULL, sender_thread,
             (void *)uv) != 0) {
                perror("Unable to create sender thread!\n");
                exit_uv(EXIT_FAILURE);
                goto join_thread;
        }

        while (!should_exit_sender) {
                /* Capture and transmit video... */
//				sleep(0.1);
                tx_frame = vidcap_grab(uv->capture_device, &audio);
                if (tx_frame != NULL) {
                        //TODO: Unghetto this
                        tx_frame = compress_frame(compression, tx_frame, i);
                        if(!tx_frame)
                                continue;

                        i = (i + 1) % 2;

                        video_export(uv->video_exporter, tx_frame);


                        /* when sending uncompressed video, we simply post it for send
                         * and wait until done */
                        if(is_compress_none(compression)) {
                                pthread_mutex_lock(&uv->sender_lock);

                                uv->tx_frame = tx_frame;

                                uv->has_item_to_send = TRUE;
                                if(uv->sender_waiting) {
                                        pthread_cond_signal(&uv->sender_cv);
                                }

                                while(uv->has_item_to_send) {
                                        uv->compress_thread_waiting = TRUE;
                                        pthread_cond_wait(&uv->compress_thread_cv, &uv->sender_lock);
                                        uv->compress_thread_waiting = FALSE;
                                }
                                pthread_mutex_unlock(&uv->sender_lock);
                        }  else
                        /* we post for sending (after previous frame is done) and schedule a new one
                         * frames may overlap then */
                        {
                                pthread_mutex_lock(&uv->sender_lock);
                                while(uv->has_item_to_send) {
                                        uv->compress_thread_waiting = TRUE;
                                        pthread_cond_wait(&uv->compress_thread_cv, &uv->sender_lock);
                                        uv->compress_thread_waiting = FALSE;
                                }

                                uv->tx_frame = tx_frame;

                                uv->has_item_to_send = TRUE;
                                if(uv->sender_waiting) {
                                        pthread_cond_signal(&uv->sender_cv);
                                }
                                pthread_mutex_unlock(&uv->sender_lock);
                        }
                }
        }

        vidcap_finish(uv_state->capture_device);

join_thread:
        sender_finish(uv);
        pthread_join(sender_thread_id, NULL);

compress_done:
        compress_done(compression);

        return NULL;
}

static bool enable_export(char *dir)
{
        if(!dir) {
                for (int i = 1; i <= 9999; i++) {
                        char name[16];
                        snprintf(name, 16, "export.%04d", i);
                        int ret = platform_mkdir(name);
                        if(ret == -1) {
                                if(errno == EEXIST) {
                                        continue;
                                } else {
                                        fprintf(stderr, "[Export] Directory creation failed: %s\n",
                                                        strerror(errno));
                                        return false;
                                }
                        } else {
                                export_dir = strdup(name);
                                break;
                        }
                }
        } else {
                int ret = platform_mkdir(dir);
                if(ret == -1) {
                                if(errno == EEXIST) {
                                        fprintf(stderr, "[Export] Warning: directory %s exists!\n", dir);
                                } else {
                                        perror("[Export] Directory creation failed");
                                        return false;
                                }
                }

                export_dir = strdup(dir);
        }

        if(export_dir) {
                printf("Using export directory: %s\n", export_dir);
                return true;
        } else {
                return false;
        }
}

struct video_frame* get_net_video_frame(){
	return net_video_frame;
}

void put_net_video_frame(struct video_frame* frame){
	net_video_frame=frame;
}


int main()
{
		net_video_frame = vf_alloc(1);
		net_video_frame->tiles = vf_get_tile(net_video_frame, 0);

#if defined HAVE_SCHED_SETSCHEDULER && defined USE_RT
        struct sched_param sp;
#endif
        char *network_device = NULL;
        char *capture_cfg = NULL;
        char *display_cfg = NULL;
        const char *audio_recv = "none";
        const char *audio_send = "none";
        char *jack_cfg = NULL;
        char *requested_video_fec = strdup("none");
        char *requested_audio_fec = strdup(DEFAULT_AUDIO_FEC);
        char *audio_channel_map = NULL;
        char *audio_scale = "mixauto";

        bool echo_cancellation = false;
        bool use_ipv6 = false;
        char *mcast_if = NULL;

        bool should_export = false;
        char *export_opts = NULL;

        int bitrate = 0;
        
        char *audio_host = NULL;
        int audio_rx_port = -1, audio_tx_port = -1;

        struct state_uv *uv;
        int ch;

        char *requested_capture_filter = NULL;

        audio_codec_t audio_codec = AC_PCM;
        
        pthread_t receiver_thread_id,
                  tx_thread_id;
	bool receiver_thread_started = false,
		  tx_thread_started = false;
        unsigned vidcap_flags = 0,
                 display_flags = 0;
        int compressed_audio_sample_rate = 48000;
        int ret;

#if defined DEBUG && defined HAVE_LINUX
        mtrace();
#endif


        //      uv = (struct state_uv *) calloc(1, sizeof(struct state_uv));
        uv = (struct state_uv *)malloc(sizeof(struct state_uv));
        uv_state = uv;

        uv->audio = NULL;
        uv->ts = 0;
        uv->capture_device = NULL;
        uv->display_device = NULL;
        uv->requested_display = "none";
        uv->requested_capture = "none";
        uv->requested_compression = "none";
        uv->decoder_mode = NULL;
        uv->postprocess = NULL;
        uv->requested_mtu = 0;
        uv->tx_protocol = ULTRAGRID_RTP;
        uv->participants = NULL;
        uv->tx = NULL;
        uv->network_devices = NULL;
        uv->video_exporter = NULL;
        uv->recv_port_number =
                uv->send_port_number =
                PORT_BASE;

        pthread_mutex_init(&uv->master_lock, NULL);

        uv->has_item_to_send = FALSE;
        uv->sender_waiting = FALSE;
        uv->compress_thread_waiting = FALSE;
        uv->should_exit_sender = FALSE;
        pthread_mutex_init(&uv->sender_lock, NULL);
        pthread_cond_init(&uv->compress_thread_cv, NULL);
        pthread_cond_init(&uv->sender_cv, NULL);

        perf_init();
        perf_record(UVP_INIT, 0);

        init_lib_common();



        // FORCE -d net
             uv->requested_display = "net";


     		// FORCE -t net
             uv->requested_capture = "net";

     		// FORCE -P 5004:6004


     		  uv->recv_port_number = 5004;
     		  uv->send_port_number = 6004;




     			network_device = "192.168.10.66";






        if (uv->requested_mtu == 0)     // mtu wasn't specified on the command line
        {
                uv->requested_mtu = 1500;       // the default value for RTP
        }

        printf("%s", PACKAGE_STRING);
#ifdef GIT_VERSION
        printf(" (rev %s)", GIT_VERSION);
#endif
        printf("\n");
        printf("Display device   : %s\n", uv->requested_display);
        printf("Capture device   : %s\n", uv->requested_capture);
        printf("Audio capture    : %s\n", audio_send);
        printf("Audio playback   : %s\n", audio_recv);
        printf("MTU              : %d B\n", uv->requested_mtu);
        printf("Video compression: %s\n", uv->requested_compression);
        printf("Audio codec      : %s\n", get_name_to_audio_codec(audio_codec));

        printf("Network protocol : ");
        switch(uv->tx_protocol) {
                case ULTRAGRID_RTP:
                        printf("UltraGrid RTP\n"); break;
        }

        printf("Audio FEC        : %s\n", requested_audio_fec);
        printf("Video FEC        : %s\n", requested_video_fec);
        printf("\n");

        if(audio_rx_port == -1) {
                audio_tx_port = uv->send_port_number + 2;
                audio_rx_port = uv->recv_port_number + 2;
        }

        if(should_export) {
                if(!enable_export(export_opts)) {
                        fprintf(stderr, "Export initialization failed.\n");
                        return EXIT_FAILURE;
                }
                uv->video_exporter = video_export_init(export_dir);
        }

        gettimeofday(&uv->start_time, NULL);

        if(uv->requested_mtu > RTP_MAX_PACKET_LEN) {
                fprintf(stderr, "Requested MTU exceeds maximal value allowed by RTP library (%d).\n",
                                RTP_MAX_PACKET_LEN);
                return EXIT_FAIL_USAGE;
        }





#ifdef WIN32
	WSADATA wsaData;
	int err = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if(err != 0) {
		fprintf(stderr, "WSAStartup failed with error %d.", err);
		return EXIT_FAILURE;
	}
	if(LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
		fprintf(stderr, "Counld not found usable version of Winsock.\n");
		WSACleanup();
		return EXIT_FAILURE;
	}
#endif

        if(!audio_host) {
                audio_host = network_device;
        }
        uv->audio = audio_cfg_init (audio_host, audio_rx_port,
                        audio_tx_port, audio_send, audio_recv,
                        jack_cfg, requested_audio_fec, audio_channel_map,
                        audio_scale, echo_cancellation, use_ipv6, mcast_if,
                        audio_codec, compressed_audio_sample_rate);
        free(requested_audio_fec);
        if(!uv->audio)
                goto cleanup;

        vidcap_flags |= audio_get_vidcap_flags(uv->audio);
        display_flags |= audio_get_display_flags(uv->audio);

        uv->participants = pdb_init();

        // Display initialization should be prior to modules that may use graphic card (eg. GLSL) in order
        // to initalize shared resource (X display) first
        ret =
             initialize_video_display(uv->requested_display, display_cfg, display_flags, &uv->display_device);
        if (ret < 0) {
                printf("Unable to open display device: %s\n",
                       uv->requested_display);
                exit_uv(EXIT_FAIL_DISPLAY);
                goto cleanup_wait_audio;
        }
        if(ret > 0) {
                exit_uv(EXIT_SUCCESS);
                goto cleanup_wait_audio;
        }

        printf("Display initialized-%s\n", uv->requested_display);

        ret = initialize_video_capture(uv->requested_capture, capture_cfg, vidcap_flags, &uv->capture_device);
        if (ret < 0) {
                printf("Unable to open capture device: %s\n",
                       uv->requested_capture);
                exit_uv(EXIT_FAIL_CAPTURE);
                goto cleanup_wait_audio;
        }
        if(ret > 0) {
                exit_uv(EXIT_SUCCESS);
                goto cleanup_wait_audio;
        }
        printf("Video capture initialized-%s\n", uv->requested_capture);

        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
#ifndef WIN32
        signal(SIGHUP, signal_handler);
#endif
        signal(SIGABRT, signal_handler);

#ifdef USE_RT
#ifdef HAVE_SCHED_SETSCHEDULER
        sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
                printf("WARNING: Unable to set real-time scheduling\n");
        }
#else
        printf("WARNING: System does not support real-time scheduling\n");
#endif /* HAVE_SCHED_SETSCHEDULER */
#endif /* USE_RT */         

        if(uv->tx_protocol == ULTRAGRID_RTP) {
                if ((uv->network_devices =
                                        initialize_network(network_device, uv->recv_port_number,
                                                uv->send_port_number, uv->participants, use_ipv6, mcast_if))
                                == NULL) {
                        printf("Unable to open network\n");
                        exit_uv(EXIT_FAIL_NETWORK);
                        goto cleanup_wait_display;
                } else {
                        struct rtp **item;
                        uv->connections_count = 0;
                        /* only count how many connections has initialize_network opened */
                        for(item = uv->network_devices; *item != NULL; ++item)
                                ++uv->connections_count;
                }

                if(bitrate == 0) { // else packet_rate defaults to 13600 or so
                        bitrate = 6618;
                }

                if(bitrate != -1) {
                        packet_rate = 1000 * uv->requested_mtu * 8 / bitrate;
                } else {
                        packet_rate = 0;
                }

                if ((uv->tx = initialize_transmit(uv->requested_mtu, requested_video_fec)) == NULL) {
                        printf("Unable to initialize transmitter.\n");
                        exit_uv(EXIT_FAIL_TRANSMIT);
                        goto cleanup_wait_display;
                }
                free(requested_video_fec);
        }

        if(uv->tx_protocol == ULTRAGRID_RTP) {
                /* following block only shows help (otherwise initialized in receiver thread */
                if((uv->postprocess && strstr(uv->postprocess, "help") != NULL) || 
                                (uv->decoder_mode && strstr(uv->decoder_mode, "help") != NULL)) {
                        struct state_decoder *dec = decoder_init(uv->decoder_mode, uv->postprocess, NULL);
                        decoder_destroy(dec);
                        exit_uv(EXIT_SUCCESS);
                        goto cleanup_wait_display;
                }
                /* following block only shows help (otherwise initialized in sender thread */
                if(strstr(uv->requested_compression,"help") != NULL) {
                        struct compress_state *compression;
                        int ret = compress_init(uv->requested_compression, &compression);

                        if(ret >= 0) {
                                if(ret == 0)
                                        compress_done(compression);
                                exit_uv(EXIT_SUCCESS);
                        } else {
                                exit_uv(EXIT_FAILURE);
                        }
                        goto cleanup_wait_display;
                }

                if (strcmp("none", uv->requested_display) != 0) {
                        pthread_mutex_lock(&uv->master_lock); 
                        if (pthread_create
                            (&receiver_thread_id, NULL, receiver_thread,
                             (void *)uv) != 0) {
                                perror("Unable to create display thread!\n");
                                exit_uv(EXIT_FAILURE);
                                goto cleanup_wait_display;
                        } else {
				receiver_thread_started = true;
			}
                }

                if (strcmp("none", uv->requested_capture) != 0) {
                        pthread_mutex_lock(&uv->master_lock); 
                        if (pthread_create
                            (&tx_thread_id, NULL, compress_thread,
                             (void *)uv) != 0) {
                                perror("Unable to create capture thread!\n");
                                exit_uv(EXIT_FAILURE);
                                goto cleanup_wait_capture;
                        } else {
				tx_thread_started = true;
			}
                }
        }
        
        pthread_mutex_lock(&uv->master_lock); 

        if (strcmp("none", uv->requested_display) != 0)
                display_run(uv->display_device);

cleanup_wait_display:
        if (strcmp("none", uv->requested_display) != 0 && receiver_thread_started)
                pthread_join(receiver_thread_id, NULL);

cleanup_wait_capture:
        if (strcmp("none", uv->requested_capture) != 0 &&
                         tx_thread_started)
                pthread_join(tx_thread_id, NULL);

cleanup_wait_audio:
        /* also wait for audio threads */
        audio_join(uv->audio);

cleanup:
        while(wait_to_finish)
                ;
        threads_joined = TRUE;

        if(uv->audio)
                audio_done(uv->audio);
        if(uv->tx)
                tx_done(uv->tx);
	if(uv->tx_protocol == ULTRAGRID_RTP && uv->network_devices)
                destroy_devices(uv->network_devices);
        if(uv->capture_device)
                vidcap_done(uv->capture_device);
        if(uv->display_device)
                display_done(uv->display_device);
        if (uv->participants != NULL) {
                pdb_iter_t it;
                struct pdb_e *cp = pdb_iter_init(uv->participants, &it);
                while (cp != NULL) {
                        struct pdb_e *item = NULL;
                        pdb_remove(uv->participants, cp->ssrc, &item);
                        cp = pdb_iter_next(&it);
                        free(item);
                }
                pdb_iter_done(&it);
                pdb_destroy(&uv->participants);
        }

        video_export_destroy(uv->video_exporter);

        pthread_mutex_destroy(&uv->sender_lock);
        pthread_cond_destroy(&uv->compress_thread_cv);
        pthread_cond_destroy(&uv->sender_cv);

        pthread_mutex_unlock(&uv->master_lock); 

        pthread_mutex_destroy(&uv->master_lock);

        free(uv);
        free(export_dir);
        
        lib_common_done();

#if defined DEBUG && defined HAVE_LINUX
        muntrace();
#endif

#ifdef WIN32
	WSACleanup();
#endif

        printf("Exit\n");

        return exit_status;
}
