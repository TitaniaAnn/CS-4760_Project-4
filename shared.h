/* shared.h
 * Author: Cynthia Brown
 * Date: 2026-04-07
 * Shared definitions for oss and user processes.
 */

#ifndef SHARED_H
#define SHARED_H

#include <sys/types.h>

/* IPC keys */
#define SHM_KEY     0x4760A4
#define MSG_KEY     0x4760B4

/* Scheduling constants */
#define BASE_QUANTUM_NS  25000000   /* 25 ms in nanoseconds */
#define MAX_PROCS        20
#define MAX_SIMUL        18
#define BLOCK_DURATION_NS 100000000 /* 100 ms blocked duration */
#define NS_PER_SEC       1000000000UL

/* Process Control Block */
struct PCB {
    int  occupied;           /* 1 if slot is in use */
    pid_t pid;               /* actual pid of child */
    int  startSeconds;       /* simulated clock when created */
    int  startNano;
    int  serviceTimeSeconds; /* total scheduled CPU time used */
    int  serviceTimeNano;
    int  eventWaitSec;       /* when blocked process becomes ready */
    int  eventWaitNano;
    int  blocked;            /* 1 if waiting on I/O event */
};

/* Shared memory layout: clock + process table */
struct SharedData {
    unsigned int seconds;
    unsigned int nanoseconds;
    struct PCB   processTable[MAX_PROCS];
};

/* Message queue format.
 * OSS  -> child: mtype = child pid, value = quantum in ns (positive)
 * child -> OSS:  mtype = 1 (OSS),   value = ns used (positive = I/O block,
 *                                           negative = terminate)
 */
struct Message {
    long mtype;
    long value;
};

#endif /* SHARED_H */
