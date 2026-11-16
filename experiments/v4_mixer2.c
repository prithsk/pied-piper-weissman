// hp: a context-mixing lossless compressor ("Pied Piper, but real").
// Usage: hp c in out | hp d in out
// Model: hashed order-N contexts -> adaptive bit predictors -> gated
// logistic mixer -> two SSE stages -> binary arithmetic coder.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void e8e9(U8 *d, long n, int fwd) {
  for (long i = 0; i + 4 < n; i++) {
    if ((d[i] & 0xfe) == 0xe8) {
      U32 a = d[i+1] | d[i+2] << 8 | d[i+3] << 16 | (U32)d[i+4] << 24;
      a = fwd ? a + (U32)i : a - (U32)i;
      d[i+1] = a; d[i+2] = a >> 8; d[i+3] = a >> 16; d[i+4] = a >> 24;
      i += 4;
    }
  }
}
static int looks_x86(const U8 *d, long n) {  // E8/E9 calls whose offset top byte is 0x00/0xFF
  long hit = 0, all = 0;
  for (long i = 0; i + 4 < n; i++) if ((d[i] & 0xfe) == 0xe8) { all++; if (d[i+4] == 0 || d[i+4] == 0xff) hit++; }
  return n > 4096 && all * 1000 > n * 4 && hit * 10 > all * 6;
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
typedef struct { int n, nsets; int *w; int x[MAXIN]; int nx; int sel; int pr; int lr; } Mixer;
static void mx_init(Mixer *m, int n, int nsets, int lr) {
  m->n = n; m->nsets = nsets; m->lr = lr; m->nx = 0;
  m->w = malloc(sizeof(int) * n * nsets);
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
  for (int i = 0; i < m->nx; i++) w[i] += (m->x[i] * err + (1 << 13)) >> 14;
}

// ---------------- APM / SSE ----------------
typedef struct { U16 *t; int idx; int rate; } APM;
static void apm_init(APM *a, int n, int rate) {
  a->t = malloc(sizeof(U16) * n * 33); a->rate = rate;
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
static U32 OFF = 0; static int LR = 10; static int LIM = 30; static int TBITS = 22;  // ablation knobs (env)
#define ON(b) (!(OFF >> (b) & 1))
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
  M->T = calloc((size_t)16 << tbits, sizeof(U32));
  for (int i = 0; i < 256 * 256; i++) M->t0[i] = CNT_INIT;
  M->mmask = (1u << 22) - 1; M->mt = calloc(M->mmask + 1, sizeof(U32));
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
    for (int i = 0; i < M->nctx; i++) if (ON(i)) M->slot[i] = find_slot(M, hash2(M->h[i], c0));
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

// ---------------- arithmetic coder ----------------
typedef struct { U32 x1, x2, x; FILE *f; } AC;

static inline void enc_bit(AC *a, int y, int p) {
  U32 xm = a->x1 + (U32)(((U64)(a->x2 - a->x1) * p) >> 12);
  if (y) a->x2 = xm; else a->x1 = xm + 1;
  while (((a->x1 ^ a->x2) & 0xff000000u) == 0) {
    putc(a->x2 >> 24, a->f); a->x1 <<= 8; a->x2 = (a->x2 << 8) | 255;
  }
}
static inline int dec_bit(AC *a, int p) {
  U32 xm = a->x1 + (U32)(((U64)(a->x2 - a->x1) * p) >> 12);
  int y = a->x <= xm;
  if (y) a->x2 = xm; else a->x1 = xm + 1;
  while (((a->x1 ^ a->x2) & 0xff000000u) == 0) {
    a->x1 <<= 8; a->x2 = (a->x2 << 8) | 255;
    int c = getc(a->f); if (c == EOF) c = 0;
    a->x = (a->x << 8) | c;
  }
  return y;
}



int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: hp c|d in out\n"); return 1; }
  init_tables(); init_dt();
  FILE *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
  if (!in || !out) { perror("open"); return 1; }
  if (getenv("HP_TBITS")) TBITS = atoi(getenv("HP_TBITS"));
  if (getenv("HP_OFF")) OFF = strtoul(getenv("HP_OFF"), 0, 0);
  if (getenv("HP_LIM")) LIM = atoi(getenv("HP_LIM"));
  if (getenv("HP_LR")) LR = atoi(getenv("HP_LR"));
  static Model M;
  if (argv[1][0] == 'c') {
    fseek(in, 0, SEEK_END); long n = ftell(in); fseek(in, 0, SEEK_SET);
    U8 *data = malloc(n + 1);
    if (fread(data, 1, n, in) != (size_t)n) { perror("read"); return 1; }
    int x86 = getenv("HP_NOX86") ? 0 : looks_x86(data, n);
    if (x86) e8e9(data, n, 1);
    fwrite(x86 ? "HP2" : "HP1", 1, 3, out);
    for (int i = 0; i < 4; i++) putc((n >> (8 * i)) & 255, out);
    U8 *hist = malloc(n + 1);
    model_init(&M, hist, n, TBITS);
    AC a = {0, 0xffffffffu, 0, out};
    for (long i = 0; i < n; i++) {
      int c = data[i];
      for (int b = 7; b >= 0; b--) {
        int y = (c >> b) & 1;
        enc_bit(&a, y, predict(&M));
        update(&M, y);
      }
    }
    putc(a.x1 >> 24, out); putc(255, out); putc(255, out); putc(255, out);
  } else {
    char mg[3];
    if (fread(mg, 1, 3, in) != 3 || memcmp(mg, "HP", 2) || (mg[2] != '1' && mg[2] != '2')) { fprintf(stderr, "bad magic\n"); return 1; }
    int x86 = mg[2] == '2';
    long n = 0;
    for (int i = 0; i < 4; i++) n |= (long)getc(in) << (8 * i);
    U8 *hist = malloc(n + 1);
    model_init(&M, hist, n, TBITS);
    AC a = {0, 0xffffffffu, 0, in};
    for (int i = 0; i < 4; i++) { int c = getc(in); if (c == EOF) c = 0; a.x = (a.x << 8) | c; }
    for (long i = 0; i < n; i++) {
      int c = 0;
      for (int b = 0; b < 8; b++) { int y = dec_bit(&a, predict(&M)); update(&M, y); c = (c << 1) | y; }
      (void)c;
    }
    if (x86) e8e9(hist, n, 0);
    fwrite(hist, 1, n, out);
  }
  fclose(out);
  return 0;
}
