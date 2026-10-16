// getifaddrs() list must contain at least one AF_INET entry. The Android
// netlink block makes the native call fail with EACCES; the hook's
// SIOCGIFCONF fallback is what makes this test pass under bionilux.
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    struct ifaddrs *ifap;
    if (getifaddrs(&ifap) != 0) {
        perror("getifaddrs");
        return 1;
    }
    int n = 0;
    for (struct ifaddrs *i = ifap; i; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET) n++;
    }
    freeifaddrs(ifap);
    if (n > 0) {
        printf("ifaddrs-ok count=%d\n", n);
        return 0;
    }
    return 1;
}