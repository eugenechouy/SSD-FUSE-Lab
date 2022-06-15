/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"

#define SSD_NAME "ssd_file"

#define ROUND_UP(n,d) (((n) + (d-1)) & (-d))
#define MIN(a,b)      ((a) > (b) ? (b) : (a))

enum {
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

static size_t physic_size; // pages number
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

/*
    pca = nand:lba
*/
typedef union pca_rule PCA_RULE;
union pca_rule {
    unsigned int pca;
    struct {
        unsigned int lba : 16;
        unsigned int nand: 16;
    } fields;
};

PCA_RULE curr_pca;
PCA_RULE hot_pca;

static unsigned int get_next_pca();
static void ftl_gc();

unsigned int *L2P;
unsigned int *P2L;
unsigned int *valid_count;
unsigned int free_block_number;

void P2L_set(unsigned int pca, unsigned int lba) {
    PCA_RULE my_pca;
    my_pca.pca = pca;
    P2L[(my_pca.fields.nand * PAGE_PER_BLOCK + my_pca.fields.lba)] = lba;
}

static int ssd_resize(size_t new_size) {
    // set logic size to new_size
    if (new_size > NAND_SIZE_KB * 1024) {
        return -ENOMEM;
    }
    else {
        logic_size = new_size;
        return 0;
    }
}

static int ssd_expand(size_t new_size) {
    // logic must less logic limit
    if (new_size > logic_size) {
        return ssd_resize(new_size);
    }
    return 0;
}

static int nand_read(char* buf, int pca) {
    char nand_name[100];
    FILE *fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.nand);

    if ( (fptr = fopen(nand_name, "r")) ) {
        fseek(fptr, my_pca.fields.lba * PAGE_SIZE, SEEK_SET);
        fread(buf, 1, PAGE_SIZE, fptr);
        fclose(fptr);
    }
    else {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return PAGE_SIZE;
}

static int nand_write(const char* buf, int pca) {
    char nand_name[100];
    FILE *fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.nand);

    if ( (fptr = fopen(nand_name, "r+")) ) {
        fseek(fptr, my_pca.fields.lba * PAGE_SIZE, SEEK_SET);
        fwrite(buf, 1, PAGE_SIZE, fptr);
        fclose(fptr);
        physic_size++;
        valid_count[my_pca.fields.nand]++;
    }
    else {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }
    nand_write_size += PAGE_SIZE;
    return PAGE_SIZE;
}

static int nand_erase(int block_index) {
    char nand_name[100];
    FILE *fptr;

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block_index);

    fptr = fopen(nand_name, "w");
    if (fptr == NULL) {
        printf("erase nand_%d fail", block_index);
        return 0;
    }
    fclose(fptr);
    valid_count[block_index] = FREE_BLOCK;
    return 1;
}

static unsigned int get_next_block() {
    for (int i=0 ; i<PHYSICAL_NAND_NUM ; i++) {
        if (valid_count[(curr_pca.fields.nand + i) % PHYSICAL_NAND_NUM] == FREE_BLOCK) {
            curr_pca.fields.nand = (curr_pca.fields.nand + i) % PHYSICAL_NAND_NUM;
            curr_pca.fields.lba  = 0;
            free_block_number--;
            valid_count[curr_pca.fields.nand] = 0;
            return curr_pca.pca;
        }
    }
    return OUT_OF_BLOCK;
}

static unsigned int get_next_pca() {
    if (curr_pca.pca == INVALID_PCA) {
        // init
        curr_pca.pca = 0;
        valid_count[0] = 0;
        free_block_number--;
        return curr_pca.pca;
    }
     
    if(curr_pca.fields.lba == (PAGE_PER_BLOCK - 1)) {
        int temp = get_next_block();
        if (temp == OUT_OF_BLOCK) {
            return OUT_OF_BLOCK;
        }
        else if(temp == -EINVAL) {
            return -EINVAL;
        }
        else {
            return temp;
        }
    }
    else {
        // get the next page
        curr_pca.fields.lba += 1;
    }
    return curr_pca.pca;
}


static int ftl_read(char* buf, size_t lba) {
    // TODO
    // 1. Check L2P to get PCA
    // 2. Send read data into buf
    int pca = L2P[lba];
    if (pca == INVALID_PCA) {
        printf("################ ftl_read: Try to read an invalid LBA = %ld\n", lba);
        return -EINVAL;
    }
    return nand_read(buf, pca);
}

static int ftl_write(const char* buf, size_t lba, size_t size, off_t offset) {
    // TODO
    // 1. invalid old PCA address
    // 2. allocate a new PCA address
    // 3. send write cmd (read_modify_write)
    // 4. update L2P
    PCA_RULE my_pca;
    int ret;
    int pca; 
    int new_pca;
    char *tmp_buf;

    new_pca = get_next_pca();
    if (new_pca == OUT_OF_BLOCK) {
        return OUT_OF_BLOCK;
    }

    pca = L2P[lba];
    if (pca != INVALID_PCA) {
        my_pca.pca  = pca;
        hot_pca.pca = pca;
        valid_count[my_pca.fields.nand]--;
        P2L_set(pca, INVALID_LBA);
    } 

    // if write size not align to PAGE_SIZE, do read_modify_write
    if (size != PAGE_SIZE) {
        tmp_buf = calloc(PAGE_SIZE, sizeof(char));
        nand_read(tmp_buf, pca);
        memcpy(tmp_buf + offset, buf, size);
        ret = nand_write(tmp_buf, new_pca);
    } else {
        ret = nand_write(buf, new_pca);
    }
    L2P[lba] = new_pca;
    P2L_set(new_pca, lba);

    if (physic_size >= 80) 
        ftl_gc();

    return ret;
}

static int ftl_gc_move(int block_index) {
    int i;
    PCA_RULE my_pca;
    unsigned int new_pca;
    unsigned int lba;
    char *tmp_buf = calloc(PAGE_SIZE, sizeof(char));

    my_pca.pca = 0;
    my_pca.fields.nand = block_index;
    for(i=0 ; i<PAGE_PER_BLOCK ; i++) {
        my_pca.fields.lba = i;
        lba = P2L[(my_pca.fields.nand * PAGE_PER_BLOCK + my_pca.fields.lba)];
        if (lba != INVALID_LBA) {
            nand_read(tmp_buf, my_pca.pca);
            printf("########### gc move %d %d\n", i, lba);
            new_pca = get_next_pca();
            if (new_pca == OUT_OF_BLOCK) {
                printf("ftl_gc: OUT_OF_BLOCK\n");
                return OUT_OF_BLOCK;
            }
            nand_write(tmp_buf, new_pca);
            // update table
            P2L_set(new_pca, lba);
            L2P[lba] = new_pca;
        }
    }
    return nand_erase(block_index);
}

static void ftl_gc() {
    int i;
    int min_valid_count = 10;
    int target_block    = -1;
    for(i=0 ; i<PHYSICAL_NAND_NUM ; i++) {
        if (i == curr_pca.fields.nand || i == hot_pca.fields.nand)
            continue;
        if (valid_count[i] == FREE_BLOCK)
            continue;
        if (valid_count[i] < min_valid_count) {
            min_valid_count = valid_count[i];
            target_block = i;
        }
    }
    printf("############## gc %d: %d valid page\n", target_block, min_valid_count);

    if (target_block != -1) {
        ftl_gc_move(target_block);
    }
}

static int ssd_file_type(const char* path) {
    if (strcmp(path, "/") == 0) {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0) {
        return SSD_FILE;
    }
    return SSD_NONE;
}

/* 
    hard-coded
    size of ssd_file = logic_size
*/
static int ssd_getattr(const char* path, struct stat* stbuf,
                       struct fuse_file_info* fi) {
    (void) fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path)) {
        case SSD_ROOT:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            return -ENOENT;
    }
    return 0;
}

/* 
    Just return success
*/
static int ssd_open(const char* path, struct fuse_file_info* fi) {
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE) {
        return 0;
    }
    return -ENOENT;
}

static int ssd_do_read(char* buf, size_t size, off_t offset) {
    int i;
    int tmp_lba, tmp_lba_range;
    char *tmp_buf;

    // off limit
    if ((offset) >= logic_size) {
        return 0;
    }

    if (size > logic_size - offset) {
        // is valid data section
        size = logic_size - offset;
    }

    tmp_lba       = offset / PAGE_SIZE;
    tmp_lba_range = (offset + size - 1) / PAGE_SIZE - (tmp_lba) + 1;
    tmp_buf       = calloc(tmp_lba_range * PAGE_SIZE, sizeof(char));

    for (i = 0; i < tmp_lba_range; i++) {
        // TODO
        if (ftl_read(tmp_buf + i * PAGE_SIZE, tmp_lba + i) != PAGE_SIZE)
            return 0;
    }

    memcpy(buf, tmp_buf + offset % PAGE_SIZE, size);
    free(tmp_buf);
    return size;
}

static int ssd_read(const char* path, char* buf, size_t size,
                    off_t offset, struct fuse_file_info* fi) {
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE) {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}

static int ssd_do_write(const char* buf, size_t size, off_t offset) {
    int i;
    int tmp_lba, tmp_lba_range;
    int process_size;
    int remain_size;
    int curr_size;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0) {
        return -ENOMEM;
    }

    tmp_lba       = offset / PAGE_SIZE;
    tmp_lba_range = (offset + size - 1) / PAGE_SIZE - (tmp_lba) + 1;

    process_size = 0;
    remain_size = size;
    for (i = 0; i < tmp_lba_range; i++) {
        // TODO
        printf("################## remain %d -> %d ####################\n", remain_size, tmp_lba + i);
        curr_size = PAGE_SIZE - offset % PAGE_SIZE;
        curr_size = MIN(remain_size, curr_size);
        
        if (ftl_write(buf + process_size, tmp_lba + i, curr_size, offset % PAGE_SIZE) != PAGE_SIZE) {
            printf("############## ssd_do_write: failed\n");
            break;
        }
        
        offset = ROUND_UP(offset, PAGE_SIZE);
        remain_size -= curr_size;
        process_size += curr_size;
    }
    return process_size;
}

static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t  offset, struct fuse_file_info* fi) {

    (void) fi;
    if (ssd_file_type(path) != SSD_FILE) {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}

static int ssd_truncate(const char* path, off_t size,
                        struct fuse_file_info* fi) {
    (void) fi;
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(valid_count, FREE_BLOCK, sizeof(int) * PHYSICAL_NAND_NUM);
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;
    if (ssd_file_type(path) != SSD_FILE) {
        return -EINVAL;
    }
    return ssd_resize(size);
}

/* 
    hard-coded
*/
static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags) {
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT) {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}

/* 
    SSD_GET_LOGIC_SIZE:  logic_size
    SSD_GET_PHYSIC_SIZE: physic_size
    SSD_GET_WA:          nand_write_size / host_write_size
*/
static int ssd_ioctl(const char* path, unsigned int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data) {

    if (ssd_file_type(path) != SSD_FILE) {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT) {
        return -ENOSYS;
    }
    switch (cmd) {
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
    }
    return -EINVAL;
}

static const struct fuse_operations ssd_oper =
{
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};


int main(int argc, char* argv[]) {
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
    curr_pca.pca = INVALID_PCA;
    hot_pca.pca  = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;

    L2P = malloc(LOGICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    P2L = malloc(PHYSICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    valid_count = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(valid_count, FREE_BLOCK, sizeof(int) * PHYSICAL_NAND_NUM);

    // create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++) {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL) {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
