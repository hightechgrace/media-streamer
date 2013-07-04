#include "config.h"
#include "config_unix.h"
#include "debug.h"
#include "ntp.h"
#include "tv.h"
#include "rtp/rtp.h"
#include "rtp/pbuf.h"
#include "pdb.h"
#include "rtp/rtp_callback.h"
#include "tfrc.h"
#include "rtp/rtpenc_h264.h"


static const uint8_t *rtp_find_startcode_internal(const uint8_t *p, const uint8_t *end)
{
    const uint8_t *a = p + 4 - ((intptr_t)p & 3);

    for (end -= 3; p < a && p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    for (end -= 3; p < end; p += 4) {
        uint32_t x = *(const uint32_t*)p;
//      if ((x - 0x01000100) & (~x) & 0x80008000) // little endian
//      if ((x - 0x00010001) & (~x) & 0x00800080) // big endian
        if ((x - 0x01010101) & (~x) & 0x80808080) { // generic
            if (p[1] == 0) {
                if (p[0] == 0 && p[2] == 1)
                    return p;
                if (p[2] == 0 && p[3] == 1)
                    return p+1;
            }
            if (p[3] == 0) {
                if (p[2] == 0 && p[4] == 1)
                    return p+2;
                if (p[4] == 0 && p[5] == 1)
                    return p+3;
            }
        }
    }

    for (end += 3; p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    return end + 3;
}

const uint8_t *rtpc_find_startcode(const uint8_t *p, const uint8_t *end){
    const uint8_t *out= rtp_find_startcode_internal(p, end);
    if(p<out && out<end && !out[-1]) out--;
    return out;
}

typedef struct {
    uint8_t *data;
    int size;
}

int rtp_parse_nal_units(const uint8_t *buf_in, int size, struct rtp_nal_t **nals, int *nnals)
{
    const uint8_t *p = buf_in;
    const uint8_t *end = p + size;
    const uint8_t *nal_start, *nal_end;

    size = 0;
    nal_start = rtp_find_startcode(p, end);
    int nnals = 0;
    for (;;) {
        while (nal_start < end && !*(nal_start++));
        if (nal_start == end)
            break;

        nal_end = rtp_find_startcode(nal_start, end);
        int nal_size = 4 + nal_end - nal_start;
        size += nal_size;

        nals[nnals] = malloc(sizeof(rtp_nal_t));
        nals[nnals]->data = nal_start;
        nals[nnals]->size = nal_size;
        nnals++;

        nal_start = nal_end;
    }
    return size;
}


void tx_init_h264()
{
    buffer_id =  lrand48() & 0x3fffff;
    bitrate = 6618;
    packet_rate = 1000 * mtu * 8 / bitrate;
}

void tx_send_base(struct tile *tile, struct rtp *rtp_session,
                       uint32_t ts, int send_m, codec_t color_spec,
                       double input_fps, enum interlacing_t interlacing,
                       unsigned int substream, int fragment_offset)
{


    char *data = tile->data;
    int data_len = tile->data_len;

    struct rtp_nal_t *nals[];
    int nnals;


    rtp_parse_nal_units(data, data_len, nals, &nnals)

    char pt = 96; // h264
    int cc = 0;
    uint32_t csrc = NULL;
    
    char *extn = 0;
    uint16_t extn_len = 0;
    uint16_t extn_type = 0;

    // according to previous code
    int headers_len = 40; //+ (sizeof(video_payload_hdr_t));

    int i;
    for (i = 0; i < nnals; i++) {
        struct rtp_nal_t *nal = nals[i];

        int rtp_size = headers_len + nal->size;
        if (rtp_size >= mtu) {
            printf("[WARNING] RTP packet size exceeds the MTU size\n");
        }
        
        int err = rtp_send_data(rtp_session, ts, pt, send_m, cc, &csrc,
                                nal->data, nal->size, extn, extn_len,
                                extn_type);
        if (err != 0) {
            printf("[ERROR] There was a problem sending the RTP packet\n");
        }
    }



/*
        int m;
        int data_len;
        // see definition in rtp_callback.h
        video_payload_hdr_t video_hdr;
        //ldgm_video_payload_hdr_t ldgm_hdr;
        int pt = PT_VIDEO;            // A value specified in our packet format
        char *data;
        unsigned int pos;
        struct timespec start;
        struct timespec stop;
        long delta;
        uint32_t tmp;
        unsigned int fps;
        unsigned int fpsd;
        unsigned int fd;
        unsigned int fi;
//        int mult_pos[FEC_MAX_MULT];
//        int mult_index = 0;
//        int mult_first_sent = 0;
        int hdrs_len = 40 + (sizeof(video_payload_hdr_t));
        char *data_to_send;
        int data_to_send_len;

        //assert(tx->magic == TRANSMIT_MAGIC);

        //tx_update(tx, tile);
        //ldgm
        //perf_record(UVP_SEND, ts);

        data_to_send = tile->data;
        data_to_send_len = tile->data_len;

        //printf("[SENDER] data to send length = %d and first byte = %x\n",data_to_send_len,data_to_send[0]);

//        if(tx->fec_scheme == FEC_MULT) {
//                int i;
//                for (i = 0; i < tx->mult_count; ++i) {
//                        mult_pos[i] = 0;
//                }
//                mult_index = 0;
//        }

        m = 0;
        pos = 0;

        video_hdr[3] = htonl(tile->width << 16 | tile->height);
        video_hdr[4] = get_fourcc(color_spec);
        video_hdr[2] = htonl(data_to_send_len);
        tmp = substream << 22;
        tmp |= buffer_id;
        video_hdr[0] = htonl(tmp);

        // word 6
        tmp = interlacing << 29;
        fps = round(input_fps);
        fpsd = 1;
        if(fabs(input_fps - round(input_fps) / 1.001) < 0.005)
                fd = 1;
        else
                fd = 0;
        fi = 0;

        tmp |= fps << 19;
        tmp |= fpsd << 15;
        tmp |= fd << 14;
        tmp |= fi << 13;
        video_hdr[5] = htonl(tmp);

        char *hdr;
        int hdr_len;

//        if(fec_is_ldgm(tx)) {
//                hdrs_len = 40 + (sizeof(ldgm_video_payload_hdr_t));
//                ldgm_encoder_encode(tx->fec_state, (char *) &video_hdr, sizeof(video_hdr),
//                                tile->data, tile->data_len, &data_to_send, &data_to_send_len);
//                tmp = substream << 22;
//                tmp |= 0x3fffff & tx->buffer;
//                // see definition in rtp_callback.h
//                ldgm_hdr[0] = htonl(tmp);
//                ldgm_hdr[2] = htonl(data_to_send_len);
//                ldgm_hdr[3] = htonl(
//                                (ldgm_encoder_get_k(tx->fec_state)) << 19 |
//                                (ldgm_encoder_get_m(tx->fec_state)) << 6 |
//                                ldgm_encoder_get_c(tx->fec_state));
//                ldgm_hdr[4] = htonl(ldgm_encoder_get_seed(tx->fec_state));
//
//                pt = PT_VIDEO_LDGM;
//
//                hdr = (char *) &ldgm_hdr;
//                hdr_len = sizeof(ldgm_hdr);
//        } else {
                hdr = (char *) &video_hdr;
                hdr_len = sizeof(video_hdr);
//        }

        do {
                int offset = pos + fragment_offset;

                video_hdr[1] = htonl(offset);

                data = data_to_send + pos;
                data_len = mtu - hdrs_len;
                data_len = (data_len / 48) * 48;

                if (pos + data_len >= (unsigned int) data_to_send_len) {
                        if (send_m) {
                                m = 1;
                        }
                        data_len = data_to_send_len - pos;
                }
                pos += data_len;
                GET_STARTTIME;
                if(data_len) { // check needed for FEC_MULT
                        rtp_send_data_hdr(rtp_session, ts, pt, m, 0, 0,
                                  hdr, hdr_len,
                                  data, data_len, 0, 0, 0);
                        if (m) { //&& tx->fec_scheme != FEC_NONE
                                int i;
                                for(i = 0; i < 5; ++i) {
                                        rtp_send_data_hdr(rtp_session, ts, pt, m, 0, 0,
                                                  hdr, hdr_len,
                                                  data, data_len, 0, 0, 0);
                                }
                        }
                }

                do {
                        GET_STOPTIME;
                        GET_DELTA;
                        if (delta < 0)
                                delta += 1000000000L;
                } while (packet_rate - delta > 0);

        } while (pos < (unsigned int) data_to_send_len);
*/
}
