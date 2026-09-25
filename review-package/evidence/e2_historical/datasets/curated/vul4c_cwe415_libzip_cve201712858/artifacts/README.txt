Affected version:
1.2.0

Source code:
git clone https://github.com/nih-at/libzip.git

Commit hash:
a23ac8a766c556827255111eb35ba928641efbc8

Command:
> cd /path/to/compile/source
> ./src/ziptool $FILE cat index

Expected Output:
=================================================================
==400695==ERROR: AddressSanitizer: heap-use-after-free on address 0x6030000000d1 at pc 0x55555544bc4a bp 0x7fffffffda90 sp 0x7fffffffda80
READ of size 1 at 0x6030000000d1 thread T0
    #0 0x55555544bc49 in _zip_buffer_free /anonymous/home/benchmark-setup/libzip/CVE-2017-12858/source/lib/zip_buffer.c:53
    #1 0x555555417425 in _zip_dirent_read /anonymous/home/benchmark-setup/libzip/CVE-2017-12858/source/lib/zip_dirent.c:582
    #2 0x55555542486b in _zip_read_cdir /anonymous/home/benchmark-setup/libzip/CVE-2017-12858/source/lib/zip_open.c:380
    #3 0x55555542604d in _zip_find_central_dir /anonymous/home/benchmark-setup/libzip/CVE-2017-12858/source/lib/zip_open.c:613
    #4 0x5555554239a1 in _zip_open /anonymous/home/benchmark-setup/libzip/CVE-2017-12858/source/lib/zip_open.c:200
    #5 0x55555542371c in zip_open_from_source /anonymous/home/benchmark-setup/libzip/CVE-2017-12858/source/lib/zip_open.c:148
    #6 0x55555542340e in zip_open /anonymous/home/benchmark-setup/libzip/CVE-2017-12858/source/lib/zip_open.c:74
    #7 0x55555540eecb in read_from_file /anonymous/home/benchmark-setup/libzip/CVE-2017-12858/source/src/ziptool.c:698
    #8 0x555555410c5f in main /anonymous/home/benchmark-setup/libzip/CVE-2017-12858/source/src/ziptool.c:1113
    #9 0x7ffff682bc86 in __libc_start_main (/lib/x86_64-linux-gnu/libc.so.6+0x21c86)
    #10 0x555555408449 in _start (/anonymous/home/benchmark-setup/libzip/CVE-2017-12858/source/src/ziptool+0x8449)

