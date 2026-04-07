/* oss.c
 * Author: Cynthia Brown
 * Date: 2026-04-07
 * OS Scheduling Simulator — master process.
 * Implements round-robin scheduling with simulated I/O blocking.
 * Communicates with child user processes via shared memory and message queues.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include "shared.h"

/* -------------------------------------------------------------------------
 * Globals for IPC cleanup
 * ---------------------------------------------------------------------- */
static int  shmid  = -1;
static int  msgid  = -1;
static struct SharedData *shm = NULL;

/* -------------------------------------------------------------------------
 * Queue helpers (circular array, capacity = MAX_PROCS)
 * ---------------------------------------------------------------------- */
typedef struct {
    int data[MAX_PROCS];
    int head, tail, size;
} Queue;

static void q_init(Queue *q)  { q->head = q->tail = q->size = 0; }
static int  q_empty(Queue *q) { return q->size == 0; }

static void q_push(Queue *q, int v) {
    q->data[q->tail] = v;
    q->tail = (q->tail + 1) % MAX_PROCS;
    q->size++;
}

static int q_pop(Queue *q) {
    int v = q->data[q->head];
    q->head = (q->head + 1) % MAX_PROCS;
    q->size--;
    return v;
}

/* -------------------------------------------------------------------------
 * Clock helpers
 * ---------------------------------------------------------------------- */
static void clock_add(unsigned int *sec, unsigned int *ns, unsigned long delta_ns) {
    *ns += (unsigned int)delta_ns;
    while (*ns >= (unsigned int)NS_PER_SEC) {
        (*sec)++;
        *ns -= (unsigned int)NS_PER_SEC;
    }
}

static int clock_ge(unsigned int as, unsigned int an,
                    unsigned int bs, unsigned int bn) {
    return (as > bs) || (as == bs && an >= bn);
}

/* -------------------------------------------------------------------------
 * Log helpers
 * ---------------------------------------------------------------------- */
static FILE *logfp      = NULL;
static int   log_lines  = 0;
#define MAX_LOG_LINES 10000

static void logprint(const char *fmt, ...) {
    va_list ap;
    if (logfp && log_lines < MAX_LOG_LINES) {
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        va_end(ap);
        log_lines++;
    }
    /* Also echo to stdout */
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* -------------------------------------------------------------------------
 * Cleanup
 * ---------------------------------------------------------------------- */
static void cleanup(void) {
    if (shm)  { shmdt(shm); shm = NULL; }
    if (shmid != -1) { shmctl(shmid, IPC_RMID, NULL); shmid = -1; }
    if (msgid != -1) { msgctl(msgid, IPC_RMID, NULL); msgid = -1; }
    if (logfp) { fclose(logfp); logfp = NULL; }
}

static void signal_handler(int sig) {
    (void)sig;
    /* Kill entire process group so all forked children die */
    kill(-getpgrp(), SIGTERM);
    /* Reap children */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    cleanup();
    _exit(1);
}

/* -------------------------------------------------------------------------
 * Print process table every 0.5 simulated seconds
 * ---------------------------------------------------------------------- */
static void print_process_table(void) {
    logprint("OSS: Process Table at %u:%u\n",
             shm->seconds, shm->nanoseconds);
    logprint("  %-4s %-8s %-10s %-10s %-8s\n",
             "Slot", "PID", "StartSec", "SvcTime(s)", "Blocked");
    for (int i = 0; i < MAX_PROCS; i++) {
        struct PCB *p = &shm->processTable[i];
        if (!p->occupied) continue;
        double svc = p->serviceTimeSeconds +
                     p->serviceTimeNano / 1e9;
        logprint("  %-4d %-8d %-10d %-10.6f %-8s\n",
                 i, p->pid, p->startSeconds,
                 svc, p->blocked ? "yes" : "no");
    }
}

/* -------------------------------------------------------------------------
 * Print ready queue contents
 * ---------------------------------------------------------------------- */
static void print_ready_queue(Queue *rq) {
    logprint("OSS: Ready queue [");
    for (int i = 0; i < rq->size; i++) {
        int idx = rq->data[(rq->head + i) % MAX_PROCS];
        logprint(" P%d", shm->processTable[idx].pid);
    }
    logprint(" ]\n");
}

/* -------------------------------------------------------------------------
 * Find a free PCB slot
 * ---------------------------------------------------------------------- */
static int find_free_slot(void) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (!shm->processTable[i].occupied) return i;
    return -1;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    /* --- Default parameters --- */
    int   n_procs  = 5;     /* total processes to spawn */
    int   s_simul  = 3;     /* max simultaneous */
    float t_limit  = 4.0f;  /* upper bound cpu burst in seconds */
    float i_inter  = 0.5f;  /* launch interval in seconds */
    char  logfile[256] = "log.txt";

    /* --- Argument parsing --- */
    int opt;
    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
        case 'h':
            printf("Usage: oss [-h] [-n proc] [-s simul] [-t timelimit]"
                   " [-i interval] [-f logfile]\n");
            return 0;
        case 'n': n_procs = atoi(optarg); break;
        case 's': s_simul = atoi(optarg); break;
        case 't': t_limit = atof(optarg); break;
        case 'i': i_inter = atof(optarg); break;
        case 'f': strncpy(logfile, optarg, sizeof(logfile)-1); break;
        default:
            fprintf(stderr, "Unknown option. Use -h for help.\n");
            return 1;
        }
    }

    if (s_simul > MAX_SIMUL) s_simul = MAX_SIMUL;

    /* --- Signal handlers --- */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* --- Open log file --- */
    logfp = fopen(logfile, "w");
    if (!logfp) { perror("fopen logfile"); return 1; }

    /* --- Shared memory --- */
    shmid = shmget(SHM_KEY, sizeof(struct SharedData), IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); cleanup(); return 1; }
    shm = (struct SharedData *)shmat(shmid, NULL, 0);
    if (shm == (void *)-1) { perror("shmat"); cleanup(); return 1; }
    memset(shm, 0, sizeof(struct SharedData));

    /* --- Message queue --- */
    msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msgid == -1) { perror("msgget"); cleanup(); return 1; }

    /* --- Queues --- */
    Queue readyQ, blockedQ;
    q_init(&readyQ);
    q_init(&blockedQ);

    /* --- Per-process cpu burst (stored outside PCB) --- */
    long burstSec [MAX_PROCS];
    long burstNano[MAX_PROCS];
    memset(burstSec,  0, sizeof(burstSec));
    memset(burstNano, 0, sizeof(burstNano));

    /* --- Statistics --- */
    unsigned long totalCpuNs    = 0; /* ns actually running processes */
    unsigned long totalOssNs    = 0; /* ns OSS overhead */
    int           terminated    = 0; /* processes that finished */

    /* --- Timing --- */
    time_t wall_start = time(NULL);

    /* Next launch time on the simulated clock */
    unsigned int nextLaunchSec  = 0;
    unsigned int nextLaunchNano = 0;

    /* Next process table print (every 500ms sim time) */
    unsigned int nextPrintSec  = 0;
    unsigned int nextPrintNano = 500000000;

    int launched    = 0;  /* total processes spawned so far */
    int activeCount = 0;  /* currently alive children */

    srand((unsigned int)time(NULL));

    /* ===================================================================
     * Main scheduling loop
     * =================================================================*/
    while (launched < n_procs || activeCount > 0) {

        /* --- Wall-clock termination check --- */
        if (difftime(time(NULL), wall_start) >= 3.0) {
            logprint("OSS: 3 real seconds elapsed. Terminating.\n");
            break;
        }

        /* --- Periodic process table output --- */
        if (clock_ge(shm->seconds, shm->nanoseconds,
                     nextPrintSec, nextPrintNano)) {
            print_process_table();
            clock_add(&nextPrintSec, &nextPrintNano, 500000000UL);
        }

        /* --- Maybe launch a new child --- */
        if (launched < n_procs &&
            activeCount < s_simul &&
            clock_ge(shm->seconds, shm->nanoseconds,
                     nextLaunchSec, nextLaunchNano)) {

            int slot = find_free_slot();
            if (slot != -1) {
                /* Random cpu burst: 1ns to t_limit seconds */
                long maxBurstNs = (long)(t_limit * NS_PER_SEC);
                long bNs = 1 + (long)((double)rand() / RAND_MAX * (maxBurstNs - 1));
                burstSec[slot]  = bNs / NS_PER_SEC;
                burstNano[slot] = bNs % NS_PER_SEC;

                /* Build argument strings */
                char argSec[32], argNano[32];
                snprintf(argSec,  sizeof(argSec),  "%ld", burstSec[slot]);
                snprintf(argNano, sizeof(argNano), "%ld", burstNano[slot]);

                pid_t pid = fork();
                if (pid < 0) { perror("fork"); break; }
                if (pid == 0) {
                    /* Child */
                    execl("./user", "user", argSec, argNano, (char *)NULL);
                    perror("execl");
                    _exit(1);
                }

                /* Parent: fill PCB */
                struct PCB *p = &shm->processTable[slot];
                memset(p, 0, sizeof(struct PCB));
                p->occupied    = 1;
                p->pid         = pid;
                p->startSeconds = (int)shm->seconds;
                p->startNano    = (int)shm->nanoseconds;

                q_push(&readyQ, slot);
                launched++;
                activeCount++;

                logprint("OSS: Generating process with PID %d and putting it"
                         " in ready queue at time %u:%u\n",
                         pid, shm->seconds, shm->nanoseconds);

                /* Small overhead for forking */
                unsigned long overhead = 1000 + rand() % 9000;
                clock_add(&shm->seconds, &shm->nanoseconds, overhead);
                totalOssNs += overhead;

                /* Schedule next launch: current_clock + rand(0, i_inter) sec */
                unsigned long interval_ns =
                    (unsigned long)((double)rand() / RAND_MAX *
                                    i_inter * NS_PER_SEC);
                nextLaunchSec  = shm->seconds;
                nextLaunchNano = shm->nanoseconds;
                clock_add(&nextLaunchSec, &nextLaunchNano, interval_ns);
            }
        }

        /* --- Unblock processes whose I/O wait has expired --- */
        if (!q_empty(&blockedQ)) {
            int bSize = blockedQ.size;
            for (int b = 0; b < bSize; b++) {
                int idx = q_pop(&blockedQ);
                struct PCB *p = &shm->processTable[idx];
                if (clock_ge(shm->seconds, shm->nanoseconds,
                             (unsigned int)p->eventWaitSec,
                             (unsigned int)p->eventWaitNano)) {
                    p->blocked = 0;
                    q_push(&readyQ, idx);
                    logprint("OSS: Unblocking process PID %d into ready queue"
                             " at time %u:%u\n",
                             p->pid, shm->seconds, shm->nanoseconds);
                    /* Overhead for unblocking */
                    unsigned long overhead = 100 + rand() % 400;
                    clock_add(&shm->seconds, &shm->nanoseconds, overhead);
                    totalOssNs += overhead;
                } else {
                    q_push(&blockedQ, idx); /* still blocked */
                }
            }
        }

        /* --- Schedule next ready process --- */
        if (!q_empty(&readyQ)) {
            print_ready_queue(&readyQ);

            int idx = q_pop(&readyQ);
            struct PCB *p = &shm->processTable[idx];

            unsigned int dispatchSec  = shm->seconds;
            unsigned int dispatchNano = shm->nanoseconds;

            /* Small overhead: scheduling decision */
            unsigned long schedOverhead = 100 + rand() % 9900;
            clock_add(&shm->seconds, &shm->nanoseconds, schedOverhead);
            totalOssNs += schedOverhead;

            logprint("OSS: Dispatching process with PID %d from ready queue"
                     " at time %u:%u,\n"
                     "OSS: total time this dispatch was %lu nanoseconds\n",
                     p->pid, dispatchSec, dispatchNano, schedOverhead);

            /* Send quantum to child */
            struct Message msg;
            msg.mtype = (long)p->pid;
            msg.value = BASE_QUANTUM_NS;
            if (msgsnd(msgid, &msg, sizeof(long), 0) == -1) {
                perror("msgsnd");
                break;
            }

            /* Wait for reply (mtype = 1 means addressed to OSS) */
            if (msgrcv(msgid, &msg, sizeof(long), 1, 0) == -1) {
                perror("msgrcv");
                break;
            }

            long reply = msg.value;

            if (reply < 0) {
                /* --- Process terminated --- */
                long usedNs = -reply;
                clock_add(&shm->seconds, &shm->nanoseconds, (unsigned long)usedNs);
                totalCpuNs += (unsigned long)usedNs;

                /* Accumulate service time */
                p->serviceTimeNano += (int)usedNs;
                while (p->serviceTimeNano >= (int)NS_PER_SEC) {
                    p->serviceTimeSeconds++;
                    p->serviceTimeNano -= (int)NS_PER_SEC;
                }

                logprint("OSS: Receiving that process with PID %d ran for"
                         " %ld nanoseconds and terminated\n",
                         p->pid, usedNs);

                waitpid(p->pid, NULL, 0);
                memset(p, 0, sizeof(struct PCB));
                activeCount--;
                terminated++;

            } else if (reply == BASE_QUANTUM_NS) {
                /* --- Used full quantum --- */
                clock_add(&shm->seconds, &shm->nanoseconds,
                          (unsigned long)BASE_QUANTUM_NS);
                totalCpuNs += (unsigned long)BASE_QUANTUM_NS;

                p->serviceTimeNano += BASE_QUANTUM_NS;
                while (p->serviceTimeNano >= (int)NS_PER_SEC) {
                    p->serviceTimeSeconds++;
                    p->serviceTimeNano -= (int)NS_PER_SEC;
                }

                logprint("OSS: Receiving that process with PID %d ran for"
                         " %d nanoseconds\n", p->pid, BASE_QUANTUM_NS);
                logprint("OSS: Putting process with PID %d into ready queue\n",
                         p->pid);

                q_push(&readyQ, idx);

            } else {
                /* --- Partial quantum, then I/O block --- */
                clock_add(&shm->seconds, &shm->nanoseconds,
                          (unsigned long)reply);
                totalCpuNs += (unsigned long)reply;

                p->serviceTimeNano += (int)reply;
                while (p->serviceTimeNano >= (int)NS_PER_SEC) {
                    p->serviceTimeSeconds++;
                    p->serviceTimeNano -= (int)NS_PER_SEC;
                }

                logprint("OSS: Receiving that process with PID %d ran for"
                         " %ld nanoseconds,\n"
                         "OSS: not using its entire time quantum\n",
                         p->pid, reply);

                /* Set unblock time: now + 100ms */
                p->blocked      = 1;
                p->eventWaitSec  = (int)shm->seconds;
                p->eventWaitNano = (int)shm->nanoseconds;
                clock_add((unsigned int *)&p->eventWaitSec,
                          (unsigned int *)&p->eventWaitNano,
                          BLOCK_DURATION_NS);

                logprint("OSS: Putting process with PID %d into blocked queue"
                         " (unblocks at %d:%d)\n",
                         p->pid, p->eventWaitSec, p->eventWaitNano);

                q_push(&blockedQ, idx);
            }

        } else if (activeCount == 0 && launched < n_procs) {
            /* No processes anywhere — advance clock to next launch time */
            if (clock_ge(nextLaunchSec, nextLaunchNano,
                         shm->seconds, shm->nanoseconds)) {
                unsigned long gap =
                    ((unsigned long)(nextLaunchSec  - shm->seconds)) * NS_PER_SEC +
                    (nextLaunchNano > shm->nanoseconds
                         ? nextLaunchNano - shm->nanoseconds
                         : 0);
                if (gap > 0) {
                    clock_add(&shm->seconds, &shm->nanoseconds, gap);
                    totalOssNs += gap;
                }
            } else {
                /* Advance by a small amount to prevent busy-wait */
                unsigned long bump = 1000000UL;
                clock_add(&shm->seconds, &shm->nanoseconds, bump);
                totalOssNs += bump;
            }
        } else if (!q_empty(&blockedQ)) {
            /* Only blocked processes exist — advance clock to next unblock */
            unsigned int earliestSec  = (unsigned int)-1;
            unsigned int earliestNano = (unsigned int)-1;
            for (int b = 0; b < blockedQ.size; b++) {
                int idx2 = blockedQ.data[(blockedQ.head + b) % MAX_PROCS];
                struct PCB *p2 = &shm->processTable[idx2];
                if (p2->eventWaitSec < (int)earliestSec ||
                    (p2->eventWaitSec == (int)earliestSec &&
                     p2->eventWaitNano < (int)earliestNano)) {
                    earliestSec  = (unsigned int)p2->eventWaitSec;
                    earliestNano = (unsigned int)p2->eventWaitNano;
                }
            }
            if (clock_ge(earliestSec, earliestNano,
                         shm->seconds, shm->nanoseconds)) {
                unsigned long gap =
                    ((unsigned long)(earliestSec  - shm->seconds)) * NS_PER_SEC +
                    (earliestNano > shm->nanoseconds
                         ? earliestNano - shm->nanoseconds
                         : 0);
                if (gap > 0) {
                    clock_add(&shm->seconds, &shm->nanoseconds, gap);
                    totalOssNs += gap;
                }
            }
        }
    } /* end main loop */

    /* Kill any remaining children */
    kill(-getpgrp(), SIGTERM);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    /* ===================================================================
     * Final report
     * =================================================================*/
    unsigned long totalNs = (unsigned long)shm->seconds * NS_PER_SEC +
                            shm->nanoseconds;
    double cpuUtil = (totalNs > 0)
                     ? (double)totalCpuNs / (double)totalNs * 100.0
                     : 0.0;

    logprint("\n--- Simulation Complete ---\n");
    logprint("Simulated clock at exit:  %u sec %u ns\n",
             shm->seconds, shm->nanoseconds);
    logprint("Total processes launched: %d\n", launched);
    logprint("Total processes finished: %d\n", terminated);
    logprint("Total CPU time (running): %lu ns\n", totalCpuNs);
    logprint("Total OSS overhead time:  %lu ns\n", totalOssNs);
    logprint("CPU utilization:          %.2f%%\n", cpuUtil);

    cleanup();
    return 0;
}
