// =============================================================================
//  user/imgdec.c -- Décodeur d'images : PNG, JPEG (baseline), BMP 24 bits, PPM.
// -----------------------------------------------------------------------------
//  Autonome (n'utilise que malloc/free/memset/memcpy fournis par urt.c). Pensé
//  pour être aussi compilable sur l'hôte afin de valider les décodeurs.
//
//  - PNG  : DEFLATE (RFC 1951) + zlib (RFC 1950) maison, profondeur 8 bits,
//           types couleur 0/2/3/4/6, filtres 0-4, sans entrelacement Adam7.
//  - JPEG : baseline (SOF0), portage compact de NanoJPEG (Martin Fiedler,
//           domaine public), Huffman + IDCT + YCbCr, sous-échantillonnage
//           4:4:4 / 4:2:2 / 4:2:0, marqueurs de redémarrage.
// =============================================================================
#include "imgdec.h"

void *malloc(unsigned long);
void  free(void *);
void *calloc(unsigned long, unsigned long);
void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);

static inline uint32_t rgb3(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static inline int iabs(int x) { return x < 0 ? -x : x; }

// =============================================================================
//  DEFLATE / inflate (RFC 1951)
// =============================================================================
typedef struct {
    const uint8_t *in; int inlen, inpos;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; int outcap, outpos;
} infl_t;

typedef struct { uint16_t counts[16]; uint16_t symbols[288]; } huff_t;

static int infl_bit(infl_t *z) {
    if (z->bitcnt == 0) {
        if (z->inpos >= z->inlen) return -1;
        z->bitbuf = z->in[z->inpos++]; z->bitcnt = 8;
    }
    int b = z->bitbuf & 1; z->bitbuf >>= 1; z->bitcnt--; return b;
}
static int infl_bits(infl_t *z, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) { int b = infl_bit(z); if (b < 0) return -1; v |= b << i; }
    return v;
}

static void huff_build(huff_t *h, const uint8_t *lengths, int n) {
    int offs[16];
    for (int i = 0; i < 16; i++) h->counts[i] = 0;
    for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    offs[0] = 0;
    for (int i = 1; i < 16; i++) offs[i] = offs[i - 1] + h->counts[i - 1];
    for (int i = 0; i < n; i++) if (lengths[i]) h->symbols[offs[lengths[i]]++] = (uint16_t)i;
}

static int huff_decode(infl_t *z, huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = infl_bit(z); if (b < 0) return -1;
        code |= b;
        int count = h->counts[len];
        if (code - first < count) return h->symbols[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

static const uint16_t LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  LEN_EXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  DIST_EXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inflate_block(infl_t *z, huff_t *lh, huff_t *dh) {
    for (;;) {
        int sym = huff_decode(z, lh); if (sym < 0) return -1;
        if (sym == 256) return 0;
        if (sym < 256) {
            if (z->outpos >= z->outcap) return -1;
            z->out[z->outpos++] = (uint8_t)sym;
        } else {
            sym -= 257; if (sym >= 29) return -1;
            int e = infl_bits(z, LEN_EXTRA[sym]); if (e < 0) return -1;
            int len = LEN_BASE[sym] + e;
            int ds = huff_decode(z, dh); if (ds < 0 || ds >= 30) return -1;
            int de = infl_bits(z, DIST_EXTRA[ds]); if (de < 0) return -1;
            int dist = DIST_BASE[ds] + de;
            if (dist > z->outpos) return -1;
            if (z->outpos + len > z->outcap) return -1;
            for (int i = 0; i < len; i++) { z->out[z->outpos] = z->out[z->outpos - dist]; z->outpos++; }
        }
    }
}

static int inflate(infl_t *z) {
    int final;
    do {
        final = infl_bit(z); if (final < 0) return -1;
        int type = infl_bits(z, 2); if (type < 0) return -1;
        if (type == 0) {                                   // bloc non compressé
            z->bitbuf = 0; z->bitcnt = 0;                  // alignement octet
            if (z->inpos + 4 > z->inlen) return -1;
            int len = z->in[z->inpos] | (z->in[z->inpos + 1] << 8);
            z->inpos += 4;
            if (z->inpos + len > z->inlen) return -1;
            if (z->outpos + len > z->outcap) return -1;
            for (int i = 0; i < len; i++) z->out[z->outpos++] = z->in[z->inpos++];
        } else if (type == 1) {                            // Huffman fixe
            huff_t lh, dh; uint8_t ll[288], dl[30];
            for (int i = 0; i < 144; i++) ll[i] = 8;
            for (int i = 144; i < 256; i++) ll[i] = 9;
            for (int i = 256; i < 280; i++) ll[i] = 7;
            for (int i = 280; i < 288; i++) ll[i] = 8;
            for (int i = 0; i < 30; i++) dl[i] = 5;
            huff_build(&lh, ll, 288); huff_build(&dh, dl, 30);
            if (inflate_block(z, &lh, &dh)) return -1;
        } else if (type == 2) {                            // Huffman dynamique
            int hlit = infl_bits(z, 5) + 257;
            int hdist = infl_bits(z, 5) + 1;
            int hclen = infl_bits(z, 4) + 4;
            if (hlit > 288 || hdist > 30) return -1;
            static const uint8_t ORD[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t cl[19]; for (int i = 0; i < 19; i++) cl[i] = 0;
            for (int i = 0; i < hclen; i++) { int v = infl_bits(z, 3); if (v < 0) return -1; cl[ORD[i]] = (uint8_t)v; }
            huff_t clh; huff_build(&clh, cl, 19);
            uint8_t lengths[288 + 30]; int n = 0, total = hlit + hdist;
            while (n < total) {
                int sym = huff_decode(z, &clh); if (sym < 0) return -1;
                if (sym < 16) lengths[n++] = (uint8_t)sym;
                else if (sym == 16) {
                    if (n == 0) return -1;
                    int r = infl_bits(z, 2) + 3; uint8_t prev = lengths[n - 1];
                    while (r-- && n < total) lengths[n++] = prev;
                } else if (sym == 17) {
                    int r = infl_bits(z, 3) + 3; while (r-- && n < total) lengths[n++] = 0;
                } else {
                    int r = infl_bits(z, 7) + 11; while (r-- && n < total) lengths[n++] = 0;
                }
            }
            huff_t lh, dh; huff_build(&lh, lengths, hlit); huff_build(&dh, lengths + hlit, hdist);
            if (inflate_block(z, &lh, &dh)) return -1;
        } else return -1;
    } while (!final);
    return z->outpos;
}

// =============================================================================
//  PNG (RFC 2083)
// =============================================================================
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static int decode_png(const uint8_t *d, int n, uint32_t *out, int maxw, int maxh, int *ow, int *oh) {
    static const uint8_t SIG[8] = {0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    if (n < 8) return 0;
    for (int i = 0; i < 8; i++) if (d[i] != SIG[i]) return 0;

    int w = 0, h = 0, depth = 0, color = -1, interlace = 0;
    uint8_t *idat = 0; int idatlen = 0, idatcap = 0;
    uint8_t palette[256 * 3]; int palcount = 0;
    uint8_t trns[256]; int ntrns = 0;

    int pos = 8;
    while (pos + 8 <= n) {
        uint32_t clen = be32(d + pos);
        const uint8_t *type = d + pos + 4;
        if ((uint32_t)(pos + 12) + clen > (uint32_t)n) break;
        const uint8_t *ch = d + pos + 8;
        if (type[0]=='I'&&type[1]=='H'&&type[2]=='D'&&type[3]=='R') {
            w = (int)be32(ch); h = (int)be32(ch + 4);
            depth = ch[8]; color = ch[9]; interlace = ch[12];
        } else if (type[0]=='P'&&type[1]=='L'&&type[2]=='T'&&type[3]=='E') {
            palcount = clen / 3; if (palcount > 256) palcount = 256;
            memcpy(palette, ch, (unsigned long)palcount * 3);
        } else if (type[0]=='t'&&type[1]=='R'&&type[2]=='N'&&type[3]=='S') {
            ntrns = clen > 256 ? 256 : (int)clen; memcpy(trns, ch, (unsigned long)ntrns);
        } else if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T') {
            if (idatlen + (int)clen > idatcap) {
                int ncap = (idatlen + (int)clen) * 2 + 4096;
                uint8_t *ni = malloc((unsigned long)ncap);
                if (!ni) { free(idat); return 0; }
                if (idat) { memcpy(ni, idat, (unsigned long)idatlen); free(idat); }
                idat = ni; idatcap = ncap;
            }
            memcpy(idat + idatlen, ch, clen); idatlen += (int)clen;
        } else if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') {
            break;
        }
        pos += 12 + (int)clen;                              // sauter aussi le CRC (4)
    }

    if (w <= 0 || h <= 0 || w > maxw || h > maxh || depth != 8 || interlace != 0 || !idat) { free(idat); return 0; }
    int ch_n = (color==2)?3 : (color==6)?4 : (color==0)?1 : (color==4)?2 : (color==3)?1 : 0;
    if (!ch_n || idatlen < 2) { free(idat); return 0; }

    int stride = w * ch_n;
    int rawcap = (stride + 1) * h;
    uint8_t *raw = malloc((unsigned long)rawcap);
    if (!raw) { free(idat); return 0; }
    infl_t z; memset(&z, 0, sizeof z);
    z.in = idat + 2; z.inlen = idatlen - 2;                 // sauter l'en-tête zlib (2 octets)
    z.out = raw; z.outcap = rawcap;
    int got = inflate(&z);
    free(idat);
    if (got != rawcap) { free(raw); return 0; }

    uint8_t *prev = calloc(1, (unsigned long)stride);
    uint8_t *cur = malloc((unsigned long)stride);
    if (!prev || !cur) { free(raw); free(prev); free(cur); return 0; }
    int bpp = ch_n;
    uint8_t *rp = raw;
    for (int y = 0; y < h; y++) {
        int f = *rp++;
        for (int x = 0; x < stride; x++) {
            int a = x >= bpp ? cur[x - bpp] : 0;
            int b = prev[x];
            int c = x >= bpp ? prev[x - bpp] : 0;
            int v = rp[x], val;
            switch (f) {
                case 1: val = v + a; break;
                case 2: val = v + b; break;
                case 3: val = v + ((a + b) >> 1); break;
                case 4: { int p = a + b - c, pa = iabs(p-a), pb = iabs(p-b), pc = iabs(p-c);
                          int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c; val = v + pr; } break;
                default: val = v; break;
            }
            cur[x] = (uint8_t)val;
        }
        rp += stride;
        for (int x = 0; x < w; x++) {
            uint8_t r, g, bl;
            if (color == 2)      { r = cur[x*3];   g = cur[x*3+1]; bl = cur[x*3+2]; }
            else if (color == 6) { r = cur[x*4];   g = cur[x*4+1]; bl = cur[x*4+2]; }
            else if (color == 0) { r = g = bl = cur[x]; }
            else if (color == 4) { r = g = bl = cur[x*2]; }
            else                 { int idx = cur[x]; r = palette[idx*3]; g = palette[idx*3+1]; bl = palette[idx*3+2]; (void)ntrns; (void)trns; }
            out[y * w + x] = rgb3(r, g, bl);
        }
        uint8_t *t = prev; prev = cur; cur = t;
    }
    free(raw); free(prev); free(cur);
    *ow = w; *oh = h; return 1;
}

// =============================================================================
//  JPEG baseline (portage compact de NanoJPEG, domaine public)
// =============================================================================
typedef struct { uint8_t bits, code; } nj_vlc_t;
typedef struct {
    int cid, ssx, ssy, width, height, stride, qtsel, actabsel, dctabsel, dcpred;
    uint8_t *pixels;
} nj_comp_t;

typedef struct {
    const uint8_t *pos; int size, length;
    int width, height, ncomp;
    int mbwidth, mbheight, mbsizex, mbsizey, ssxmax, ssymax;
    nj_comp_t comp[3];
    int qtused, qtavail;
    uint8_t qtab[4][64];
    nj_vlc_t vlctab[4][65536];
    int buf, bufbits;
    int block[64];
    int rstinterval;
    int error;
} nj_ctx;

enum { NJ_OK = 0, NJ_NOTJPEG, NJ_UNSUP, NJ_SYNTAX, NJ_OOM, NJ_INTERNAL, NJ_FINISHED };

static const uint8_t NJ_ZZ[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };

static inline uint8_t njClip(int x) { return (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x)); }

#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

static void njRowIDCT(int *blk) {
    int x0,x1,x2,x3,x4,x5,x6,x7,x8;
    if (!((x1=blk[4]<<11)|(x2=blk[6])|(x3=blk[2])|(x4=blk[1])|(x5=blk[7])|(x6=blk[5])|(x7=blk[3]))) {
        blk[0]=blk[1]=blk[2]=blk[3]=blk[4]=blk[5]=blk[6]=blk[7]=blk[0]<<3; return;
    }
    x0=(blk[0]<<11)+128;
    x8=W7*(x4+x5);  x4=x8+(W1-W7)*x4;  x5=x8-(W1+W7)*x5;
    x8=W3*(x6+x7);  x6=x8-(W3-W5)*x6;  x7=x8-(W3+W5)*x7;
    x8=x0+x1; x0-=x1;
    x1=W6*(x3+x2);  x2=x1-(W2+W6)*x2;  x3=x1+(W2-W6)*x3;
    x1=x4+x6; x4-=x6; x6=x5+x7; x5-=x7;
    x7=x8+x3; x8-=x3; x3=x0+x2; x0-=x2;
    x2=(181*(x4+x5)+128)>>8; x4=(181*(x4-x5)+128)>>8;
    blk[0]=(x7+x1)>>8; blk[1]=(x3+x2)>>8; blk[2]=(x0+x4)>>8; blk[3]=(x8+x6)>>8;
    blk[4]=(x8-x6)>>8; blk[5]=(x0-x4)>>8; blk[6]=(x3-x2)>>8; blk[7]=(x7-x1)>>8;
}

static void njColIDCT(const int *blk, uint8_t *out, int stride) {
    int x0,x1,x2,x3,x4,x5,x6,x7,x8;
    if (!((x1=blk[8*4]<<8)|(x2=blk[8*6])|(x3=blk[8*2])|(x4=blk[8*1])|(x5=blk[8*7])|(x6=blk[8*5])|(x7=blk[8*3]))) {
        x1=njClip(((blk[0]+32)>>6)+128);
        for (x0=8; x0; --x0) { *out=(uint8_t)x1; out+=stride; }
        return;
    }
    x0=(blk[0]<<8)+8192;
    x8=W7*(x4+x5)+4;  x4=(x8+(W1-W7)*x4)>>3;  x5=(x8-(W1+W7)*x5)>>3;
    x8=W3*(x6+x7)+4;  x6=(x8-(W3-W5)*x6)>>3;  x7=(x8-(W3+W5)*x7)>>3;
    x8=x0+x1; x0-=x1;
    x1=W6*(x3+x2)+4;  x2=(x1-(W2+W6)*x2)>>3;  x3=(x1+(W2-W6)*x3)>>3;
    x1=x4+x6; x4-=x6; x6=x5+x7; x5-=x7;
    x7=x8+x3; x8-=x3; x3=x0+x2; x0-=x2;
    x2=(181*(x4+x5)+128)>>8; x4=(181*(x4-x5)+128)>>8;
    *out=njClip(((x7+x1)>>14)+128); out+=stride;
    *out=njClip(((x3+x2)>>14)+128); out+=stride;
    *out=njClip(((x0+x4)>>14)+128); out+=stride;
    *out=njClip(((x8+x6)>>14)+128); out+=stride;
    *out=njClip(((x8-x6)>>14)+128); out+=stride;
    *out=njClip(((x0-x4)>>14)+128); out+=stride;
    *out=njClip(((x3-x2)>>14)+128); out+=stride;
    *out=njClip(((x7-x1)>>14)+128);
}

static int njShowBits(nj_ctx *nj, int bits) {
    if (!bits) return 0;
    while (nj->bufbits < bits) {
        if (nj->size <= 0) { nj->buf = (nj->buf << 8) | 0xFF; nj->bufbits += 8; continue; }
        uint8_t b = *nj->pos++; nj->size--; nj->bufbits += 8; nj->buf = (nj->buf << 8) | b;
        if (b == 0xFF) {
            if (nj->size) {
                uint8_t marker = *nj->pos++; nj->size--;
                switch (marker) {
                    case 0x00: case 0xFF: break;
                    case 0xD9: nj->size = 0; break;
                    default:
                        if ((marker & 0xF8) != 0xD0) nj->error = NJ_SYNTAX;
                        else { nj->buf = (nj->buf << 8) | marker; nj->bufbits += 8; }
                }
            } else nj->error = NJ_SYNTAX;
        }
    }
    return (nj->buf >> (nj->bufbits - bits)) & ((1 << bits) - 1);
}
static void njSkipBits(nj_ctx *nj, int bits) { if (nj->bufbits < bits) (void)njShowBits(nj, bits); nj->bufbits -= bits; }
static int  njGetBits(nj_ctx *nj, int bits) { int r = njShowBits(nj, bits); njSkipBits(nj, bits); return r; }
static void njByteAlign(nj_ctx *nj) { nj->bufbits &= 0xF8; }

static void njSkip(nj_ctx *nj, int n) { nj->pos += n; nj->size -= n; nj->length -= n; if (nj->size < 0) nj->error = NJ_SYNTAX; }
static uint16_t njDecode16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void njDecodeLength(nj_ctx *nj) {
    if (nj->size < 2) { nj->error = NJ_SYNTAX; return; }
    nj->length = njDecode16(nj->pos);
    if (nj->length > nj->size) { nj->error = NJ_SYNTAX; return; }
    njSkip(nj, 2);                       // njSkip décrémente déjà length de 2
}
static void njSkipMarker(nj_ctx *nj) { njDecodeLength(nj); njSkip(nj, nj->length); }

static int njGetVLC(nj_ctx *nj, nj_vlc_t *vlc, uint8_t *code) {
    int value = njShowBits(nj, 16);
    int bits = vlc[value].bits;
    if (!bits) { nj->error = NJ_SYNTAX; return 0; }
    njSkipBits(nj, bits);
    value = vlc[value].code;
    if (code) *code = (uint8_t)value;
    bits = value & 15;
    if (!bits) return 0;
    value = njGetBits(nj, bits);
    if (value < (1 << (bits - 1))) value += ((-1) << bits) + 1;
    return value;
}

static void njDecodeSOF(nj_ctx *nj) {
    int i; nj_comp_t *c;
    njDecodeLength(nj); if (nj->error) return;
    if (nj->length < 9) { nj->error = NJ_SYNTAX; return; }
    if (nj->pos[0] != 8) { nj->error = NJ_UNSUP; return; }
    nj->height = njDecode16(nj->pos + 1);
    nj->width  = njDecode16(nj->pos + 3);
    if (!nj->width || !nj->height) { nj->error = NJ_SYNTAX; return; }
    nj->ncomp = nj->pos[5];
    njSkip(nj, 6);
    if (nj->ncomp != 1 && nj->ncomp != 3) { nj->error = NJ_UNSUP; return; }
    if (nj->length < nj->ncomp * 3) { nj->error = NJ_SYNTAX; return; }
    nj->ssxmax = nj->ssymax = 0;
    for (i = 0, c = nj->comp; i < nj->ncomp; ++i, ++c) {
        c->cid = nj->pos[0];
        if (!(c->ssx = nj->pos[1] >> 4)) { nj->error = NJ_SYNTAX; return; }
        if (c->ssx & (c->ssx - 1)) { nj->error = NJ_UNSUP; return; }
        if (!(c->ssy = nj->pos[1] & 15)) { nj->error = NJ_SYNTAX; return; }
        if (c->ssy & (c->ssy - 1)) { nj->error = NJ_UNSUP; return; }
        if ((c->qtsel = nj->pos[2]) & 0xFC) { nj->error = NJ_SYNTAX; return; }
        njSkip(nj, 3);
        nj->qtused |= 1 << c->qtsel;
        if (c->ssx > nj->ssxmax) nj->ssxmax = c->ssx;
        if (c->ssy > nj->ssymax) nj->ssymax = c->ssy;
    }
    if (nj->ncomp == 1) { c = nj->comp; c->ssx = c->ssy = nj->ssxmax = nj->ssymax = 1; }
    nj->mbsizex = nj->ssxmax << 3;
    nj->mbsizey = nj->ssymax << 3;
    nj->mbwidth  = (nj->width  + nj->mbsizex - 1) / nj->mbsizex;
    nj->mbheight = (nj->height + nj->mbsizey - 1) / nj->mbsizey;
    for (i = 0, c = nj->comp; i < nj->ncomp; ++i, ++c) {
        c->width  = (nj->width  * c->ssx + nj->ssxmax - 1) / nj->ssxmax;
        c->height = (nj->height * c->ssy + nj->ssymax - 1) / nj->ssymax;
        c->stride = (nj->mbwidth * c->ssx) << 3;
        if (((c->width < 3) && (c->ssx != nj->ssxmax)) || ((c->height < 3) && (c->ssy != nj->ssymax))) { nj->error = NJ_UNSUP; return; }
        c->pixels = malloc((unsigned long)(c->stride * ((nj->mbheight * c->ssy) << 3)));
        if (!c->pixels) { nj->error = NJ_OOM; return; }
    }
    njSkip(nj, nj->length);
}

static void njDecodeDHT(nj_ctx *nj) {
    int codelen, currcnt, remain, spread, i, j;
    nj_vlc_t *vlc; uint8_t counts[16];
    njDecodeLength(nj); if (nj->error) return;
    while (nj->length >= 17) {
        i = nj->pos[0];
        if (i & 0xEC) { nj->error = NJ_SYNTAX; return; }
        if (i & 0x02) { nj->error = NJ_UNSUP; return; }
        i = (i | (i >> 3)) & 3;
        for (codelen = 1; codelen <= 16; ++codelen) counts[codelen - 1] = nj->pos[codelen];
        njSkip(nj, 17);
        vlc = &nj->vlctab[i][0];
        remain = spread = 65536;
        for (codelen = 1; codelen <= 16; ++codelen) {
            spread >>= 1;
            currcnt = counts[codelen - 1];
            if (!currcnt) continue;
            if (nj->length < currcnt) { nj->error = NJ_SYNTAX; return; }
            remain -= currcnt << (16 - codelen);
            if (remain < 0) { nj->error = NJ_SYNTAX; return; }
            for (i = 0; i < currcnt; ++i) {
                uint8_t code = nj->pos[i];
                for (j = spread; j; --j) { vlc->bits = (uint8_t)codelen; vlc->code = code; ++vlc; }
            }
            njSkip(nj, currcnt);
        }
        while (remain--) { vlc->bits = 0; ++vlc; }
    }
    if (nj->length) nj->error = NJ_SYNTAX;
}

static void njDecodeDQT(nj_ctx *nj) {
    int i; uint8_t *t;
    njDecodeLength(nj); if (nj->error) return;
    while (nj->length >= 65) {
        i = nj->pos[0];
        if (i & 0xFC) { nj->error = NJ_SYNTAX; return; }
        nj->qtavail |= 1 << i;
        t = &nj->qtab[i][0];
        for (i = 0; i < 64; ++i) t[i] = nj->pos[i + 1];
        njSkip(nj, 65);
    }
    if (nj->length) nj->error = NJ_SYNTAX;
}

static void njDecodeDRI(nj_ctx *nj) {
    njDecodeLength(nj); if (nj->error) return;
    if (nj->length < 2) { nj->error = NJ_SYNTAX; return; }
    nj->rstinterval = njDecode16(nj->pos);
    njSkip(nj, nj->length);
}

static void njDecodeBlock(nj_ctx *nj, nj_comp_t *c, uint8_t *out) {
    uint8_t code = 0; int value, coef = 0;
    memset(nj->block, 0, sizeof nj->block);
    c->dcpred += njGetVLC(nj, &nj->vlctab[c->dctabsel][0], 0);
    nj->block[0] = c->dcpred * nj->qtab[c->qtsel][0];
    do {
        value = njGetVLC(nj, &nj->vlctab[c->actabsel][0], &code);
        if (!code) break;
        if (!(code & 0x0F) && (code != 0xF0)) { nj->error = NJ_SYNTAX; return; }
        coef += (code >> 4) + 1;
        if (coef > 63) { nj->error = NJ_SYNTAX; return; }
        nj->block[(int)NJ_ZZ[coef]] = value * nj->qtab[c->qtsel][coef];
    } while (coef < 63);
    for (coef = 0; coef < 64; coef += 8) njRowIDCT(&nj->block[coef]);
    for (coef = 0; coef < 8; ++coef) njColIDCT(&nj->block[coef], &out[coef], c->stride);
}

static void njDecodeScan(nj_ctx *nj) {
    int i, mbx, mby, sbx, sby;
    int rstcount = nj->rstinterval, nextrst = 0;
    nj_comp_t *c;
    njDecodeLength(nj); if (nj->error) return;
    if (nj->length < 4 + 2 * nj->ncomp) { nj->error = NJ_SYNTAX; return; }
    if (nj->pos[0] != nj->ncomp) { nj->error = NJ_UNSUP; return; }
    njSkip(nj, 1);
    for (i = 0, c = nj->comp; i < nj->ncomp; ++i, ++c) {
        if (nj->pos[0] != c->cid) { nj->error = NJ_SYNTAX; return; }
        if (nj->pos[1] & 0xEE) { nj->error = NJ_SYNTAX; return; }
        c->dctabsel = nj->pos[1] >> 4;
        c->actabsel = (nj->pos[1] & 1) | 2;
        njSkip(nj, 2);
    }
    if (nj->pos[0] || (nj->pos[1] != 63) || nj->pos[2]) { nj->error = NJ_UNSUP; return; }
    njSkip(nj, 3);
    for (mby = 0; mby < nj->mbheight; ++mby)
        for (mbx = 0; mbx < nj->mbwidth; ++mbx) {
            for (i = 0, c = nj->comp; i < nj->ncomp; ++i, ++c)
                for (sby = 0; sby < c->ssy; ++sby)
                    for (sbx = 0; sbx < c->ssx; ++sbx) {
                        njDecodeBlock(nj, c, &c->pixels[((mby * c->ssy + sby) * c->stride + mbx * c->ssx + sbx) << 3]);
                        if (nj->error) return;
                    }
            if (nj->rstinterval && !(--rstcount)) {
                njByteAlign(nj);
                i = njGetBits(nj, 16);
                if (((i & 0xFFF8) != 0xFFD0) || ((i & 7) != nextrst)) { nj->error = NJ_SYNTAX; return; }
                nextrst = (nextrst + 1) & 7;
                rstcount = nj->rstinterval;
                for (i = 0; i < 3; ++i) nj->comp[i].dcpred = 0;
            }
        }
    nj->error = NJ_FINISHED;
}

static int decode_jpeg(const uint8_t *d, int n, uint32_t *out, int maxw, int maxh, int *ow, int *oh) {
    if (n < 2 || d[0] != 0xFF || d[1] != 0xD8) return 0;
    nj_ctx *nj = calloc(1, sizeof(nj_ctx));
    if (!nj) return 0;
    nj->pos = d + 2; nj->size = n - 2;

    while (!nj->error) {
        if (nj->size < 2 || nj->pos[0] != 0xFF) { nj->error = NJ_SYNTAX; break; }
        nj->pos += 2; nj->size -= 2;
        switch (nj->pos[-1]) {
            case 0xC0: njDecodeSOF(nj); break;
            case 0xC4: njDecodeDHT(nj); break;
            case 0xDB: njDecodeDQT(nj); break;
            case 0xDD: njDecodeDRI(nj); break;
            case 0xDA: njDecodeScan(nj); break;
            case 0xFE: njSkipMarker(nj); break;
            case 0xC1: case 0xC2: case 0xC3: nj->error = NJ_UNSUP; break;  // progressif/étendu
            default:
                if ((nj->pos[-1] & 0xF0) == 0xE0) njSkipMarker(nj);
                else nj->error = NJ_UNSUP;
        }
    }
    int ok = 0;
    if (nj->error == NJ_FINISHED && nj->width > 0 && nj->height > 0 &&
        nj->width <= maxw && nj->height <= maxh) {
        int w = nj->width, h = nj->height;
        nj_comp_t *Y = &nj->comp[0], *Cb = &nj->comp[1], *Cr = &nj->comp[2];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int yy = Y->pixels[(y * Y->ssy / nj->ssymax) * Y->stride + (x * Y->ssx / nj->ssxmax)];
                uint8_t r, g, b;
                if (nj->ncomp == 1) { r = g = b = (uint8_t)yy; }
                else {
                    int cb = Cb->pixels[(y * Cb->ssy / nj->ssymax) * Cb->stride + (x * Cb->ssx / nj->ssxmax)] - 128;
                    int cr = Cr->pixels[(y * Cr->ssy / nj->ssymax) * Cr->stride + (x * Cr->ssx / nj->ssxmax)] - 128;
                    int Y8 = yy << 8;
                    r = njClip((Y8 + 359 * cr + 128) >> 8);
                    g = njClip((Y8 - 88 * cb - 183 * cr + 128) >> 8);
                    b = njClip((Y8 + 454 * cb + 128) >> 8);
                }
                out[y * w + x] = rgb3(r, g, b);
            }
        }
        *ow = w; *oh = h; ok = 1;
    }
    for (int i = 0; i < 3; i++) free(nj->comp[i].pixels);
    free(nj);
    return ok;
}

// =============================================================================
//  BMP 24 bits et PPM (P6)
// =============================================================================
static uint32_t le32(const uint8_t *p) { return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }

static int decode_bmp(const uint8_t *d, int n, uint32_t *out, int maxw, int maxh, int *ow, int *oh) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return 0;
    uint32_t off = le32(d + 10);
    int w = (int)le32(d + 18), h = (int)le32(d + 22);
    int bpp = d[28] | (d[29] << 8);
    if (bpp != 24 || w <= 0 || w > maxw) return 0;
    int flip = h > 0; if (h < 0) h = -h;
    if (h <= 0 || h > maxh) return 0;
    int rowsz = (w * 3 + 3) & ~3;
    for (int y = 0; y < h; y++) {
        int sy = flip ? (h - 1 - y) : y;
        const uint8_t *row = d + off + (uint32_t)sy * rowsz;
        if (row + w * 3 > d + n) break;
        for (int x = 0; x < w; x++)
            out[y * w + x] = rgb3(row[x*3+2], row[x*3+1], row[x*3+0]);
    }
    *ow = w; *oh = h; return 1;
}

static int decode_ppm(const uint8_t *d, int n, uint32_t *out, int maxw, int maxh, int *ow, int *oh) {
    if (n < 10 || d[0] != 'P' || d[1] != '6') return 0;
    int i = 2, v[3], k = 0;
    while (k < 3 && i < n) {
        while (i < n && (d[i]==' '||d[i]=='\n'||d[i]=='\t'||d[i]=='\r')) i++;
        if (d[i] == '#') { while (i < n && d[i] != '\n') i++; continue; }
        int s = i; while (i < n && d[i] > ' ') i++;
        int val = 0; for (int j = s; j < i; j++) val = val * 10 + (d[j] - '0'); v[k++] = val;
    }
    i++;
    int w = v[0], h = v[1];
    if (w <= 0 || w > maxw || h <= 0 || h > maxh) return 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int o = i + (y * w + x) * 3;
            if (o + 2 >= n) { *ow = w; *oh = y; return 1; }
            out[y * w + x] = rgb3(d[o], d[o+1], d[o+2]);
        }
    *ow = w; *oh = h; return 1;
}

// =============================================================================
//  Aiguillage
// =============================================================================
int img_decode(const uint8_t *d, int n, uint32_t *out, int maxw, int maxh, int *w, int *h) {
    if (n < 4) return 0;
    if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return decode_png(d, n, out, maxw, maxh, w, h);
    if (d[0] == 0xFF && d[1] == 0xD8)                              return decode_jpeg(d, n, out, maxw, maxh, w, h);
    if (d[0] == 'B'  && d[1] == 'M')                              return decode_bmp(d, n, out, maxw, maxh, w, h);
    if (d[0] == 'P'  && d[1] == '6')                              return decode_ppm(d, n, out, maxw, maxh, w, h);
    return 0;
}
