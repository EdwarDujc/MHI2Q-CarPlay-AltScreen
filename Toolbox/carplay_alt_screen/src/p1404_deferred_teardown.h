/* Included after fullchain_teardown_admitted. Each task holds the receiver,
 * request and a guard reservation until cleanup, so timeout cannot lose the
 * request or forget that another full teardown already completed. */
#ifndef P1404_DEFERRED_TEARDOWN_H
#define P1404_DEFERRED_TEARDOWN_H
#define ALT_DEFERRED_TEARDOWN_SLOTS 32u
struct fullchain_deferred_teardown {
    void *receiver;
    const void *params;
    int reason, full;
    volatile int ready;
    struct alt_session_lease fence;
};
static struct fullchain_deferred_teardown deferred_teardowns[ALT_DEFERRED_TEARDOWN_SLOTS];
static volatile unsigned deferred_teardown_lock;

static void deferred_teardown_acquire(void) {
    while (__sync_lock_test_and_set(&deferred_teardown_lock, 1u)) usleep(1000u);
}
static void deferred_teardown_release(void) {
    __sync_lock_release(&deferred_teardown_lock);
}

static void *fullchain_deferred_teardown_worker(void *arg) {
    struct fullchain_deferred_teardown *job = arg;
    struct alt_session_lease lease;
    unsigned char done = 0;
    int admission, finished = 0;
    unsigned retries = 0;
    while (!job->ready) usleep(1000u);
    __sync_synchronize();
    do {
        admission = fullchain_lifecycle_enter(job->receiver, ALT_SESSION_DEFERRED_TEARDOWN, &lease);
        if (admission == ALT_SESSION_ENTERED)
            finished = fullchain_teardown_admitted(job->receiver, job->params, job->reason, &done, &lease);
        else {
            done = (unsigned char)lease.completed;
            finished = lease.completed;
            fullchain_lifecycle_leave(job->receiver, &lease, 0);
        }
        if (!finished) {
            ++retries;
            if (retries == 1u || (retries % 60u) == 0u)
                altscreen_log("WARN PHASE=TEARDOWN_DEFERRED_RETRY receiver=%p attempts=%u resources_retained=1", job->receiver, retries);
            /* One retained job, no recursive queue growth or busy loop. A
             * failed join/Stop may become safe later; never force deletion. */
            usleep(1000000u);
        }
    } while (!finished);
    altscreen_log("PHASE=TEARDOWN_DEFERRED_FINISH receiver=%p admission=%d done=%d",
                  job->receiver, admission, (int)done);
    deferred_teardown_acquire(); job->ready = 2; deferred_teardown_release();
    if (job->params) alt_airplay_release_object((void *)job->params);
    fullchain_lifecycle_leave(job->receiver, &job->fence, 0);
    deferred_teardown_acquire();
    memset(job, 0, sizeof(*job));
    deferred_teardown_release();
    return NULL;
}

static int fullchain_defer_teardown(void *receiver, const void *params, int reason,
                                   struct alt_session_lease *fence) {
    struct fullchain_deferred_teardown *job = NULL;
    pthread_t thread;
    int (*create_fn)(pthread_t *, const void *, void *(*)(void *), void *);
    int (*detach_fn)(pthread_t);
    int full, rc;
    unsigned i;
    (void)fullchain_teardown_scope(params, &full);
    detach_fn = (int (*)(pthread_t))dlsym(RTLD_DEFAULT, "pthread_detach");
    create_fn = (int (*)(pthread_t *, const void *, void *(*)(void *), void *))
        dlsym(RTLD_DEFAULT, "pthread_create");
    if (!detach_fn || !create_fn) return 0;
    deferred_teardown_acquire();
    for (i = 0; i < ALT_DEFERRED_TEARDOWN_SLOTS; ++i) {
        struct fullchain_deferred_teardown *current = &deferred_teardowns[i];
        if (current->ready == 1 && current->receiver == receiver && current->full) {
            deferred_teardown_release();
            return 1; /* existing retained request covers this stop */
        }
        if (!current->receiver && !job) job = current;
    }
    if (job) {
        memset(job, 0, sizeof(*job));
        job->receiver = receiver;
        job->params = params;
        job->reason = reason;
        job->full = full;
        job->fence = *fence;
    }
    deferred_teardown_release();
    if (!job) {
        altscreen_log("ERROR PHASE=TEARDOWN_DEFERRED_QUEUE receiver=%p reason=queue_full retry_required=1", receiver);
        return 0;
    }
    if (params && !alt_airplay_retain_object((void *)params)) {
        deferred_teardown_acquire(); memset(job, 0, sizeof(*job)); deferred_teardown_release();
        return 0;
    }
    if (!alt_session_guard_defer(fence)) {
        if (params) alt_airplay_release_object((void *)params);
        deferred_teardown_acquire(); memset(job, 0, sizeof(*job)); deferred_teardown_release();
        return 0;
    }
    job->fence = *fence;
    /* No metadata lock around thread creation. The caller still owns fence
     * until successful creation; the worker waits for ownership publication. */
    rc = create_fn(&thread, NULL, fullchain_deferred_teardown_worker, job);
    if (rc != 0) {
        if (params) alt_airplay_release_object((void *)params);
        deferred_teardown_acquire(); memset(job, 0, sizeof(*job)); deferred_teardown_release();
        altscreen_log("ERROR PHASE=TEARDOWN_DEFERRED_QUEUE receiver=%p thread_rc=%d retry_required=1", receiver, rc);
        return 0;
    }
    rc = detach_fn(thread);
    if (rc) altscreen_log("ERROR PHASE=TEARDOWN_DEFERRED_DETACH receiver=%p rc=%d", receiver, rc);
    memset(fence, 0, sizeof(*fence)); /* transferred, not released by caller */
    altscreen_log("PHASE=TEARDOWN_DEFERRED_QUEUE receiver=%p full=%d request_retained=1 guard_reservation_held=1", receiver, full);
    __sync_synchronize();
    job->ready = 1;
    return 1;
}

static void fullchain_retry_cleanup(void *receiver, const void *params, int reason,
                                   struct alt_session_lease *owner) {
    struct alt_session_lease reservation;
    if (!alt_airplay_retain_object(receiver)) return;
    if (!alt_session_guard_reserve_retry(owner, &reservation)) {
        alt_airplay_release_object(receiver);
        return;
    }
    reservation.retained = 1;
    if (!fullchain_defer_teardown(receiver, params, reason, &reservation))
        altscreen_log("ERROR PHASE=TEARDOWN_CLEANUP_RETRY receiver=%p queued=0", receiver);
    fullchain_lifecycle_leave(receiver, &reservation, 0);
}
#endif
