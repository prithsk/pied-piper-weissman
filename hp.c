// hp: a lossless compressor ("Pied Piper, but real").
// Usage: hp c [-1|-9] in out | hp d in out
//   -9 (default): context mixing, best ratio, ~0.2 MB/s
//   -1: LZ77 + adaptive range coder, fast
// Model: hashed order-N contexts -> adaptive bit predictors -> gated
// logistic mixer -> two SSE stages -> binary arithmetic coder.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static void *xalloc(size_t n, int zero) {
  void *p = zero ? calloc(n ? n : 1, 1) : malloc(n ? n : 1);
  if (!p) { fprintf(stderr, "hp: out of memory (%zu bytes)\n", n); exit(3); }
  return p;
}
typedef uint8_t U8; typedef uint16_t U16; typedef uint32_t U32; typedef uint64_t U64;

// ---------------- numeric helpers ----------------
static int squash_t[4096];   // stretch domain (-2047..2047) -> 12-bit prob
static short stretch_t[4096];// 12-bit prob -> stretch domain

static int squash_calc(int d) {  // logistic, integer interpolation
  static const int t[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,
    2047,2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094};
  if (d > 2047) return 4095;
  if (d < -2047) return 1;
  int w = d & 127; d = (d >> 7) + 16;
  return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
}
static inline int squash(int d) {
  if (d > 2047) d = 2047; if (d < -2047) d = -2047;
  return squash_t[d + 2048];
}
static inline int stretch(int p) { return stretch_t[p]; }

static void e8e9(U8 *d, int64_t n, int fwd) {
  for (int64_t i = 0; i + 4 < n; i++) {
    if ((d[i] & 0xfe) == 0xe8) {
      U32 a = d[i+1] | d[i+2] << 8 | d[i+3] << 16 | (U32)d[i+4] << 24;
      a = fwd ? a + (U32)i : a - (U32)i;
      d[i+1] = a; d[i+2] = a >> 8; d[i+3] = a >> 16; d[i+4] = a >> 24;
      i += 4;
    }
  }
}
static int looks_x86(const U8 *d, int64_t n) {  // E8/E9 calls whose offset top byte is 0x00/0xFF
  int64_t hit = 0, all = 0;
  int64_t seen = 0;  // sample the first 4 KB of every 64 KB
  for (int64_t blk = 0; blk < n; blk += 65536)
    for (int64_t i = blk; i < blk + 4096 && i + 4 < n; i++, seen++)
      if ((d[i] & 0xfe) == 0xe8) { all++; if (d[i+4] == 0 || d[i+4] == 0xff) hit++; }
  return n > 4096 && all * 1000 > seen * 4 && hit * 10 > all * 6;
}
static void init_tables(void) {
  for (int i = 0; i < 4096; i++) squash_t[i] = squash_calc(i - 2048);
  int pi = 0;
  for (int x = -2047; x <= 2047; x++) {  // invert squash
    int v = squash_calc(x);
    for (int j = pi; j <= v; j++) stretch_t[j] = x;
    pi = v + 1;
  }
  for (int j = pi; j < 4096; j++) stretch_t[j] = 2047;
}

static inline U32 hash2(U32 a, U32 b) {
  U64 x = (((U64)a << 32) | b) * 0x9E3779B97F4A7C15ull;
  x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
  return (U32)x;
}

// ---------------- adaptive probability counters ----------------
// Each counter: high 22 bits = P(1), low 10 bits = hit count (caps the rate).
static int dt[1024];
static void init_dt(void) { for (int i = 0; i < 1024; i++) dt[i] = 131072 / (2 * i + 3); }
static inline void cnt_update(U32 *e, int y, int limit) {  // 22-bit p, 10-bit n (direct tables)
  U32 v = *e; int n = v & 1023; int p = v >> 10;
  int target = y ? (1 << 22) - 1 : 0;
  p += (int)(((int64_t)(target - p) * dt[n]) >> 16);
  if (n < limit) n++;
  *e = ((U32)p << 10) | n;
}
#define CNT_INIT ((U32)(1u << 31))  // p = 0.5, n = 0
// hashed-slot entries: p20 | n6 | lastbit1 | run5
static inline void ent_update(U32 *e, int y, int limit) {
  U32 v = *e; int p = v >> 12, n = (v >> 6) & 63, lb = (v >> 5) & 1, run = v & 31;
  int target = y ? (1 << 20) - 1 : 0;
  p += (int)(((int64_t)(target - p) * dt[n]) >> 16);
  if (n == 0) { lb = y; run = 1; }
  else if (y == lb) { if (run < 31) run++; }
  else { lb = y; run = 1; }
  if (n < limit) n++;
  *e = ((U32)p << 12) | (n << 6) | (lb << 5) | run;
}
#define ENT_INIT ((U32)(1u << 31))

// ---------------- mixer ----------------
#define MAXIN 48
#define WMAX (1 << 24)
typedef struct { int n, nsets; int *w; int x[MAXIN]; int nx; int sel; int pr; int lr; } Mixer;
static void mx_init(Mixer *m, int n, int nsets, int lr) {
  m->n = n; m->nsets = nsets; m->lr = lr; m->nx = 0;
  m->w = xalloc(sizeof(int) * n * nsets, 0);
  for (int i = 0; i < n * nsets; i++) m->w[i] = (1 << 16) / 4;
}
static inline void mx_add(Mixer *m, int x) { m->x[m->nx++] = x; }
static inline int mx_dot(Mixer *m, int sel) {
  m->sel = sel; int *w = m->w + sel * m->n; int64_t s = 0;
  for (int i = 0; i < m->nx; i++) s += (int64_t)m->x[i] * w[i];
  int d = (int)(s >> 16);
  if (d > 2047) d = 2047; if (d < -2047) d = -2047;
  return d;
}
static inline void mx_train(Mixer *m, int y, int pr) {
  int err = ((y << 12) - pr) * m->lr;
  int *w = m->w + m->sel * m->n;
  for (int i = 0; i < m->nx; i++) {
    int v = w[i] + ((m->x[i] * err + (1 << 13)) >> 14);
    w[i] = v > WMAX ? WMAX : v < -WMAX ? -WMAX : v;
  }
}

// ---------------- APM / SSE ----------------
typedef struct { U16 *t; int idx; int rate; } APM;
static void apm_init(APM *a, int n, int rate) {
  a->t = xalloc(sizeof(U16) * n * 33, 0); a->rate = rate;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < 33; j++) a->t[i * 33 + j] = squash((j - 16) * 128) * 16;
}
static inline int apm_p(APM *a, int pr, int cx) {
  int s = stretch(pr) + 2048; int lo = s >> 7, w = s & 127;
  a->idx = cx * 33 + lo + (w >> 6);
  return (a->t[cx * 33 + lo] * (128 - w) + a->t[cx * 33 + lo + 1] * w) >> 11;
}
static inline void apm_update(APM *a, int y) {
  int g = (y << 16) + (y << a->rate) - y - y;
  a->t[a->idx] += (g - a->t[a->idx]) >> a->rate;
}

// ---------------- model ----------------
// Fixed model settings. Earlier versions read these from the environment for
// ablations (see experiments/); that let a mismatched environment corrupt output.
#define LR 10
#define LIM 30
#define ON(b) 1
static int TBITS = 24;
#define NCTX 16
typedef struct {
  U8 *buf; U32 pos, n;              // history (entire input)
  U32 c0, c4, c8, bitpos;           // partial byte, last bytes
  U32 h[NCTX];                      // per-byte context hashes
  U32 *slot[NCTX];                  // current nibble slot per context
  U32 *T; U32 tmask;                // shared hash table of 16-entry slots
  U32 *ent[NCTX];                   // counter used for current bit
  U32 rm[NCTX][64], *rent[NCTX], rdummy;  // run-length -> probability maps
  int nctx;
  U32 t0[256 * 256];                // direct order-1 (c1, c0)
  // match model
  U32 *mt; U32 mmask; U32 mptr, mlen; int mexp; U32 mcnt[64 * 2];
  // record model
  U32 lastpos[256][4]; U32 rlen, rcand, rcount;
  // word model state
  U32 word, word1;
  Mixer m1, m2, m3, mf; APM a1, a2, a3;
  int pr, pr_mix;
  int mstate;
} Model;

static void model_init(Model *M, U8 *buf, U32 n, int tbits) {
  memset(M, 0, sizeof(*M));
  M->buf = buf; M->n = n; M->c0 = 1;
  M->tmask = (1u << tbits) - 1;
  M->T = xalloc(((size_t)16 << tbits) * sizeof(U32), 1);
  for (int i = 0; i < 256 * 256; i++) M->t0[i] = CNT_INIT;
  M->mmask = (1u << 22) - 1; M->mt = xalloc(((size_t)M->mmask + 1) * sizeof(U32), 1);
  for (int i = 0; i < 128; i++) M->mcnt[i] = CNT_INIT;
  for (int i = 0; i < NCTX; i++) for (int j = 0; j < 64; j++) M->rm[i][j] = CNT_INIT;
  for (int i = 0; i < NCTX; i++) M->rent[i] = &M->rdummy;
  M->nctx = NCTX;
  static U32 dummy[16];
  for (int i = 0; i < NCTX; i++) { M->slot[i] = dummy; M->ent[i] = dummy + 1; }
  int ninputs = 2 * NCTX + 4;
  mx_init(&M->m1, ninputs, 256, LR);
  mx_init(&M->m2, ninputs, 2048, LR);
  mx_init(&M->m3, ninputs, 256 * 8, LR);
  mx_init(&M->mf, 4, 256, 2);
  for (int i = 0; i < 4 * 256; i++) M->mf.w[i] = (1 << 16) / 3;
  apm_init(&M->a1, 256, 7);
  apm_init(&M->a2, 65536, 7);
  apm_init(&M->a3, 65536, 7);
  M->pr = 2048;
}

static inline U32 *find_slot(Model *M, U32 h) {
  U32 idx = h & M->tmask, chk = (h >> 8) | 1;
  U32 *s = M->T + ((size_t)idx << 4);
  if (s[0] != chk) {
    // try neighbor slot (2-way), replace the one with less use
    U32 *s2 = M->T + ((size_t)(idx ^ 1) << 4);
    if (s2[0] == chk) return s2;
    U32 u1 = (s[1] >> 6) & 63, u2 = (s2[1] >> 6) & 63;
    U32 *r = (u2 < u1) ? s2 : s;
    r[0] = chk; for (int j = 1; j < 16; j++) r[j] = ENT_INIT;
    return r;
  }
  return s;
}

static void byte_update(Model *M) {
  U32 c4 = M->c4, c8 = M->c8; U8 c = c4 & 255;
  // word hash (letters only, case folded)
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 128) {
    U32 lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    M->word = hash2(M->word + lc, 0x1234);
  } else if (M->word) { M->word1 = M->word; M->word = 0; }

  int i = 0;
  M->h[i++] = hash2(c4 & 0xffff, 2);
  M->h[i++] = hash2(c4 & 0xffffff, 3);
  M->h[i++] = hash2(c4, 4);
  M->h[i++] = hash2(hash2(c4, c8 & 0xff), 5);
  M->h[i++] = hash2(hash2(c4, c8 & 0xffff), 6);
  M->h[i++] = hash2(hash2(c4, c8), 8);
  M->h[i++] = hash2(M->word, 20 + (c4 & 0xff) * 0);
  M->h[i++] = hash2(hash2(M->word, M->word1), 21);
  M->h[i++] = hash2(c4 & 0xff00, 30);          // sparse: byte -2
  M->h[i++] = hash2(c4 & 0xff0000, 31);        // sparse: byte -3
  M->h[i++] = hash2(c4 & 0xffff0000, 32);      // sparse: bytes -3,-4
  M->h[i++] = hash2(c4 & 0xff00ff00, 33);      // sparse: bytes -2,-4
  M->h[i++] = hash2(c4 & 0xf0f0f0f0, 34);      // high nibbles (binary data)
  // record (fixed-width row) detection
  { U32 *lp = M->lastpos[c]; U32 p0 = M->pos;
    U32 r = p0 - lp[0];
    if (lp[2] && r == lp[0] - lp[1] && r == lp[1] - lp[2] && r > 1) {
      if (r == M->rcand) { if (++M->rcount > 8 && M->rlen != r) M->rlen = r; }
      else { M->rcand = r; M->rcount = 0; }
    }
    lp[3] = lp[2]; lp[2] = lp[1]; lp[1] = lp[0]; lp[0] = p0; }
  { U32 r = M->rlen, p0 = M->pos;
    U32 a = (r && p0 >= r) ? M->buf[p0 - r] : 0, a2 = (r && p0 >= 2 * r) ? M->buf[p0 - 2 * r] : 0;
    U32 ap = (r && p0 >= r - 1 && r > 1) ? M->buf[p0 - r + 1] : 0;
    M->h[i++] = hash2(hash2(a, c4 & 0xff), 40 + r * 64);
    M->h[i++] = hash2(hash2(a, a2), 41 + r * 64);
    M->h[i++] = hash2(hash2(a, ap << 8 | (c4 & 0xff)), 42 + r * 64);
  }

  // match model
  U32 pos = M->pos;
  if (M->mlen > 0 && M->mptr < pos && M->buf[M->mptr] == c) { M->mlen++; M->mptr++; if (M->mlen > 65535) M->mlen = 65535; }
  else M->mlen = 0;
  if (pos >= 6) {
    U32 hm = hash2(c4, c8 & 0xffff) & M->mmask;
    if (M->mlen == 0) {
      U32 p = M->mt[hm];
      if (p > 0) {
        U32 l = 0;
        while (l < 64 && l < p && M->buf[p - 1 - l] == M->buf[pos - 1 - l]) l++;
        if (l >= 6) { M->mlen = l; M->mptr = p; }
      }
    }
    M->mt[hm] = pos;
  }
}

static inline int ilog_bucket(U32 l) {  // 0..15
  if (l == 0) return 0; if (l < 16) return l < 8 ? 1 + (l >> 1) : 5 + ((l - 8) >> 1);
  if (l < 32) return 9; if (l < 64) return 10; if (l < 128) return 11; if (l < 256) return 12;
  if (l < 1024) return 13; if (l < 4096) return 14; return 15;
}

static int predict(Model *M) {
  U32 c0 = M->c0;
  Mixer *m1 = &M->m1, *m2 = &M->m2;
  m1->nx = 0;
  // nibble boundary: (re)locate slots
  if (M->bitpos == 0 || M->bitpos == 4) {
    U32 hh[NCTX];
    for (int i = 0; i < M->nctx; i++) {
      hh[i] = hash2(M->h[i], c0);
      __builtin_prefetch(M->T + ((size_t)(hh[i] & M->tmask) << 4));
      __builtin_prefetch(M->T + ((size_t)((hh[i] & M->tmask) ^ 1) << 4));
    }
    for (int i = 0; i < M->nctx; i++) if (ON(i)) M->slot[i] = find_slot(M, hh[i]);
  }
  U32 j = (M->bitpos < 4) ? (c0 & ((1u << M->bitpos) - 1)) | (1u << M->bitpos)
                          : ((c0 & ((1u << (M->bitpos - 4)) - 1)) | (1u << (M->bitpos - 4)));
  for (int i = 0; i < M->nctx; i++) {
    U32 *e = M->slot[i] + j; M->ent[i] = e;
    U32 v = *e; int n = (v >> 6) & 63;
    if (n && ON(i)) {
      mx_add(m1, stretch(v >> 20));
      U32 *r = &M->rm[i][((v & 31) << 1) | ((v >> 5) & 1)]; M->rent[i] = r;
      mx_add(m1, stretch(*r >> 20));
    } else { mx_add(m1, 0); mx_add(m1, 0); M->rent[i] = &M->rdummy; }
  }
  // order-1 direct
  U32 *e0 = &M->t0[((M->c4 & 255) << 8) | c0];
  mx_add(m1, ON(14) ? stretch(*e0 >> 20) : 0);
  // match model
  int mb = 0;
  if (!ON(15)) M->mlen = 0;
  if (M->mlen > 0) {
    U32 pb = M->buf[M->mptr];
    U32 hi = (pb + 256) >> (8 - M->bitpos);
    if (hi == c0) {
      M->mexp = (pb >> (7 - M->bitpos)) & 1;
      mb = ilog_bucket(M->mlen);
    } else M->mlen = 0;
  }
  U32 *me = &M->mcnt[mb * 2 + (mb ? M->mexp : 0)];
  int mp = stretch(*me >> 20);
  mx_add(m1, mb ? mp : 0);
  mx_add(m1, mb ? (M->mexp ? 1 : -1) * (mb * 64) : 0);
  mx_add(m1, 256);  // bias
  M->mstate = mb * 2 + (mb ? M->mexp : 0);
  // second mixer shares inputs
  memcpy(m2->x, m1->x, sizeof(int) * m1->nx); m2->nx = m1->nx;
  Mixer *m3 = &M->m3, *mf = &M->mf;
  memcpy(m3->x, m1->x, sizeof(int) * m1->nx); m3->nx = m1->nx;
  int d1 = mx_dot(m1, c0);
  int d2 = mx_dot(m2, M->mstate * 64 + ((M->c4 & 0xff) >> 2));
  int p1 = squash(d1), p2 = squash(d2);
  m1->pr = p1; m2->pr = p2;
  int d3 = mx_dot(m3, ((M->c4 >> 8) & 0xff) * 8 + (M->bitpos));
  m3->pr = squash(d3);
  mf->nx = 0; mx_add(mf, d1); mx_add(mf, d2); mx_add(mf, d3); mx_add(mf, 256);
  int df = mx_dot(mf, c0); mf->pr = squash(df);
  int pm = ON(16) ? mf->pr : p1;
  M->pr_mix = pm;
  // SSE
  int q1 = apm_p(&M->a1, pm, c0);
  int q2 = apm_p(&M->a2, pm, c0 | ((M->c4 & 0xff) << 8));
  int q3 = apm_p(&M->a3, pm, (hash2(M->c4 & 0xffff, c0) & 0xffff));
  int p = ON(17) ? (pm * 2 + q1 * 2 + q2 * 2 + q3 * 2) >> 3 : pm;
  if (p < 1) p = 1; if (p > 4095) p = 4095;
  M->pr = p;
  return p;
}

static void update(Model *M, int y) {
  for (int i = 0; i < M->nctx; i++) { ent_update(M->ent[i], y, LIM); cnt_update(M->rent[i], y, 1023); }
  cnt_update(&M->t0[((M->c4 & 255) << 8) | M->c0], y, 255);
  cnt_update(&M->mcnt[M->mstate], y, 1023);
  mx_train(&M->m1, y, M->m1.pr);
  mx_train(&M->m2, y, M->m2.pr);
  mx_train(&M->m3, y, M->m3.pr);
  mx_train(&M->mf, y, M->mf.pr);
  apm_update(&M->a1, y); apm_update(&M->a2, y); apm_update(&M->a3, y);
  M->c0 = (M->c0 << 1) | y;
  M->bitpos++;
  if (M->bitpos == 8) {
    U8 c = M->c0 & 255;
    M->buf[M->pos++] = c;
    M->c8 = (M->c8 << 8) | (M->c4 >> 24);
    M->c4 = (M->c4 << 8) | c;
    M->c0 = 1; M->bitpos = 0;
    byte_update(M);
  }
}

// ---------------- byte buffers (bounds-checked input) ----------------
typedef struct { U8 *p; size_t n, cap; } Buf;
static void buf_reserve(Buf *b, size_t need) {
  if (need <= b->cap) return;
  size_t c = b->cap ? b->cap : (1u << 16);
  while (c < need) c *= 2;
  U8 *q = realloc(b->p, c);
  if (!q) { fprintf(stderr, "hp: out of memory\n"); exit(3); }
  b->p = q; b->cap = c;
}
static inline void bput(Buf *b, U8 c) { if (b->n == b->cap) buf_reserve(b, b->n + 1); b->p[b->n++] = c; }
typedef struct { const U8 *p; size_t n, i; int eof; } In;
static inline int bget(In *r) { if (r->i < r->n) return r->p[r->i++]; r->eof = 1; return 0; }

// ---------------- CRC32C (hardware when available, same result either way) ----------------
static U32 crc_t[256];
static void crc_init(void) {
  for (U32 i = 0; i < 256; i++) { U32 c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1; crc_t[i] = c; }
}
static U32 crc32c(const U8 *p, size_t n) {
  U32 c = ~0u;
#if defined(__SSE4_2__) && defined(__x86_64__) && !defined(HP_SOFT_CRC)
  U64 c64 = c;
  while (n >= 8) { U64 v; memcpy(&v, p, 8); c64 = __builtin_ia32_crc32di(c64, v); p += 8; n -= 8; }
  c = (U32)c64;
  while (n--) c = __builtin_ia32_crc32qi(c, *p++);
#else
  while (n--) c = crc_t[(c ^ *p++) & 255] ^ (c >> 8);
#endif
  return ~c;
}

// ---------------- CM mode: binary arithmetic coder ----------------
typedef struct { U32 x1, x2, x; Buf *o; In *in; } AC;
static inline void enc_bit(AC *a, int y, int p) {
  U32 xm = a->x1 + (U32)(((U64)(a->x2 - a->x1) * p) >> 12);
  if (y) a->x2 = xm; else a->x1 = xm + 1;
  while (((a->x1 ^ a->x2) & 0xff000000u) == 0) {
    bput(a->o, a->x2 >> 24); a->x1 <<= 8; a->x2 = (a->x2 << 8) | 255;
  }
}
static inline int dec_bit(AC *a, int p) {
  U32 xm = a->x1 + (U32)(((U64)(a->x2 - a->x1) * p) >> 12);
  int y = a->x <= xm;
  if (y) a->x2 = xm; else a->x1 = xm + 1;
  while (((a->x1 ^ a->x2) & 0xff000000u) == 0) {
    a->x1 <<= 8; a->x2 = (a->x2 << 8) | 255;
    a->x = (a->x << 8) | bget(a->in);
  }
  return y;
}
static Model CM;
static void cm_compress(const U8 *data, U32 n, Buf *o) {
  U8 *hist = xalloc((size_t)n + 1, 0);
  model_init(&CM, hist, n, TBITS);
  AC a = {0, 0xffffffffu, 0, o, 0};
  for (U32 i = 0; i < n; i++) {
    int c = data[i];
    for (int b = 7; b >= 0; b--) { int y = (c >> b) & 1; enc_bit(&a, y, predict(&CM)); update(&CM, y); }
  }
  bput(o, a.x1 >> 24); bput(o, 255); bput(o, 255); bput(o, 255);
}
static int cm_decompress(In *in, U8 *out, U32 n) {  // 0 = ok
  model_init(&CM, out, n, TBITS);
  AC a = {0, 0xffffffffu, 0, 0, in};
  for (int i = 0; i < 4; i++) a.x = (a.x << 8) | bget(in);
  for (U32 i = 0; i < n; i++) {
    for (int b = 0; b < 8; b++) { int y = dec_bit(&a, predict(&CM)); update(&CM, y); }
    if (in->eof) return 1;  // a valid stream never reads past its end
  }
  return 0;
}

// ---------------- LZ mode: LZ77 + adaptive binary range coder ----------------
#define PB 11
#define PMAX (1 << PB)
#define PMOVE 5
#define MINLEN 4
#define MAXLEN 273
typedef U16 P;

typedef struct { U64 low; U32 range; U8 cache; U64 csize; Buf *o; } RE;
static void re_init(RE *e, Buf *o) { e->low = 0; e->range = 0xFFFFFFFFu; e->cache = 0; e->csize = 1; e->o = o; }
static inline void re_shift(RE *e) {
  if ((U32)e->low < 0xFF000000u || (e->low >> 32)) {
    U8 carry = (U8)(e->low >> 32), t = e->cache;
    do { bput(e->o, (U8)(t + carry)); t = 0xFF; } while (--e->csize);
    e->cache = (U8)(e->low >> 24);
  }
  e->csize++;
  e->low = (e->low & 0x00FFFFFFu) << 8;
}
static inline void re_bit(RE *e, P *p, int b) {
  U32 v = *p, bound = (e->range >> PB) * v, mask = 0u - (U32)b;
  e->low += bound & mask;
  e->range = b ? e->range - bound : bound;
  *p = (P)(b ? v - (v >> PMOVE) : v + ((PMAX - v) >> PMOVE));
  if (e->range < (1u << 24)) { e->range <<= 8; re_shift(e); }
}
static inline void re_direct(RE *e, U32 v, int nb) {  // uniform bits, up to 8 per step
  while (nb > 0) {
    int k = nb < 8 ? nb : 8; nb -= k;
    e->range >>= k;
    e->low += (U64)((v >> nb) & ((1u << k) - 1)) * e->range;
    while (e->range < (1u << 24)) { e->range <<= 8; re_shift(e); }
  }
}
static void re_flush(RE *e) { for (int i = 0; i < 5; i++) re_shift(e); }

typedef struct { U32 range, code; In *in; } RD;
static int rd_init(RD *d, In *in) {
  d->range = 0xFFFFFFFFu; d->code = 0; d->in = in;
  if (bget(in) != 0) return 1;
  for (int i = 0; i < 4; i++) d->code = (d->code << 8) | bget(in);
  return 0;
}
static inline int rd_bit(RD *d, P *p) {
  U32 bound = (d->range >> PB) * *p; int b;
  if (d->code < bound) { d->range = bound; *p += (PMAX - *p) >> PMOVE; b = 0; }
  else { d->code -= bound; d->range -= bound; *p -= *p >> PMOVE; b = 1; }
  if (d->range < (1u << 24)) { d->range <<= 8; d->code = (d->code << 8) | bget(d->in); }
  return b;
}
static inline U32 rd_direct(RD *d, int nb, int *bad) {
  U32 v = 0;
  while (nb > 0) {
    int k = nb < 8 ? nb : 8; nb -= k;
    d->range >>= k;
    U32 q = d->code / d->range;
    if (q >> k) { *bad = 1; q = 0; }  // only possible for a corrupt stream
    d->code -= q * d->range;
    v = (v << k) | q;
    while (d->range < (1u << 24)) { d->range <<= 8; d->code = (d->code << 8) | bget(d->in); }
  }
  return v;
}

typedef struct { P choice, choice2, low[4][8], mid[4][8], high[256]; } LenM;
typedef struct {
  P is_match[9][4], is_rep[9];
  P lit[8][0x300];
  LenM mlen, rlen;
  P slot[4][64], spec[14][32], align[16];
} LZM;
static void lzm_init(LZM *m) { P *p = (P *)m; for (size_t i = 0; i < sizeof(LZM) / sizeof(P); i++) p[i] = PMAX / 2; }

// one coder body for both directions: ENC != 0 encodes v, else decodes and returns it
#define TREE_ENC(e, probs, nb, v) do { U32 m_ = 1; for (int i_ = (nb) - 1; i_ >= 0; i_--) { int b_ = ((v) >> i_) & 1; re_bit(e, (probs) + m_, b_); m_ = (m_ << 1) | b_; } } while (0)
static inline U32 tree_dec(RD *d, P *probs, int nb) { U32 m = 1; for (int i = 0; i < nb; i++) m = (m << 1) | rd_bit(d, probs + m); return m - (1u << nb); }
static inline void rtree_enc(RE *e, P *probs, int nb, U32 v) { U32 m = 1; for (int i = 0; i < nb; i++) { int b = v & 1; v >>= 1; re_bit(e, probs + m, b); m = (m << 1) | b; } }
static inline U32 rtree_dec(RD *d, P *probs, int nb) { U32 m = 1, v = 0; for (int i = 0; i < nb; i++) { int b = rd_bit(d, probs + m); m = (m << 1) | b; v |= (U32)b << i; } return v; }

static inline void len_enc(RE *e, LenM *L, U32 len, int ps) {
  U32 l = len - 2;
  if (l < 8) { re_bit(e, &L->choice, 0); TREE_ENC(e, L->low[ps], 3, l); }
  else if (l < 16) { re_bit(e, &L->choice, 1); re_bit(e, &L->choice2, 0); TREE_ENC(e, L->mid[ps], 3, l - 8); }
  else { re_bit(e, &L->choice, 1); re_bit(e, &L->choice2, 1); TREE_ENC(e, L->high, 8, l - 16); }
}
static inline U32 len_dec(RD *d, LenM *L, int ps) {
  if (!rd_bit(d, &L->choice)) return 2 + tree_dec(d, L->low[ps], 3);
  if (!rd_bit(d, &L->choice2)) return 10 + tree_dec(d, L->mid[ps], 3);
  return 18 + tree_dec(d, L->high, 8);
}
static inline U32 dist_slot(U32 d) {
  if (d < 4) return d;
  int nb = 31 - __builtin_clz(d);
  return 2 * nb + ((d >> (nb - 1)) & 1);
}
static inline void dist_enc(RE *e, LZM *m, U32 dist, U32 len) {
  U32 d = dist - 1, s = dist_slot(d), ls = len - 2 < 3 ? len - 2 : 3;
  TREE_ENC(e, m->slot[ls], 6, s);
  if (s >= 4) {
    int fb = (s >> 1) - 1; U32 base = (2 | (s & 1)) << fb, r = d - base;
    if (s < 14) rtree_enc(e, m->spec[s], fb, r);
    else { re_direct(e, r >> 4, fb - 4); rtree_enc(e, m->align, 4, r & 15); }
  }
}
static int dist_bad;
static inline U32 dist_dec(RD *d, LZM *m, U32 len) {
  U32 ls = len - 2 < 3 ? len - 2 : 3, s = tree_dec(d, m->slot[ls], 6);
  if (s < 4) return s + 1;
  int fb = (s >> 1) - 1; U32 base = (2 | (s & 1)) << fb, r;
  if (s < 14) r = rtree_dec(d, m->spec[s], fb);
  else { r = rd_direct(d, fb - 4, &dist_bad) << 4; r |= rtree_dec(d, m->align, 4); }
  return base + r + 1;  // may wrap for garbage input; caller validates
}
static inline void lit_enc(RE *e, LZM *m, int prev, int c, int matched, int mb) {
  P *pr = m->lit[prev >> 5]; U32 sym = 1;
  for (int b = 7; b >= 0; b--) {
    int bit = (c >> b) & 1;
    if (matched) {
      int mbit = (mb >> b) & 1;
      re_bit(e, pr + 0x100 + (mbit << 8) + sym, bit);
      if (bit != mbit) matched = 0;
    } else re_bit(e, pr + sym, bit);
    sym = (sym << 1) | bit;
  }
}
static inline int lit_dec(RD *d, LZM *m, int prev, int matched, int mb) {
  P *pr = m->lit[prev >> 5]; U32 sym = 1;
  for (int b = 7; b >= 0; b--) {
    int bit;
    if (matched) {
      int mbit = (mb >> b) & 1;
      bit = rd_bit(d, pr + 0x100 + (mbit << 8) + sym);
      if (bit != mbit) matched = 0;
    } else bit = rd_bit(d, pr + sym);
    sym = (sym << 1) | bit;
  }
  return sym & 255;
}

// match finder: 4-byte hash -> bucket of the most recent positions
#ifndef HBITS
#define HBITS 18
#endif
#ifndef WAYS
#define WAYS 4
#endif
#ifndef FARLEN4
#define FARLEN4 (1u << 16)
#endif
#ifndef INSMAX
#define INSMAX 1000
#endif
#ifndef SKIPRUN
#define SKIPRUN 64
#endif
#ifndef LAZYMAX
#define LAZYMAX 64
#endif
static inline U32 rd32(const U8 *p) { U32 v; memcpy(&v, p, 4); return v; }
static inline U32 hh4(const U8 *p) { return (rd32(p) * 2654435761u) >> (32 - HBITS); }
static inline U32 mlen_at(const U8 *a, const U8 *b, U32 lim) {
  U32 l = 0;
  while (l + 8 <= lim) {
    U64 x, y; memcpy(&x, a + l, 8); memcpy(&y, b + l, 8);
    if (x != y) return l + (__builtin_ctzll(x ^ y) >> 3);
    l += 8;
  }
  while (l < lim && a[l] == b[l]) l++;
  return l;
}
static inline void mf_insert(U32 *T, const U8 *buf, U32 pos) {
  U32 *b = T + (size_t)hh4(buf + pos) * WAYS;
  for (int w = WAYS - 1; w > 0; w--) b[w] = b[w - 1];
  b[0] = pos + 1;
}
// index positions inside a match: all of a short match, the head and tail of a long one
static inline void mf_insert_run(U32 *T, const U8 *buf, U32 pos, U32 k0, U32 len, U32 n) {
  for (U32 k = k0; k < len; k++) {
    if (k >= INSMAX && k + 2 < len) { k = len - 3; continue; }
    if (pos + k + MINLEN + 4 > n) break;
    mf_insert(T, buf, pos + k);
  }
}
static inline U32 mf_find(U32 *T, const U8 *buf, U32 pos, U32 avail, U32 *dist) {
  U32 *b = T + (size_t)hh4(buf + pos) * WAYS, best = 0, lim = avail < MAXLEN ? avail : MAXLEN;
  for (int w = 0; w < WAYS; w++) {
    U32 c = b[w]; if (!c) break; c--;
    if (buf[c + best] != buf[pos + best]) continue;
    U32 l = mlen_at(buf + c, buf + pos, lim);
    if (l > best) { best = l; *dist = pos - c; if (l == lim) break; }
  }
  for (int w = WAYS - 1; w > 0; w--) b[w] = b[w - 1];
  b[0] = pos + 1;
  return best;
}

#ifdef STATS
static U32 st_lit, st_match, st_rep;
#define ST(x) (x)++
#else
#define ST(x)
#endif
static void lz_compress(const U8 *buf, U32 n, Buf *o) {
  static LZM m; lzm_init(&m);
  U32 *T = xalloc(((size_t)WAYS << HBITS) * sizeof(U32), 1);
  RE e; re_init(&e, o);
  U32 pos = 0, rep0 = 1, state = 0, nlen = 0, ndist = 0, lrun = 0; int have_next = 0;
  while (pos < n) {
    U32 avail = n - pos, ps = pos & 3, mlen = 0, mdist = 0;
    if (pos + 1 + MINLEN + 4 <= n) __builtin_prefetch(T + (size_t)hh4(buf + pos + 1) * WAYS);
    if (!have_next && lrun >= SKIPRUN && (pos & 3)) goto literal;  // data looks incompressible: probe every 4th byte
    if (have_next) { mlen = nlen; mdist = ndist; have_next = 0; }
    else if (avail >= MINLEN + 4) mlen = mf_find(T, buf, pos, avail, &mdist);
    if (mlen == MINLEN && mdist > FARLEN4) mlen = 0;  // a far 4-byte match costs more than 4 literals
    U32 rlen = (pos >= rep0) ? mlen_at(buf + pos, buf + pos - rep0, avail < MAXLEN ? avail : MAXLEN) : 0;
    if (rlen >= 2 && rlen + 1 >= mlen) {
      re_bit(&e, &m.is_match[state][ps], 1); re_bit(&e, &m.is_rep[state], 1);
      len_enc(&e, &m.rlen, rlen, ps);
      mf_insert_run(T, buf, pos, 1, rlen, n);
      pos += rlen; state = 6 + state / 3; lrun = 0; ST(st_rep);
      continue;
    }
    if (mlen >= MINLEN) {
      if (mlen < LAZYMAX && pos + 1 + MINLEN + 4 <= n) {
        nlen = mf_find(T, buf, pos + 1, avail - 1, &ndist);
        if (nlen > mlen) { have_next = 1; goto literal; }
      } else nlen = 0;
      re_bit(&e, &m.is_match[state][ps], 1); re_bit(&e, &m.is_rep[state], 0);
      len_enc(&e, &m.mlen, mlen, ps);
      dist_enc(&e, &m, mdist, mlen);
      U32 k0 = (mlen < LAZYMAX && pos + 1 + MINLEN + 4 <= n) ? 2 : 1;  // pos+1 already inserted by lookahead
      mf_insert_run(T, buf, pos, k0, mlen, n);
      rep0 = mdist; pos += mlen; state = 3 + state / 3; lrun = 0; ST(st_match);
      continue;
    }
  literal:
    re_bit(&e, &m.is_match[state][ps], 0);
    lit_enc(&e, &m, pos ? buf[pos - 1] : 0, buf[pos], state / 3 != 0, state / 3 != 0 ? buf[pos - rep0] : 0);
    pos++; state = state / 3; lrun++; ST(st_lit);
  }
#ifdef STATS
  fprintf(stderr, "lit %u match %u rep %u\n", st_lit, st_match, st_rep);
#endif
  re_flush(&e);
  free(T);
}
static int lz_decompress(In *in, U8 *out, U32 n) {  // 0 = ok
  static LZM m; lzm_init(&m); dist_bad = 0;
  RD d; if (rd_init(&d, in)) return 1;
  U32 pos = 0, rep0 = 1, state = 0;
  while (pos < n) {
    U32 ps = pos & 3, len, dist;
    if (!rd_bit(&d, &m.is_match[state][ps])) {
      int mt = state / 3 != 0;
      out[pos] = (U8)lit_dec(&d, &m, pos ? out[pos - 1] : 0, mt, mt ? out[pos - rep0] : 0);
      pos++; state = state / 3;
    } else {
      if (rd_bit(&d, &m.is_rep[state])) { len = len_dec(&d, &m.rlen, ps); dist = rep0; state = 6 + state / 3; }
      else {
        len = len_dec(&d, &m.mlen, ps); dist = dist_dec(&d, &m, len);
        rep0 = dist; state = 3 + state / 3;
      }
      if (dist_bad || dist == 0 || dist > pos || len > n - pos) return 1;  // reject references outside the output
      U8 *dst = out + pos; const U8 *src = dst - dist;
      for (U32 k = 0; k < len; k++) dst[k] = src[k];
      pos += len;
    }
    if (in->eof) return 1;
  }
  return 0;
}

// ---------------- file I/O and container ----------------
// header: "HP3" | flags (bit0 x86 filter, bit1 LZ mode) | table bits | size u32 LE | crc32c u32 LE
#define HDR 13
#define MAXSIZE 0xFFFFFFF0u
static U8 *read_all(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  Buf b = {0, 0, 0}; size_t r;
  buf_reserve(&b, 1 << 20);
  while ((r = fread(b.p + b.n, 1, b.cap - b.n, f)) > 0) {
    b.n += r;
    if (b.n > MAXSIZE) { fprintf(stderr, "hp: %s: inputs over 4 GB are not supported\n", path); exit(1); }
    if (b.n == b.cap) buf_reserve(&b, b.cap * 2);
  }
  if (ferror(f)) { perror(path); exit(1); }
  fclose(f);
  *len = b.n;
  return b.p;
}
static void write_all(const char *path, const U8 *p, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); exit(1); }
  if (fwrite(p, 1, n, f) != n || fflush(f) || ferror(f) || fclose(f)) {
    perror(path); remove(path); exit(1);
  }
}
static int same_file(const char *a, const char *b) {
#ifdef _WIN32  // st_ino is always 0 on Windows, so compare volume serial and file index
  DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  HANDLE ha = CreateFileA(a, 0, share, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (ha == INVALID_HANDLE_VALUE) return 0;
  HANDLE hb = CreateFileA(b, 0, share, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (hb == INVALID_HANDLE_VALUE) { CloseHandle(ha); return 0; }
  BY_HANDLE_FILE_INFORMATION ia, ib;
  int same = GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib) &&
             ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
             ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
  CloseHandle(ha); CloseHandle(hb);
  return same;
#else
  struct stat sa, sb;
  if (stat(a, &sa) || stat(b, &sb)) return 0;
  return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}
static void put32(U8 *p, U32 v) { for (int i = 0; i < 4; i++) p[i] = (U8)(v >> (8 * i)); }
static U32 get32(const U8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (U32)p[3] << 24; }

int main(int argc, char **argv) {
  int lz = 0, ai = 2;
  if (argc >= 3 && argv[1][0] == 'c' && !strcmp(argv[2], "-1")) { lz = 1; ai = 3; }
  else if (argc >= 3 && argv[1][0] == 'c' && !strcmp(argv[2], "-9")) ai = 3;
  if (argc != ai + 2 || (strcmp(argv[1], "c") && strcmp(argv[1], "d"))) {
    fprintf(stderr, "usage: hp c [-1|-9] input output   (compress; -1 fast, -9 best, default -9)\n"
                    "       hp d input output            (decompress)\n");
    return 1;
  }
  const char *ip = argv[ai], *op = argv[ai + 1];
  if (same_file(ip, op)) { fprintf(stderr, "hp: input and output are the same file\n"); return 1; }
  init_tables(); init_dt(); crc_init();
  size_t n; U8 *data = read_all(ip, &n);
  if (argv[1][0] == 'c') {
    if (getenv("HP_TBITS")) {  // stored in the header, so safe to vary
      int t = atoi(getenv("HP_TBITS"));
      if (t < 16 || t > 24) { fprintf(stderr, "hp: HP_TBITS must be 16..24\n"); return 1; }
      TBITS = t;
    }
    U32 crc = crc32c(data, n);
    int x86 = looks_x86(data, (int64_t)n);
    if (x86) e8e9(data, (int64_t)n, 1);
    Buf o = {0, 0, 0};
    buf_reserve(&o, n / 2 + 1024);
    o.n = HDR;
    memcpy(o.p, "HP3", 3); o.p[3] = (U8)(x86 | (lz << 1)); o.p[4] = (U8)TBITS;
    put32(o.p + 5, (U32)n); put32(o.p + 9, crc);
    if (lz) lz_compress(data, (U32)n, &o); else cm_compress(data, (U32)n, &o);
    write_all(op, o.p, o.n);
  } else {
    if (n < HDR || memcmp(data, "HP3", 3) || (data[3] & ~3u) || data[4] < 16 || data[4] > 24) {
      fprintf(stderr, "hp: %s: not an hp file, or an unsupported version\n", ip); return 2;
    }
    int x86 = data[3] & 1; lz = (data[3] >> 1) & 1; TBITS = data[4];
    U32 len = get32(data + 5), crc = get32(data + 9);
    if (len > MAXSIZE) { fprintf(stderr, "hp: %s: corrupt header\n", ip); return 2; }
    In in = {data + HDR, n - HDR, 0, 0};
    U8 *out = xalloc((size_t)len + 1, 0);
    int bad = lz ? lz_decompress(&in, out, len) : cm_decompress(&in, out, len);
    if (!bad && x86) e8e9(out, (int64_t)len, 0);
    if (bad || in.i != in.n || crc32c(out, len) != crc) {
      fprintf(stderr, "hp: %s: corrupt or truncated data, nothing written\n", ip); return 2;
    }
    write_all(op, out, len);
  }
  return 0;
}
