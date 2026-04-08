/* user.c
 * Author: Cynthia Brown
 * Date: 2026-04-07
 * Child worker process for the OS scheduling simulator.
 * Receives a time quantum from oss, decides whether to run to completion,
 * use the full quantum, or block on simulated I/O, then replies to oss.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include "shared.h"

int main(int argc, char *argv[]) {
    /* Arguments: cpuBurstSec cpuBurstNano */
    if (argc != 3) {
        fprintf(stderr, "user: usage: user <cpuBurstSec> <cpuBurstNano>\n");
        return 1;
    }

    long burstSec  = atol(argv[1]);
    long burstNano = atol(argv[2]);

    /* Seed random per-process using pid so each child differs */
    srand((unsigned int)(getpid() ^ (getpid() << 16)));

    /* Attach shared memory (read-only; only oss writes the clock) */
    int shmid = shmget(SHM_KEY, sizeof(struct SharedData), 0666);
    if (shmid == -1) {
        perror("user: shmget");
        return 1;
    }
    struct SharedData *shm = (struct SharedData *)shmat(shmid, NULL, SHM_RDONLY);
    if (shm == (void *)-1) {
        perror("user: shmat");
        return 1;
    }

    /* Open message queue */
    int msgid = msgget(MSG_KEY, 0666);
    if (msgid == -1) {
        perror("user: msgget");
        shmdt(shm);
        return 1;
    }

    /* Track total CPU time this process has used (ns accumulated) */
    long totalUsedSec  = 0;
    long totalUsedNano = 0;

    struct Message msg;

    while (1) {
        /* Wait for a quantum from oss (mtype = our pid) */
        if (msgrcv(msgid, &msg, sizeof(long), (long)getpid(), 0) == -1) {
            perror("user: msgrcv");
            break;
        }

        long quantum = msg.value; /* nanoseconds we were given */

        /* Remaining burst we are allowed to consume */
        long remainSec  = burstSec  - totalUsedSec;
        long remainNano = burstNano - totalUsedNano;
        if (remainNano < 0) { remainSec--; remainNano += NS_PER_SEC; }
        long remainTotal = remainSec * (long)NS_PER_SEC + remainNano;

        /* --- Option 3: terminate if quantum would exceed remaining burst --- */
        if (remainTotal <= 0 || quantum >= remainTotal) {
            /* Use only what remains (at least 1ns), then terminate */
            long used = (remainTotal > 0) ? remainTotal : 1;
            msg.mtype = 1;
            msg.value = -used; /* negative = terminating */
            msgsnd(msgid, &msg, sizeof(long), 0);
            break;
        }

        /* --- Options 1 & 2: 20% chance of I/O block --- */
        int willBlock = ((rand() % 100) < 20);

        if (willBlock) {
            /* Use a random fraction (1–99%) of the quantum, then block */
            long fraction = (long)(quantum * (1 + rand() % 99) / 100);
            if (fraction < 1) fraction = 1;

            /* Accumulate service time */
            totalUsedNano += fraction;
            if (totalUsedNano >= (long)NS_PER_SEC) {
                totalUsedSec++;
                totalUsedNano -= NS_PER_SEC;
            }

            msg.mtype = 1;
            msg.value = fraction; /* positive & < quantum = I/O block */
            msgsnd(msgid, &msg, sizeof(long), 0);
            /* loop: wait for next scheduling message */
        } else {
            /* Use the full quantum */
            totalUsedNano += quantum;
            if (totalUsedNano >= (long)NS_PER_SEC) {
                totalUsedSec++;
                totalUsedNano -= NS_PER_SEC;
            }

            msg.mtype = 1;
            msg.value = quantum; /* positive & == quantum = full run */
            msgsnd(msgid, &msg, sizeof(long), 0);
            /* loop: wait for next scheduling message */
        }
    }

    shmdt(shm);
    return 0;
}
