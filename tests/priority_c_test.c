/* Exercise the actual C scheduling code without running its shared-memory main. */
static long long frozen_now_ms;

#define main spooler_program_main
#define clock_gettime test_clock_gettime
#include "../dist/print_spooler_v2.c"
#undef clock_gettime
#undef main

#include <assert.h>

/* now_ms() in the included source reads this deterministic test clock. */
int test_clock_gettime(clockid_t clock_id, struct timespec *ts) {
    assert(clock_id == CLOCK_MONOTONIC);
    ts->tv_sec = frozen_now_ms / 1000;
    ts->tv_nsec = (frozen_now_ms % 1000) * 1000000;
    return 0;
}

static Job make_job(int id, int priority, long long enqueue_ms, int pages) {
    Job job = {0};
    job.id = id;
    job.priority = priority;
    job.enqueue_ms = enqueue_ms;
    job.pages = pages;
    return job;
}

static void fixture_init(SharedQueue *q, int aging, const Job *jobs, int count) {
    assert(count >= 0 && count <= QUEUE_CAPACITY);
    memset(q, 0, sizeof(*q));
    q->aging_enabled = aging;
    q->count = count;
    if (count > 0) memcpy(q->jobs, jobs, (size_t)count * sizeof(*jobs));
    assert(sem_init(&q->mutex, 0, 1) == 0);
    assert(sem_init(&q->empty_slots, 0, QUEUE_CAPACITY - count) == 0);
    assert(sem_init(&q->filled_slots, 0, count) == 0);
    assert(sem_init(&q->stats_lock, 0, 1) == 0);
}

static void fixture_destroy(SharedQueue *q) {
    assert(sem_destroy(&q->stats_lock) == 0);
    assert(sem_destroy(&q->filled_slots) == 0);
    assert(sem_destroy(&q->empty_slots) == 0);
    assert(sem_destroy(&q->mutex) == 0);
}

static void expect_dequeue(SharedQueue *q, int id, int expected_eff,
                           long long expected_wait) {
    assert(q->count > 0);
    int before = q->count;
    int eff;
    long long wait;
    Job job = dequeue(q, &eff, &wait);
    assert(job.id == id);
    assert(eff == expected_eff);
    assert(wait == expected_wait);
    assert(q->count == before - 1);

    int value;
    assert(sem_getvalue(&q->filled_slots, &value) == 0);
    assert(value == q->count);
    assert(sem_getvalue(&q->empty_slots, &value) == 0);
    assert(value == QUEUE_CAPACITY - q->count);
    assert(sem_getvalue(&q->mutex, &value) == 0);
    assert(value == 1);
}

static void test_effective_boundaries(void) {
    SharedQueue q = {0};
    q.aging_enabled = 1;
    Job job = make_job(101, 1, 0, 1);
    const long long times[] = {0, 1999, 2000, 3999, 4000, 6000, 12000};
    const int values[] = {1, 1, 0, 0, -1, -2, -5};
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        assert(effective_priority(&q, &job, times[i]) == values[i]);
    }
    assert(job.priority == 1);
    q.aging_enabled = 0;
    assert(effective_priority(&q, &job, 12000) == 1);

    /* Identity, rather than the numeric priority, marks a shutdown job. */
    Job sentinel = make_job(-1, SENTINEL_PRIORITY, 0, 0);
    assert(effective_priority(&q, &sentinel, 100000) == INT_MAX);
    q.aging_enabled = 1;
    assert(effective_priority(&q, &sentinel, 100000) == INT_MAX);
    sentinel.priority = 5;
    assert(effective_priority(&q, &sentinel, 100000) == INT_MAX);
}

static void test_lower_priority_wins(void) {
    SharedQueue q;
    frozen_now_ms = 10000;
    Job jobs[] = {make_job(105, 5, 0, 1), make_job(101, 1, 10000, 1),
                  make_job(103, 3, 1000, 1)};
    fixture_init(&q, 0, jobs, 3);
    expect_dequeue(&q, 101, 1, 0);
    expect_dequeue(&q, 103, 3, 9000);
    expect_dequeue(&q, 105, 5, 10000);
    fixture_destroy(&q);
}

static void test_aging_tie_prefers_older_job(void) {
    SharedQueue q;
    frozen_now_ms = 8000;
    Job jobs[] = {make_job(101, 1, 8000, 1), make_job(105, 5, 0, 1),
                  make_job(-1, SENTINEL_PRIORITY, 0, 0)};
    fixture_init(&q, 1, jobs, 3);
    expect_dequeue(&q, 105, 1, 8000);
    expect_dequeue(&q, 101, 1, 0);
    expect_dequeue(&q, -1, INT_MAX, 8000);
    fixture_destroy(&q);
}

static void test_negative_effective_jobs_precede_sentinel(void) {
    SharedQueue q;
    frozen_now_ms = 6000;
    Job jobs[] = {make_job(-1, SENTINEL_PRIORITY, 0, 0),
                  make_job(101, 1, 0, 1), make_job(102, 1, 2000, 1)};
    fixture_init(&q, 1, jobs, 3);
    expect_dequeue(&q, 101, -2, 6000);
    expect_dequeue(&q, 102, -1, 4000);
    expect_dequeue(&q, -1, INT_MAX, 6000);
    fixture_destroy(&q);
}

static void test_exact_tie_keeps_array_order(void) {
    SharedQueue q;
    frozen_now_ms = 4000;
    Job jobs[] = {make_job(201, 3, 2000, 1), make_job(202, 3, 2000, 1)};
    fixture_init(&q, 1, jobs, 2);
    expect_dequeue(&q, 201, 2, 2000);
    expect_dequeue(&q, 202, 2, 2000);
    fixture_destroy(&q);
}

static void test_shutdown_only_queue(void) {
    SharedQueue q;
    frozen_now_ms = 10000;
    Job jobs[] = {make_job(-1, SENTINEL_PRIORITY, 1000, 0),
                  make_job(-1, SENTINEL_PRIORITY, 0, 0)};
    fixture_init(&q, 1, jobs, 2);
    expect_dequeue(&q, -1, INT_MAX, 10000);
    expect_dequeue(&q, -1, INT_MAX, 9000);
    assert(q.count == 0);
    assert(q.stats.jobs_done == 0);
    fixture_destroy(&q);
}

static void test_statistics_use_base_priority(void) {
    SharedQueue q;
    fixture_init(&q, 1, NULL, 0);
    Job aged = make_job(105, 5, 0, 3);
    Job unchanged = make_job(101, 1, 0, 2);
    Job mildly_aged = make_job(205, 5, 0, 4);
    record_stats(&q, 1, &aged, -2, 14000);
    record_stats(&q, 2, &unchanged, 1, 0);
    record_stats(&q, 1, &mildly_aged, 4, 2000);
    assert(q.stats.jobs_done == 3);
    assert(q.stats.aged_jobs == 2);
    assert(q.stats.total_wait_ms == 16000);
    assert(q.stats.max_wait_ms == 14000);
    assert(q.stats.prio_count[5] == 2);
    assert(q.stats.prio_wait_ms[5] == 16000);
    assert(q.stats.prio_max_ms[5] == 14000);
    assert(q.stats.prio_count[1] == 1);
    assert(q.stats.prio_wait_ms[1] == 0);
    assert(q.stats.prio_max_ms[1] == 0);
    for (int p = 0; p <= MAX_PRIORITY; p++) {
        if (p == 1 || p == 5) continue;
        assert(q.stats.prio_count[p] == 0);
        assert(q.stats.prio_wait_ms[p] == 0);
        assert(q.stats.prio_max_ms[p] == 0);
    }
    assert(q.stats.printer_jobs[0] == 2);
    assert(q.stats.printer_pages[0] == 7);
    assert(q.stats.printer_jobs[1] == 1);
    assert(q.stats.printer_pages[1] == 2);
    assert(q.stats.printer_jobs[2] == 0);
    assert(q.stats.printer_jobs[3] == 0);
    fixture_destroy(&q);
}

int main(void) {
    test_effective_boundaries();
    test_lower_priority_wins();
    test_aging_tie_prefers_older_job();
    test_negative_effective_jobs_precede_sentinel();
    test_exact_tie_keeps_array_order();
    test_shutdown_only_queue();
    test_statistics_use_base_priority();
    puts("C priority regression tests: 7 passed");
    return 0;
}
