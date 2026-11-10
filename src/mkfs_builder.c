// Creates an empty MiniVSFS disk image that contains only the root directory.
// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "minivsfs.h"

struct options {
    char image[256];
    int size_kib;
    int inodes;
};

static void *xcalloc(size_t size) {
    void *p = calloc(1, size);
    if (!p) { perror("calloc"); exit(1); }
    return p;
}

static void parse_args(int argc, char **argv, struct options *opts) {
    static struct option long_opts[] = {
        {"image", required_argument, 0, 'i'},
        {"size-kib", required_argument, 0, 's'},
        {"inodes", required_argument, 0, 'n'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:s:n:", long_opts, NULL)) != -1) {
        switch (c) {
        case 'i':
            if (strlen(optarg) >= sizeof(opts->image)) {
                fprintf(stderr, "Image path is too long\n");
                exit(1);
            }
            strcpy(opts->image, optarg);
            break;
        case 's': opts->size_kib = atoi(optarg); break;
        case 'n': opts->inodes = atoi(optarg); break;
        default:
            fprintf(stderr, "Usage: %s --image FILE --size-kib N --inodes N\n", argv[0]);
            exit(1);
        }
    }

    if (opts->size_kib < 180 || opts->size_kib > 4096 || opts->size_kib % 4 != 0) {
        fprintf(stderr, "Invalid size-kib: must be a multiple of 4 between 180 and 4096\n");
        exit(1);
    }
    if (opts->inodes < 128 || opts->inodes > 512) {
        fprintf(stderr, "Invalid inode count: must be between 128 and 512\n");
        exit(1);
    }
    if (opts->image[0] == '\0') {
        fprintf(stderr, "No image file specified\n");
        exit(1);
    }
}

static inline void set_bit(uint8_t *bm, uint64_t index) {
    bm[index >> 3] |= (uint8_t)(1u << (index & 7));
}

static void write_block(FILE *f, uint64_t blkno, const void *data) {
    if (fseek(f, (long)(blkno * (uint64_t)BS), SEEK_SET) != 0 || fwrite(data, 1, BS, f) != BS) {
        perror("write image");
        exit(1);
    }
}

static void build_fs_image(const char *image_path, int size_kib, int inodes) {
    uint64_t total_blocks = (size_kib * 1024ULL) / BS;
    uint64_t inode_table_blocks = (inodes * INODE_SIZE + BS - 1) / BS;

    uint64_t superblock_start   = 0;
    uint64_t inode_bitmap_start = 1;
    uint64_t data_bitmap_start  = 2;
    uint64_t inode_table_start  = 3;
    uint64_t data_region_start  = inode_table_start + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;
    uint64_t now = (uint64_t)time(NULL);

    uint8_t *inode_bitmap = xcalloc(BS);
    uint8_t *data_bitmap  = xcalloc(BS);
    uint8_t *inode_table_bytes = xcalloc(inode_table_blocks * BS);
    uint8_t *root_block = xcalloc(BS);
    uint8_t *zero = xcalloc(BS);

    // The checksum covers the whole block, so the superblock is built in place.
    uint8_t *sb_block = xcalloc(BS);
    superblock_t *sb = (superblock_t *)sb_block;
    sb->magic = MVFS_MAGIC;
    sb->version = 1;
    sb->block_size = BS;
    sb->total_blocks = total_blocks;
    sb->inode_count = (uint64_t)inodes;
    sb->inode_bitmap_start = inode_bitmap_start;
    sb->inode_bitmap_blocks = 1;
    sb->data_bitmap_start = data_bitmap_start;
    sb->data_bitmap_blocks = 1;
    sb->inode_table_start = inode_table_start;
    sb->inode_table_blocks = inode_table_blocks;
    sb->data_region_start = data_region_start;
    sb->data_region_blocks = data_region_blocks;
    sb->root_inode = ROOT_INO;
    sb->mtime_epoch = now;
    sb->flags = 0;
    superblock_crc_finalize(sb);

    set_bit(inode_bitmap, 0); // inode #1 is the root
    set_bit(data_bitmap, 0);  // the first data block holds the root directory

    inode_t *root = (inode_t *)inode_table_bytes;
    root->mode = MODE_DIR;
    root->links = 2;
    root->size_bytes = BS;
    root->atime = root->mtime = root->ctime = now;
    root->direct[0] = (uint32_t)data_region_start;
    inode_crc_finalize(root);

    dirent64_t *dot = (dirent64_t *)root_block;
    dirent64_t *dotdot = dot + 1;
    dot->inode_no = ROOT_INO;
    dot->type = DT_DIRECTORY;
    strcpy(dot->name, ".");
    dirent_checksum_finalize(dot);
    dotdot->inode_no = ROOT_INO;
    dotdot->type = DT_DIRECTORY;
    strcpy(dotdot->name, "..");
    dirent_checksum_finalize(dotdot);

    FILE *f = fopen(image_path, "wb+");
    if (!f) { perror("fopen image"); exit(1); }

    for (uint64_t b = 0; b < total_blocks; b++) write_block(f, b, zero);
    write_block(f, superblock_start, sb_block);
    write_block(f, inode_bitmap_start, inode_bitmap);
    write_block(f, data_bitmap_start, data_bitmap);
    for (uint64_t i = 0; i < inode_table_blocks; i++) {
        write_block(f, inode_table_start + i, inode_table_bytes + i * BS);
    }
    write_block(f, data_region_start, root_block);

    if (fclose(f) != 0) { perror("fclose image"); exit(1); }
    free(inode_bitmap); free(data_bitmap); free(inode_table_bytes);
    free(root_block); free(zero); free(sb_block);

    printf("Wrote image '%s' successfully.\n", image_path);
}

int main(int argc, char **argv) {
    crc32_init();
    struct options opts = {0};
    parse_args(argc, argv, &opts);
    printf("Image = %s\nSize  = %d KiB\nInodes= %d\n", opts.image, opts.size_kib, opts.inodes);
    build_fs_image(opts.image, opts.size_kib, opts.inodes);
    return 0;
}
