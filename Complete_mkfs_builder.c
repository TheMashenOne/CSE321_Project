#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>

#define BS 4096u           
#define INODE_SIZE 128u
#define ROOT_INO 1u

uint64_t g_random_seed = 0; 
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
    uint32_t checksum;            
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;                
    uint16_t links;               
    uint32_t uid;                 
    uint32_t gid;                 
    uint64_t size_bytes;         
    uint64_t atime;              
    uint64_t mtime;               
    uint64_t ctime;               
    uint32_t direct[12];          
    uint32_t reserved_0;         
    uint32_t reserved_1;          
    uint32_t reserved_2;          
    uint32_t proj_id;             
    uint32_t uid16_gid16;         
    uint64_t xattr_ptr;           
    uint64_t inode_crc;           
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;            
    uint8_t type;                 
    char name[58];                
    uint8_t checksum;             
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}


void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);

    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; 
}

void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   
    de->checksum = x;
}

void print_usage(const char* program_name) {
    printf("Usage: %s --image <filename> --size-kib <180..4096> --inodes <128..512>\n", program_name);
    printf("  --image: the name of the output image\n");
    printf("  --size-kib: the total size of the image in kilobytes (multiple of 4)\n");
    printf("  --inodes: number of inodes in the file system\n");
}


int parse_args(int argc, char* argv[], char** image_name, uint64_t* size_kib, uint64_t* inodes) {
    if (argc != 7) {
        return -1;
    }
    
    for (int i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "--image") == 0) {
            *image_name = argv[i + 1];
        } else if (strcmp(argv[i], "--size-kib") == 0) {
            *size_kib = strtoull(argv[i + 1], NULL, 10);
            if (*size_kib < 180 || *size_kib > 4096 || *size_kib % 4 != 0) {
                printf("Error: size-kib must be between 180-4096 and multiple of 4\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--inodes") == 0) {
            *inodes = strtoull(argv[i + 1], NULL, 10);
            if (*inodes < 128 || *inodes > 512) {
                printf("Error: inodes must be between 128-512\n");
                return -1;
            }
        } else {
            return -1;
        }
    }
    
    if (*image_name == NULL || *size_kib == 0 || *inodes == 0) {
        return -1;
    }
    
    return 0;
}

int main(int argc, char* argv[]) {
    printf("DEBUG: sizeof(superblock_t) = %zu bytes\n", sizeof(superblock_t));
    char* image_name = NULL;
    uint64_t size_kib = 0;
    uint64_t inodes = 0;
    
 
    if (parse_args(argc, argv, &image_name, &size_kib, &inodes) != 0) {
        print_usage(argv[0]);
        return 1;
    }
    
    crc32_init();
    
    uint64_t total_blocks = (size_kib * 1024) / BS;
    uint64_t inode_table_blocks = (inodes * INODE_SIZE + BS - 1) / BS;
    uint64_t data_region_start = 3 + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;
    
    printf("Creating filesystem with:\n");
    printf("  Image: %s\n", image_name);
    printf("  Size: %lu KiB (%lu blocks)\n", size_kib, total_blocks);
    printf("  Inodes: %lu\n", inodes);
    printf("  Inode table blocks: %lu\n", inode_table_blocks);
    printf("  Data region blocks: %lu\n", data_region_blocks);

    FILE* img = fopen(image_name, "wb");
    if (!img) {
        perror("Failed to create image file");
        return 1;
    }
    

    superblock_t sb = {0};
    sb.magic = 0x4D565346;
    sb.version = 1;
    sb.block_size = BS;
    sb.total_blocks = total_blocks;
    sb.inode_count = inodes;
    sb.inode_bitmap_start = 1;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start = 2;
    sb.data_bitmap_blocks = 1;
    sb.inode_table_start = 3;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_region_start = data_region_start;
    sb.data_region_blocks = data_region_blocks;
    sb.root_inode = ROOT_INO;
    sb.mtime_epoch = time(NULL);
    sb.flags = 0;
    

    superblock_crc_finalize(&sb);
    

    uint8_t block[BS] = {0};
    memcpy(block, &sb, sizeof(sb));
    fwrite(block, 1, BS, img);
    

    memset(block, 0, BS);
 
    block[0] = 0x01;
    fwrite(block, 1, BS, img);
    
    memset(block, 0, BS);
    block[0] = 0x01;
    fwrite(block, 1, BS, img);

    inode_t root_inode = {0};
    root_inode.mode = 0040000;  
    root_inode.links = 2;      
    root_inode.uid = 0;
    root_inode.gid = 0;
    root_inode.size_bytes = 2 * sizeof(dirent64_t);  
    root_inode.atime = time(NULL);
    root_inode.mtime = time(NULL);
    root_inode.ctime = time(NULL);
    root_inode.direct[0] = data_region_start; 
    for (int i = 1; i < 12; i++) {
        root_inode.direct[i] = 0;  
    }
    root_inode.reserved_0 = 0;
    root_inode.reserved_1 = 0;
    root_inode.reserved_2 = 0;
    root_inode.proj_id = 13;  
    root_inode.uid16_gid16 = 0;
    root_inode.xattr_ptr = 0;

    inode_crc_finalize(&root_inode);
    
    for (uint64_t i = 0; i < inode_table_blocks; i++) {
        memset(block, 0, BS);
        if (i == 0) {
            memcpy(block, &root_inode, sizeof(root_inode));
        }
        fwrite(block, 1, BS, img);
    }
    
    dirent64_t dot_entry = {0};
    dot_entry.inode_no = ROOT_INO;
    dot_entry.type = 2;  
    strcpy(dot_entry.name, ".");
    dirent_checksum_finalize(&dot_entry);
    
    dirent64_t dotdot_entry = {0};
    dotdot_entry.inode_no = ROOT_INO;
    dotdot_entry.type = 2;  
    strcpy(dotdot_entry.name, "..");
    dirent_checksum_finalize(&dotdot_entry);

    memset(block, 0, BS);
    memcpy(block, &dot_entry, sizeof(dot_entry));
    memcpy(block + sizeof(dot_entry), &dotdot_entry, sizeof(dotdot_entry));
    fseek(img, data_region_start * BS, SEEK_SET);
    fwrite(block, 1, BS, img);
    
    memset(block, 0, BS);
    for (uint64_t i = 1; i < data_region_blocks; i++) {
        fwrite(block, 1, BS, img);
    }
    
    fclose(img);
    printf("Filesystem created successfully: %s\n", image_name);
    
    return 0;
}
