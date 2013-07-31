#ifndef _MESSAGING_H
#define _MESSAGING_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif

//#include <transmit.h>

#ifdef __cplusplus
extern "C" {
#endif

struct messaging;
struct module;

struct response {
        int status;
        void (*deleter)(struct response*);
        char *text;
};

#define RESPONSE_OK           200
#define RESPONSE_ACCEPTED     202
#define RESPONSE_BAD_REQUEST  400
#define RESPONSE_NOT_FOUND    404
#define RESPONSE_INT_SERV_ERR 500
#define RESPONSE_NOT_IMPL     501

struct message;

struct message {
        /**
         * Individual message types may have defined custom data deleters.
         * Please note that the deleter must not delete the struct itself.
         */
        void (*data_deleter)(struct message *);
};

struct msg_change_receiver_address {
        struct message m;
        char receiver[128];
};

struct msg_change_fec_data {
        struct message m;
        //enum tx_media_type media_type;
        char fec[128];
};

struct msg_change_compress_data {
        struct message m;
        enum {
                CHANGE_COMPRESS,
                CHANGE_PARAMS
        } what;
        char config_string[128];
};

struct msg_stats {
        struct message m;
        char what[128];
        int value;
};

struct response *new_response(int status, char *optional_message);

typedef struct response *(*msg_callback_t)(struct module *mod, struct message *msg);

struct response *send_message(struct module *, const char *path, struct message *msg);
struct response *send_message_to_receiver(struct module *, struct message *msg);
struct message *new_message(size_t length);
void free_message(struct message *m);
const char *response_status_to_text(int status);


#ifdef __cplusplus
}
#endif

#endif// _MESSAGING_H

