// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#define ETHER_ADDR_LEN 6
#define ETHER_MAX_LEN 1518
struct ether_addr { uint8_t ether_addr_octet[6]; };
#ifdef __cplusplus
extern "C" {
#endif
char* ether_ntoa(const struct ether_addr* address);
#ifdef __cplusplus
}
#endif
