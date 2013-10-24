#include "config_unix.h"
#include "transmitter.h"
#include "rtp/rtp.h"
#include "pdb.h"
#include "video_codec.h"
#include "video_compress.h"
#include "module.h"
#include "debug.h"
#include "tv.h"

#include <stdlib.h>

void *transmitter_rtp_routine(void *arg);
int transmitter_init_threads(struct participant_data *participant);
void *transmitter_master_routine(void *arg);

void transmitter_dummy_callback(struct rtp *session, rtp_event *e);
void transmitter_destroy_encoder_thread(encoder_thread_t **encoder);

int init_transmission_rtp(participant_data_t *participant);

float WAIT_TIME;
float FRAMERATE;
sem_t FRAME_SEM;


transmitter_t *init_transmitter(participant_list_t *list, float fps)
{
    transmitter_t *transmitter = malloc(sizeof(transmitter_t));
    if (transmitter == NULL) {
        error_msg("init_transmitter: malloc error");
        return NULL;
    }

    transmitter->run = FALSE;
    
    if (fps == -1) {
        transmitter->fps = DEFAULT_FPS;
    } else {
        transmitter->fps = fps;
    }
    transmitter->wait_time = (1.0/transmitter->fps)*1000000;

    transmitter->recv_port = DEFAULT_RECV_PORT;
    transmitter->ttl = DEFAULT_TTL;
    transmitter->send_buffer_size = DEFAULT_SEND_BUFFER_SIZE;
    transmitter->mtu = MTU;

    transmitter->participants = list;

    return transmitter;
}

int init_transmission_rtp(participant_data_t *participant)
{
    pthread_mutex_lock(&participant->lock);
    assert(participant->type == OUTPUT);
    assert(participant->protocol == RTP);

    int ret = pthread_create(&participant->rtp.thread, NULL,
                             transmitter_rtp_routine, participant);
    participant->rtp.run = TRUE;
    if (ret < 0) {
        error_msg("init_transmission_rtp: pthread_create error");
    }

    pthread_mutex_unlock(&participant->lock);
    return ret;
}

int init_transmission(participant_data_t *participant)
{
    pthread_mutex_lock(&participant->lock);
    
    if (participant->type != OUTPUT) {
        pthread_mutex_unlock(&participant->lock);
        return FALSE;
    }

    int ret = TRUE;

    if (participant->protocol == RTSP) {
        // TODO: implement RTSP transmission
        error_msg("init_transmission: transmission RTP support not ready yet");
        ret = FALSE;
    } else if (participant->protocol == RTP) {
        ret = init_transmission_rtp(participant);
    }

    pthread_mutex_unlock(&participant->lock);
    return ret;
}

int stop_transmission(participant_data_t *participant)
{
    // TODO
    pthread_mutex_lock(&participant->lock);

    pthread_mutex_unlock(&participant->lock);
}

void transmitter_dummy_callback(struct rtp *session, rtp_event *e)
{
    UNUSED(session);
    UNUSED(e);
}

void *transmitter_rtp_routine(void *arg)
{
    debug_msg(" transmitter rtpenc routine START\n");
    participant_data_t *participant = (struct participant_data *)arg;
    rtp_session_t *session = &participant->rtp;

    char *mcast_if = NULL;
    double rtcp_bw = DEFAULT_RTCP_BW;
    int ttl = DEFAULT_TTL;
    int recv_port = DEFAULT_RECV_PORT;
    
    struct module tmod;
    struct tx *tx_session;
    
    module_init_default(&tmod);

    struct rtp *rtp  = rtp_init_if(session->addr, mcast_if,
                                   recv_port, session->port, ttl,
                                   rtcp_bw, 0, transmitter_dummy_callback,
                                   (void *)NULL, 0);
    
    rtp_set_option(rtp, RTP_OPT_WEAK_VALIDATION, 1);
    rtp_set_sdes(rtp, rtp_my_ssrc(rtp), RTCP_SDES_TOOL, PACKAGE_STRING, strlen(PACKAGE_STRING));
    rtp_set_send_buf(rtp, DEFAULT_SEND_BUFFER_SIZE);

    tx_session = tx_init_h264(&tmod, MTU, TX_MEDIA_VIDEO, NULL, NULL);
    
    struct timeval curr_time;
    struct timeval start_time;
    double timestamp;
    gettimeofday(&start_time, NULL);

    // Only one stream

    while (participant->rtp.run && participant->streams_count > 0) {

        stream_data_t *stream = participant->streams[0];
        assert(stream != NULL);
        encoder_thread_t *encoder = stream->encoder;
        assert(encoder != NULL);
        

        if (sem_wait(&encoder->output_sem) < 0) {
            break;
        }

        pthread_rwlock_rdlock(&stream->video.lock);

        /* implement something like this?
        pthread_mutex_lock(&encoder->lock);
        encoder->rtpenc->ready = 0;
        pthread_mutex_unlock(&encoder->lock);
        */

        gettimeofday(&curr_time, NULL);
        rtp_update(rtp, curr_time);
        timestamp = tv_diff(curr_time, start_time)*90000;
        rtp_send_ctrl(rtp, timestamp, 0, curr_time);

        tx_send_h264(tx_session, stream->encoder->frame, rtp, FRAMERATE);

        /*
        pthread_mutex_lock(&encoder->lock);
        encoder->rtpenc->ready = 1;
        pthread_mutex_unlock(&encoder->lock);
         */

        pthread_rwlock_unlock(&stream->video.lock);
    }   

    rtp_send_bye(rtp);
    rtp_done(rtp);
    module_done(CAST_MODULE(&tmod));
    
    pthread_exit(NULL);
}

void *transmitter_master_routine(void *arg)
{
    debug_msg("transmitter master routine START\n");
    transmitter_t *transmitter = (transmitter_t *)arg;

    participant_list_t *list = transmitter->participants;
    struct participant_data *participant = list->first;
    while (participant != NULL) {
        debug_msg("participant found, initializing its threads...\n");
        init_transmission(participant);
        debug_msg("participant threads initialized\n");
        participant = participant->next;
    }

    debug_msg("entering the master loop\n");
    while (transmitter->run) {
        usleep(transmitter->wait_time);
        
        pthread_rwlock_rdlock(&list->lock);
        participant_data_t *ptc = list->first;
        while (ptc != NULL) {
            int i = 0;
            while (i++ < ptc->streams_count) {
                stream_data_t *str = ptc->streams[i];
                if (str->encoder != NULL && str->encoder->run) {
                    sem_post(&str->encoder->input_sem);
                }
            }
            ptc = ptc->next;
        }
        pthread_rwlock_unlock(&list->lock);
    }

    debug_msg(" terminating pairs of threads\n");
    pthread_rwlock_rdlock(&list->lock);
    participant = list->first;
    while (participant != NULL) {
        stop_transmission(participant);
        participant = participant->next;
    }
    pthread_rwlock_unlock(&list->lock);
    pthread_exit((void *)NULL);
}

int start_transmitter(transmitter_t *transmitter)
{
    transmitter->run = TRUE;
    debug_msg("creating the master thread...\n");
    int ret = pthread_create(&transmitter->thread, NULL, transmitter_master_routine, transmitter);
    if (ret < 0) {
        error_msg("could not initiate the transmitter master thread\n");
    }
    return ret;
}

int stop_transmitter(transmitter_t *transmitter)
{
    transmitter->run = FALSE;
    int ret = pthread_join(transmitter->thread, NULL);
    free(transmitter);
    return ret;
}
