/*
 * dc_image.c  The disc's files, unpacked once from an image of the disc into the disc's folder: a CHD, a .gdi
 * with its tracks, a .cue with its .bins (Redump's), or an .iso of track 3
 *
 * The files are those of track 3, the GD-ROM's high-density area, which starts at sector 45000 and holds an
 * ISO 9660 file system whose sectors count from the disc's start.
 */

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0777)
#endif

#include "../ext/libchdr/include/libchdr/chd.h"
#include "../ext/libchdr/include/libchdr/cdrom.h"

#include "naomi_host.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifdef _WIN32
#define lseek64_ _lseeki64
#else
#include <unistd.h>
#define lseek64_(fd, at, how) lseek((fd), (off_t)(at), (how))
#endif

#define HD_FIRST 45000          /* track 3's first sector */
#define SECTOR 2048
#define CHUNK 512               /* sectors a read: 1 MB of files, a few calls (the Vita's card pays for each) */

typedef struct {
    int fd;                     /* a file track's (-1 none): read with read(), each read at its place by lseek() first */
    long long at;               /* the file offset of the track's first sector */
    chd_file* chd;
    uint32_t frame0;            /* the CHD frame of the track's first sector */
    uint32_t per_hunk;          /* frames a hunk */
    uint8_t* hunk;
    uint32_t hunk_no;           /* the one in hunk, or ~0 */
    int size;                   /* bytes a sector in the image */
    int data;                   /* where in a sector its 2048 bytes start */
    uint8_t* raw;               /* CHUNK sectors of the image */
} TRACK;

static char why[300];

/* microseconds, for the unpacking's account of where its time went */
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
static uint64_t now_us(void) {
    return sceKernelGetProcessTimeWide();
}
#else
static uint64_t now_us(void) {
    return (uint64_t)clock() * 1000000u / CLOCKS_PER_SEC;
}
#endif
static uint64_t us_read, us_open, us_write, us_close;

static const char* fail(const char* fmt, const char* what) {
    snprintf(why, sizeof why, fmt, what);
    host_log("disc image: %s", why);
    return why;
}

static int ends_with(const char* name, const char* end) {
    size_t n = strlen(name), e = strlen(end);
    size_t i;
    if (n < e) {
        return 0;
    }
    for (i = 0; i < e; i++) {
        if (tolower((unsigned char)name[n - e + i]) != end[i]) {
            return 0;
        }
    }
    return 1;
}

/* The disc folder's layout: the files of the disc's root in 32 subfolders by a hash of their name (disc/N/SELTEX.BIN),
   a few dozen a folder, the disc's own folders as they are (disc/DPTEX/...). Making a file in one folder of 720
   cost the Vita's card about 120 ms, the folder's entries looked through each time; a folder of all of them unpacked
   before (flat) is still read */
static char bucket_of(const char* name) {
    uint32_t h = 2166136261u;   /* FNV-1a of the name, upper case: 32 folders, about 24 files each (by first letter
                                   one had 400: STG..., SE...) */
    for (; *name != 0; name++) {
        h = (h ^ (uint32_t)toupper((unsigned char)*name)) * 16777619u;
    }
    return "0123456789ABCDEFGHIJKLMNOPQRSTUV"[h % 32];
}

void dc_disc_path(char* out, size_t n, const char* dir, const char* name, int buckets) {
    if (buckets && strchr(name, '/') == NULL) {
        snprintf(out, n, "%s/%c/%s", dir, bucket_of(name), name);
    } else {
        snprintf(out, n, "%s/%s", dir, name);
    }
}

/* 1 when dir is laid out in subfolders (its program in its own one) */
int dc_disc_buckets(const char* dir) {
    char path[1400];
    struct stat st;
    dc_disc_path(path, sizeof path, dir, "1ST_READ.BIN", 1);
    return stat(path, &st) == 0;
}

/* n bytes read whole (a read may give fewer: on the Vita a call reads up to 1 MB of the card at a time), or 0 */
static int read_all(int fd, void* buf, size_t n) {
    uint8_t* p = buf;
    while (n > 0) {
        long k = (long)read(fd, p, (unsigned)n);
        if (k <= 0) {
            return 0;
        }
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

static int write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = buf;
    while (n > 0) {
        long k = (long)write(fd, p, (unsigned)n);
        if (k <= 0) {
            return 0;
        }
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

/* n sectors of track 3 from sector lba on, their 2048 bytes each */
static int read_sectors(TRACK* t, uint32_t lba, uint32_t n, uint8_t* out) {
    uint32_t i;
    if (lba < HD_FIRST) {
        return 0;
    }
    if (t->chd != NULL) {
        for (i = 0; i < n; i++) {
            uint32_t frame = t->frame0 + lba - HD_FIRST + i, h = frame / t->per_hunk;
            if (h != t->hunk_no) {
                if (chd_read(t->chd, h, t->hunk) != CHDERR_NONE) {
                    return 0;
                }
                t->hunk_no = h;
            }
            memcpy(out + i * SECTOR, t->hunk + (frame % t->per_hunk) * CD_FRAME_SIZE + t->data, SECTOR);
        }
        return 1;
    }
    if (t->size == SECTOR) {
        long long at = t->at + (long long)(lba - HD_FIRST) * SECTOR;
        return lseek64_(t->fd, at, SEEK_SET) >= 0 && read_all(t->fd, out, (size_t)n * SECTOR);
    }
    while (n > 0) {
        uint32_t k = n < CHUNK ? n : CHUNK;
        long long at = t->at + (long long)(lba - HD_FIRST) * t->size;
        /* sought each time: on the Vita an unbuffered read did not leave the file where it ended (a seek skipped
           for the next read there read other files' bytes into these, the sizes all right) */
        if (lseek64_(t->fd, at, SEEK_SET) < 0 || !read_all(t->fd, t->raw, (size_t)k * t->size)) {
            return 0;
        }
        for (i = 0; i < k; i++) {
            memcpy(out + i * SECTOR, t->raw + i * t->size + t->data, SECTOR);
        }
        out += k * SECTOR;
        lba += k;
        n -= k;
    }
    return 1;
}

static int has_volume(TRACK* t) {
    uint8_t s[SECTOR];
    t->hunk_no = ~0u;
    return read_sectors(t, HD_FIRST + 16, 1, s) && s[0] == 1 && memcmp(s + 1, "CD001", 5) == 0;
}

/* the track's start in the file at, or 150 sectors (a pregap) on */
static int file_track(TRACK* t, const char* path, long long at, int size) {
    static const int gaps[] = { 0, 150, -150 };
    size_t i;
    t->fd = open(path, O_RDONLY | O_BINARY);
    t->size = size;
    t->data = size == 2352 ? 16 : 0;
    if (t->fd < 0 || (size != SECTOR && size != 2352)) {
        return 0;
    }
    for (i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
        t->at = at + (long long)gaps[i] * size;
        if (t->at >= 0 && has_volume(t)) {
            return 1;
        }
    }
    return 0;
}

/* the next field of a line of a .gdi or .cue: a word, or a quoted name */
static const char* field(const char** p, char* out, size_t size) {
    const char* s = *p;
    size_t n = 0;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == 0 || *s == '\r' || *s == '\n') {
        return NULL;
    }
    if (*s == '"') {
        for (s++; *s != 0 && *s != '"'; s++) {
            if (n + 1 < size) {
                out[n++] = *s;
            }
        }
        if (*s == '"') {
            s++;
        }
    } else {
        for (; *s != 0 && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n'; s++) {
            if (n + 1 < size) {
                out[n++] = *s;
            }
        }
    }
    out[n] = 0;
    *p = s;
    return out;
}

/* track 3 as a .gdi names it: "3 45000 4 2352 track03.bin 0" */
static int gdi_track(TRACK* t, const char* dir, const char* sheet) {
    char line[1200], f[5][1100], path[2400];
    FILE* s;
    int ok = 0;
    snprintf(path, sizeof path, "%s/%s", dir, sheet);
    if ((s = fopen(path, "r")) == NULL) {
        return 0;
    }
    while (!ok && fgets(line, sizeof line, s) != NULL) {
        const char* p = line;
        int i;
        for (i = 0; i < 5 && field(&p, f[i], sizeof f[i]) != NULL; i++) {
        }
        if (i == 5 && atol(f[1]) == HD_FIRST) {
            snprintf(path, sizeof path, "%s/%s", dir, f[4]);
            ok = file_track(t, path, 0, atoi(f[3]));
        }
    }
    fclose(s);
    return ok;
}

/* track 3 as a .cue names it: its FILE, and INDEX 01's place in it */
static int cue_track(TRACK* t, const char* dir, const char* sheet) {
    char line[1200], word[1100], file[1100] = "", path[2400];
    FILE* s;
    int track = 0, size = 2352, ok = 0;
    snprintf(path, sizeof path, "%s/%s", dir, sheet);
    if ((s = fopen(path, "r")) == NULL) {
        return 0;
    }
    while (!ok && fgets(line, sizeof line, s) != NULL) {
        const char* p = line;
        if (field(&p, word, sizeof word) == NULL) {
            continue;
        }
        if (strcmp(word, "FILE") == 0 && field(&p, word, sizeof word) != NULL) {
            snprintf(file, sizeof file, "%s", word);
        } else if (strcmp(word, "TRACK") == 0 && field(&p, word, sizeof word) != NULL) {
            track = atoi(word);
            if (field(&p, word, sizeof word) != NULL) {
                size = strstr(word, "2048") != NULL ? SECTOR : 2352;
            }
        } else if (strcmp(word, "INDEX") == 0 && track == 3 && field(&p, word, sizeof word) != NULL &&
                   atoi(word) == 1 && field(&p, word, sizeof word) != NULL) {
            int m = 0, sec = 0, fr = 0;
            sscanf(word, "%d:%d:%d", &m, &sec, &fr);
            snprintf(path, sizeof path, "%s/%s", dir, file);
            ok = file_track(t, path, (long long)((m * 60 + sec) * 75 + fr) * size, size);
        }
    }
    fclose(s);
    return ok;
}

/* track 3 of a CHD: its frames follow the earlier tracks' */
static int chd_track(TRACK* t, const char* path, const char** err) {
    char meta[512], type[64], sub[64], pgtype[64], pgsub[64];
    const chd_header* h;
    uint32_t phys = 0, i;
    if (chd_open(path, CHD_OPEN_READ, NULL, &t->chd) != CHDERR_NONE) {
        *err = "the CHD could not be opened";
        return 0;
    }
    h = chd_get_header(t->chd);
    t->per_hunk = h->hunkbytes / CD_FRAME_SIZE;
    if (h->unitbytes != CD_FRAME_SIZE || t->per_hunk == 0 || (t->hunk = malloc(h->hunkbytes)) == NULL) {
        *err = "the CHD is not of a CD or GD-ROM";
        return 0;
    }
    for (i = 0; i < 99; i++) {
        int track = 0, frames = 0, pad = 0, pregap = 0, postgap = 0, n = 0;
        uint32_t len = 0;
        if (chd_get_metadata(t->chd, GDROM_TRACK_METADATA_TAG, i, meta, sizeof meta - 1, &len, NULL, NULL) ==
            CHDERR_NONE) {
            meta[len] = 0;
            n = sscanf(meta, GDROM_TRACK_METADATA_FORMAT, &track, type, sub, &frames, &pad, &pregap, pgtype, pgsub,
                       &postgap);
        } else if (chd_get_metadata(t->chd, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof meta - 1, &len, NULL, NULL) ==
                   CHDERR_NONE) {
            meta[len] = 0;
            n = sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &track, type, sub, &frames, &pregap, pgtype, pgsub, &postgap);
        } else {
            break;
        }
        if (n < 4) {
            break;
        }
        if (track == 3) {
            static const int gaps[] = { 0, 150 };
            size_t g;
            t->data = strstr(type, "RAW") != NULL ? 16 : 0;
            if (strncmp(type, "MODE1", 5) != 0) {
                *err = "track 3 of the CHD is not a data track";
                return 0;
            }
            for (g = 0; g < sizeof gaps / sizeof gaps[0] + 1; g++) {
                t->frame0 = phys + (g < sizeof gaps / sizeof gaps[0] ? (uint32_t)gaps[g] : (uint32_t)pregap);
                if (has_volume(t)) {
                    return 1;
                }
            }
            break;
        }
        /* a GD-ROM's FRAMES take in PAD, the gap to the next track; every track is stored padded to 4 frames */
        phys += ((frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING) * CD_TRACK_PADDING;
    }
    *err = "no ISO 9660 track 3 in the CHD (is it the Dreamcast disc?)";
    return 0;
}

static void track_close(TRACK* t) {
    if (t->fd >= 0) {
        close(t->fd);
    }
    if (t->chd != NULL) {
        chd_close(t->chd);
    }
    free(t->hunk);
    free(t->raw);
    memset(t, 0, sizeof *t);
    t->fd = -1;
}

/* a file in dir whose name ends so */
static int find_kind(const char* dir, const char* end, char* name, size_t size) {
    DIR* d = opendir(dir);
    struct dirent* e;
    int found = 0;
    while (d != NULL && !found && (e = readdir(d)) != NULL) {
        if (ends_with(e->d_name, end)) {
            snprintf(name, size, "%s", e->d_name);
            found = 1;
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    return found;
}

/* the image in dir, the first of these kinds there */
const char* dc_image_find(const char* dir) {
    static const char* const kinds[] = { ".chd", ".gdi", ".cue", ".iso", "(track 3).bin", "track03.bin" };
    static char name[1100];
    size_t k;
    for (k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        if (find_kind(dir, kinds[k], name, sizeof name)) {
            return name;
        }
    }
    return NULL;
}

/* the image's own files in dir: the image, and for a .gdi or .cue the track files it names (each once); how many */
int dc_image_files(const char* dir, const char* image, char (*out)[256], int max) {
    char line[1200], f[6][1100], path[2400];
    FILE* s;
    int n = 0, i, k;
    if (max < 1) {
        return 0;
    }
    snprintf(out[n++], 256, "%s", image);
    if (!ends_with(image, ".gdi") && !ends_with(image, ".cue")) {
        return n;
    }
    snprintf(path, sizeof path, "%s/%s", dir, image);
    if ((s = fopen(path, "r")) == NULL) {
        return n;
    }
    while (n < max && fgets(line, sizeof line, s) != NULL) {
        const char* p = line;
        const char* file = NULL;
        if (ends_with(image, ".gdi")) {
            for (i = 0; i < 6 && field(&p, f[i], sizeof f[i]) != NULL; i++) {
            }
            file = i >= 5 ? f[4] : NULL;    /* "3 45000 4 2352 track03.bin 0" */
        } else if (field(&p, f[0], sizeof f[0]) != NULL && strcmp(f[0], "FILE") == 0 &&
                   field(&p, f[1], sizeof f[1]) != NULL) {
            file = f[1];
        }
        if (file == NULL || strchr(file, '/') != NULL || strchr(file, '\\') != NULL) {
            continue;       /* only files beside it */
        }
        for (k = 0; k < n && strcmp(out[k], file) != 0; k++) {
        }
        if (k == n) {
            snprintf(out[n++], 256, "%s", file);
        }
    }
    fclose(s);
    return n;
}

/* the image in dir deleted, its files all, once the disc's files are unpacked from it: they alone stay */
void dc_image_remove(const char* dir) {
    static char names[128][256];
    char path[1400];
    const char* image = dc_image_find(dir);
    int n, i;
    if (image == NULL) {
        return;
    }
    n = dc_image_files(dir, image, names, 128);
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (remove(path) == 0) {
            host_log("disc image: %s deleted (unpacked)", names[i]);
        }
    }
}

/* the image in from moved into to (the same card: renamed, nothing copied); 1 when it was */
int dc_image_move(const char* from, const char* to) {
    static char names[128][256];
    char a[1400], b[1400];
    const char* image = dc_image_find(from);
    int n, i, ok = 1;
    if (image == NULL) {
        return 0;
    }
    make_dir(to);
    n = dc_image_files(from, image, names, 128);
    for (i = 0; i < n; i++) {
        snprintf(a, sizeof a, "%s/%s", from, names[i]);
        snprintf(b, sizeof b, "%s/%s", to, names[i]);
        if (rename(a, b) != 0) {
            host_log("disc image: %s could not be moved to %s", a, to);
            ok = 0;
        }
    }
    return ok;
}

typedef struct {
    char name[256];             /* its path in the disc, as A/B */
    uint32_t lba, bytes;
} ENTRY;

static ENTRY* entries;
static int n_entries, max_entries;

static int add_entry(const char* name, uint32_t lba, uint32_t bytes) {
    if (n_entries == max_entries) {
        int more = max_entries ? max_entries * 2 : 1024;
        ENTRY* e = realloc(entries, (size_t)more * sizeof *e);
        if (e == NULL) {
            return 0;
        }
        entries = e;
        max_entries = more;
    }
    snprintf(entries[n_entries].name, sizeof entries[n_entries].name, "%s", name);
    entries[n_entries].lba = lba;
    entries[n_entries].bytes = bytes;
    n_entries++;
    return 1;
}

/* a folder's files and those below it, as entries */
static int walk(TRACK* t, uint32_t lba, uint32_t bytes, const char* path, int depth) {
    uint32_t n = (bytes + SECTOR - 1) / SECTOR, i = 0;
    uint8_t* d;
    int ok = 1;
    if (depth > 8 || n == 0 || n > 4096 || (d = malloc((size_t)n * SECTOR)) == NULL) {
        return 0;
    }
    if (!read_sectors(t, lba, n, d)) {
        free(d);
        return 0;
    }
    while (ok && i + 33 < n * SECTOR) {
        uint8_t len = d[i], name_len = d[i + 32];
        char name[256], full[256];
        uint32_t at = d[i + 2] | d[i + 3] << 8 | d[i + 4] << 16 | (uint32_t)d[i + 5] << 24;
        uint32_t size = d[i + 10] | d[i + 11] << 8 | d[i + 12] << 16 | (uint32_t)d[i + 13] << 24;
        int k;
        if (len == 0) {
            i = (i / SECTOR + 1) * SECTOR;     /* records do not cross sectors */
            continue;
        }
        if (len < 34 || i + len > n * SECTOR || name_len == 0 || 33u + name_len > len) {
            break;
        }
        for (k = 0; k < name_len && d[i + 33 + k] != ';'; k++) {
            name[k] = (char)d[i + 33 + k];
        }
        name[k] = 0;
        if (name_len == 1 && (d[i + 33] == 0 || d[i + 33] == 1)) {
            /* the folder itself, and its parent */
        } else if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL || strcmp(name, "..") == 0 ||
                   strlen(path) + strlen(name) + 2 > sizeof full) {
            ok = 0;
        } else {
            size_t p = strlen(path);
            memcpy(full, path, p);
            if (p > 0) {
                full[p++] = '/';
            }
            memcpy(full + p, name, strlen(name) + 1);
            ok = (d[i + 25] & 2) ? walk(t, at, size, full, depth + 1) : add_entry(full, at, size);
        }
        i += len;
    }
    free(d);
    return ok;
}

static int by_lba(const void* a, const void* b) {
    const ENTRY* x = a;
    const ENTRY* y = b;
    /* 1ST_READ.BIN last: until it is there, the folder is not taken for a whole disc */
    int fx = strcmp(x->name, "1ST_READ.BIN") == 0, fy = strcmp(y->name, "1ST_READ.BIN") == 0;
    if (fx != fy) {
        return fx - fy;
    }
    return x->lba < y->lba ? -1 : x->lba > y->lba;
}

/* the folders a file's path needs, under dir */
static void make_dirs(const char* dir, const char* name) {
    static char made[300];      /* the folder the file before was in, made then */
    char path[1400];
    const char* s;
    const char* last = strrchr(name, '/');
    if (last == NULL || ((size_t)(last - name) < sizeof made && strncmp(made, name, (size_t)(last - name)) == 0 &&
                         made[last - name] == 0)) {
        return;
    }
    snprintf(made, sizeof made, "%.*s", (int)(last - name), name);
    for (s = strchr(name, '/'); s != NULL; s = strchr(s + 1, '/')) {
        snprintf(path, sizeof path, "%s/%.*s", dir, (int)(s - name), name);
        make_dir(path);
    }
}

/* the unpacking's journal in the output folder while it runs: the image, its file count and how many of its files
   (in the order they are unpacked, by their place on the disc) are finished. Its being there says the folder's files
   are not all there yet (dc_gd.c's check); an unpacking of the same image goes on from it */
#define JOURNAL DC_UNPACK_JOURNAL
#define JOURNAL_FILES 32
#define JOURNAL_BYTES (16u << 20)

static int journal_read(const char* out_dir, const char* image, int files) {
    char path[1400], line[1200], name[1100];
    int n = 0, finished = 0;
    FILE* f;
    snprintf(path, sizeof path, "%s/" JOURNAL, out_dir);
    if ((f = fopen(path, "r")) == NULL) {
        return 0;
    }
    name[0] = 0;
    if (fgets(line, sizeof line, f) != NULL) {
        line[strcspn(line, "\r\n")] = 0;
        snprintf(name, sizeof name, "%s", line);
    }
    if (fgets(line, sizeof line, f) != NULL) {
        n = atoi(line);
    }
    if (fgets(line, sizeof line, f) != NULL) {
        finished = atoi(line);
    }
    /* the layout: 2 the root's files in subfolders (an unpacking begun flat, by an older build, begins again) */
    if (fgets(line, sizeof line, f) == NULL || atoi(line) != 2) {
        finished = 0;
    }
    fclose(f);
    return strcmp(name, image) == 0 && n == files && finished > 0 && finished <= files ? finished : 0;
}

/* dc_gd.c's listing of the folder (_index.bin: "MVC2IDX1", the count, then each root file's name in 24 bytes and its
   size): the files just unpacked, so the game does not ask the card for the folder's entries (18 to 36 s on a slow
   card) at its first start */
static void index_write(const char* out_dir) {
    char path[1400], name[24];
    uint32_t n = 0;
    FILE* f;
    int i;
    for (i = 0; i < n_entries; i++) {
        n += strchr(entries[i].name, '/') == NULL && strlen(entries[i].name) < sizeof name;
    }
    snprintf(path, sizeof path, "%s/_index.bin", out_dir);
    if ((f = fopen(path, "wb")) == NULL) {
        return;
    }
    fwrite("MVC2IDX1", 1, 8, f);
    fwrite(&n, 4, 1, f);
    for (i = 0; i < n_entries; i++) {
        if (strchr(entries[i].name, '/') == NULL && strlen(entries[i].name) < sizeof name) {
            memset(name, 0, sizeof name);
            memcpy(name, entries[i].name, strlen(entries[i].name));
            fwrite(name, 1, sizeof name, f);
            fwrite(&entries[i].bytes, 4, 1, f);
        }
    }
    fclose(f);
}

/* the USA disc's program as unpacked: its CRC-32 */
#define PROGRAM_CRC 0x84C6AF93u
static int program_ok(const char* out_dir) {
    static uint32_t table[256];
    char path[1400];
    uint8_t chunk[65536];
    uint32_t crc = 0xFFFFFFFFu;
    size_t n, i;
    FILE* f;
    if (table[1] == 0) {
        for (i = 0; i < 256; i++) {
            uint32_t c = (uint32_t)i;
            int k;
            for (k = 0; k < 8; k++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    dc_disc_path(path, sizeof path, out_dir, "1ST_READ.BIN", 1);
    if ((f = fopen(path, "rb")) == NULL) {
        return 0;
    }
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        for (i = 0; i < n; i++) {
            crc = table[(crc ^ chunk[i]) & 0xFF] ^ (crc >> 8);
        }
    }
    fclose(f);
    crc ^= 0xFFFFFFFFu;
    if (crc != PROGRAM_CRC) {
        host_log("disc image: 1ST_READ.BIN's CRC-32 %08X, not %08X", (unsigned)crc, (unsigned)PROGRAM_CRC);
    }
    return crc == PROGRAM_CRC;
}

/* one record of a fixed size written over the last in a single call, the file never emptied first: an unpacking
   stopped while it writes keeps the record before */
static int journal_write(const char* out_dir, const char* image, int files, int finished) {
    char path[1400], rec[1280];
    FILE* f;
    int ok;
    snprintf(path, sizeof path, "%s/" JOURNAL, out_dir);
    if ((f = fopen(path, "r+b")) == NULL && (f = fopen(path, "wb")) == NULL) {
        return 0;
    }
    setvbuf(f, NULL, _IONBF, 0);
    memset(rec, ' ', sizeof rec);
    snprintf(rec, sizeof rec - 1, "%s\n%d\n%d\n2\n", image, files, finished);
    rec[strlen(rec)] = ' ';     /* the terminator's place padded too: the record is all text */
    rec[sizeof rec - 1] = '\n';
    ok = fwrite(rec, 1, sizeof rec, f) == sizeof rec;
    return fclose(f) == 0 && ok;
}

const char* dc_image_unpack(const char* dir, void (*progress)(uint32_t done, uint32_t total)) {
    return dc_image_unpack_to(dir, dir, progress);
}

/* the disc's files from the image in dir into out (the same folder or another) */
const char* dc_image_unpack_to(const char* dir, const char* out_dir, void (*progress)(uint32_t done, uint32_t total)) {
    const char* image = dc_image_find(dir);
    const char* err = NULL;
    char path[1400];
    uint8_t vol[SECTOR];
    uint8_t* buf = NULL;
    uint64_t total = 0, done = 0, since;
    TRACK t;
    int ok = 0, i, first;
    time_t t0 = time(NULL);
    memset(&t, 0, sizeof t);
    t.fd = -1;
    t.hunk_no = ~0u;
    if (image == NULL) {
        return fail("no disc image in %s", dir);
    }
    host_log("disc image: unpacking %s/%s", dir, image);
    make_dir(out_dir);
    snprintf(path, sizeof path, "%s/_index.bin", out_dir);
    remove(path);       /* dc_gd.c's listing of the folder, made again from the files unpacked now */
    snprintf(path, sizeof path, "%s/%s", dir, image);
    t.raw = malloc((size_t)CHUNK * 2352);
    if (t.raw == NULL) {
        return fail("%s", "out of memory");
    }
    if (ends_with(image, ".chd")) {
        ok = chd_track(&t, path, &err);
    } else if (ends_with(image, ".gdi")) {
        ok = gdi_track(&t, dir, image);
    } else if (ends_with(image, ".cue")) {
        ok = cue_track(&t, dir, image);
    } else {
        ok = file_track(&t, path, 0, ends_with(image, ".iso") ? SECTOR : 2352);
    }
    if (!ok) {
        track_close(&t);
        return fail("%s", err != NULL ? err : "no ISO 9660 track 3 found in the image (is it the Dreamcast disc?)");
    }
    n_entries = 0;
    if (!read_sectors(&t, HD_FIRST + 16, 1, vol) ||
        !walk(&t, vol[158] | vol[159] << 8 | vol[160] << 16 | (uint32_t)vol[161] << 24,
              vol[166] | vol[167] << 8 | vol[168] << 16 | (uint32_t)vol[169] << 24, "", 0) ||
        n_entries == 0) {
        track_close(&t);
        return fail("%s", "the image's file system could not be read");
    }
    qsort(entries, n_entries, sizeof *entries, by_lba);
    for (i = 0; i < n_entries; i++) {
        total += entries[i].bytes;
    }
    buf = malloc((size_t)CHUNK * SECTOR);
    ok = buf != NULL;
    /* where an unpacking left off (the same image, the same files): the files it had finished kept */
    first = journal_read(out_dir, image, n_entries);
    for (i = 0; i < first; i++) {
        done += entries[i].bytes;
    }
    if (first > 0) {
        host_log("disc image: going on from file %d of %d (%lu KB done)", first + 1, n_entries,
                 (unsigned long)(done >> 10));
    }
    if (ok && !journal_write(out_dir, image, n_entries, first)) {
        err = "a file could not be written (is the SD card full?)";
        ok = 0;
    }
    {
        /* the subfolders, each once */
        char made[256] = { 0 };
        for (i = 0; ok && i < n_entries; i++) {
            char b = bucket_of(entries[i].name);
            if (strchr(entries[i].name, '/') == NULL && !made[(unsigned char)b]) {
                made[(unsigned char)b] = 1;
                snprintf(path, sizeof path, "%s/%c", out_dir, b);
                make_dir(path);
            }
        }
    }
    since = 0;
    for (i = first; ok && i < n_entries; i++) {
        const ENTRY* e = &entries[i];
        uint32_t left = e->bytes, lba = e->lba;
        int out;
        if (strchr(e->name, '/') != NULL) {
            make_dirs(out_dir, e->name);    /* the disc's own folders */
        }
        dc_disc_path(path, sizeof path, out_dir, e->name, 1);
        /* written as itself: an unpacking stopped in it starts it again (the journal says it was not finished) */
        uint64_t q = now_us();
        out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
        us_open += now_us() - q;
        if (out < 0) {
            err = "a file could not be written (is the SD card full?)";
            ok = 0;
            break;
        }
        while (ok && left > 0) {
            uint32_t k = (left + SECTOR - 1) / SECTOR, bytes;
            k = k < CHUNK ? k : CHUNK;
            bytes = k * SECTOR < left ? k * SECTOR : left;
            q = now_us();
            if (!read_sectors(&t, lba, k, buf)) {
                err = "the image could not be read";
                ok = 0;
            }
            us_read += now_us() - q;
            q = now_us();
            if (ok && !write_all(out, buf, bytes)) {
                err = "a file could not be written (is the SD card full?)";
                ok = 0;
            }
            us_write += now_us() - q;
            lba += k;
            left -= bytes;
            done += bytes;
            since += bytes;
            if (progress != NULL) {
                progress((uint32_t)(done >> 10), (uint32_t)(total >> 10));
            }
        }
        q = now_us();
        if (close(out) != 0) {
            ok = 0;
        }
        us_close += now_us() - q;
        /* the journal brought up to date now and then (each write of it costs the card's calls too) */
        if (ok && (since >= JOURNAL_BYTES || (i + 1 - first) % JOURNAL_FILES == 0)) {
            if (!journal_write(out_dir, image, n_entries, i + 1)) {
                err = "a file could not be written (is the SD card full?)";
                ok = 0;
            }
            since = 0;
        }
    }
    free(buf);
    track_close(&t);
    if (!ok) {
        return fail("%s", err != NULL ? err : "the disc's files could not be written");
    }
    /* the program's bytes checked, not their size alone: a bad unpacking redone from the start at the next start
       (the journal set back to none finished, the image kept) */
    if (!program_ok(out_dir)) {
        journal_write(out_dir, image, n_entries, 0);
        return fail("%s", "the unpacked files are not the disc's (1ST_READ.BIN's check failed): unpacked again at the next "
                          "start");
    }
    index_write(out_dir);   /* the game's listing of the folder, so its first start need not list it */
    snprintf(path, sizeof path, "%s/" JOURNAL, out_dir);
    remove(path);       /* finished */
    host_log("disc image: %d files, %lu KB in %ld s (reading the image %.1f s, making files %.1f s, writing %.1f s, "
             "closing %.1f s)", n_entries, (unsigned long)(total >> 10), (long)(time(NULL) - t0), us_read / 1e6,
             us_open / 1e6, us_write / 1e6, us_close / 1e6);
    return NULL;
}
