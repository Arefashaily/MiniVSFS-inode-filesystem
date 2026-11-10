// On-disk format of MiniVSFS, shared by mkfs_builder and mkfs_adder.
// All integers are little-endian and all structures are packed.
#ifndef MINIVSFS_H
#define MINIVSFS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
#define MVFS_MAGIC 0x4D565346u // "MVFS"
#define MAX_NAME_LEN 57        // name[58] always keeps a terminating NUL

#define MODE_DIR 040000
#define MODE_FILE 0100000
#define DT_FILE 1
#define DT_DIRECTORY 2

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;         // crc32 of block bytes [0..4091], with this field zeroed
} superblock_t;

typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX]; // absolute block numbers, 0 when unused
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;        // low 4 bytes hold the crc32 of bytes [0..119]
} inode_t;

typedef struct {
    uint32_t inode_no;         // 0 marks a free slot
    uint8_t type;
    char name[58];
    uint8_t checksum;          // XOR of bytes [0..62]
} dirent64_t;
#pragma pack(pop)

_Static_assert(sizeof(superblock_t) == 116, "superblock size mismatch");
_Static_assert(sizeof(inode_t) == INODE_SIZE, "inode size mismatch");
_Static_assert(sizeof(dirent64_t) == 64, "dirent size mismatch");

static uint32_t CRC32_TAB[256];

static inline void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        CRC32_TAB[i] = c;
    }
}

static inline uint32_t crc32(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = CRC32_TAB[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// sb must point to the start of a full BS-byte block, because the checksum
// covers the whole block and not only the structure.
static inline uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *)sb, BS - 4);
    sb->checksum = s;
    return s;
}

static inline int superblock_crc_valid(const uint8_t *block) {
    uint8_t tmp[BS];
    memcpy(tmp, block, BS);
    superblock_t *sb = (superblock_t *)tmp;
    uint32_t stored = sb->checksum;
    sb->checksum = 0;
    return crc32(tmp, BS - 4) == stored;
}

static inline void inode_crc_finalize(inode_t *ino) {
    uint8_t tmp[INODE_SIZE];
    memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    ino->inode_crc = (uint64_t)crc32(tmp, 120);
}

static inline void dirent_checksum_finalize(dirent64_t *de) {
    const uint8_t *p = (const uint8_t *)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}

#endif
