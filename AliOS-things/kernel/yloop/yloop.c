/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <sys/time.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include <errno.h>
#include <aos/aos.h>
#include <aos/network.h>

#include "yloop.h"

#define TAG "yloop"

typedef struct {
    int              sock;
    void            *private_data;
    aos_poll_call_t  cb;
} yloop_sock_t;

typedef struct yloop_timeout_s {
    dlist_t          next;
    long long        timeout_ms;
    void            *private_data;
    aos_call_t       cb;
    int              ms;
} yloop_timeout_t;

typedef struct {
    dlist_t          timeouts; /* ��ʱ�¼�����˫���� */
    struct pollfd   *pollfds;
    yloop_sock_t    *readers;
    int              eventfd; /* /dev/event��fd */
    uint8_t          max_sock; /* ����fd */
    uint8_t          reader_count; /* ʹ����yloop��task���� */
    bool             pending_terminate;
    bool             terminate; /* �ͷ���ֹyloop */
} yloop_ctx_t;

static yloop_ctx_t    *g_main_ctx;
static aos_task_key_t  g_loop_key;

static inline void _set_context(yloop_ctx_t *ctx)
{
    aos_task_setspecific(g_loop_key, ctx);
}

static inline yloop_ctx_t *_get_context(void)
{ /* ��g_loop_keyΪ��������ȡktask_t->user_info[g_loop_key] */
    return aos_task_getspecific(g_loop_key);
}

static inline yloop_ctx_t *get_context(void)
{
    yloop_ctx_t *ctx = _get_context();
    if (!ctx) {
        _set_context(g_main_ctx);
        return g_main_ctx;
    }
    return ctx;
}

void aos_loop_set_eventfd(int fd)
{ /* ����yloop��eventfd */
    yloop_ctx_t *ctx = get_context();
    ctx->eventfd = fd;
}

int aos_loop_get_eventfd(void *loop)
{/* ��ȡyloop��eventfd */
    yloop_ctx_t *ctx = loop ? loop : get_context();
    return ctx->eventfd;
}

aos_loop_t aos_current_loop(void)
{
    return get_context();
}
AOS_EXPORT(aos_loop_t, aos_current_loop, void);

aos_loop_t aos_loop_init(void)
{ /* ��ȡyloop_ctx_t�����֮ǰû�У��򴴽�(malloc)һ�� */
    yloop_ctx_t *ctx = _get_context(); /* �����k_task->user_info�� */

    if (!g_main_ctx) { /* ��֤ϵͳ��ֻ��һ��g_loop_key */
        aos_task_key_create(&g_loop_key);
    } else if (ctx) {
        LOGE(TAG, "yloop already inited");
        return ctx;
    }

    ctx = aos_zalloc(sizeof(*g_main_ctx));
    if (!g_main_ctx) {
        g_main_ctx = ctx;
    }

    dlist_init(&ctx->timeouts);
    ctx->eventfd = -1;
    _set_context(ctx); /* ��yloop_ctx_t�ŵ�ktask_t->user_info[g_loop_key] */

    aos_event_service_init(); /* ��eventfd(/dev/event)����yloop */

    return ctx;
}
AOS_EXPORT(aos_loop_t, aos_loop_init, void);

int aos_poll_read_fd(int sock, aos_poll_call_t cb, void *private_data)
{ /* ��sock�ŵ�yloop->readers[]�С��ظ�sock��ô��? */
    yloop_ctx_t *ctx = get_context();
    if (sock  < 0) {
        return -EINVAL;
    }

    yloop_sock_t *new_sock;
    struct pollfd *new_loop_pollfds;
    int cnt = ctx->reader_count + 1;

    new_sock = aos_malloc(cnt * sizeof(yloop_sock_t));
    new_loop_pollfds = aos_malloc(cnt * sizeof(struct pollfd));

    if (new_sock == NULL || new_loop_pollfds == NULL) {
        LOGE(TAG, "out of memory");
        aos_free(new_sock);
        aos_free(new_loop_pollfds);
        return -ENOMEM;
    }

    int status = aos_fcntl(sock, F_GETFL, 0);
    aos_fcntl(sock, F_SETFL, status | O_NONBLOCK); /* ��sockfd��Ϊ������ */

    ctx->reader_count++;

    memcpy(new_sock, ctx->readers, (cnt - 1) * sizeof(yloop_sock_t)); /* ��sockfd����yloop_t->readers[] */
    aos_free(ctx->readers);
    ctx->readers = new_sock;

    memcpy(new_loop_pollfds, ctx->pollfds, (cnt - 1) * sizeof(struct pollfd));
    aos_free(ctx->pollfds);
    ctx->pollfds = new_loop_pollfds;

    new_sock += cnt - 1;
    new_sock->sock = sock; /* ����yloop_t->readers[yloop_t-> reader_count-1]->sock*/
    new_sock->private_data = private_data; /* ����yloop_t->readers[yloop_t-> reader_count-1]���� */
    new_sock->cb = cb; /* ����yloop_t->readers[yloop_t-> reader_count-1]�ص����� */

    if (sock > ctx->max_sock) { /* ��������yloop_t���fd */
        ctx->max_sock = sock;
    }

    return 0;
}
AOS_EXPORT(int, aos_poll_read_fd, int, aos_poll_call_t, void *);

void aos_cancel_poll_read_fd(int sock, aos_poll_call_t action, void *param)
{
    yloop_ctx_t *ctx = get_context();
    if (ctx->readers == NULL || ctx->reader_count == 0) {
        return;
    }

    int i;
    for (i = 0; i < ctx->reader_count; i++) {
        if (ctx->readers[i].sock == sock) {
            break; /*���滹����ͬ��sock��ô��? */
        }
    }

    if (i == ctx->reader_count) {
        return;
    }

    if (i != ctx->reader_count - 1) {
        memmove(&ctx->readers[i], &ctx->readers[i + 1],
                (ctx->reader_count - i - 1) *
                sizeof(yloop_sock_t));
    }

    ctx->reader_count--;
}
AOS_EXPORT(void, aos_cancel_poll_read_fd, int, aos_poll_call_t, void *);

int aos_post_delayed_action(int ms, aos_call_t action, void *param)
{ /* ���һ����ʱ��ʱ�� */
    if (action == NULL) {
        return -EINVAL;
    }

    yloop_ctx_t *ctx = get_context();
    yloop_timeout_t *timeout = aos_malloc(sizeof(*timeout));
    if (timeout == NULL) {
        return -ENOMEM;
    }

    timeout->timeout_ms = aos_now_ms() + ms; /* ��ʱ����ʱʱ�� */
    timeout->private_data = param; /* ��ʱ���ص����� */
    timeout->cb = action; /* ��ʱ���ص����� */
    timeout->ms = ms; /* ��ʱʱ�� */

    yloop_timeout_t *tmp;

    dlist_for_each_entry(&ctx->timeouts, tmp, yloop_timeout_t, next) {
        if (timeout->timeout_ms < tmp->timeout_ms) {
            break;
        }
    } /* ����ʱʱ���������е�˫���� */

    dlist_add_tail(&timeout->next, &tmp->next); /* �Ѷ�ʱ�������������� */

    return 0;
}
AOS_EXPORT(int, aos_post_delayed_action, int, aos_call_t, void *);

void aos_cancel_delayed_action(int ms, aos_call_t cb, void *private_data)
{ /* ɾ����ʱ��ʱ�� */
    yloop_ctx_t *ctx = get_context();
    yloop_timeout_t *tmp;
    /* O(n)�ܷ��Ż���O(1)? */
    dlist_for_each_entry(&ctx->timeouts, tmp, yloop_timeout_t, next) {
        if (ms != -1 && tmp->ms != ms) {
            continue;
        }

        if (tmp->cb != cb) {
            continue;
        }

        if (tmp->private_data != private_data) {
            continue;
        }

        dlist_del(&tmp->next);
        aos_free(tmp);
        return;
    }
}
AOS_EXPORT(void, aos_cancel_delayed_action, int, aos_call_t, void *);

void aos_loop_run(void)
{
    yloop_ctx_t *ctx = get_context();

    while (!ctx->terminate &&
           (!dlist_empty(&ctx->timeouts) || ctx->reader_count > 0)) { /* ���ڶ�ʱ���¼�����IO�¼� */
        int delayed_ms = -1;
        int readers = ctx->reader_count;
        int i;

        if (!dlist_empty(&ctx->timeouts)) {
            yloop_timeout_t *tmo = dlist_first_entry(&ctx->timeouts, yloop_timeout_t, next);
            long long now = aos_now_ms();

            if (now < tmo->timeout_ms) {
                delayed_ms = tmo->timeout_ms - now;
            } else {
                delayed_ms = 0;
            } /* �ҵ����һ����ʱ����ʱ���ʱ�� */
        }

        for (i = 0; i < readers; i++) {
            ctx->pollfds[i].fd = ctx->readers[i].sock;
            ctx->pollfds[i].events = POLLIN;
        }

        int res = aos_poll(ctx->pollfds, readers, delayed_ms); /* IO��·���� */

        if (res < 0 && errno != EINTR) {
            LOGE(TAG, "aos_poll");
            return;
        }

        /* check if some registered timeouts have occurred */ /* �ٴ�ִ�е��ˣ�˵����ʱ���¼����� */
        if (!dlist_empty(&ctx->timeouts)) {
            yloop_timeout_t *tmo = dlist_first_entry(&ctx->timeouts, yloop_timeout_t, next);
            long long now = aos_now_ms();

            if (now >= tmo->timeout_ms) {
                dlist_del(&tmo->next); /* ɾ��׼��ִ�еĶ�ʱ�� */
                tmo->cb(tmo->private_data); /* ִ��ÿһ����ʱ���¼��Ļص����� */
                aos_free(tmo);
            }
        }

        if (res <= 0) {
            continue;
        }

        for (i = 0; i < readers; i++) { /* ִ��ÿһ��IO�¼��Ļص����� */
            if (ctx->pollfds[i].revents & POLLIN) {
                ctx->readers[i].cb(
                    ctx->readers[i].sock,
                    ctx->readers[i].private_data);
            }
        }
    }

    ctx->terminate = 0;
}
AOS_EXPORT(void, aos_loop_run, void);

void aos_loop_exit(void)
{
    yloop_ctx_t *ctx = get_context();
    ctx->terminate = 1;
}
AOS_EXPORT(void, aos_loop_exit, void);

void aos_loop_destroy(void)
{
    yloop_ctx_t *ctx = _get_context();

    if (ctx == NULL) {
        return;
    }

    aos_event_service_deinit(ctx->eventfd);

    while (!dlist_empty(&ctx->timeouts)) {
        yloop_timeout_t *timeout = dlist_first_entry(&ctx->timeouts, yloop_timeout_t,
                                                     next);
        dlist_del(&timeout->next);
        aos_free(timeout);
    }

    aos_free(ctx->readers);
    aos_free(ctx->pollfds);

    _set_context(NULL);
    if (ctx == g_main_ctx) {
        g_main_ctx = NULL;
    }
    aos_free(ctx);
}
AOS_EXPORT(void, aos_loop_destroy, void);

