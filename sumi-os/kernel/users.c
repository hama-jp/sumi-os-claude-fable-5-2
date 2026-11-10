/* Sumi OS - user accounts: /etc/passwd with salted SHA-256 password hashes */
#include "kernel.h"

char session_user[32];

/* ---- SHA-256 (FIPS 180-4), written for this project ---- */
static const u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(u32 h[8], const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = (p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        u32 S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + S1 + ch + K[i] + w[i];
        u32 S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
void sha256(const void *data, size_t len, u8 out[32]) {
    u32 h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const u8 *p = data; size_t n = len;
    while (n >= 64) { sha_block(h, p); p += 64; n -= 64; }
    u8 tail[128]; memset(tail, 0, sizeof tail);
    memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t tl = n < 56 ? 64 : 128;
    u32 bits_lo = (u32)len << 3, bits_hi = (u32)len >> 29;
    for (int i = 0; i < 4; i++) { tail[tl - 1 - i] = bits_lo >> (i * 8); tail[tl - 5 - i] = bits_hi >> (i * 8); }
    sha_block(h, tail);
    if (tl == 128) sha_block(h, tail + 64);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = h[i] >> (24 - j * 8);
}

static void hash_password(const char *salt, const char *pass, char hex[65]) {
    char buf[160]; u8 d[32];
    int n = snprintf(buf, sizeof buf, "%s:%s", salt, pass);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    sha256(buf, n, d);
    for (int r = 0; r < 1000; r++) sha256(d, 32, d);       /* a little key stretching */
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
}

#define PASSWD "/etc/passwd"

/* find "name:salt:hash" line; returns pointer into buf or NULL */
static char *find_user(char *buf, const char *name, char **line_end) {
    size_t nl = strlen(name);
    for (char *p = buf; p && *p;) {
        char *e = strchr(p, '\n');
        if (!strncmp(p, name, nl) && p[nl] == ':') { if (line_end) *line_end = e; return p; }
        p = e ? e + 1 : NULL;
    }
    return NULL;
}
static bool valid_name(const char *n) {
    size_t l = strlen(n);
    if (l < 1 || l > 24) return false;
    for (; *n; n++) if (!isalnum(*n) && *n != '_' && *n != '-') return false;
    return true;
}

int user_check(const char *name, const char *pass) {
    if (!valid_name(name)) return 0;
    void *data; u32 size;
    if (fs_read(PASSWD, &data, &size) != 0) {
        /* no disk: built-in guest account so the system stays usable */
        return !strcmp(name, "guest") && !strcmp(pass, "guest");
    }
    int ok = 0;
    char *line = find_user(data, name, NULL);
    if (line) {
        char salt[32], hash[80], hex[65];
        char *s = line + strlen(name) + 1; int i = 0;
        while (*s && *s != ':' && *s != '\n' && i < 31) salt[i++] = *s++;
        salt[i] = 0;
        if (*s == ':') s++;
        i = 0;
        while (*s && *s != ':' && *s != '\n' && i < 79) hash[i++] = *s++;
        hash[i] = 0;
        hash_password(salt, pass, hex);
        ok = !strcmp(hex, hash);
    }
    kfree(data);
    return ok;
}

static int write_user(const char *name, const char *pass, bool must_exist) {
    if (!valid_name(name) || strlen(pass) < 1 || strlen(pass) > 64) return FS_EINVAL;
    void *data = NULL; u32 size = 0;
    int rc = fs_read(PASSWD, &data, &size);
    if (rc && rc != FS_ENOENT) return rc;
    char salt[20], hex[65], line[160];
    snprintf(salt, sizeof salt, "%08x%08x", rand32() ^ ticks, rand32() ^ time_now());
    hash_password(salt, pass, hex);
    int ll = snprintf(line, sizeof line, "%s:%s:%s\n", name, salt, hex);
    char *out = kmalloc(size + ll + 1);
    if (!out) { kfree(data); return FS_ENOMEM; }
    u32 on = 0;
    char *end = NULL, *old = data ? find_user(data, name, &end) : NULL;
    if (!old && must_exist) { kfree(data); kfree(out); return FS_ENOENT; }
    if (old && !must_exist) { kfree(data); kfree(out); return FS_EEXIST; }
    if (old) {
        memcpy(out, data, old - (char *)data); on = old - (char *)data;
        memcpy(out + on, line, ll); on += ll;
        if (end) { u32 rest = size - (end + 1 - (char *)data); memcpy(out + on, end + 1, rest); on += rest; }
    } else {
        if (data) { memcpy(out, data, size); on = size; }
        memcpy(out + on, line, ll); on += ll;
    }
    rc = fs_write(PASSWD, out, on);
    kfree(data); kfree(out);
    return rc;
}
int user_set_password(const char *name, const char *pass) { return write_user(name, pass, true); }
int user_add(const char *name, const char *pass) {
    int rc = write_user(name, pass, false);
    if (rc == 0) {
        char home[64]; snprintf(home, sizeof home, "/home/%s", name);
        fs_mkdir(home);
    }
    return rc;
}

static void put_file(const char *path, const char *text) { fs_write(path, text, strlen(text)); }

/* Called once at boot: populate a freshly formatted disk with the default tree. */
void users_init(void) {
    srand32(time_now() * 2654435761u + ticks);
    if (!fs_ready()) return;
    FsEntry st;
    if (fs_stat("/etc", &st) != 0) fs_mkdir("/etc");
    if (fs_stat("/home", &st) != 0) fs_mkdir("/home");
    if (fs_stat(PASSWD, &st) != 0) {
        user_add("guest", "guest");
        user_add("sumi", "ink");
        put_file("/etc/motd",
                 "Welcome to Sumi. Ink once ground does not return to the stone;\n"
                 "files once saved do not leave the disk. Type 'help' for commands.\n");
        put_file("/home/guest/welcome.txt",
                 "Welcome to Sumi OS\n"
                 "==================\n\n"
                 "This file lives on the MakiFS disk. Edit it, press Ctrl+S, reboot,\n"
                 "and it will still be here.\n\n"
                 "Things to try:\n"
                 "  - Terminal: type 'help', 'ls -l', 'cat welcome.txt', 'sysinfo'\n"
                 "  - Files: double-click a text file to open it in the editor\n"
                 "  - Ink: paint with a brush that thins as it moves faster\n"
                 "  - Gomoku: five stones in a row wins\n\n"
                 "Shortcuts: Alt+Tab switch window, Alt+F4 close window.\n");
        put_file("/home/guest/haiku.txt",
                 "Ink on the grindstone\na kernel wakes in silence\nthe cursor blinks once\n");
        fs_mkdir("/home/guest/notes");
        put_file("/home/guest/notes/todo.txt", "- grind ink\n- wet brush\n- write kernel\n");
    }
}
