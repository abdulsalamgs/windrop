// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stdlib.h>
#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define htole64(x) ((uint64_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define le64toh(x) ((uint64_t)(x))
#define htobe16(x) _byteswap_ushort((uint16_t)(x))
#define htobe32(x) _byteswap_ulong((uint32_t)(x))
#define htobe64(x) _byteswap_uint64((uint64_t)(x))
#define be16toh(x) htobe16(x)
#define be32toh(x) htobe32(x)
#define be64toh(x) htobe64(x)
