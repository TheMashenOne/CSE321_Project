#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
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
    printf("Usage: %s --input <input_image> --output <output_image> --file <filename>\n", program_name);
    printf("  --input: the name of the input image\n");
    printf("  --output: name of the output image\n");
    printf("  --file: the file to be added to the file system\n");
}

int parse_args(int argc, char* argv[], char** input_name, char** output_name, char** file_name) {
    if (argc != 7) {
        return -1;
    }
    
    for (int i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "--input") == 0) {
            *input_name = argv[i + 1];
        } else if (strcmp(argv[i], "--output") == 0) {
            *output_name = argv[i + 1];
        } else if (strcmp(argv[i], "--file") == 0) {
            *file_name = argv[i + 1];
        } else {
            return -1;
        }
    }
    
    if (*input_name == NULL || *output_name == NULL || *file_name == NULL) {
        return -1;
    }
    
    return 0;
}

uint64_t find_free_inode(FILE* img, superblock_t* sb) {
    uint8_t bitmap[BS];
    
    fseek(img, sb->inode_bitmap_start * BS, SEEK_SET);
    if (fread(bitmap, 1, BS, img) != BS) {
        return 0; 
    }
    
    for (uint64_t byte_idx = 0; byte_idx < BS; byte_idx++) {
        for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
            uint64_t inode_num = byte_idx * 8 + bit_idx + 1; 
            if (inode_num > sb->inode_count) {
                return 0; 
            }
            
            if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                bitmap[byte_idx] |= (1 << bit_idx);
                
                fseek(img, sb->inode_bitmap_start * BS, SEEK_SET);
                fwrite(bitmap, 1, BS, img);
                
                return inode_num;
            }
        }
    }
    
    return 0; 
}

uint64_t find_free_data_block(FILE* img, superblock_t* sb) {
    uint8_t bitmap[BS];

    fseek(img, sb->data_bitmap_start * BS, SEEK_SET);
    if (fread(bitmap, 1, BS, img) != BS) {
        return 0; 
    }

    for (uint64_t byte_idx = 0; byte_idx < BS; byte_idx++) {
        for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
            uint64_t block_num = byte_idx * 8 + bit_idx;
            if (block_num >= sb->data_region_blocks) {
                return 0; 
            }
            if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                bitmap[byte_idx] |= (1 << bit_idx);
                
                fseek(img, sb->data_bitmap_start * BS, SEEK_SET);
                fwrite(bitmap, 1, BS, img);
                
                return sb->data_region_start + block_num;
            }
        }
    }
    
    return 0; 
}

int file_exists_in_root(FILE* img, superblock_t* sb, const char* filename_sanitized, uint64_t* inode_out_opt) {

    inode_t root_inode;
    fseek(img, (sb->inode_table_start * BS) + ((ROOT_INO - 1) * INODE_SIZE), SEEK_SET);
    if (fread(&root_inode, sizeof(root_inode), 1, img) != 1) {
        return -1; 
    }

    if (root_inode.direct[0] == 0) {
        return -1; 
    }

    uint8_t dir_block[BS];
    fseek(img, root_inode.direct[0] * BS, SEEK_SET);
    if (fread(dir_block, 1, BS, img) != BS) {
        return -1; 
    }

    dirent64_t* entries = (dirent64_t*)dir_block;
    int max_entries = BS / sizeof(dirent64_t);

    for (int i = 0; i < max_entries; i++) {
        if (entries[i].inode_no != 0) {
            if (strncmp(entries[i].name, filename_sanitized, 58) == 0) {
                if (inode_out_opt) *inode_out_opt = entries[i].inode_no;
                return 1; 
            }
        }
    }
    return 0; 
}

int add_to_root_directory(FILE* img, superblock_t* sb, const char* filename, uint64_t inode_num) {
    inode_t root_inode;
    fseek(img, (sb->inode_table_start * BS) + ((ROOT_INO - 1) * INODE_SIZE), SEEK_SET);
    if (fread(&root_inode, sizeof(root_inode), 1, img) != 1) {
        return -1;
    }
    
    uint8_t dir_block[BS];
    fseek(img, root_inode.direct[0] * BS, SEEK_SET);
    if (fread(dir_block, 1, BS, img) != BS) {
        return -1; 
    }
    
    dirent64_t* entries = (dirent64_t*)dir_block;
    int max_entries = BS / sizeof(dirent64_t);
    
    for (int i = 0; i < max_entries; i++) {
        if (entries[i].inode_no == 0) {
            entries[i].inode_no = (uint32_t)inode_num;
            entries[i].type = 1; // file
            strncpy(entries[i].name, filename, 57);
            entries[i].name[57] = '\0';
            dirent_checksum_finalize(&entries[i]);

            root_inode.size_bytes += sizeof(dirent64_t);
            root_inode.links++;
            root_inode.mtime = time(NULL);
            inode_crc_finalize(&root_inode);

            fseek(img, (sb->inode_table_start * BS) + ((ROOT_INO - 1) * INODE_SIZE), SEEK_SET);
            fwrite(&root_inode, sizeof(root_inode), 1, img);

            fseek(img, root_inode.direct[0] * BS, SEEK_SET);
            fwrite(dir_block, 1, BS, img);
            
            return 0; 
        }
    }
    
    return -1; 
}

int main(int argc, char* argv[]) {
    (void)&superblock_crc_finalize;
    char* input_name = NULL;
    char* output_name = NULL;
    char* file_name = NULL;
    
    if (parse_args(argc, argv, &input_name, &output_name, &file_name) != 0) {
        print_usage(argv[0]);
        return 1;
    }
    
    crc32_init();
    
    if (access(input_name, F_OK) != 0) {
        printf("Error: Input image file '%s' does not exist\n", input_name);
        return 1;
    }
    
    if (access(file_name, F_OK) != 0) {
        printf("Error: File to add '%s' does not exist\n", file_name);
        return 1;
    }
    
    struct stat file_stat;
    if (stat(file_name, &file_stat) != 0) {
        perror("Failed to get file stats");
        return 1;
    }
    
    uint64_t blocks_needed = (file_stat.st_size + BS - 1) / BS;
    if (blocks_needed > DIRECT_MAX) {
        printf("Error: File too large. Maximum size is %u bytes (%d blocks)\n", 
               DIRECT_MAX * BS, DIRECT_MAX);
        return 1;
    }
    
    printf("Adding file: %s (size: %ld bytes, blocks needed: %lu)\n", 
           file_name, (long)file_stat.st_size, (unsigned long)blocks_needed);
    
    FILE* input = fopen(input_name, "rb");
    FILE* output = fopen(output_name, "wb");
    if (!input || !output) {
        perror("Failed to open files");
        if (input) fclose(input);
        if (output) fclose(output);
        return 1;
    }
    
    uint8_t buffer[BS];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BS, input)) > 0) {
        fwrite(buffer, 1, bytes_read, output);
    }
    fclose(input);
    fclose(output);
    
    output = fopen(output_name, "r+b");
    if (!output) {
        perror("Failed to reopen output file");
        return 1;
    }
    
    superblock_t sb;
    fseek(output, 0, SEEK_SET);
    size_t sb_bytes_read = fread(&sb, 1, sizeof(sb), output);
    if (sb_bytes_read != sizeof(sb)) {
        printf("Error: Failed to read superblock (read %zu bytes, expected %zu)\n", sb_bytes_read, sizeof(sb));
        printf("File position: %ld\n", ftell(output));
        fclose(output);
        return 1;
    }
    
    printf("Read superblock: magic=0x%08X, size=%zu bytes\n", sb.magic, sizeof(sb));
    
    if (sb.magic != 0x4D565346) {
        printf("Error: Invalid filesystem magic number\n");
        fclose(output);
        return 1;
    }
    
    printf("Filesystem info:\n");
    printf("  Total blocks: %lu\n", (unsigned long)sb.total_blocks);
    printf("  Inodes: %lu\n", (unsigned long)sb.inode_count);
    printf("  Data region start: %lu\n", (unsigned long)sb.data_region_start);

    const char* base = strrchr(file_name, '/');
    const char* basename = base ? base + 1 : file_name;
    char name_on_disk[58];
    strncpy(name_on_disk, basename, 57);
    name_on_disk[57] = '\0';

    int exists = file_exists_in_root(output, &sb, name_on_disk, NULL);
    if (exists < 0) {
        printf("Error: Failed to read root directory to check duplicates\n");
        fclose(output);
        return 1;
    }
    if (exists == 1) {
        printf("Error: A file named '%s' already exists in the root directory. Aborting.\n", name_on_disk);
        fclose(output);
        return 1;
    }
    
    uint64_t free_inode = find_free_inode(output, &sb);
    if (free_inode == 0) {
        printf("Error: No free inodes available\n");
        fclose(output);
        return 1;
    }
    
    printf("Allocated inode: %lu\n", (unsigned long)free_inode);
    
    uint32_t data_blocks[DIRECT_MAX] = {0};
    for (uint64_t i = 0; i < blocks_needed; i++) {
        uint64_t block = find_free_data_block(output, &sb);
        if (block == 0) {
            printf("Error: No free data blocks available\n");
            fclose(output);
            return 1;
        }
        data_blocks[i] = (uint32_t)block;
        printf("Allocated data block: %lu\n", (unsigned long)block);
    }
    
    inode_t new_inode = {0};
    new_inode.mode = 0100000;  
    new_inode.links = 1;
    new_inode.uid = 0;
    new_inode.gid = 0;
    new_inode.size_bytes = (uint64_t)file_stat.st_size;
    new_inode.atime = time(NULL);
    new_inode.mtime = (uint64_t)file_stat.st_mtime;
    new_inode.ctime = time(NULL);
    
    for (int i = 0; i < DIRECT_MAX; i++) {
        new_inode.direct[i] = data_blocks[i];
    }
    
    new_inode.reserved_0 = 0;
    new_inode.reserved_1 = 0;
    new_inode.reserved_2 = 0;
    new_inode.proj_id = 13;  
    new_inode.uid16_gid16 = 0;
    new_inode.xattr_ptr = 0;
    
    inode_crc_finalize(&new_inode);
    
    fseek(output, (sb.inode_table_start * BS) + ((free_inode - 1) * INODE_SIZE), SEEK_SET);
    fwrite(&new_inode, sizeof(new_inode), 1, output);
    
    FILE* file_to_add = fopen(file_name, "rb");
    if (!file_to_add) {
        perror("Failed to open file to add");
        fclose(output);
        return 1;
    }
    
    for (uint64_t i = 0; i < blocks_needed; i++) {
        uint8_t file_block[BS] = {0};
        size_t r = fread(file_block, 1, BS, file_to_add);
        if (r == 0 && ferror(file_to_add)) {
            perror("Failed to read from file to add");
            fclose(file_to_add);
            fclose(output);
            return 1;
        }
        fseek(output, (long long)data_blocks[i] * BS, SEEK_SET);
        fwrite(file_block, 1, BS, output);
        
        printf("Written %zu bytes to block %u\n", r, data_blocks[i]);
    }
    fclose(file_to_add);
    
    if (add_to_root_directory(output, &sb, name_on_disk, free_inode) != 0) {
        printf("Error: Failed to add directory entry\n");
        fclose(output);
        return 1;
    }
    
    printf("Added directory entry: %s -> inode %lu\n", name_on_disk, (unsigned long)free_inode);

    fclose(output);
    printf("File '%s' successfully added to the filesystem image '%s'\n", name_on_disk, output_name);
    return 0;
}