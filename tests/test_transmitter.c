#include "config.h"
#include "transmitter.h"
#include "video_compress.h"
#include "debug.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

int load_video(const char* path, AVFormatContext *pFormatCtx, AVCodecContext *pCodecCtx, int *videostream);
int read_frame(AVFormatContext *pFormatCtx, int videostream, AVCodecContext *pCodecCtx, uint8_t *buff);

int load_video(const char* path, AVFormatContext *pFormatCtx, AVCodecContext *pCodecCtx, int *videostream)
{

    AVDictionary *rawdict = NULL, *optionsDict = NULL;
    AVCodec *pCodec = NULL;
    AVCodecContext *aux_codec_ctx = NULL;

    // Define YUV input video features
    pFormatCtx->iformat = av_find_input_format("rawvideo");
    unsigned int i;

    av_dict_set(&rawdict, "video_size", "1280x720", 0);
    av_dict_set(&rawdict, "pixel_format", "rgb24", 0);

    // Open video file
    if(avformat_open_input(&pFormatCtx, path, pFormatCtx->iformat, &rawdict)!=0)
        return -1; // Couldn't open file

    av_dict_free(&rawdict);

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information

    // Find the first video stream
    *videostream=-1;
    for(i=0; i<pFormatCtx->nb_streams; i++){
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO) {
            *videostream=i;
            break;
        }
    }
    if(*videostream==-1)
        return -1; // Didn't find a video stream

    // Get a pointer to the codec context for the video stream
    aux_codec_ctx=pFormatCtx->streams[*videostream]->codec;

    // Find the decoder for the video stream
    pCodec=avcodec_find_decoder(aux_codec_ctx->codec_id);
    if(pCodec==NULL) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1; // Codec not found
    }

    // Open codec
    if(avcodec_open2(aux_codec_ctx, pCodec, &optionsDict)<0)
        return -1; // Could not open codec

    *pCodecCtx = *aux_codec_ctx;

    return 0;

}

int read_frame(AVFormatContext *pFormatCtx, int videostream, AVCodecContext *pCodecCtx, uint8_t *buff)
{
    AVPacket packet;
    AVFrame* pFrame;
    int frameFinished, ret;

    pFrame = avcodec_alloc_frame();
    ret = av_read_frame(pFormatCtx, &packet);

    if(packet.stream_index==videostream) {
        // Decode video frame
        avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
        // Did we get a video frame?
        if(frameFinished) {
            avpicture_layout((AVPicture *)pFrame, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, buff,
                    avpicture_get_size(pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height)*sizeof(uint8_t));

            // Free the packet that was allocated by av_read_frame
            av_free_packet(&packet);
        }
    }

    av_free(pFrame);

    return ret;
}


int main(int argc, char **argv)
{
    int stop_at = 0;
    char *yuv_path;
    if (argc == 2) {
        yuv_path = argv[1];
    } else if (argc == 3) {
        yuv_path = argv[1];
        stop_at = atoi(argv[2]);
    } else {
        printf("usage: %s input [max frames]\n", argv[0]);
        return -1;
    }

    printf("[test] initializing streams list\n");
    printf("[test] init_stream_list\n");
    stream_list_t *streams = init_stream_list();
    printf("[test] init_stream\n");
    stream_data_t *stream = init_stream(VIDEO, OUTPUT, 0, ACTIVE);
    printf("[test] set_stream_video_data\n");
    set_video_data(stream->video, H264, 1280, 720);
    printf("[test] add_stream\n");
    add_stream(streams, stream);

    printf("[test] initializing transmitter\n");
    transmitter_t *transmitter = init_transmitter(streams, 25.0);
    start_transmitter(transmitter);

    participant_data_t *p1 = init_participant(0, OUTPUT, "127.0.0.1", 8000);
    participant_data_t *p2 = init_participant(0, OUTPUT, "127.0.0.1", 9000);

    add_transmitter_participant(transmitter, p1);
    add_participant_stream(p1, stream);    

    add_transmitter_participant(transmitter, p2);
    add_participant_stream(p2, stream);

    init_encoder(stream->video);
    
    printf("[test_transmitter] transmitter->participants->first->id: %d\n", transmitter->participants->first->id);
    printf("[test_transmitter] transmitter->participants->last->id: %d\n", transmitter->participants->last->id);
    
    // Stuff ... 
    AVFormatContext *pformat_ctx = avformat_alloc_context();
    AVCodecContext codec_ctx;
    int video_stream = -1;
    av_register_all();

    int width = 1280;
    int height = 720;

    load_video(yuv_path, pformat_ctx, &codec_ctx, &video_stream);

    uint8_t *b1 = (uint8_t *)av_malloc(avpicture_get_size(codec_ctx.pix_fmt,
                        codec_ctx.width, codec_ctx.height)*sizeof(uint8_t));
    
    int counter = 0;

    printf("[test] entering main test loop\n");

    struct timeval a, b;

    while(1) {
    
        gettimeofday(&a, NULL);
        
        int ret = read_frame(pformat_ctx, video_stream, &codec_ctx, b1);
        if (stop_at > 0 && counter == stop_at) {
            break;
        }

        if (ret == 0) {
            counter++;

            pthread_rwlock_wrlock(&stream->video->decoded_frame_lock);
            stream->video->decoded_frame_len = vc_get_linesize(width, RGB)*height;
            memcpy(stream->video->decoded_frame, b1, stream->video->decoded_frame_len); 
            pthread_rwlock_unlock(&stream->video->decoded_frame_lock);

            sem_post(&stream->video->encoder->input_sem);
        } else {
            break;
        }
        gettimeofday(&b, NULL);
        long diff = (b.tv_sec - a.tv_sec)*1000000 + b.tv_usec - a.tv_usec;

        if (diff < 40000) {
            usleep(40000 - diff);
        } else {
            usleep(0);
        }
    }
    debug_msg(" deallocating resources and terminating threads\n");
    av_free(pformat_ctx);
    av_free(b1);
    debug_msg(" done!\n");

    stop_transmitter(transmitter);

    destroy_stream_list(streams);

    return 0;
}
