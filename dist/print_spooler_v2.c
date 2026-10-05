/*
 * ============================================================
 *  Print Spooler Simulation System  (เวอร์ชัน 2 - ฉบับสมบูรณ์)
 *  Mini Project: OS System Calls (POSIX)
 * ============================================================
 *
 * สิ่งที่เพิ่มจากเวอร์ชันแรก (Next steps ที่แจ้งอาจารย์ไว้):
 *   1) Aging mechanism  : งานที่รอนานจะได้ "effective priority" ลดลง
 *                         เลขน้อยสำคัญกว่า ช่วยป้องกัน starvation
 *   2) Multiple printers: รองรับเครื่องพิมพ์หลายตัว (1-4) ดึงงานจากคิวเดียวกัน
 *   3) Wait-time stats  : เก็บสถิติเวลารอของแต่ละงาน สรุปเมื่อจบโปรแกรม
 *                         (เฉลี่ย / สูงสุด / แยกตาม priority / แยกตามเครื่องพิมพ์)
 *
 * การเปลี่ยนแปลงของโครงสร้างคิว:
 *   เดิม  : insertion sort ตอน enqueue -> หัวคิวคืองานสำคัญที่สุด
 *   ใหม่  : คิวไม่เรียง, ตอน dequeue "สแกนหางานที่ effective priority น้อยที่สุด"
 *           เพราะ effective priority เปลี่ยนตามเวลา จึงเรียงล่วงหน้าไม่ได้
 *           (คิวมีแค่ 10 ช่อง การสแกนจึงถูกมาก)
 *
 *   effective_priority = priority - (เวลารอเป็น ms / AGING_INTERVAL_MS)
 *   เช่น งาน priority 5 รอครบ 2 วินาที (interval=2000) จะมี effective priority 4
 *   effective priority ลดลงถึง 0 หรือติดลบได้ โดย priority เดิมยังเป็น 1-5
 *   งาน SHUTDOWN มี id=-1 และ effective priority=INT_MAX เพื่อถูกเลือกหลังงานจริง
 *   ถ้า effective เท่ากัน -> งานที่เข้าคิวก่อนได้ก่อน (FIFO)
 *
 * Semaphore ที่ใช้ (4 ตัว):
 *   mutex        : ป้องกันการเข้าถึง queue พร้อมกัน
 *   empty_slots  : นับช่องว่างในคิว
 *   filled_slots : นับงานที่รอพิมพ์
 *   stats_lock   : ป้องกันการอัปเดตสถิติพร้อมกัน (หลาย printer เขียนสถิติชุดเดียวกัน)
 *
 * คอมไพล์:
 *   gcc -Wall -o spooler_v2 print_spooler_v2.c -lrt -lpthread
 * รัน:
 *   ./spooler_v2 [จำนวนเครื่องพิมพ์ 1-4] [aging: 1=เปิด, 0=ปิด]
 *   ./spooler_v2            -> 1 เครื่อง, เปิด aging
 *   ./spooler_v2 2 1        -> 2 เครื่อง, เปิด aging
 *   ./spooler_v2 1 0        -> 1 เครื่อง, ปิด aging (เทียบผลกับแบบเปิด)
 * ============================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <time.h>

/* ----------------------- ค่าตั้งต้นของระบบ ----------------------- */
#define SHM_NAME            "/print_spooler_shm_v2"
#define QUEUE_CAPACITY      10
#define NUM_PRODUCERS       4
#define JOBS_PER_PRODUCER   5
#define MAX_PRINTERS        4
#define MAX_PRIORITY        5
#define SENTINEL_PRIORITY   (-1)
#define AGING_INTERVAL_MS   2000   /* รอทุกๆ 2 วินาที ลด effective priority ลง 1 */
#define MS_PER_PAGE         200    /* เวลาพิมพ์ต่อหน้า (ms) */
#define LOG_FILE            "spooler_log.txt"

typedef struct {
    int       id;
    int       priority;       /* 1 (สำคัญที่สุด) - 5 (สำคัญน้อยที่สุด) */
    int       pages;
    char      filename[64];
    long long enqueue_ms;     /* เวลาที่เข้าคิว (ใช้คำนวณ waiting time + aging) */
} Job;

/* สถิติรวม อยู่ใน shared memory เพราะ printer หลาย process ช่วยกันเขียน */
typedef struct {
    int       jobs_done;
    long long total_wait_ms;
    long long max_wait_ms;
    int       aged_jobs;                       /* งานที่ได้ประโยชน์จาก aging */
    int       prio_count[MAX_PRIORITY + 1];    /* จำนวนงานแยกตาม priority */
    long long prio_wait_ms[MAX_PRIORITY + 1];  /* เวลารอรวมแยกตาม priority */
    long long prio_max_ms[MAX_PRIORITY + 1];   /* เวลารอสูงสุดแยกตาม priority */
    int       printer_jobs[MAX_PRINTERS];      /* จำนวนงานที่แต่ละเครื่องพิมพ์ */
    int       printer_pages[MAX_PRINTERS];     /* จำนวนหน้าที่แต่ละเครื่องพิมพ์ */
} Stats;

typedef struct {
    Job   jobs[QUEUE_CAPACITY];
    int   count;
    int   aging_enabled;
    Stats stats;
    sem_t mutex;
    sem_t empty_slots;
    sem_t filled_slots;
    sem_t stats_lock;
} SharedQueue;

/* ----------------------- helper: เวลาปัจจุบันเป็น ms ----------------------- */
/* CLOCK_MONOTONIC ใช้ร่วมกันได้ทุก process ในเครื่องเดียวกัน และไม่กระโดดตามนาฬิการะบบ */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ----------------------- helper: เขียน log พร้อม timestamp ----------------------- */
static void log_line(const char *fmt, ...) {
    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_buf[16];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm_info);

    char line[320];
    int len = snprintf(line, sizeof(line), "[%s] (pid=%d) %s\n", time_buf, getpid(), msg);
    if (len >= (int)sizeof(line)) len = sizeof(line) - 1;

    /* O_APPEND + เขียนก้อนเล็ก = atomic บน Linux หลาย process เขียนพร้อมกันได้ไม่ปนกัน */
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        if (write(fd, line, len) < 0) { /* ignore */ }
        close(fd);
    }
    fputs(line, stdout);
}

/* ----------------------- Aging: คำนวณ effective priority ----------------------- */
static int effective_priority(const SharedQueue *q, const Job *j, long long now) {
    if (j->id == -1) return INT_MAX; /* SHUTDOWN ไม่ aging และถูกเลือกหลังงานจริง */
    int eff = j->priority;
    if (q->aging_enabled) {
        eff -= (int)((now - j->enqueue_ms) / AGING_INTERVAL_MS);
    }
    return eff;
}

/* ----------------------- enqueue ----------------------- */
/* ลำดับสำคัญ: รอ empty_slots ก่อนล็อก mutex เสมอ (ไม่งั้น deadlock) */
static void enqueue(SharedQueue *q, Job job) {
    sem_wait(&q->empty_slots);
    sem_wait(&q->mutex);

    job.enqueue_ms = now_ms();          /* ประทับเวลาเข้าคิว */
    q->jobs[q->count++] = job;          /* คิวไม่เรียงแล้ว ต่อท้ายได้เลย */

    sem_post(&q->mutex);
    sem_post(&q->filled_slots);
}

/* ----------------------- dequeue ----------------------- */
/* out_eff  : effective priority ตอนถูกเลือก (เอาไว้ log)
   out_wait : เวลาที่งานนี้รอในคิว (ms) */
static Job dequeue(SharedQueue *q, int *out_eff, long long *out_wait) {
    sem_wait(&q->filled_slots);
    sem_wait(&q->mutex);

    long long now = now_ms();

    /* สแกนหางานที่ effective priority น้อยที่สุด; เท่ากันเลือกงานที่เข้าคิวก่อน */
    int best = 0;
    int best_eff = effective_priority(q, &q->jobs[0], now);
    for (int i = 1; i < q->count; i++) {
        int e = effective_priority(q, &q->jobs[i], now);
        if (e < best_eff ||
            (e == best_eff && q->jobs[i].enqueue_ms < q->jobs[best].enqueue_ms)) {
            best = i;
            best_eff = e;
        }
    }

    Job job = q->jobs[best];
    q->jobs[best] = q->jobs[q->count - 1];  /* เอางานท้ายสุดมาแทนที่ (ลำดับไม่สำคัญแล้ว) */
    q->count--;

    *out_eff  = best_eff;
    *out_wait = now - job.enqueue_ms;

    sem_post(&q->mutex);
    sem_post(&q->empty_slots);
    return job;
}

/* ----------------------- บันทึกสถิติ (เรียกโดย printer) ----------------------- */
static void record_stats(SharedQueue *q, int printer_id, const Job *job,
                         int eff, long long wait_ms) {
    sem_wait(&q->stats_lock);
    Stats *s = &q->stats;
    s->jobs_done++;
    s->total_wait_ms += wait_ms;
    if (wait_ms > s->max_wait_ms) s->max_wait_ms = wait_ms;
    if (eff < job->priority) s->aged_jobs++;
    s->prio_count[job->priority]++;
    s->prio_wait_ms[job->priority] += wait_ms;
    if (wait_ms > s->prio_max_ms[job->priority]) s->prio_max_ms[job->priority] = wait_ms;
    s->printer_jobs[printer_id - 1]++;
    s->printer_pages[printer_id - 1] += job->pages;
    sem_post(&q->stats_lock);
}

/* ----------------------- Producer process ----------------------- */
static void run_producer(SharedQueue *q, int producer_id) {
    srand(getpid() ^ (unsigned)time(NULL));

    for (int i = 0; i < JOBS_PER_PRODUCER; i++) {
        Job job;
        memset(&job, 0, sizeof(job));
        job.id = producer_id * 100 + i;
        job.priority = (rand() % MAX_PRIORITY) + 1;
        job.pages = (rand() % 8) + 1;
        snprintf(job.filename, sizeof(job.filename), "user%d_doc%d.txt", producer_id, i);

        enqueue(q, job);
        log_line("[PRODUCER %d] ส่งงาน #%-3d '%s' (priority=%d, pages=%d)",
                 producer_id, job.id, job.filename, job.priority, job.pages);

        usleep((rand() % 400 + 100) * 1000);
    }
    exit(0);
}

/* ----------------------- Printer (consumer) process ----------------------- */
static void run_printer(SharedQueue *q, int printer_id) {
    while (1) {
        int eff;
        long long wait_ms;
        Job job = dequeue(q, &eff, &wait_ms);

        if (job.id == -1) {
            log_line("[PRINTER %d] ได้รับสัญญาณปิดระบบ หยุดทำงาน", printer_id);
            break;
        }

        if (eff < job.priority) {
            log_line("[PRINTER %d] เริ่มพิมพ์ #%-3d '%s' (priority=%d -> aged=%d, %d หน้า, รอมา %lld ms) *AGED*",
                     printer_id, job.id, job.filename, job.priority, eff, job.pages, wait_ms);
        } else {
            log_line("[PRINTER %d] เริ่มพิมพ์ #%-3d '%s' (priority=%d, %d หน้า, รอมา %lld ms)",
                     printer_id, job.id, job.filename, job.priority, job.pages, wait_ms);
        }

        record_stats(q, printer_id, &job, eff, wait_ms);

        usleep(job.pages * MS_PER_PAGE * 1000);

        log_line("[PRINTER %d] พิมพ์เสร็จ #%-3d '%s'", printer_id, job.id, job.filename);
    }
    exit(0);
}

/* ----------------------- แสดงรายงานสถิติ ----------------------- */
static void print_report(const SharedQueue *q, int num_printers, long long elapsed_ms) {
    const Stats *s = &q->stats;
    printf("\n================ สรุปสถิติ ================\n");
    printf("โหมด aging          : %s\n", q->aging_enabled ? "เปิด" : "ปิด");
    printf("จำนวนเครื่องพิมพ์    : %d\n", num_printers);
    printf("งานที่พิมพ์เสร็จ     : %d / %d\n", s->jobs_done, NUM_PRODUCERS * JOBS_PER_PRODUCER);
    printf("เวลารวมทั้งระบบ      : %.2f วินาที\n", elapsed_ms / 1000.0);
    if (s->jobs_done > 0) {
        printf("เวลารอเฉลี่ย         : %.2f วินาที\n", s->total_wait_ms / 1000.0 / s->jobs_done);
        printf("เวลารอสูงสุด         : %.2f วินาที\n", s->max_wait_ms / 1000.0);
    }
    printf("งานที่ได้ประโยชน์จาก aging : %d งาน\n", s->aged_jobs);

    printf("\n-- เวลารอแยกตาม priority --\n");
    printf("priority | จำนวนงาน | รอเฉลี่ย(s) | รอสูงสุด(s)\n");
    for (int p = 1; p <= MAX_PRIORITY; p++) {
        if (s->prio_count[p] == 0) {
            printf("   %d     |    0     |      -      |      -\n", p);
        } else {
            printf("   %d     |   %2d     |   %6.2f    |   %6.2f\n", p, s->prio_count[p],
                   s->prio_wait_ms[p] / 1000.0 / s->prio_count[p],
                   s->prio_max_ms[p] / 1000.0);
        }
    }

    printf("\n-- งานที่แต่ละเครื่องพิมพ์ --\n");
    for (int i = 0; i < num_printers; i++) {
        printf("เครื่องที่ %d : %2d งาน, %3d หน้า\n", i + 1, s->printer_jobs[i], s->printer_pages[i]);
    }
    printf("===========================================\n");
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* กัน bug stdout buffer ซ้ำหลัง fork() */

    int num_printers = 1;
    int aging = 1;
    if (argc >= 2) num_printers = atoi(argv[1]);
    if (argc >= 3) aging = atoi(argv[2]);
    if (num_printers < 1 || num_printers > MAX_PRINTERS) {
        fprintf(stderr, "การใช้งาน: %s [เครื่องพิมพ์ 1-%d] [aging 0|1]\n", argv[0], MAX_PRINTERS);
        return 1;
    }

    remove(LOG_FILE);

    /* 1) สร้าง shared memory */
    shm_unlink(SHM_NAME);
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (shm_fd < 0) { perror("shm_open"); exit(1); }
    if (ftruncate(shm_fd, sizeof(SharedQueue)) < 0) { perror("ftruncate"); exit(1); }

    SharedQueue *q = mmap(NULL, sizeof(SharedQueue),
                          PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (q == MAP_FAILED) { perror("mmap"); exit(1); }
    close(shm_fd);

    /* 2) เตรียมข้อมูลเริ่มต้น (shared memory ที่ ftruncate ใหม่ถูกเติม 0 อยู่แล้ว) */
    memset(q, 0, sizeof(SharedQueue));
    q->aging_enabled = aging ? 1 : 0;
    sem_init(&q->mutex, 1, 1);
    sem_init(&q->empty_slots, 1, QUEUE_CAPACITY);
    sem_init(&q->filled_slots, 1, 0);
    sem_init(&q->stats_lock, 1, 1);

    printf("=== Print Spooler v2 เริ่มทำงาน (producers=%d, jobs/producer=%d, printers=%d, aging=%s, queue cap=%d) ===\n\n",
           NUM_PRODUCERS, JOBS_PER_PRODUCER, num_printers, q->aging_enabled ? "ON" : "OFF", QUEUE_CAPACITY);

    long long start_ms = now_ms();

    /* 3) fork producer */
    pid_t producer_pids[NUM_PRODUCERS];
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            run_producer(q, i + 1);
        } else if (pid > 0) {
            producer_pids[i] = pid;
        } else {
            perror("fork producer"); exit(1);
        }
    }

    /* 4) fork printer หลายตัว */
    pid_t printer_pids[MAX_PRINTERS];
    for (int i = 0; i < num_printers; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            run_printer(q, i + 1);
        } else if (pid > 0) {
            printer_pids[i] = pid;
        } else {
            perror("fork printer"); exit(1);
        }
    }

    /* 5) รอ producer ครบ */
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        waitpid(producer_pids[i], NULL, 0);
    }
    log_line("[MAIN] Producer ทุกตัวส่งงานครบแล้ว กำลังส่งสัญญาณปิดให้ printer ทั้ง %d ตัว", num_printers);

    /* 6) poison pill: ต้องส่ง "1 อันต่อ 1 printer" ไม่งั้นบาง printer จะรอตลอดไป */
    for (int i = 0; i < num_printers; i++) {
        Job sentinel;
        memset(&sentinel, 0, sizeof(sentinel));
        sentinel.id = -1;
        sentinel.priority = SENTINEL_PRIORITY;
        strcpy(sentinel.filename, "SHUTDOWN");
        enqueue(q, sentinel);
    }

    /* 7) รอ printer ทุกตัวปิดตัว */
    for (int i = 0; i < num_printers; i++) {
        waitpid(printer_pids[i], NULL, 0);
    }
    long long elapsed = now_ms() - start_ms;

    /* 8) แสดงสถิติ (แม่อ่านจาก shared memory ก่อนทำลาย) */
    print_report(q, num_printers, elapsed);
    printf("รายละเอียดทั้งหมดดูได้ที่ %s\n", LOG_FILE);

    /* 9) เคลียร์ทรัพยากร */
    sem_destroy(&q->mutex);
    sem_destroy(&q->empty_slots);
    sem_destroy(&q->filled_slots);
    sem_destroy(&q->stats_lock);
    munmap(q, sizeof(SharedQueue));
    shm_unlink(SHM_NAME);

    return 0;
}
