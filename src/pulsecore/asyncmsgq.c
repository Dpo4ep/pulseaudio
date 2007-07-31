/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <errno.h>

#include <pulsecore/atomic.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/semaphore.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/flist.h>
#include <pulse/xmalloc.h>

#include "asyncmsgq.h"

PA_STATIC_FLIST_DECLARE(asyncmsgq, 0, pa_xfree);

struct asyncmsgq_item {
    int code;
    pa_msgobject *object;
    void *userdata;
    pa_free_cb_t free_cb;
    int64_t offset;
    pa_memchunk memchunk;
    pa_semaphore *semaphore;
    int ret;
};

struct pa_asyncmsgq {
    pa_asyncq *asyncq;
    pa_mutex *mutex; /* only for the writer side */

    struct asyncmsgq_item *current;
};

pa_asyncmsgq *pa_asyncmsgq_new(unsigned size) {
    pa_asyncmsgq *a;

    a = pa_xnew(pa_asyncmsgq, 1);

    pa_assert_se(a->asyncq = pa_asyncq_new(size));
    pa_assert_se(a->mutex = pa_mutex_new(0));
    a->current = NULL;

    return a;
}

void pa_asyncmsgq_free(pa_asyncmsgq *a) {
    struct asyncmsgq_item *i;
    pa_assert(a);

    while ((i = pa_asyncq_pop(a->asyncq, 0))) {

        pa_assert(!i->semaphore);

        if (i->object)
            pa_msgobject_unref(i->object);

        if (i->memchunk.memblock)
            pa_memblock_unref(i->memchunk.memblock);

        if (i->free_cb)
            i->free_cb(i->userdata);

        if (pa_flist_push(PA_STATIC_FLIST_GET(asyncmsgq), i) < 0)
            pa_xfree(i);
    }

    pa_asyncq_free(a->asyncq, NULL);
    pa_mutex_free(a->mutex);
    pa_xfree(a);
}

void pa_asyncmsgq_post(pa_asyncmsgq *a, pa_msgobject *object, int code, const void *userdata, int64_t offset, const pa_memchunk *chunk, pa_free_cb_t free_cb) {
    struct asyncmsgq_item *i;
    pa_assert(a);

    if (!(i = pa_flist_pop(PA_STATIC_FLIST_GET(asyncmsgq))))
        i = pa_xnew(struct asyncmsgq_item, 1);

    i->code = code;
    i->object = object ? pa_msgobject_ref(object) : NULL;
    i->userdata = (void*) userdata;
    i->free_cb = free_cb;
    i->offset = offset;
    if (chunk) {
        pa_assert(chunk->memblock);
        i->memchunk = *chunk;
        pa_memblock_ref(i->memchunk.memblock);
    } else
        pa_memchunk_reset(&i->memchunk);
    i->semaphore = NULL;

    /* Thus mutex makes the queue multiple-writer safe. This lock is only used on the writing side */
    pa_mutex_lock(a->mutex);
    pa_assert_se(pa_asyncq_push(a->asyncq, i, 1) == 0);
    pa_mutex_unlock(a->mutex);
}

int pa_asyncmsgq_send(pa_asyncmsgq *a, pa_msgobject *object, int code, const void *userdata, int64_t offset, const pa_memchunk *chunk) {
    struct asyncmsgq_item i;
    pa_assert(a);

    i.code = code;
    i.object = object;
    i.userdata = (void*) userdata;
    i.free_cb = NULL;
    i.ret = -1;
    i.offset = offset;
    if (chunk) {
        pa_assert(chunk->memblock);
        i.memchunk = *chunk;
    } else
        pa_memchunk_reset(&i.memchunk);
    pa_assert_se(i.semaphore = pa_semaphore_new(0));

    /* Thus mutex makes the queue multiple-writer safe. This lock is only used on the writing side */
    pa_mutex_lock(a->mutex);
    pa_assert_se(pa_asyncq_push(a->asyncq, &i, 1) == 0);
    pa_mutex_unlock(a->mutex);

    pa_semaphore_wait(i.semaphore);
    pa_semaphore_free(i.semaphore);

    return i.ret;
}

int pa_asyncmsgq_get(pa_asyncmsgq *a, pa_msgobject **object, int *code, void **userdata, int64_t *offset, pa_memchunk *chunk, int wait) {
    pa_assert(a);
    pa_assert(code);
    pa_assert(!a->current);

    if (!(a->current = pa_asyncq_pop(a->asyncq, wait))) {
/*         pa_log("failure"); */
        return -1;
    }

/*     pa_log("success"); */

    *code = a->current->code;
    if (userdata)
        *userdata = a->current->userdata;
    if (offset)
        *offset = a->current->offset;
    if (object) {
        if ((*object = a->current->object))
            pa_msgobject_assert_ref(*object);
    }
    if (chunk)
        *chunk = a->current->memchunk;

    pa_log_debug("Get q=%p object=%p (%s) code=%i data=%p chunk.length=%u", (void*) a, (void*) a->current->object, a->current->object ? a->current->object->parent.type_name : NULL, a->current->code, (void*) a->current->userdata, a->current->memchunk.length);
    
    return 0;
}

void pa_asyncmsgq_done(pa_asyncmsgq *a, int ret) {
    pa_assert(a);
    pa_assert(a->current);

    if (a->current->semaphore) {
        a->current->ret = ret;
        pa_semaphore_post(a->current->semaphore);
    } else {

        if (a->current->free_cb)
            a->current->free_cb(a->current->userdata);

        if (a->current->object)
            pa_msgobject_unref(a->current->object);

        if (a->current->memchunk.memblock)
            pa_memblock_unref(a->current->memchunk.memblock);

        if (pa_flist_push(PA_STATIC_FLIST_GET(asyncmsgq), a->current) < 0)
            pa_xfree(a->current);
    }

    a->current = NULL;
}

int pa_asyncmsgq_wait_for(pa_asyncmsgq *a, int code) {
    int c;
    pa_assert(a);

    do {
        pa_msgobject *o;
        void *data;
    int64_t offset;
        pa_memchunk chunk;
        int ret;

        if (pa_asyncmsgq_get(a, &o, &c, &data, &offset, &chunk, 1) < 0)
            return -1;

        ret = pa_asyncmsgq_dispatch(o, c, data, offset, &chunk);
        pa_asyncmsgq_done(a, ret);

    } while (c != code);

    return 0;
}

int pa_asyncmsgq_get_fd(pa_asyncmsgq *a) {
    pa_assert(a);

    return pa_asyncq_get_fd(a->asyncq);
}

int pa_asyncmsgq_before_poll(pa_asyncmsgq *a) {
    pa_assert(a);

    return pa_asyncq_before_poll(a->asyncq);
}

void pa_asyncmsgq_after_poll(pa_asyncmsgq *a) {
    pa_assert(a);

    pa_asyncq_after_poll(a->asyncq);
}

int pa_asyncmsgq_dispatch(pa_msgobject *object, int code, void *userdata, int64_t offset, pa_memchunk *memchunk) {

    if (object)
        return object->process_msg(object, code, userdata, offset, memchunk);

    return 0;
}
