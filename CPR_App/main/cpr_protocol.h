#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PACKET_MAGIC_0           0xC9
#define PACKET_MAGIC_1           0xC2
#define PROTOCOL_VER             0x06
#define PTYPE_ANNOUNCE           0x01
#define PTYPE_COMPRESSION        0x02
#define PTYPE_AED                0x04
#define PTYPE_AIR                0x05

#define MANIKIN_TYPE_DEFAULT    1

typedef struct __attribute__((packed)) {
    uint8_t  magic[2];       /* 0xC9, 0xC2 */
    uint8_t  version;        /* 0x06 */
    uint8_t  type;           /* PTYPE_* */
    uint32_t timestamp;      /* uptime ms */
    uint16_t id;             /* packet sequence id */
    int8_t   depth;          /* depth or state */
    uint8_t  deviceID[3];    /* last 3 bytes of MAC */
    uint8_t  checksum;       /* XOR [0..13] */
} CprPacket;

typedef struct __attribute__((packed)) {
    uint8_t  magic[2];       /* 0xC9, 0xC2 */
    uint8_t  version;        /* 0x06 */
    uint8_t  type;           /* 0x01 */
    uint32_t timestamp;      /* uptime ms */
    uint16_t id;             /* packet sequence id */
    int8_t   depth;          /* battery pct */
    uint8_t  deviceID[3];    /* last 3 bytes of MAC */
    char     manikinName[4]; /* e.g. "103\0" */
    uint8_t  manikinType;    /* 1 */
    uint8_t  checksum;       /* XOR [0..18] */
} AnnouncePacket;

static inline uint8_t cpr_calc_checksum(const uint8_t *buf, size_t n) {
    uint8_t cs = 0;
    for (size_t i = 0; i < n; ++i) {
        cs ^= buf[i];
    }
    return cs;
}
