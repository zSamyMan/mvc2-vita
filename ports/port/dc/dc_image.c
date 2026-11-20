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
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0777)
#endif

#include "../ext/libchdr/include/libchdr/chd.h"
#include "../ext/libchdr/include/libchdr/cdrom.h"

#include "naomi_host.h"

#define HD_FIRST 45000          /* track 3's first sector */
#define SECTOR 2048
#define CHUNK 128               /* sectors a read */

typedef struct {
    FILE* f;
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

static int seek(FILE* f, long long at) {
#ifdef _WIN32
    return _fseeki64(f, at, SEEK_SET);
#else
    return fseeko(f, (off_t)at, SEEK_SET);
#endif
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
        return seek(t->f, t->at + (long long)(lba - HD_FIRST) * SECTOR) == 0 && fread(out, SECTOR, n, t->f) == n;
    }
    while (n > 0) {
        uint32_t k = n < CHUNK ? n : CHUNK;
        if (seek(t->f, t->at + (long long)(lba - HD_FIRST) * t->size) != 0 || fread(t->raw, t->size, k, t->f) != k) {
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
    t->f = fopen(path, "rb");
    t->size = size;
    t->data = size == 2352 ? 16 : 0;
    if (t->f == NULL || (size != SECTOR && size != 2352)) {
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
    if (t->f != NULL) {
        fclose(t->f);
    }
    if (t->chd != NULL) {
        chd_close(t->chd);
    }
    free(t->hunk);
    free(t->raw);
    memset(t, 0, sizeof *t);
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
    char path[1400];
    const char* s;
    for (s = strchr(name, '/'); s != NULL; s = strchr(s + 1, '/')) {
        snprintf(path, sizeof path, "%s/%.*s", dir, (int)(s - name), name);
        make_dir(path);
    }
}

const char* dc_image_unpack(const char* dir, void (*progress)(uint32_t done, uint32_t total)) {
    const char* image = dc_image_find(dir);
    const char* err = NULL;
    char path[1400], part[1410];
    uint8_t vol[SECTOR];
    uint8_t* buf = NULL;
    uint64_t total = 0, done = 0;
    TRACK t;
    int ok = 0, i;
    memset(&t, 0, sizeof t);
    t.hunk_no = ~0u;
    if (image == NULL) {
        return fail("no disc image in %s", dir);
    }
    host_log("disc image: unpacking %s/%s", dir, image);
    snprintf(path, sizeof path, "%s/_index.bin", dir);
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
    for (i = 0; ok && i < n_entries; i++) {
        const ENTRY* e = &entries[i];
        uint32_t left = e->bytes, lba = e->lba;
        FILE* out;
        make_dirs(dir, e->name);
        snprintf(path, sizeof path, "%s/%s", dir, e->name);
        snprintf(part, sizeof part, "%s.part", path);
        if ((out = fopen(part, "wb")) == NULL) {
            err = "a file could not be written (is the SD card full?)";
            ok = 0;
            break;
        }
        while (ok && left > 0) {
            uint32_t k = (left + SECTOR - 1) / SECTOR, bytes;
            k = k < CHUNK ? k : CHUNK;
            bytes = k * SECTOR < left ? k * SECTOR : left;
            if (!read_sectors(&t, lba, k, buf)) {
                err = "the image could not be read";
                ok = 0;
            } else if (fwrite(buf, 1, bytes, out) != bytes) {
                err = "a file could not be written (is the SD card full?)";
                ok = 0;
            }
            lba += k;
            left -= bytes;
            done += bytes;
            if (progress != NULL) {
                progress((uint32_t)(done >> 10), (uint32_t)(total >> 10));
            }
        }
        if (fclose(out) != 0) {
            ok = 0;
        }
        remove(path);
        if (!ok || rename(part, path) != 0) {
            remove(part);
            ok = 0;
        }
    }
    free(buf);
    track_close(&t);
    if (!ok) {
        return fail("%s", err != NULL ? err : "the disc's files could not be written");
    }
    host_log("disc image: %d files, %lu KB", n_entries, (unsigned long)(total >> 10));
    return NULL;
}
