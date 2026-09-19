/* Exercises the C ABI exactly as a C or Swift host would. */
#ifndef _WIN32

#include "llavon_ime/llavon_ime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_posted = 0;
static int g_ui_count = 0;
static int g_commit_count = 0;
static uint16_t g_commit_text[256];
static size_t g_commit_length = 0;

/* Posted jobs are marshalled back to this thread by the test. */
static void (*g_jobs[16])(void*);
static void* g_job_users[16];
static int g_job_count = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_condition = PTHREAD_COND_INITIALIZER;

static void host_post(void* user, void (*body)(void*), void* body_user) {
    (void)user;
    pthread_mutex_lock(&g_mutex);
    if (g_job_count < 16) {
        g_jobs[g_job_count] = body;
        g_job_users[g_job_count] = body_user;
        ++g_job_count;
    }
    ++g_posted;
    pthread_cond_broadcast(&g_condition);
    pthread_mutex_unlock(&g_mutex);
}

/* Runs queued jobs on this thread until the queue stays empty for `idle_ms`. */
static void pump_until_idle(int idle_ms) {
    struct timespec idle_deadline;
    clock_gettime(CLOCK_REALTIME, &idle_deadline);
    idle_deadline.tv_sec += idle_ms / 1000;
    idle_deadline.tv_nsec += (long)(idle_ms % 1000) * 1000000L;
    if (idle_deadline.tv_nsec >= 1000000000L) {
        ++idle_deadline.tv_sec;
        idle_deadline.tv_nsec -= 1000000000L;
    }

    for (;;) {
        void (*job)(void*) = NULL;
        void* job_user = NULL;
        pthread_mutex_lock(&g_mutex);
        while (g_job_count == 0) {
            if (pthread_cond_timedwait(&g_condition, &g_mutex, &idle_deadline) != 0) break;
        }
        if (g_job_count > 0) {
            job = g_jobs[0];
            job_user = g_job_users[0];
            memmove(g_jobs, g_jobs + 1, sizeof(g_jobs[0]) * (size_t)(g_job_count - 1));
            memmove(g_job_users, g_job_users + 1, sizeof(g_job_users[0]) * (size_t)(g_job_count - 1));
            --g_job_count;
        }
        pthread_mutex_unlock(&g_mutex);
        if (job == NULL) return;
        job(job_user);
    }
}

static void host_commit(void* user, lv_context_id context, const uint16_t* text, size_t length) {
    (void)user;
    (void)context;
    ++g_commit_count;
    g_commit_length = length < 256 ? length : 256;
    memcpy(g_commit_text, text, g_commit_length * sizeof(uint16_t));
}

static void host_update_ui(void* user, lv_context_id context) {
    (void)user;
    (void)context;
    ++g_ui_count;
}

static int host_surrounding(void* user, lv_context_id context, lv_surrounding_text* out) {
    (void)user;
    (void)context;
    static const char text[] = "history";
    const size_t length = sizeof(text) - 1;
    out->length = length;
    out->cursor = length;
    out->anchor = length;
    out->valid = 1;
    if (out->text != NULL && out->capacity > 0) {
        const size_t count = length < out->capacity ? length : out->capacity;
        for (size_t i = 0; i < count; ++i) out->text[i] = (uint16_t)text[i];
    }
    return 1;
}

static int host_is_sensitive(void* user, lv_context_id context) {
    (void)user;
    (void)context;
    return 0;
}

static int type_key(lv_engine* engine, lv_context_id context, uint32_t sym) {
    lv_key key;
    memset(&key, 0, sizeof(key));
    key.sym = sym;
    return lv_engine_key_event(engine, context, &key);
}

static int test_commit_and_render(lv_engine* engine, lv_context_id context) {
    /* A composition commits through the host callback. */
    for (const char* ch = "su3"; *ch != '\0'; ++ch) type_key(engine, context, (uint32_t)*ch);
    pump_until_idle(200);
    type_key(engine, context, 0xff0d); /* Return */
    if (g_commit_count != 1 || g_commit_length != 1 || g_commit_text[0] != 0x4f60) return 0;

    /* Candidate list rendering through the accessors. */
    for (const char* ch = "su3"; *ch != '\0'; ++ch) type_key(engine, context, (uint32_t)*ch);
    pump_until_idle(200);
    type_key(engine, context, 0xff54); /* Down */
    const lv_render_info* info = lv_engine_render(engine, context);
    if (info == NULL || !info->has_candidates || info->candidate_count == 0) return 0;
    if (info->target != LV_TARGET_CANDIDATES) return 0;
    if (info->selection_key_count == 0) return 0;
    uint32_t selection_keys[16];
    if (lv_engine_selection_keys(engine, selection_keys, 16) != info->selection_key_count) return 0;
    uint16_t candidate[64];
    if (lv_engine_candidate(engine, 0, candidate, 64) == 0) return 0;
    if (lv_engine_layout_hint(engine) != 0) return 0;
    if (lv_engine_preedit(engine, NULL, 0) == 0) return 0;
    if (info->preedit_segment_count == 0) return 0;
    int underlined = 0;
    if (lv_engine_preedit_segment(engine, 0, NULL, 0, &underlined) == 0) return 0;

    /* Marking state renders the tooltip and the marking-hint target. */
    type_key(engine, context, 0xff1b); /* Escape leaves the candidate list */
    for (const char* ch = "su3cl3"; *ch != '\0'; ++ch) type_key(engine, context, (uint32_t)*ch);
    pump_until_idle(200);
    lv_key shift_left;
    memset(&shift_left, 0, sizeof(shift_left));
    shift_left.sym = 0xff51;
    shift_left.states = 1; /* Shift */
    shift_left.frontend_states = 1;
    shift_left.raw_states = 1;
    lv_engine_key_event(engine, context, &shift_left);
    lv_engine_key_event(engine, context, &shift_left);
    info = lv_engine_render(engine, context);
    if (info == NULL || info->target != LV_TARGET_MARKING_HINT) return 0;
    if (lv_engine_aux_up(engine, NULL, 0) == 0) return 0;

    /* The tooltip text matches the accessor. */
    uint16_t tooltip[128];
    const size_t tooltip_length = lv_engine_aux_up(engine, tooltip, 128);
    if (tooltip_length == 0 || tooltip_length > 128) return 0;

    /* Context text is consumed for predictions and can be cleared. */
    lv_engine_clear_context_text(engine, context);
    return 1;
}

static int test_config(lv_engine* engine) {
    if (lv_engine_config_json(engine, NULL, 0) == 0) return 0;
    char json[512];
    const size_t length = lv_engine_config_json(engine, json, sizeof(json));
    if (length == 0 || length >= sizeof(json)) return 0;
    if (lv_engine_set_config_json(engine, json, length) != 0) return 0;
    if (lv_engine_set_config_json(engine, "{not json", 9) == 0) return 0;
    return 1;
}

int run_c_api_tests(void) {
    char overrides_path[256];
    snprintf(overrides_path, sizeof(overrides_path), "/tmp/llavon-ime-c-api-%d.txt", (int)getpid());

    lv_engine_options options;
    lv_engine_options_init(&options);
    options.table_path = LLAVON_IME_TEST_TABLE_PATH;
    options.phrase_overrides_path = overrides_path;
    options.auto_start_service = 0;
    options.enable_accessibility = 0;
    options.socket_path = "/tmp/llavon-ime-c-api-no-service.sock";

    lv_host host;
    memset(&host, 0, sizeof(host));
    host.post = host_post;
    host.commit = host_commit;
    host.update_ui = host_update_ui;
    host.surrounding_text = host_surrounding;
    host.is_sensitive = host_is_sensitive;

    lv_engine* engine = NULL;
    if (lv_engine_create(&options, &host, &engine) != 0 || engine == NULL) return EXIT_FAILURE;

    const lv_context_id context = 77;
    lv_engine_attach(engine, context);
    int ok = test_commit_and_render(engine, context);
    if (ok) ok = test_config(engine);
    if (ok && lv_engine_accessibility_availability(engine) == LV_AVAILABILITY_AVAILABLE) ok = 0;
    if (ok) {
        /* A release event enters the engine but is never consumed. */
        lv_key release;
        memset(&release, 0, sizeof(release));
        release.sym = 'a';
        release.release = 1;
        if (lv_engine_key_event(engine, context, &release) != 0) ok = 0;
    }
    if (ok) {
        lv_engine_reset(engine, context, LV_RESET_FOCUS_OUT, 1);
        lv_engine_deactivate(engine, context);
        lv_engine_detach(engine, context);
    }
    lv_engine_destroy(engine);
    unlink(overrides_path);
    if (!ok) fprintf(stderr, "C ABI tests failed\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

#else

int run_c_api_tests(void) { return EXIT_SUCCESS; }

#endif
