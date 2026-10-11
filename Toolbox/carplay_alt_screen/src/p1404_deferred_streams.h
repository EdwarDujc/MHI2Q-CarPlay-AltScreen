#ifndef P1404_DEFERRED_STREAMS_H
#define P1404_DEFERRED_STREAMS_H
/* Included by the fullchain wrapper. Per-type epochs order a delayed audio
 * teardown against SETUP without stopping unrelated video setup. A reservation
 * keeps the guard record alive while these snapshots exist. */
static int setup_ops_ready(struct alt_setup_cf_ops *);
static void fullchain_note_setup_epochs(const void *params, struct alt_session_lease *lease) {
    cf_obj streams;
    unsigned i, n;
    int64_t type;
    if (!params || !alt_cf_is_dict(params) || !cf_array_count || !cf_array_at) return;
    streams = cf_dict_get_cstr((cf_obj)params, "streams");
    if (!streams || !alt_cf_is_array(streams)) return;
    n = cf_array_count(streams);
    for (i = 0; i < n; ++i) {
        cf_obj item = (cf_obj)cf_array_at(streams, i);
        if (item && alt_cf_is_dict(item) &&
            alt_cf_int64(cf_dict_get_cstr(item, "type"), &type) && type >= 0 && type < 256)
            alt_session_guard_note_stream_setup(lease, (unsigned)type);
    }
}
static int fullchain_audio_snapshot(const void *params, struct alt_session_lease *lease,
                                   uint32_t epochs[256]) {
    cf_obj streams;
    unsigned i, n;
    int64_t type;
    if (!params || !alt_cf_is_dict(params) || !cf_array_count || !cf_array_at) return 0;
    streams = cf_dict_get_cstr((cf_obj)params, "streams");
    if (!streams || !alt_cf_is_array(streams) || !(n = cf_array_count(streams)) || n > 128) return 0;
    for (i = 0; i < n; ++i) {
        cf_obj item = (cf_obj)cf_array_at(streams, i);
        if (!item || !alt_cf_is_dict(item) || !alt_cf_int64(cf_dict_get_cstr(item, "type"), &type)
            || type < 0 || type >= 256 || type == CP_STREAM_MAIN_SCREEN || type == CP_STREAM_ALT_SCREEN)
            return 0;
    }
    if (!lease->registered) return 0;
    memcpy(epochs, lease->entry_stream_epochs, 256 * sizeof(uint32_t));
    return 1;
}
/* Return 1 with original/filtered request, 2 if all requested streams have been
 * superseded, 0 if filtering cannot safely preserve the original request. Never
 * hand stock an empty streams array: it means FULL teardown, not a no-op. */
static int fullchain_audio_filter(const void *params, struct alt_session_lease *lease,
                                 const uint32_t saved[256], const void **filtered,
                                 void **owned) {
    uint32_t now[256];
    unsigned char stale[256] = {0};
    unsigned i, n, removed = 0;
    cf_obj streams = cf_dict_get_cstr((cf_obj)params, "streams");
    cf_obj current = (cf_obj)params;
    struct alt_setup_cf_ops ops;
    *filtered = params; *owned = NULL;
    if (!alt_session_guard_copy_stream_epochs(lease, now) || !streams || !alt_cf_is_array(streams)) return 0;
    n = cf_array_count(streams);
    for (i = 0; i < n; ++i) {
        int64_t type;
        cf_obj item = (cf_obj)cf_array_at(streams, i);
        if (!alt_cf_int64(cf_dict_get_cstr(item, "type"), &type) || type < 0 || type >= 256) return 0;
        if (saved[type] != now[type]) { stale[type] = 1; ++removed; }
    }
    if (!removed) return 1;
    altscreen_log("PHASE=TEARDOWN_AUDIO_GENERATION receiver_slot=%u superseded=%u requested=%u new_streams_preserved=1", lease->slot, removed, n);
    if (removed == n) return 2;
    if (!setup_ops_ready(&ops)) return 0;
    for (i = 0; i < 256; ++i) if (stale[i]) {
        alt_setup_obj ignored;
        unsigned count;
        void *copy = alt_setup_build_stock_only(&ops, (alt_setup_obj)current, i, &ignored, &count);
        if (!copy || !count) {
            if (copy) alt_airplay_release_object(copy);
            if (*owned) alt_airplay_release_object(*owned);
            *owned = NULL;
            return 0;
        }
        if (*owned) alt_airplay_release_object(*owned);
        *owned = copy; current = copy;
    }
    *filtered = current;
    return 1;
}
#endif
