// =============================================================================
//  kernel/fat.c -- Pilote FAT32 (lecture + ecriture incrementale)
// -----------------------------------------------------------------------------
//  Lit une partition FAT32 (formatee sous Windows/Mac) dans le VFS, et ecrit
//  les modifications directement dans la structure FAT32 (preserve le format,
//  donc relisible par Windows). Gere les noms longs (LFN).
// =============================================================================
#include "fat.h"
#include "vfs.h"
#include "klib.h"

typedef struct {
    fat_rd_t rd; fat_wr_t wr;
    uint16_t byts_per_sec;
    uint8_t  sec_per_clus;
    uint16_t rsvd;
    uint8_t  num_fats;
    uint32_t fat_sz;          // secteurs par FAT
    uint32_t root_clus;
    uint32_t total_sec;
    uint32_t fat_start;       // = rsvd
    uint32_t data_start;      // = rsvd + num_fats*fat_sz
    uint32_t clus_bytes;      // sec_per_clus * byts_per_sec
    uint32_t total_clus;
    bool     active;
} fat_t;

static fat_t F;
static uint8_t clbuf[65536];          // tampon d'un cluster (max 64 Kio)

bool fat_active(void) { return F.active; }

// --- E/S secteur -------------------------------------------------------------
static bool rdsec(uint32_t lba, void *b)        { return F.rd(lba, 1, b); }
static bool wrsec(uint32_t lba, const void *b)  { return F.wr(lba, 1, b); }

static uint32_t fat_get(uint32_t c) {
    uint8_t s[512];
    uint32_t off = c * 4;
    if (!rdsec(F.fat_start + off / 512, s)) return 0x0FFFFFFF;
    return (*(uint32_t *)(s + off % 512)) & 0x0FFFFFFF;
}
static void fat_set(uint32_t c, uint32_t val) {
    uint8_t s[512];
    uint32_t off = c * 4;
    for (uint32_t f = 0; f < F.num_fats; f++) {
        uint32_t sec = F.fat_start + f * F.fat_sz + off / 512;
        if (!rdsec(sec, s)) return;
        uint32_t *e = (uint32_t *)(s + off % 512);
        *e = (*e & 0xF0000000) | (val & 0x0FFFFFFF);
        wrsec(sec, s);
    }
}
static uint32_t clus_sec(uint32_t c) { return F.data_start + (c - 2) * F.sec_per_clus; }

static bool read_clus(uint32_t c, void *buf) {
    return F.rd(clus_sec(c), F.sec_per_clus, buf);
}
static bool write_clus(uint32_t c, const void *buf) {
    return F.wr(clus_sec(c), F.sec_per_clus, buf);
}

static uint32_t alloc_clus(void) {
    uint8_t s[512];
    for (uint32_t sec = 0; sec < F.fat_sz; sec++) {
        if (!rdsec(F.fat_start + sec, s)) return 0;
        for (int i = 0; i < 128; i++) {
            uint32_t c = sec * 128 + i;
            if (c < 2) continue;
            if (c >= F.total_clus + 2) return 0;
            if (((*(uint32_t *)(s + i * 4)) & 0x0FFFFFFF) == 0) {
                fat_set(c, 0x0FFFFFFF);
                return c;
            }
        }
    }
    return 0;
}
static void free_chain(uint32_t c) {
    while (c >= 2 && c < 0x0FFFFFF8) { uint32_t n = fat_get(c); fat_set(c, 0); c = n; }
}

// --- chaine de clusters d'un repertoire --------------------------------------
#define MAX_DIRCLUS 128
static int dir_chain(uint32_t first, uint32_t *out) {
    int n = 0; uint32_t c = first;
    while (c >= 2 && c < 0x0FFFFFF8 && n < MAX_DIRCLUS) { out[n++] = c; c = fat_get(c); }
    return n;
}
static int epc(void) { return F.clus_bytes / 32; }   // entrees par cluster

// lit l'entree d'index global i (dans la chaine 'ch' de n clusters) -> e[32]
static bool get_entry(uint32_t *ch, int n, int i, uint8_t *e) {
    int per = epc();
    int ci = i / per; if (ci >= n) return false;
    int ein = i % per;
    uint8_t s[512];
    uint32_t sec = clus_sec(ch[ci]) + (ein * 32) / 512;
    if (!rdsec(sec, s)) return false;
    memcpy(e, s + (ein * 32) % 512, 32);
    return true;
}
static void put_entry(uint32_t *ch, int n, int i, const uint8_t *e) {
    int per = epc();
    int ci = i / per; if (ci >= n) return;
    int ein = i % per;
    uint8_t s[512];
    uint32_t sec = clus_sec(ch[ci]) + (ein * 32) / 512;
    if (!rdsec(sec, s)) return;
    memcpy(s + (ein * 32) % 512, e, 32);
    wrsec(sec, s);
}

// =============================================================================
//  Lecture : montage de l'arborescence FAT32 dans le VFS
// =============================================================================
static char up(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

// Reconstruit le nom 8.3 depuis une entree courte -> out (minuscule si flags NT).
static void name_83(const uint8_t *e, char *out) {
    int o = 0;
    bool low_base = e[12] & 0x08, low_ext = e[12] & 0x10;
    for (int i = 0; i < 8 && e[i] != ' '; i++) { char c = e[i]; out[o++] = low_base ? (c>='A'&&c<='Z'?c+32:c) : c; }
    if (e[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) { char c = e[i]; out[o++] = low_ext ? (c>='A'&&c<='Z'?c+32:c) : c; }
    }
    out[o] = 0;
}

static void load_dir(uint32_t dir_clus, vfs_node_t *parent, int depth);

static void load_file_content(vfs_node_t *node, uint32_t first, uint32_t size) {
    uint32_t off = 0, c = first;
    while (c >= 2 && c < 0x0FFFFFF8 && off < size) {
        if (!read_clus(c, clbuf)) break;
        uint32_t n = size - off; if (n > F.clus_bytes) n = F.clus_bytes;
        vfs_write(node, off, clbuf, n);
        off += n; c = fat_get(c);
    }
}

static void load_dir(uint32_t dir_clus, vfs_node_t *parent, int depth) {
    if (depth > 16) return;
    uint32_t ch[MAX_DIRCLUS]; int nc = dir_chain(dir_clus, ch);
    int total = nc * epc();
    char lfn[260]; int lfn_have = 0;
    for (int i = 0; i < total; i++) {
        uint8_t e[32];
        if (!get_entry(ch, nc, i, e)) break;
        if (e[0] == 0x00) break;                 // fin du repertoire
        if (e[0] == 0xE5) { lfn_have = 0; continue; }   // supprimee
        if (e[11] == 0x0F) {                     // entree LFN
            int seq = e[0] & 0x3F;
            char part[14]; int p = 0;
            static const int idx[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
            for (int k = 0; k < 13; k++) {
                uint16_t ch16 = e[idx[k]] | (e[idx[k]+1] << 8);
                if (ch16 == 0 || ch16 == 0xFFFF) break;
                part[p++] = (ch16 < 128) ? (char)ch16 : '?';
            }
            part[p] = 0;
            int pos = (seq - 1) * 13;
            if (pos >= 0 && pos < 255) { for (int k = 0; k <= p && pos + k < 259; k++) lfn[pos + k] = part[k]; lfn_have = 1; }
            continue;
        }
        if (e[11] & 0x08) { lfn_have = 0; continue; }   // etiquette de volume
        char name[260];
        if (lfn_have) { int k = 0; while (lfn[k] && k < 255) { name[k] = lfn[k]; k++; } name[k] = 0; }
        else name_83(e, name);
        lfn_have = 0;
        if (name[0] == '.') continue;            // "." et ".."
        uint32_t first = ((uint32_t)(e[20] | (e[21] << 8)) << 16) | (e[26] | (e[27] << 8));
        uint32_t size = e[28] | (e[29] << 8) | (e[30] << 16) | ((uint32_t)e[31] << 24);
        if (e[11] & 0x10) {                      // repertoire
            vfs_node_t *d = vfs_create(parent, name, VFS_DIR);
            if (d && first >= 2) load_dir(first, d, depth + 1);
        } else {                                 // fichier
            vfs_node_t *f = vfs_create(parent, name, VFS_FILE);
            if (f && size > 0 && first >= 2) load_file_content(f, first, size);
        }
    }
}

bool fat_mount(fat_rd_t rd, fat_wr_t wr, vfs_node_t *mount) {
    uint8_t bs[512];
    F.rd = rd; F.wr = wr; F.active = false;
    if (!rd(0, 1, bs)) return false;
    uint16_t bps = bs[11] | (bs[12] << 8);
    if (bps != 512) return false;
    uint16_t root_ent = bs[17] | (bs[18] << 8);
    uint16_t fatsz16  = bs[22] | (bs[23] << 8);
    uint32_t fatsz32  = bs[36] | (bs[37] << 8) | (bs[38] << 16) | ((uint32_t)bs[39] << 24);
    if (root_ent != 0 || fatsz16 != 0 || fatsz32 == 0) return false;   // pas du FAT32
    F.byts_per_sec = bps;
    F.sec_per_clus = bs[13];
    F.rsvd         = bs[14] | (bs[15] << 8);
    F.num_fats     = bs[16];
    F.fat_sz       = fatsz32;
    F.root_clus    = bs[44] | (bs[45] << 8) | (bs[46] << 16) | ((uint32_t)bs[47] << 24);
    F.total_sec    = bs[32] | (bs[33] << 8) | (bs[34] << 16) | ((uint32_t)bs[35] << 24);
    if (F.sec_per_clus == 0 || F.num_fats == 0) return false;
    F.fat_start  = F.rsvd;
    F.data_start = F.rsvd + (uint32_t)F.num_fats * F.fat_sz;
    F.clus_bytes = (uint32_t)F.sec_per_clus * 512;
    if (F.clus_bytes > sizeof(clbuf)) return false;
    F.total_clus = (F.total_sec - F.data_start) / F.sec_per_clus;
    F.active = true;
    load_dir(F.root_clus, mount, 0);
    return true;
}

// =============================================================================
//  Ecriture : creation / ecriture / suppression d'entrees
// =============================================================================
static bool name_eq(const char *a, const char *b) {
    while (*a && *b) { if (up(*a) != up(*b)) return false; a++; b++; }
    return *a == *b;
}

// Cherche 'name' dans le repertoire dir_clus -> renvoie l'index de l'entree
// courte (et remplit first/attr), ou -1.
static int find_in_dir(uint32_t dir_clus, const char *name, uint32_t *first, uint8_t *attr) {
    uint32_t ch[MAX_DIRCLUS]; int nc = dir_chain(dir_clus, ch);
    int total = nc * epc();
    char lfn[260]; int lfn_have = 0;
    for (int i = 0; i < total; i++) {
        uint8_t e[32];
        if (!get_entry(ch, nc, i, e)) break;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { lfn_have = 0; continue; }
        if (e[11] == 0x0F) {
            int seq = e[0] & 0x3F;
            static const int idx[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
            int pos = (seq - 1) * 13;
            for (int k = 0; k < 13 && pos + k < 259; k++) {
                uint16_t c16 = e[idx[k]] | (e[idx[k]+1] << 8);
                lfn[pos + k] = (c16 == 0 || c16 == 0xFFFF) ? 0 : (c16 < 128 ? (char)c16 : '?');
            }
            lfn_have = 1; continue;
        }
        if (e[11] & 0x08) { lfn_have = 0; continue; }
        char nm[260];
        if (lfn_have) { int k = 0; while (lfn[k] && k < 255) { nm[k] = lfn[k]; k++; } nm[k] = 0; }
        else name_83(e, nm);
        lfn_have = 0;
        if (nm[0] == '.') continue;
        if ( name_eq(nm, name)) {
            if (first) *first = ((uint32_t)(e[20] | (e[21] << 8)) << 16) | (e[26] | (e[27] << 8));
            if (attr) *attr = e[11];
            return i;
        }
    }
    return -1;
}

// Resout le repertoire parent d'un chemin relatif ; *leaf = dernier composant.
static bool resolve_parent(const char *relpath, uint32_t *parent_clus, const char **leaf) {
    uint32_t cur = F.root_clus;
    const char *p = relpath;
    const char *seg = p;
    for (;;) {
        const char *slash = seg;
        while (*slash && *slash != '/') slash++;
        if (*slash == 0) { *parent_clus = cur; *leaf = seg; return true; }   // dernier
        char comp[64]; int n = 0;
        for (const char *q = seg; q < slash && n < 63; q++) comp[n++] = *q;
        comp[n] = 0;
        uint32_t first; uint8_t attr;
        if (find_in_dir(cur, comp, &first, &attr) < 0 || !(attr & 0x10)) return false;
        cur = first ? first : cur;
        seg = slash + 1;
    }
}

static uint8_t lfn_checksum(const uint8_t *sh) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + sh[i]);
    return s;
}

// Fabrique un nom court 8.3 unique (ALIAS~N) pour 'name' dans dir_clus.
static void make_alias(uint32_t dir_clus, const char *name, uint8_t *sh) {
    for (int i = 0; i < 11; i++) sh[i] = ' ';
    // base
    int bi = 0;
    for (const char *p = name; *p && *p != '.' && bi < 6; p++) {
        char c = up(*p);
        if ((c>='A'&&c<='Z') || (c>='0'&&c<='9')) sh[bi++] = c;
    }
    if (bi == 0) sh[bi++] = 'F';
    // extension
    const char *dot = 0; for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    if (dot) { int ei = 0; for (const char *p = dot + 1; *p && ei < 3; p++) { char c = up(*p); if (c > ' ') sh[8 + ei++] = c; } }
    // suffixe ~N unique
    for (int nn = 1; nn < 10; nn++) {
        sh[bi] = '~'; sh[bi + 1] = '0' + nn;
        char test[13]; name_83(sh, test);
        if (find_in_dir(dir_clus, test, 0, 0) < 0) return;
    }
}

// Ecrit 'count' entrees consecutives (LFN... + courte) a partir de l'index libre.
static int add_dir_entry(uint32_t dir_clus, const char *name, uint32_t first, uint32_t size, uint8_t attr) {
    uint8_t sh[11];
    make_alias(dir_clus, name, sh);
    uint8_t sum = lfn_checksum(sh);
    int nlen = (int)strlen(name);
    int nlfn = (nlen + 12) / 13; if (nlfn < 1) nlfn = 1;
    int need = nlfn + 1;

    // trouve 'need' entrees libres consecutives (0x00/0xE5) ; etend si besoin
    uint32_t ch[MAX_DIRCLUS]; int nc = dir_chain(dir_clus, ch);
    int total = nc * epc();
    int start = -1, run = 0;
    for (int i = 0; i < total; i++) {
        uint8_t e[32];
        if (!get_entry(ch, nc, i, e)) break;
        if (e[0] == 0x00 || e[0] == 0xE5) { if (run == 0) start = i; run++; if (run >= need) break; }
        else { run = 0; start = -1; }
    }
    if (start < 0 || run < need) {
        // etend le repertoire d'un cluster
        uint32_t nclus = alloc_clus();
        if (!nclus) return -1;
        memset(clbuf, 0, F.clus_bytes); write_clus(nclus, clbuf);
        fat_set(ch[nc - 1], nclus);
        if (start < 0) start = total;             // commence au debut du nouveau cluster
        nc = dir_chain(dir_clus, ch);
    }
    // ecrit les entrees LFN (ordre inverse), puis l'entree courte
    for (int k = 0; k < nlfn; k++) {
        int seq = nlfn - k;                       // sequence decroissante sur le disque
        uint8_t e[32]; memset(e, 0, 32);
        e[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0));
        e[11] = 0x0F; e[13] = sum;
        static const int idx[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
        int base = (seq - 1) * 13;
        for (int j = 0; j < 13; j++) {
            uint16_t c16;
            int ci = base + j;
            if (ci < nlen) c16 = (uint8_t)name[ci];
            else if (ci == nlen) c16 = 0;
            else c16 = 0xFFFF;
            e[idx[j]] = c16 & 0xFF; e[idx[j] + 1] = c16 >> 8;
        }
        put_entry(ch, nc, start + k, e);
    }
    uint8_t e[32]; memset(e, 0, 32);
    memcpy(e, sh, 11);
    e[11] = attr;
    // date/heure d'ecriture valides (FAT : annee depuis 1980) -> 2026-01-01 00:00
    uint16_t fdate = ((2026 - 1980) << 9) | (1 << 5) | 1;
    e[14] = 0; e[15] = 0;                          // heure de creation
    e[16] = fdate & 0xFF; e[17] = fdate >> 8;      // date de creation
    e[18] = fdate & 0xFF; e[19] = fdate >> 8;      // date d'acces
    e[22] = 0; e[23] = 0;                          // heure d'ecriture
    e[24] = fdate & 0xFF; e[25] = fdate >> 8;      // date d'ecriture
    e[20] = (first >> 16) & 0xFF; e[21] = (first >> 24) & 0xFF;
    e[26] = first & 0xFF; e[27] = (first >> 8) & 0xFF;
    e[28] = size & 0xFF; e[29] = (size >> 8) & 0xFF; e[30] = (size >> 16) & 0xFF; e[31] = (size >> 24) & 0xFF;
    put_entry(ch, nc, start + nlfn, e);
    return start + nlfn;                           // index de l'entree courte
}

// Met a jour first/size d'une entree courte deja presente (index i).
static void update_entry(uint32_t dir_clus, int i, uint32_t first, uint32_t size) {
    uint32_t ch[MAX_DIRCLUS]; int nc = dir_chain(dir_clus, ch);
    uint8_t e[32];
    if (!get_entry(ch, nc, i, e)) return;
    e[20] = (first >> 16) & 0xFF; e[21] = (first >> 24) & 0xFF;
    e[26] = first & 0xFF; e[27] = (first >> 8) & 0xFF;
    e[28] = size & 0xFF; e[29] = (size >> 8) & 0xFF; e[30] = (size >> 16) & 0xFF; e[31] = (size >> 24) & 0xFF;
    put_entry(ch, nc, i, e);
}

int fat_create(const char *relpath, bool is_dir) {
    if (!F.active) return -1;
    uint32_t parent; const char *leaf;
    if (!resolve_parent(relpath, &parent, &leaf) || !leaf[0]) return -1;
    if (find_in_dir(parent, leaf, 0, 0) >= 0) return 0;   // existe deja
    uint32_t first = 0;
    if (is_dir) {
        first = alloc_clus();
        if (!first) return -1;
        memset(clbuf, 0, F.clus_bytes);
        // entrees "." et ".."
        uint8_t *dot = clbuf;  memset(dot, ' ', 11); dot[0] = '.'; dot[11] = 0x10;
        dot[26] = first & 0xFF; dot[27] = (first >> 8) & 0xFF;
        dot[20] = (first >> 16) & 0xFF; dot[21] = (first >> 24) & 0xFF;
        uint8_t *dd = clbuf + 32; memset(dd, ' ', 11); dd[0] = '.'; dd[1] = '.'; dd[11] = 0x10;
        uint32_t pc = (parent == F.root_clus) ? 0 : parent;
        dd[26] = pc & 0xFF; dd[27] = (pc >> 8) & 0xFF;
        dd[20] = (pc >> 16) & 0xFF; dd[21] = (pc >> 24) & 0xFF;
        write_clus(first, clbuf);
    }
    return add_dir_entry(parent, leaf, first, 0, is_dir ? 0x10 : 0x20) >= 0 ? 0 : -1;
}

int fat_write(const char *relpath, const void *data, uint32_t len) {
    if (!F.active) return -1;
    uint32_t parent; const char *leaf;
    if (!resolve_parent(relpath, &parent, &leaf) || !leaf[0]) return -1;
    uint32_t oldfirst = 0; uint8_t attr = 0;
    int idx = find_in_dir(parent, leaf, &oldfirst, &attr);
    if (idx < 0) {                                 // cree le fichier
        if (add_dir_entry(parent, leaf, 0, 0, 0x20) < 0) return -1;
        idx = find_in_dir(parent, leaf, &oldfirst, 0);
        if (idx < 0) return -1;
    }
    if (oldfirst >= 2) free_chain(oldfirst);       // libere l'ancien contenu

    const uint8_t *src = (const uint8_t *)data;
    uint32_t first = 0, prev = 0, off = 0;
    while (off < len) {
        uint32_t c = alloc_clus();
        if (!c) break;
        if (prev) fat_set(prev, c); else first = c;
        uint32_t n = len - off; if (n > F.clus_bytes) n = F.clus_bytes;
        memset(clbuf, 0, F.clus_bytes);
        memcpy(clbuf, src + off, n);
        write_clus(c, clbuf);
        prev = c; off += n;
    }
    update_entry(parent, idx, first, len);
    return 0;
}

int fat_delete(const char *relpath) {
    if (!F.active) return -1;
    uint32_t parent; const char *leaf;
    if (!resolve_parent(relpath, &parent, &leaf) || !leaf[0]) return -1;
    uint32_t first = 0; uint8_t attr = 0;
    int idx = find_in_dir(parent, leaf, &first, &attr);
    if (idx < 0) return -1;
    if (first >= 2) free_chain(first);
    // marque l'entree courte ET les entrees LFN precedentes comme supprimees
    uint32_t ch[MAX_DIRCLUS]; int nc = dir_chain(parent, ch);
    for (int i = idx; i >= 0; i--) {
        uint8_t e[32];
        if (!get_entry(ch, nc, i, e)) break;
        bool islfn = (i < idx && e[11] == 0x0F);
        e[0] = 0xE5;
        put_entry(ch, nc, i, e);
        if (i < idx && !islfn) break;              // on s'arrete avant l'entree precedente
    }
    return 0;
}
