/* Sumi OS - MakiFS ("scroll" file system): a small persistent file system of our own design.

   The disk carries a classic MBR partition table written by Sumi itself:
     sectors 2048..32767   "boot" partition (type 0xEF): empty until the installer copies a
                           FAT image with the boot loader and kernel there (see bootinst.c)
     sectors 32768..end    MakiFS partition (type 0xDA)

   MakiFS layout inside its partition (4 KiB blocks):
     block 0            superblock
     blocks 1..F        allocation table, one u32 per block: 0 free, next block of the
                        chain, 0xFFFFFFFF end of chain, 0xFFFFFFFE reserved
     block F+1          first block of the root directory
     everything after   data
   A directory is a chain of blocks holding 64-byte entries; a file is a chain of data
   blocks. All updates are written through to the disk before the call returns (new data
   first, then the table, then the directory entry), so a saved file survives power loss. */
#include "kernel.h"

#define BLK 4096
#define SPB (BLK / 512)
#define FAT_FREE 0u
#define FAT_EOC  0xFFFFFFFFu
#define FAT_SYS  0xFFFFFFFEu
#define EPB (BLK / 64)
#define FPB (BLK / 4)

typedef struct Super {
    char magic[8]; u32 version, total_blocks, fat_start, fat_blocks, root_block;
    char label[16];
} Super;
typedef struct DirEnt {
    char name[FS_NAME_MAX + 1]; u8 type; u8 pad[3]; u32 size, first, mtime, ctime;
} DirEnt;
_Static_assert(sizeof(DirEnt) == 64, "dirent must be 64 bytes");

static Mutex fs_lock = { .name = "fs" };
static bool mounted, formatted_now;
static Super sb;
static u32 *fat;
static u8 *fat_dirty;
static u32 alloc_rover;
static u8 blk[BLK], dblk[BLK];

static u32 fs_base;              /* first sector of the MakiFS partition */
static const char *fs_state = "no disk";

static int bread(u32 b, void *buf) { return ata_read(fs_base + b * SPB, SPB, buf) ? FS_EIO : 0; }
static int bwrite(u32 b, const void *buf) { return ata_write(fs_base + b * SPB, SPB, buf) ? FS_EIO : 0; }
const char *fs_status(void) { return fs_state; }

const char *fs_strerror(int e) {
    switch (e) {
    case FS_OK: return "ok"; case FS_ENOENT: return "no such file or directory";
    case FS_EEXIST: return "already exists"; case FS_ENOSPC: return "no space left on disk";
    case FS_EIO: return "disk I/O error"; case FS_ENOTDIR: return "not a directory";
    case FS_EISDIR: return "is a directory"; case FS_ENOTEMPTY: return "directory not empty";
    case FS_EINVAL: return "invalid name"; case FS_ENODISK: return "no disk";
    case FS_ENOMEM: return "out of memory";
    }
    return "unknown error";
}
bool fs_ready(void) { return mounted; }
bool fs_was_formatted(void) { return formatted_now; }

static void fat_set(u32 b, u32 v) { fat[b] = v; fat_dirty[b / FPB] = 1; }
static int fat_flush(void) {
    int rc = 0;
    for (u32 i = 0; i < sb.fat_blocks; i++) if (fat_dirty[i]) {
        if (bwrite(sb.fat_start + i, (u8 *)fat + i * BLK)) rc = FS_EIO; else fat_dirty[i] = 0;
    }
    ata_flush();
    return rc;
}
static u32 free_blocks(void) {
    u32 n = 0;
    for (u32 i = 0; i < sb.total_blocks; i++) if (fat[i] == FAT_FREE) n++;
    return n;
}
static u32 alloc_block(void) {
    for (u32 k = 0; k < sb.total_blocks; k++) {
        u32 b = alloc_rover + k;
        if (b >= sb.total_blocks) b -= sb.total_blocks;
        if (fat[b] == FAT_FREE) { fat_set(b, FAT_EOC); alloc_rover = b + 1; return b; }
    }
    return 0;
}
static void free_chain(u32 b) {
    u32 guard = 0;
    while (b && b < sb.total_blocks && fat[b] != FAT_FREE && fat[b] != FAT_SYS && guard++ < sb.total_blocks) {
        u32 n = fat[b];
        fat_set(b, FAT_FREE);
        if (n == FAT_EOC) break;
        b = n;
    }
}
static bool valid_block(u32 b) { return b > sb.root_block - 1 && b < sb.total_blocks; }

/* --- directories --- */
static int dir_find(u32 dir, const char *name, DirEnt *out, u32 *oblk, int *oidx) {
    u32 guard = 0;
    for (u32 b = dir; valid_block(b) && guard++ < sb.total_blocks; b = fat[b]) {
        if (bread(b, dblk)) return FS_EIO;
        DirEnt *e = (DirEnt *)dblk;
        for (int i = 0; i < EPB; i++) if (e[i].type != FS_FREE && !strcmp(e[i].name, name)) {
            if (out) *out = e[i];
            if (oblk) *oblk = b;
            if (oidx) *oidx = i;
            return 0;
        }
        if (fat[b] == FAT_EOC) break;
    }
    return FS_ENOENT;
}
static int dir_set(u32 b, int idx, const DirEnt *e) {
    if (bread(b, dblk)) return FS_EIO;
    if (e) ((DirEnt *)dblk)[idx] = *e; else memset(&((DirEnt *)dblk)[idx], 0, sizeof(DirEnt));
    return bwrite(b, dblk);
}
static int dir_add(u32 dir, const DirEnt *ne) {
    u32 last = dir, guard = 0;
    for (u32 b = dir; valid_block(b) && guard++ < sb.total_blocks; b = fat[b]) {
        last = b;
        if (bread(b, dblk)) return FS_EIO;
        DirEnt *e = (DirEnt *)dblk;
        for (int i = 0; i < EPB; i++) if (e[i].type == FS_FREE) { e[i] = *ne; return bwrite(b, dblk); }
        if (fat[b] == FAT_EOC) break;
    }
    u32 nb = alloc_block();
    if (!nb) return FS_ENOSPC;
    memset(dblk, 0, BLK);
    ((DirEnt *)dblk)[0] = *ne;
    if (bwrite(nb, dblk)) { fat_set(nb, FAT_FREE); return FS_EIO; }
    fat_set(last, nb);
    return 0;
}
static bool name_ok(const char *n) {
    size_t l = strlen(n);
    if (l == 0 || l > FS_NAME_MAX || !strcmp(n, ".") || !strcmp(n, "..")) return false;
    for (; *n; n++) if (*n == '/' || (u8)*n < 32) return false;
    return true;
}

/* Walk an absolute, normalised path. On success *ent describes the object; for the root
   a synthetic entry is returned and *eblk is 0. */
static int lookup(const char *path, DirEnt *ent, u32 *eblk, int *eidx) {
    DirEnt cur; memset(&cur, 0, sizeof cur);
    cur.type = FS_DIR; cur.first = sb.root_block;
    u32 cb = 0; int ci = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        char name[FS_NAME_MAX + 1]; int n = 0;
        while (*p && *p != '/') { if (n >= FS_NAME_MAX) return FS_ENOENT; name[n++] = *p++; }
        name[n] = 0;
        while (*p == '/') p++;
        if (cur.type != FS_DIR) return FS_ENOTDIR;
        int rc = dir_find(cur.first, name, &cur, &cb, &ci);
        if (rc) return rc;
    }
    if (ent) *ent = cur;
    if (eblk) *eblk = cb;
    if (eidx) *eidx = ci;
    return 0;
}
static int lookup_parent(const char *path, DirEnt *parent, char *name) {
    char dir[FS_PATH_MAX];
    path_dirname(path, dir);
    const char *base = path_basename(path);
    if (!name_ok(base)) return FS_EINVAL;
    strlcpy(name, base, FS_NAME_MAX + 1);
    int rc = lookup(dir, parent, NULL, NULL);
    if (rc) return rc;
    if (parent->type != FS_DIR) return FS_ENOTDIR;
    return 0;
}

/* --- public API --- */
static int do_read(const char *path, void **data, u32 *size) {
    DirEnt e;
    int rc = lookup(path, &e, NULL, NULL);
    if (rc) return rc;
    if (e.type != FS_FILE) return FS_EISDIR;
    u8 *buf = kmalloc(e.size + 1);
    if (!buf) return FS_ENOMEM;
    u32 done = 0, b = e.first, guard = 0;
    while (done < e.size) {
        if (!valid_block(b) || guard++ > sb.total_blocks) { kfree(buf); return FS_EIO; }
        u32 n = e.size - done;
        if (n >= BLK) { if (bread(b, buf + done)) { kfree(buf); return FS_EIO; } n = BLK; }
        else { if (bread(b, blk)) { kfree(buf); return FS_EIO; } memcpy(buf + done, blk, n); }
        done += n;
        b = fat[b];
    }
    buf[e.size] = 0;
    *data = buf;
    if (size) *size = e.size;
    return 0;
}

static int write_chain(const void *data, u32 size, u32 *first) {
    *first = 0;
    u32 need = (size + BLK - 1) / BLK;
    if (need > free_blocks()) return FS_ENOSPC;
    u32 prev = 0;
    const u8 *p = data;
    for (u32 i = 0; i < need; i++) {
        u32 b = alloc_block();
        if (!b) { free_chain(*first); return FS_ENOSPC; }
        if (prev) fat_set(prev, b); else *first = b;
        prev = b;
        u32 n = size - i * BLK;
        int rc;
        if (n >= BLK) rc = bwrite(b, p + i * BLK);
        else { memset(blk, 0, BLK); memcpy(blk, p + i * BLK, n); rc = bwrite(b, blk); }
        if (rc) { free_chain(*first); *first = 0; return FS_EIO; }
    }
    return 0;
}

static int do_write(const char *path, const void *data, u32 size) {
    DirEnt parent, e; char name[FS_NAME_MAX + 1];
    u32 eb; int ei;
    int rc = lookup_parent(path, &parent, name);
    if (rc) return rc;
    rc = dir_find(parent.first, name, &e, &eb, &ei);
    if (rc && rc != FS_ENOENT) return rc;
    bool exists = rc == 0;
    if (exists && e.type != FS_FILE) return FS_EISDIR;
    u32 first;
    rc = write_chain(data, size, &first);
    if (rc) { fat_flush(); return rc; }
    u32 now = time_now();
    if (exists) {
        u32 old = e.first;
        e.first = first; e.size = size; e.mtime = now;
        fat_flush();
        rc = dir_set(eb, ei, &e);
        if (rc) { free_chain(first); fat_flush(); return rc; }
        free_chain(old);
    } else {
        memset(&e, 0, sizeof e);
        strlcpy(e.name, name, sizeof e.name);
        e.type = FS_FILE; e.first = first; e.size = size; e.mtime = e.ctime = now;
        rc = dir_add(parent.first, &e);
        if (rc) { free_chain(first); fat_flush(); return rc; }
    }
    return fat_flush();
}

static int do_mkdir(const char *path) {
    DirEnt parent, e; char name[FS_NAME_MAX + 1];
    int rc = lookup_parent(path, &parent, name);
    if (rc) return rc;
    if (dir_find(parent.first, name, NULL, NULL, NULL) == 0) return FS_EEXIST;
    u32 b = alloc_block();
    if (!b) return FS_ENOSPC;
    memset(blk, 0, BLK);
    if (bwrite(b, blk)) { fat_set(b, FAT_FREE); return FS_EIO; }
    memset(&e, 0, sizeof e);
    strlcpy(e.name, name, sizeof e.name);
    e.type = FS_DIR; e.first = b; e.mtime = e.ctime = time_now();
    rc = dir_add(parent.first, &e);
    if (rc) fat_set(b, FAT_FREE);
    int rc2 = fat_flush();
    return rc ? rc : rc2;
}

static int do_remove(const char *path) {
    DirEnt e; u32 eb; int ei;
    int rc = lookup(path, &e, &eb, &ei);
    if (rc) return rc;
    if (eb == 0) return FS_EINVAL;                         /* the root */
    if (e.type == FS_DIR) {
        u32 guard = 0;
        for (u32 b = e.first; valid_block(b) && guard++ < sb.total_blocks; b = fat[b]) {
            if (bread(b, dblk)) return FS_EIO;
            for (int i = 0; i < EPB; i++) if (((DirEnt *)dblk)[i].type != FS_FREE) return FS_ENOTEMPTY;
            if (fat[b] == FAT_EOC) break;
        }
    }
    rc = dir_set(eb, ei, NULL);
    if (rc) return rc;
    free_chain(e.first);
    return fat_flush();
}

static int do_rename(const char *from, const char *to) {
    DirEnt e, parent; u32 eb; int ei; char name[FS_NAME_MAX + 1];
    int rc = lookup(from, &e, &eb, &ei);
    if (rc) return rc;
    if (eb == 0) return FS_EINVAL;
    size_t fl = strlen(from);
    if (!strncmp(from, to, fl) && (to[fl] == '/' || to[fl] == 0)) return to[fl] ? FS_EINVAL : 0;
    rc = lookup_parent(to, &parent, name);
    if (rc) return rc;
    if (dir_find(parent.first, name, NULL, NULL, NULL) == 0) return FS_EEXIST;
    DirEnt ne = e;
    memset(ne.name, 0, sizeof ne.name);
    strlcpy(ne.name, name, sizeof ne.name);
    rc = dir_add(parent.first, &ne);
    if (rc) { fat_flush(); return rc; }
    fat_flush();
    rc = dir_set(eb, ei, NULL);
    ata_flush();
    return rc;
}

static int do_list(const char *path, FsEntry **ents, int *count) {
    DirEnt d;
    int rc = lookup(path, &d, NULL, NULL);
    if (rc) return rc;
    if (d.type != FS_DIR) return FS_ENOTDIR;
    int cap = 32, n = 0;
    FsEntry *arr = kmalloc(cap * sizeof(FsEntry));
    if (!arr) return FS_ENOMEM;
    u32 guard = 0;
    for (u32 b = d.first; valid_block(b) && guard++ < sb.total_blocks; b = fat[b]) {
        if (bread(b, dblk)) { kfree(arr); return FS_EIO; }
        DirEnt *e = (DirEnt *)dblk;
        for (int i = 0; i < EPB; i++) {
            if (e[i].type == FS_FREE) continue;
            if (n == cap) {
                FsEntry *na = kmalloc(cap * 2 * sizeof(FsEntry));
                if (!na) { kfree(arr); return FS_ENOMEM; }
                memcpy(na, arr, n * sizeof(FsEntry)); kfree(arr); arr = na; cap *= 2;
            }
            memcpy(arr[n].name, e[i].name, FS_NAME_MAX + 1); arr[n].name[FS_NAME_MAX] = 0;
            arr[n].type = e[i].type; arr[n].size = e[i].size; arr[n].mtime = e[i].mtime;
            n++;
        }
        if (fat[b] == FAT_EOC) break;
    }
    for (int i = 1; i < n; i++) {                          /* insertion sort: directories first, then by name */
        FsEntry t = arr[i]; int j = i - 1;
        while (j >= 0 && (arr[j].type < t.type || (arr[j].type == t.type && strcasecmp(arr[j].name, t.name) > 0))) {
            arr[j + 1] = arr[j]; j--;
        }
        arr[j + 1] = t;
    }
    *ents = arr; *count = n;
    return 0;
}

#define FS_CALL(expr) do { if (!mounted) return FS_ENODISK; mutex_lock(&fs_lock); int rc_ = (expr); mutex_unlock(&fs_lock); return rc_; } while (0)
int fs_read(const char *path, void **data, u32 *size) { FS_CALL(do_read(path, data, size)); }
int fs_write(const char *path, const void *data, u32 size) { FS_CALL(do_write(path, data, size)); }
int fs_mkdir(const char *path) { FS_CALL(do_mkdir(path)); }
int fs_remove(const char *path) { FS_CALL(do_remove(path)); }
int fs_rename(const char *from, const char *to) { FS_CALL(do_rename(from, to)); }
int fs_list(const char *path, FsEntry **ents, int *count) { FS_CALL(do_list(path, ents, count)); }
int fs_append(const char *path, const void *data, u32 size) {
    void *old = NULL; u32 osz = 0;
    int rc = fs_read(path, &old, &osz);
    if (rc == FS_ENOENT) return fs_write(path, data, size);
    if (rc) return rc;
    u8 *nb = kmalloc(osz + size + 1);
    if (!nb) { kfree(old); return FS_ENOMEM; }
    memcpy(nb, old, osz); memcpy(nb + osz, data, size);
    kfree(old);
    rc = fs_write(path, nb, osz + size);
    kfree(nb);
    return rc;
}
int fs_stat(const char *path, FsEntry *st) {
    if (!mounted) return FS_ENODISK;
    mutex_lock(&fs_lock);
    DirEnt e;
    int rc = lookup(path, &e, NULL, NULL);
    if (rc == 0 && st) {
        strlcpy(st->name, path_basename(path), sizeof st->name);
        st->type = e.type; st->size = e.size; st->mtime = e.mtime;
    }
    mutex_unlock(&fs_lock);
    return rc;
}
void fs_usage(u32 *total, u32 *freeb) {
    if (!mounted) { if (total) *total = 0; if (freeb) *freeb = 0; return; }
    mutex_lock(&fs_lock);
    if (total) *total = sb.total_blocks * (BLK / 1024);
    if (freeb) *freeb = free_blocks() * (BLK / 1024);
    mutex_unlock(&fs_lock);
}

/* Write the partition table. Only sector 0 is touched. */
static int write_mbr(u32 disk_sectors) {
    static u8 mbr[512];
    memset(mbr, 0, sizeof mbr);
    static const char note[] = "Sumi OS disk. Boot it with UEFI firmware (after running the installer).";
    memcpy(mbr + 8, note, sizeof note);
    for (int i = 0; i < 2; i++) {
        u8 *e = mbr + 446 + i * 16;
        u32 start = i == 0 ? FS_BOOT_LBA : FS_DATA_LBA;
        u32 count = i == 0 ? FS_BOOT_SECTORS : disk_sectors - FS_DATA_LBA;
        e[0] = 0; e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;          /* CHS fields: "use LBA" */
        e[4] = i == 0 ? 0xEF : 0xDA;
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        memcpy(e + 8, &start, 4); memcpy(e + 12, &count, 4);
    }
    mbr[510] = 0x55; mbr[511] = 0xAA;
    return ata_write(0, 1, mbr) ? FS_EIO : 0;
}
static bool mbr_is_ours(const u8 *mbr) {
    u32 start; memcpy(&start, mbr + 446 + 16 + 8, 4);
    return mbr[510] == 0x55 && mbr[511] == 0xAA && mbr[446 + 4] == 0xEF && mbr[446 + 16 + 4] == 0xDA && start == FS_DATA_LBA;
}

int fs_format(void) {
    u32 secs = ata_sectors();
    if (secs < FS_DATA_LBA + 2048) return FS_ENODISK;
    mutex_lock(&fs_lock);
    mounted = false;
    if (write_mbr(secs)) { mutex_unlock(&fs_lock); return FS_EIO; }
    fs_base = FS_DATA_LBA; secs -= FS_DATA_LBA;
    memset(&sb, 0, sizeof sb);
    memcpy(sb.magic, "MAKIFS01", 8);
    sb.version = 1;
    sb.total_blocks = secs / SPB;
    if (sb.total_blocks > 1u << 20) sb.total_blocks = 1u << 20;      /* cap at 4 GiB */
    sb.fat_start = 1;
    sb.fat_blocks = (sb.total_blocks + FPB - 1) / FPB;
    sb.root_block = sb.fat_start + sb.fat_blocks;
    strcpy(sb.label, "sumi");
    if (fat) { kfree(fat); kfree(fat_dirty); }
    fat = kmalloc_k(sb.fat_blocks * BLK);
    fat_dirty = kmalloc_k(sb.fat_blocks);
    int rc = FS_ENOMEM;
    if (fat && fat_dirty) {
        memset(fat, 0, sb.fat_blocks * BLK);
        for (u32 i = 0; i < sb.root_block; i++) fat[i] = FAT_SYS;
        for (u32 i = sb.total_blocks; i < sb.fat_blocks * FPB; i++) fat[i] = FAT_SYS;
        fat[sb.root_block] = FAT_EOC;
        memset(fat_dirty, 1, sb.fat_blocks);
        memset(blk, 0, BLK);
        rc = bwrite(sb.root_block, blk);
        if (!rc) rc = fat_flush();
        memcpy(blk, &sb, sizeof sb);
        if (!rc) rc = bwrite(0, blk);
        ata_flush();
    }
    alloc_rover = sb.root_block + 1;
    mounted = rc == 0;
    fs_state = mounted ? "mounted" : "format failed";
    mutex_unlock(&fs_lock);
    return rc;
}

int fs_init(void) {
    mounted = false; formatted_now = false;
    if (!ata_sectors()) return FS_ENODISK;
    if (ata_read(0, 1, blk)) { fs_state = "disk read error"; return FS_EIO; }
    bool blank = true;
    for (int i = 0; i < 512; i++) if (blk[i]) { blank = false; break; }
    bool ours = mbr_is_ours(blk);
    if (!blank && !ours) {
        /* never erase a disk that holds something we do not understand */
        klog("fs: the disk is neither blank nor a Sumi disk - leaving it untouched (use 'format' in the Terminal to erase it)\n");
        fs_state = "foreign disk (not mounted)";
        return FS_ENODISK;
    }
    fs_base = FS_DATA_LBA;
    if (ours) {
        if (bread(0, blk)) { fs_state = "disk read error"; return FS_EIO; }
        memcpy(&sb, blk, sizeof sb);
        ours = memcmp(sb.magic, "MAKIFS01", 8) == 0 && sb.total_blocks <= (ata_sectors() - fs_base) / SPB && sb.fat_blocks != 0
               && sb.fat_start == 1 && sb.root_block == 1 + sb.fat_blocks && sb.fat_blocks >= (sb.total_blocks + FPB - 1) / FPB;
    }
    if (!ours) {
        klog("fs: blank disk, creating the partition table and a MakiFS file system\n");
        int rc = fs_format();
        if (rc) return rc;
        formatted_now = true;
        return 0;
    }
    fat = kmalloc_k(sb.fat_blocks * BLK);
    fat_dirty = kmalloc_k(sb.fat_blocks);
    if (!fat || !fat_dirty) return FS_ENOMEM;
    memset(fat_dirty, 0, sb.fat_blocks);
    for (u32 i = 0; i < sb.fat_blocks; i++) if (bread(sb.fat_start + i, (u8 *)fat + i * BLK)) { fs_state = "disk read error"; return FS_EIO; }
    alloc_rover = sb.root_block + 1;
    mounted = true; fs_state = "mounted";
    klog("fs: mounted MakiFS, %u blocks, %u free\n", sb.total_blocks, free_blocks());
    return 0;
}

/* --- path helpers --- */
void path_resolve(const char *cwd, const char *in, char *out) {
    char tmp[FS_PATH_MAX * 2];
    if (in[0] == '/') strlcpy(tmp, in, sizeof tmp);
    else { strlcpy(tmp, cwd && *cwd ? cwd : "/", sizeof tmp); strlcat(tmp, "/", sizeof tmp); strlcat(tmp, in, sizeof tmp); }
    char res[FS_PATH_MAX]; int len = 0;
    res[0] = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        int n = p - s;
        if (n == 1 && s[0] == '.') continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (len > 0 && res[len - 1] != '/') len--;
            if (len > 0) len--;
            res[len] = 0;
            continue;
        }
        if (len + n + 2 >= FS_PATH_MAX) break;
        res[len++] = '/';
        memcpy(res + len, s, n); len += n; res[len] = 0;
    }
    if (len == 0) strcpy(res, "/");
    strlcpy(out, res, FS_PATH_MAX);
}
const char *path_basename(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
void path_dirname(const char *p, char *out) {
    strlcpy(out, p, FS_PATH_MAX);
    char *s = strrchr(out, '/');
    if (!s || s == out) strcpy(out, "/"); else *s = 0;
}
