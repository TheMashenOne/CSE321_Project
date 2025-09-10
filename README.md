# CSE321_Project

**Example:**
```bash
./mkfs_builder --image myfs.img --size-kib 256 --inodes 128
```

### Step 2: Add Files to Filesystem
```bash
./mkfs_adder --input <input_image> --output <output_image> --file <filename>
```

**Parameters:**
- `--input`: Input filesystem image
- `--output`: Output filesystem image (with added file)
- `--file`: File to add (must exist in current directory)

**Example:**
```bash
./mkfs_adder --input myfs.img --output myfs_with_file.img --file file_19.txt
```

## Complete Example

```bash
# 1. Compile the programs
gcc -O2 -std=c17 -Wall -Wextra Complete_mkfs_builder.c -o mkfs_builder
gcc -O2 -std=c17 -Wall -Wextra Complete_mkfs_adder.c -o mkfs_adder

# 2. Create a filesystem
./mkfs_builder --image test.img --size-kib 180 --inodes 128

# 3. Add files one by one
./mkfs_adder --input test.img --output test_with_file1.img --file file_19.txt
./mkfs_adder --input test_with_file1.img --output test_with_file2.img --file file_31.txt
./mkfs_adder --input test_with_file2.img --output test_final.img --file file_38.txt

# 4. Check created files
ls -la *.img
```
