#include "participants.h"
#include "video_decompress.h"
#include "video_decompress/libavcodec.h"

participant_data_t *init_participant(int width, int height, codec_t codec, char *dst, uint32_t port, ptype_t type){
  participant_data_t *participant;
  rtp_session_t *rtp;
  
  participant = (participant_data_t *) malloc(sizeof(participant_data_t));
  
  pthread_mutex_init(&participant->lock, NULL);
  participant->new = FALSE;
  participant->ssrc = 0;
  participant->frame = NULL;
  participant->height = height;
  participant->width = width;
  participant->codec = codec;
  participant->proc.decoder = participant->proc.encoder = NULL;
  participant->next = participant->previous = NULL;
  participant->type = type;
  
  if (dst == NULL) {
    participant->session = NULL;
  } else {
    rtp = (rtp_session_t *) malloc(sizeof(rtp_session_t));
    rtp->addr = dst;
    rtp->port = port;
    participant->session = rtp;
  }
  
  return participant;
}

void destroy_participant(participant_data_t *src){
  free(src->frame);
  free(src->session);
  
  if (src->type == INPUT){
    destroy_decoder_thread(src->proc.decoder);
  }
  
  pthread_mutex_destroy(&src->lock);
  
  free(src);
}

participant_list_t *init_participant_list(){
  participant_list_t 	*list;
    
  list = (participant_list_t *) malloc(sizeof(participant_list_t));
  
  pthread_rwlock_init(&list->lock, NULL);
  
  list->count = 0;
  list->first = NULL;
  list->last = NULL;
  
  return list;
}

decoder_thread_t *init_decoder_thread(participant_data_t *src){
	decoder_thread_t *decoder;
	struct video_desc des;
	
	initialize_video_decompress();
	
	decoder = malloc(sizeof(decoder_thread_t));
	decoder->data = malloc(vc_get_linesize(src->width, RGB)*src->height*sizeof(char));
	decoder->new_frame = FALSE;
	decoder->run = FALSE;
	
	pthread_mutex_init(&decoder->lock, NULL);
	pthread_cond_init(&decoder->notify_frame, NULL);
	
	if (decompress_is_available(LIBAVCODEC_MAGIC)) { //TODO: add some magic to determine codec
	  decoder->sd = decompress_init(LIBAVCODEC_MAGIC);

	  des.width = src->width;
	  des.height = src->height;
	  des.color_spec  = src->codec;
	  des.tile_count = 0;
	  des.interlacing = PROGRESSIVE;
        
	  decompress_reconfigure(decoder->sd, des, 16, 8, 0, vc_get_linesize(des.width, UYVY), UYVY);
      } else {
	//TODO: error_msg
      }
      
      return decoder;
}

void destroy_decoder_thread(decoder_thread_t *dec_th){
    assert(dec_th->run == FALSE);
  
    pthread_mutex_destroy(&dec_th->lock);
    pthread_cond_destroy(&dec_th->notify_frame);
    
    decompress_done(dec_th->sd);
    
    free(dec_th->data);
    free(dec_th);
}

int add_participant(participant_list_t *list, int width, int height, codec_t codec, char *dst, uint32_t port, ptype_t type){
  struct participant_data *participant;
  
  participant = init_participant(width, height, codec, dst, port, type);
  
  if (list->count == 0) {
    
    assert(list->first == NULL && list->last == NULL);
    list->count++;
    list->first = list->last = participant;
    
  } else if (list->count > 0){
    
    assert(list->first != NULL && list->last != NULL);
    participant->previous = list->last;
    list->count++;
    list->last->next = participant;
    list->last = participant;

  } else{
    //TODO: error_msg
    return FALSE;
  }
  
  return TRUE;
}

participant_data_t *get_participant_id(participant_list_t *list, uint32_t id){
  participant_data_t *participant;
  
  participant = list->first;
  while(participant != NULL){
    if(participant->id == id)
      return participant;
    participant = participant->next;
  }

  return NULL;
}

participant_data_t *get_participant_ssrc(participant_list_t *list, uint32_t ssrc){
  participant_data_t *participant;
  
  participant = list->first;
  while(participant != NULL){
    if(participant->ssrc == ssrc){
      return participant;
    }
    if (participant->ssrc == 0){
      assert(participant->proc.decoder == NULL);
      return participant;
    }
    participant = participant->next;
  }

  return NULL;
}

int remove_participant(participant_list_t *list, uint32_t id){
  participant_data_t *participant;
  
  if (list->count == 0) {
    return FALSE;
  }
  
  participant = get_participant_id(list, id);
  
  if (participant == NULL)
    return FALSE;

  pthread_mutex_lock(&participant->lock);
  
  if (participant->type == INPUT && participant->proc.decoder->run == TRUE){
   participant->proc.decoder->run = FALSE;
   
   pthread_mutex_unlock(&participant->lock);
   pthread_join(participant->proc.decoder->th_id, NULL); //TODO: timeout to force thread kill
   pthread_mutex_lock(&participant->lock);
  } else if (participant->type == OUTPUT /*&& participant->proc.encoder->run == TRUE*/){
    //TODO: check if there is a thread to stop
  }
  
  
  if (participant->next == NULL){
    assert(list->last == participant);
    list->last = participant->previous;
  } else if (participant->previous == NULL){
    assert(list->first == participant);
    list->first = participant->next;
  } else {
    assert(participant->next != NULL && participant->next != NULL);
    participant->previous->next = participant->next;
  }
  list->count--;
  
  pthread_mutex_unlock(&participant->lock);
  
  destroy_participant(participant);
  
  return TRUE;
}

void destroy_participant_list(participant_list_t *list){
  participant_data_t *participant;
  
  participant = list->first;

  while(participant != NULL){
    remove_participant(list, participant->id);
    participant = participant->next;
  }
  
  assert(list->count == 0);
 
  pthread_rwlock_destroy(&list->lock);
  
  free(list);
}
