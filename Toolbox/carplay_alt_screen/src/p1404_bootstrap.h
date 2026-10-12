/* Included by fullchain after the session guard. Metadata never spans OEM I/O. */
#ifndef P1404_BOOTSTRAP_H
#define P1404_BOOTSTRAP_H
#include <time.h>
#ifndef ALT_BOOTSTRAP_ACK_POLLS
#define ALT_BOOTSTRAP_ACK_POLLS 40u
#endif
#ifndef ALT_BOOTSTRAP_POLL_US
#define ALT_BOOTSTRAP_POLL_US 100000u
#endif
#define ALT_BOOTSTRAP_ATTEMPTS 3u
static struct {
    void *receiver;
    int main_ready, sent, worker, pending, failed, show_ack, key_ack, local_failed;
    uint32_t token, request;
} bootstrap[8];
static volatile unsigned bootstrap_guard;
static uint32_t bootstrap_token;
struct bootstrap_job { void *receiver; unsigned slot; uint32_t token; };

static void bootstrap_lock(void) {
    while (__sync_lock_test_and_set(&bootstrap_guard, 1u)) p1404_lock_wait_yield();
}
static void bootstrap_unlock(void) { __sync_lock_release(&bootstrap_guard); }
static uint32_t bootstrap_identity_locked(void) {
    if (bootstrap_token == UINT32_MAX) return 0;
    return ++bootstrap_token;
}
static void bootstrap_forget(void *receiver) {
    unsigned i;
    bootstrap_lock();
    for (i = 0; i < 8; ++i)
        if (bootstrap[i].receiver == receiver) memset(&bootstrap[i], 0, sizeof(bootstrap[i]));
    bootstrap_unlock();
}
static int bootstrap_command_current(void *receiver, uint32_t seq) {
    unsigned i; int current = 0;
    bootstrap_lock();
    for (i = 0; i < 8; ++i)
        if (bootstrap[i].receiver == receiver && bootstrap[i].request == seq &&
            bootstrap[i].pending && !bootstrap[i].failed) { current = 1; break; }
    bootstrap_unlock();
    return current;
}
static void bootstrap_event_result(void *receiver, uint32_t seq, int event,
                                    int status, int response) {
    unsigned i; int applied = 0;
    bootstrap_lock();
    for (i = 0; i < 8; ++i) {
        if (bootstrap[i].receiver != receiver || bootstrap[i].request != seq ||
            !bootstrap[i].pending) continue;
        applied = 1;
        if (status || !response) { bootstrap[i].failed = 1; if (status && !response) bootstrap[i].local_failed = 1; }
        else if (event == ALT111_EVENT_SHOW_UI) bootstrap[i].show_ack = 1;
        else if (event == ALT111_EVENT_FORCE_KEYFRAME) bootstrap[i].key_ack = 1;
        break;
    }
    bootstrap_unlock();
    altscreen_log("PHASE=ALT111_BOOTSTRAP_RESULT receiver=%p request=%u event=%d status=%d response=%d applied=%d",
                  receiver, seq, event, status, response, applied);
}
static int bootstrap_job_current_locked(const struct bootstrap_job *job) {
    return bootstrap[job->slot].receiver == job->receiver &&
           bootstrap[job->slot].token == job->token;
}
static void *bootstrap_worker(void *opaque) {
    struct bootstrap_job *job = opaque;
    unsigned attempt, poll, local_failures = 0;
    uint32_t seq;
    int rc, current, accepted = 0, failed, local_failed;
    /* Submission runs after the outer SETUP/START lease has normally left.
     * Command admission still fences any concurrent stop before OEM access. */
    usleep(ALT_BOOTSTRAP_POLL_US);
    for (attempt = 1; attempt <= ALT_BOOTSTRAP_ATTEMPTS; ++attempt) {
        bootstrap_lock();
        current = bootstrap_job_current_locked(job);
        seq = current ? bootstrap_identity_locked() : 0;
        if (current && seq) {
            bootstrap[job->slot].request = seq;
            bootstrap[job->slot].pending = 1;
            bootstrap[job->slot].failed = bootstrap[job->slot].local_failed = 0;
            bootstrap[job->slot].show_ack = bootstrap[job->slot].key_ack = 0;
        }
        bootstrap_unlock();
        if (!current || !seq) break;
        rc = alt_request_cluster_after_main_tracked(job->receiver, seq);
        for (poll = 0; poll < ALT_BOOTSTRAP_ACK_POLLS; ++poll) {
            bootstrap_lock();
            current = bootstrap_job_current_locked(job);
            failed = !current || bootstrap[job->slot].failed || rc != 0;
            accepted = current && !failed && bootstrap[job->slot].show_ack && bootstrap[job->slot].key_ack;
            bootstrap_unlock();
            if (failed || accepted) break;
            usleep(ALT_BOOTSTRAP_POLL_US);
        }
        bootstrap_lock();
        current = bootstrap_job_current_locked(job);
        if (current) {
            bootstrap[job->slot].pending = 0; /* fence late completion */
            bootstrap[job->slot].sent = accepted;
        }
        local_failed = current && (rc != 0 || bootstrap[job->slot].local_failed);
        bootstrap_unlock();
        altscreen_log("PHASE=ALT111_BOOTSTRAP_ATTEMPT receiver=%p request=%u attempt=%u submit_rc=%d remote_accepted=%d current=%d ack_timeout=%d picture_ready=UNPROVEN",
                      job->receiver, seq, attempt, rc, accepted, current, poll == ALT_BOOTSTRAP_ACK_POLLS);
        if (!current || accepted) break;
        if (local_failed) {
            /* Local admission failures are not phone attempts. Bound the worker
             * lifetime separately; a later lifecycle notification may retry. */
            if (++local_failures >= 10u) break;
            --attempt;
        }
        /* Bounded backoff without wall-clock/date assumptions. */
        for (poll = 0; poll < 5u * (attempt ? attempt : 1u); ++poll) {
            usleep(ALT_BOOTSTRAP_POLL_US);
            bootstrap_lock(); current = bootstrap_job_current_locked(job); bootstrap_unlock();
            if (!current) break;
        }
    }
    bootstrap_lock();
    if (bootstrap_job_current_locked(job)) {
        bootstrap[job->slot].worker = 0;
        bootstrap[job->slot].pending = 0;
    }
    bootstrap_unlock();
    if (!accepted) altscreen_log("WARN PHASE=ALT111_BOOTSTRAP_FINISH receiver=%p token=%u accepted=0 retries_bounded=1", job->receiver, job->token);
    alt_airplay_release_object(job->receiver);
    free(job);
    return NULL;
}
static void bootstrap_note(void *receiver, int main_ready) {
    unsigned i, slot = 8;
    void *event_client = NULL;
    struct bootstrap_job *job = NULL;
    pthread_t thread;
    int rc = -1, deferred = 0;
    int (*detach_fn)(pthread_t) = (int (*)(pthread_t))dlsym(RTLD_DEFAULT, "pthread_detach");
    if (!receiver) return;
    bootstrap_lock();
    for (i = 0; i < 8; ++i) {
        if (bootstrap[i].receiver == receiver) { slot = i; break; }
        if (!bootstrap[i].receiver && slot == 8) slot = i;
    }
    if (slot < 8) {
        if (!bootstrap[slot].receiver) {
            bootstrap[slot].token = bootstrap_identity_locked();
            if (bootstrap[slot].token) bootstrap[slot].receiver = receiver;
        }
        if (main_ready) bootstrap[slot].main_ready = 1;
        memcpy(&event_client, (const unsigned char *)receiver + 0x2c0, sizeof(event_client));
        deferred = bootstrap[slot].main_ready && !event_client;
        if (bootstrap[slot].receiver && bootstrap[slot].main_ready && event_client &&
            !bootstrap[slot].sent && !bootstrap[slot].worker && detach_fn) {
            job = malloc(sizeof(*job));
            if (job) {
                job->receiver = receiver; job->slot = slot; job->token = bootstrap[slot].token;
                bootstrap[slot].worker = 1;
            }
        }
    }
    bootstrap_unlock();
    if (deferred)
        altscreen_log("PHASE=ALT111_REQUEST_DEFERRED receiver=%p reason=event_channel_not_started waiting=SessionStart", receiver);
    if (slot == 8)
        altscreen_log("ERROR PHASE=ALT111_REQUEST_DEFERRED receiver=%p reason=bootstrap_slots_full", receiver);
    if (!job) return;
    if (!alt_airplay_retain_object(receiver)) goto failed;
    rc = pthread_create(&thread, NULL, bootstrap_worker, job);
    if (!rc) {
        int detach_rc = detach_fn(thread);
        if (detach_rc) altscreen_log("ERROR PHASE=ALT111_BOOTSTRAP_DETACH receiver=%p rc=%d", receiver, detach_rc);
        return;
    }
    alt_airplay_release_object(receiver);
failed:
    bootstrap_lock();
    if (bootstrap_job_current_locked(job)) bootstrap[job->slot].worker = 0;
    bootstrap_unlock();
    free(job);
    altscreen_log("ERROR PHASE=ALT111_BOOTSTRAP_WORKER receiver=%p rc=%d retry_requires_lifecycle_notification=1", receiver, rc);
}
#endif
