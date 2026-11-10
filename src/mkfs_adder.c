// Adds one host file to the root directory of a MiniVSFS image and writes the
// result to a new image. The input image is never modified.
// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c -o mkfs_adder
#define _FILE_OFFSET_BITS 64
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "minivsfs.h"

struct options {
    char input[256];
    char output[256];
    char file[256];
};

static void copy_arg(char *dest, size_t size, const char *value) {
    if (strlen(value) >= size) {
        fprintf(stderr, "Path is too long: %s\n", value);
        exit(1);
    }
    strcpy(dest, value);
}

static void parse_args(int argc, char **argv, struct options *opts) {
    static struct option long_opts[] = {
        {"input",  required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"file",   required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:o:f:", long_opts, NULL)) != -1) {
        switch (c) {
        case 'i': copy_arg(opts->input,  sizeof(opts->input),  optarg); break;
        case 'o': copy_arg(opts->output, sizeof(opts->output), optarg); break;
        case 'f': copy_arg(opts->file,   sizeof(opts->file),   optarg); break;
        default:
            fprintf(stderr, "Usage: %s --input in.img --output out.img --file host.txt\n", argv[0]);
            exit(1);
        }
    }
    if (!opts->input[0] || !opts->output[0] || !opts->file[0]) {
        fprintf(stderr, "Missing required args. Usage: %s --input in.img --output out.img --file host.txt\n", argv[0]);
        exit(1);
    }
}

static int layout_is_valid(const superblock_t *sb, uint64_t img_sz) {
    return sb->total_blocks * BS == img_sz &&
           sb->inode_bitmap_blocks > 0 && sb->data_bitmap_blocks > 0 &&
           sb->inode_bitmap_start + sb->inode_bitmap_blocks <= sb->data_bitmap_start &&
           sb->data_bitmap_start + sb->data_bitmap_blocks <= sb->inode_table_start &&
           sb->inode_table_start + sb->inode_table_blocks <= sb->data_region_start &&
           sb->data_region_start + sb->data_region_blocks == sb->total_blocks &&
           sb->inode_count * INODE_SIZE <= sb->inode_table_blocks * BS &&
           sb->inode_count <= sb->inode_bitmap_blocks * BS * 8 &&
           sb->data_region_blocks <= sb->data_bitmap_blocks * BS * 8;
}

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

static void add_file_to_image(const char *in_path, const char *out_path, const char *hostfile) {
    // 1) Read the whole image into memory. Nothing is written until every step succeeds.
    FILE *fi = fopen(in_path, "rb");
    if (!fi) { perror("fopen input"); exit(1); }
    if (fseek(fi, 0, SEEK_END) != 0) { perror("fseek"); exit(1); }
    long img_sz = ftell(fi);
    if (img_sz < (long)BS) { fprintf(stderr, "Invalid MiniVSFS image\n"); exit(1); }
    rewind(fi);
    uint8_t *img = malloc((size_t)img_sz);
    if (!img) { perror("malloc image"); exit(1); }
    if (fread(img, 1, (size_t)img_sz, fi) != (size_t)img_sz) { perror("fread image"); exit(1); }
    fclose(fi);

    // 2) Validate the image and locate its regions
    superblock_t *sb = (superblock_t *)img;
    if (sb->magic != MVFS_MAGIC || sb->version != 1 || sb->block_size != BS) {
        fprintf(stderr, "Invalid MiniVSFS image\n");
        exit(1);
    }
    if (!superblock_crc_valid(img)) {
        fprintf(stderr, "Superblock checksum mismatch: image is corrupted\n");
        exit(1);
    }
    if (!layout_is_valid(sb, (uint64_t)img_sz)) {
        fprintf(stderr, "Superblock layout is inconsistent with the image\n");
        exit(1);
    }
    uint8_t *inode_bm = img + sb->inode_bitmap_start * BS;
    uint8_t *data_bm  = img + sb->data_bitmap_start * BS;
    inode_t *itable   = (inode_t *)(img + sb->inode_table_start * BS);
    uint8_t *data_reg = img + sb->data_region_start * BS;

    // 3) Locate the root directory block
    inode_t *root = &itable[0];
    int64_t root_rel = (int64_t)root->direct[0] - (int64_t)sb->data_region_start;
    if (root_rel < 0 || (uint64_t)root_rel >= sb->data_region_blocks) {
        fprintf(stderr, "Root directory block pointer invalid\n");
        exit(1);
    }
    dirent64_t *dirblk = (dirent64_t *)(data_reg + (size_t)root_rel * BS);
    int dir_slots = (int)(BS / sizeof(dirent64_t));

    // 4) Validate the file name and find a directory slot
    const char *base = base_name(hostfile);
    if (strlen(base) == 0 || strlen(base) > MAX_NAME_LEN) {
        fprintf(stderr, "Invalid file name (1 to %d characters allowed)\n", MAX_NAME_LEN);
        exit(1);
    }
    int free_slot = -1;
    for (int i = 0; i < dir_slots; ++i) {
        if (dirblk[i].inode_no == 0) {
            if (free_slot < 0) free_slot = i;
        } else if (strncmp(dirblk[i].name, base, sizeof(dirblk[i].name)) == 0) {
            fprintf(stderr, "File '%s' already exists in the image\n", base);
            exit(1);
        }
    }
    if (free_slot < 0) {
        fprintf(stderr, "Root directory is full\n");
        exit(1);
    }

    // 5) Check the host file
    struct stat st;
    if (stat(hostfile, &st) != 0) { perror("stat file"); exit(1); }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Not a regular file: %s\n", hostfile);
        exit(1);
    }
    uint64_t fsize = (uint64_t)st.st_size;
    uint64_t need_blocks = (fsize + BS - 1) / BS;
    if (need_blocks > DIRECT_MAX) {
        fprintf(stderr, "File requires %llu blocks (> %d direct)\n",
                (unsigned long long)need_blocks, DIRECT_MAX);
        exit(1);
    }

    // 6) First-fit allocation of one inode and the data blocks
    int free_ino_idx = -1;
    for (uint64_t i = 0; i < sb->inode_count; ++i) {
        if ((inode_bm[i >> 3] & (1u << (i & 7))) == 0) { free_ino_idx = (int)i; break; }
    }
    if (free_ino_idx < 0) { fprintf(stderr, "No free inode\n"); exit(1); }
    uint32_t new_ino = (uint32_t)(free_ino_idx + 1); // inode numbers start at 1

    uint32_t rel_blk_idx[DIRECT_MAX];
    uint64_t found = 0;
    for (uint64_t i = 0; i < sb->data_region_blocks && found < need_blocks; ++i) {
        if ((data_bm[i >> 3] & (1u << (i & 7))) == 0) rel_blk_idx[found++] = (uint32_t)i;
    }
    if (found < need_blocks) { fprintf(stderr, "Not enough free data blocks\n"); exit(1); }

    // 7) Copy the file into its blocks. The last block is zero-padded.
    FILE *fh = fopen(hostfile, "rb");
    if (!fh) { perror("fopen file"); exit(1); }
    uint64_t remaining = fsize;
    for (uint64_t k = 0; k < need_blocks; ++k) {
        uint8_t *dest = data_reg + (size_t)rel_blk_idx[k] * BS;
        size_t want = remaining < BS ? (size_t)remaining : BS;
        memset(dest, 0, BS);
        if (fread(dest, 1, want, fh) != want) { perror("fread file"); exit(1); }
        remaining -= want;
        data_bm[rel_blk_idx[k] >> 3] |= (uint8_t)(1u << (rel_blk_idx[k] & 7));
    }
    fclose(fh);

    // 8) Create the inode
    uint64_t now = (uint64_t)time(NULL);
    inode_t *ni = &itable[free_ino_idx];
    memset(ni, 0, sizeof(*ni));
    ni->mode = MODE_FILE;
    ni->links = 1;
    ni->size_bytes = fsize;
    ni->atime = ni->mtime = ni->ctime = now;
    for (uint64_t k = 0; k < need_blocks; ++k) {
        ni->direct[k] = (uint32_t)(sb->data_region_start + rel_blk_idx[k]);
    }
    inode_crc_finalize(ni);
    inode_bm[free_ino_idx >> 3] |= (uint8_t)(1u << (free_ino_idx & 7));

    // 9) Add the directory entry and update the root inode
    dirent64_t *de = &dirblk[free_slot];
    memset(de, 0, sizeof(*de));
    de->inode_no = new_ino;
    de->type = DT_FILE;
    memcpy(de->name, base, strlen(base));
    dirent_checksum_finalize(de);
    root->mtime = now;
    inode_crc_finalize(root);

    // 10) Update the superblock, then write the new image
    sb->mtime_epoch = now;
    superblock_crc_finalize(sb);

    FILE *fo = fopen(out_path, "wb");
    if (!fo) { perror("fopen output"); exit(1); }
    if (fwrite(img, 1, (size_t)img_sz, fo) != (size_t)img_sz) { perror("fwrite output"); exit(1); }
    if (fclose(fo) != 0) { perror("fclose output"); exit(1); }
    free(img);

    printf("Added '%s' as inode #%u using %llu block(s). Output: %s\n",
           base, new_ino, (unsigned long long)need_blocks, out_path);
}

int main(int argc, char **argv) {
    crc32_init();
    struct options opts = {0};
    parse_args(argc, argv, &opts);
    printf("Input  = %s\nOutput = %s\nFile   = %s\n", opts.input, opts.output, opts.file);
    add_file_to_image(opts.input, opts.output, opts.file);
    return 0;
}
