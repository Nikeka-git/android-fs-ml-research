#define _GNU_SOURCE
#include <stdio.h>
#include <sched.h>
#include <sys/socket.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int main() {
    printf("[*] Testing unshare(CLONE_NEWUSER|CLONE_NEWNET)...\n");

    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) < 0) {
        printf("[-] unshare failed: %s (errno=%d)\n", strerror(errno), errno);
        return 1;
    }
    printf("[+] unshare succeeded! We have CAP_NET_ADMIN in new namespace.\n");

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0) {
        printf("[-] socket failed: %s (errno=%d)\n", strerror(errno), errno);
        return 1;
    }
    printf("[+] socket created.\n");

    // Пробуем вызвать setsockopt (даже с некорректными данными, чтобы проверить доступ)
    struct ipt_replace repl = {0};
    int ret = setsockopt(sock, IPPROTO_IP, IPT_SO_SET_REPLACE, &repl, sizeof(repl));
    if (ret < 0) {
        printf("[-] setsockopt failed: %s (errno=%d)\n", strerror(errno), errno);
    } else {
        printf("[+] setsockopt succeeded! (unexpected, but access granted)\n");
    }

    close(sock);
    return 0;
}