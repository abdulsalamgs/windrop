// SPDX-License-Identifier: MIT
#include <windows.h>
#include <time.h>
#include <stdio.h>
#include <net/ethernet.h>

int windrop_clock_gettime(int clock,struct timespec* out) {
    LARGE_INTEGER now,frequency;
    (void)clock;
    if(!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency)) return -1;
    out->tv_sec=(time_t)(now.QuadPart/frequency.QuadPart);
    out->tv_nsec=(long)((now.QuadPart%frequency.QuadPart)*1000000000/frequency.QuadPart);
    return 0;
}
char* ether_ntoa(const struct ether_addr* address) {
    static char text[18];
    const uint8_t* a=address->ether_addr_octet;
    snprintf(text,sizeof(text),"%02x:%02x:%02x:%02x:%02x:%02x",a[0],a[1],a[2],a[3],a[4],a[5]);
    return text;
}
