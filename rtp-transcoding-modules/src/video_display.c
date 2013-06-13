/*
 * FILE:   video_display.c
 * AUTHOR: Colin Perkins <csp@isi.edu>
 *         Martin Benes     <martinbenesh@gmail.com>
 *         Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 *         Petr Holub       <hopet@ics.muni.cz>
 *         Milos Liska      <xliska@fi.muni.cz>
 *         Jiri Matela      <matela@ics.muni.cz>
 *         Dalibor Matura   <255899@mail.muni.cz>
 *         Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 *
 * Copyright (c) 2001-2003 University of Southern California
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
 *      This product includes software developed by the University of Southern
 *      California Information Sciences Institute. This product also includes
 *      software developed by CESNET z.s.p.o.
 * 
 * 4. Neither the name of the University, Institute, CESNET nor the names of
 *    its contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
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

#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#include "debug.h"
#include "perf.h"
#include "video_display.h"

#include "video_display/net.h"

#include "lib_common.h"

#include <string.h>

display_type_t *(*display_get_device_details_extrn)(int index) = display_get_device_details;
void (*display_free_devices_extrn)(void) = display_free_devices;
display_id_t (*display_get_null_device_id_extrn)(void) = display_get_null_device_id;
int (*display_init_extrn)(display_id_t id, char *fmt, unsigned int flags, struct display **) = display_init;
int (*display_get_device_count_extrn)(void) = display_get_device_count;
int (*display_init_devices_extrn)(void)  = display_init_devices;

/*
 * Interface to probing the valid display types. 
 *
 */

typedef struct {
        display_id_t              id;
        const  char              *library_name;
        display_type_t         *(*func_probe) (void);
        const char               *func_probe_str;
        void                   *(*func_init) (char *fmt, unsigned int flags);
        const char               *func_init_str;
        void                    (*func_run) (void *state);
        const char               *func_run_str;
        void                    (*func_done) (void *state);
        const char               *func_done_str;
        void                    (*func_finish) (void *state);
        const char               *func_finish_str;
        struct video_frame     *(*func_getf) (void *state);
        const char               *func_getf_str;
        int                     (*func_putf) (void *state, struct video_frame *frame, int nonblock);
        const char               *func_putf_str;
        int                     (*func_reconfigure)(void *state, struct video_desc desc);
        const char               *func_reconfigure_str;
        int                     (*func_get_property)(void *state, int property, void *val, size_t *len);
        const char               *func_get_property_str;
        
        void                    (*func_put_audio_frame) (void *state, struct audio_frame *frame);
        const char               *func_put_audio_frame_str;
        int                     (*func_reconfigure_audio) (void *state, int quant_samples, int channels,
                        int sample_rate);
        const char               *func_reconfigure_audio_str;

        void                     *handle;
} display_table_t;

static display_table_t display_device_table[] = {
        {
         0,
         NULL,
         MK_STATIC(display_net_probe),
         MK_STATIC(display_net_init),
         MK_STATIC(display_net_run),
         MK_STATIC(display_net_done),
         MK_STATIC(display_net_finish),
         MK_STATIC(display_net_getf),
         MK_STATIC(display_net_putf),
         MK_STATIC(display_net_reconfigure),
         MK_STATIC(display_net_get_property),
         MK_STATIC(display_net_put_audio_frame),
         MK_STATIC(display_net_reconfigure_audio),
         NULL
         }
};

#define DISPLAY_DEVICE_TABLE_SIZE (sizeof(display_device_table) / sizeof(display_table_t))

static display_type_t *available_devices[DISPLAY_DEVICE_TABLE_SIZE];
static int available_device_count = 0;

#ifdef BUILD_LIBRARIES
static int display_fill_symbols(display_table_t *device);
static void *display_open_library(const char *display_name);

static void *display_open_library(const char *display_name)
{
        char name[128];
        snprintf(name, sizeof(name), "display_%s.so.%d", display_name, VIDEO_DISPLAY_ABI_VERSION);

        return open_library(name);
}

static int display_fill_symbols(display_table_t *device)
{
        void *handle = device->handle;

        device->func_probe = (display_type_t *(*) (void))
                dlsym(handle, device->func_probe_str);
        device->func_init = (void *(*) (char *, unsigned int))
                dlsym(handle, device->func_init_str);
        device->func_run = (void (*) (void *))
                dlsym(handle, device->func_run_str);
        device->func_done = (void (*) (void *))
                dlsym(handle, device->func_done_str);
        device->func_finish = (void (*) (void *))
                dlsym(handle, device->func_finish_str);
        device->func_getf = (struct video_frame *(*) (void *))
                dlsym(handle, device->func_getf_str);
        device->func_putf = (int (*) (void *, struct video_frame *, int))
                dlsym(handle, device->func_putf_str);
        device->func_reconfigure = (int (*)(void *, struct video_desc))
                dlsym(handle, device->func_reconfigure_str);
        device->func_get_property = (int (*)(void *, int, void *, size_t *))
                dlsym(handle, device->func_get_property_str);
        
        device->func_put_audio_frame = (void (*) (void *, struct audio_frame *))
                dlsym(handle, device->func_put_audio_frame_str);
        device->func_reconfigure_audio = (int (*) (void *, int, int,
                        int))
                dlsym(handle, device->func_reconfigure_audio_str);

        if(!device->func_probe || !device->func_init || !device->func_run ||
                        !device->func_done || !device->func_finish ||
                        !device->func_getf || !device->func_getf ||
                        !device->func_putf || !device->func_reconfigure ||
                        !device->func_get_property ||
                        !device->func_put_audio_frame || !device->func_reconfigure_audio) {
                fprintf(stderr, "Library %s opening error: %s \n", device->library_name, dlerror());
                return FALSE;
        }

        return TRUE;
}
#endif

int display_init_devices(void)
{
        unsigned int i;
        display_type_t *dt;

        assert(available_device_count == 0);

        for (i = 0; i < DISPLAY_DEVICE_TABLE_SIZE; i++) {
#ifdef BUILD_LIBRARIES
                display_device_table[i].handle = NULL;
                if(display_device_table[i].library_name) {
                        display_device_table[i].handle =
                                display_open_library(display_device_table[i].library_name);
                        if(display_device_table[i].handle) {
                                int ret;
                                ret = display_fill_symbols(&display_device_table[i]);
                                if(!ret) continue;
                        } else {
                                continue;
                        }
                }
#endif
                dt = display_device_table[i].func_probe();
                if (dt != NULL) {
                        display_device_table[i].id = dt->id;
                        available_devices[available_device_count++] = dt;
                }
        }
        return 0;
}

void display_free_devices(void)
{
        int i;

        for (i = 0; i < available_device_count; i++) {
                free(available_devices[i]);
                available_devices[i] = NULL;
        }
        available_device_count = 0;
}

int display_get_device_count(void)
{
        return available_device_count;
}

display_type_t *display_get_device_details(int index)
{
        assert(index < available_device_count);
        assert(available_devices[index] != NULL);

        return available_devices[index];
}

display_id_t display_get_null_device_id(void)
{
        return 0;
}

/*
 * Display initialisation and playout routines...
 */

#define DISPLAY_MAGIC 0x01ba7ef1

struct display {
        uint32_t magic;
        int index;
        void *state;
};

int display_init_noerr;

int display_init(display_id_t id, char *fmt, unsigned int flags, struct display **state)
{
        unsigned int i;

        for (i = 0; i < DISPLAY_DEVICE_TABLE_SIZE; i++) {
                if (display_device_table[i].id == id) {
                        struct display *d =
                            (struct display *)malloc(sizeof(struct display));
                        d->magic = DISPLAY_MAGIC;
                        d->state = display_device_table[i].func_init(fmt, flags);
                        d->index = i;
                        if (d->state == NULL) {
                                debug_msg("Unable to start display 0x%08lx\n",
                                          id);
                                free(d);
                                return -1;
                        } else if (d->state == &display_init_noerr) {
                                free(d);
                                return 1;
                        }
                        *state = d;
                        return 0;
                }
        }
        debug_msg("Unknown display id: 0x%08x\n", id);
        return -1;
}

void display_finish(struct display *d)
{
        assert(d->magic == DISPLAY_MAGIC);
        display_device_table[d->index].func_finish(d->state);
}

void display_done(struct display *d)
{
        assert(d->magic == DISPLAY_MAGIC);
        display_device_table[d->index].func_done(d->state);
        free(d);
}

void display_run(struct display *d)
{
        assert(d->magic == DISPLAY_MAGIC);
        display_device_table[d->index].func_run(d->state);
}

struct video_frame *display_get_frame(struct display *d)
{
        perf_record(UVP_GETFRAME, d);
        assert(d->magic == DISPLAY_MAGIC);
        return display_device_table[d->index].func_getf(d->state);
}

int display_put_frame(struct display *d, struct video_frame *frame, int nonblock)
{
        perf_record(UVP_PUTFRAME, frame);
        assert(d->magic == DISPLAY_MAGIC);
        return display_device_table[d->index].func_putf(d->state, frame, nonblock);
}

int display_reconfigure(struct display *d, struct video_desc desc)
{
        assert(d->magic == DISPLAY_MAGIC);
        return display_device_table[d->index].func_reconfigure(d->state, desc);
}

int display_get_property(struct display *d, int property, void *val, size_t *len)
{
        assert(d->magic == DISPLAY_MAGIC);
        return display_device_table[d->index].func_get_property(d->state, property, val, len);
}

void display_put_audio_frame(struct display *d, struct audio_frame *frame)
{
        assert(d->magic == DISPLAY_MAGIC);
        display_device_table[d->index].func_put_audio_frame(d->state, frame);
}

int display_reconfigure_audio(struct display *d, int quant_samples, int channels, int sample_rate)
{
        assert(d->magic == DISPLAY_MAGIC);
        return display_device_table[d->index].func_reconfigure_audio(d->state, quant_samples, channels, sample_rate);
}

