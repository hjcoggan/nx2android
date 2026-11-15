// Linux build: the movie library's decoders on the system's FFmpeg (libavcodec), the counterpart of dm/android/movie_avc.c (MediaCodec H.264) and
// dm/android/compat/audioconverter_mediacodec.c (MediaCodec AAC).
// FFmpeg is loaded at run time (whichever libavcodec major the system has: ROCKNIX ships a newer one than the build container), so no FFmpeg header is used and no
// structure is read past what has not moved since FFmpeg 1.0: AVPacket's buf / pts / dts / data / size and AVFrame's data / linesize / extended_data / width /
// height / nb_samples / format. Nothing is configured through AVCodecContext fields: H.264 goes in as Annex B with its SPS / PPS in band, AAC with an ADTS header
// made from its AudioSpecificConfig, so both decoders read their configuration from the stream.
#include "dm.h"
#include "hle_movie_avc.h"
#include <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// ---- libavcodec / libavutil ----
typedef struct { void *buf; int64_t pts, dts; uint8_t *data; int size; } avpkt_head;
typedef struct { uint8_t *data[8]; int linesize[8]; uint8_t **extended_data; int width, height, nb_samples, format; } avframe_head;
enum { PIX_YUV420P = 0, PIX_YUVJ420P = 12, SMP_S16 = 1, SMP_S32 = 2, SMP_FLT = 3, SMP_S16P = 6, SMP_S32P = 7, SMP_FLTP = 8 };
#define AVERR_EAGAIN (-11)
static struct {
    int ok;
    void *(*find)(const char *); void *(*alloc_ctx)(const void *); int (*open)(void *, const void *, void **);
    int (*send)(void *, const void *); int (*recv)(void *, void *); void (*flush)(void *); void (*free_ctx)(void **);
    void *(*pkt_alloc)(void); void (*pkt_free)(void **); int (*pkt_new)(void *, int); void (*pkt_unref)(void *);
    void *(*frm_alloc)(void); void (*frm_free)(void **); void (*frm_unref)(void *); int (*opt_int)(void *, const char *, int64_t, int);
} F;
static void *open_first(const char *base, int hi, int lo) {
    char n[64]; void *h = NULL;
    for (int v = hi; v >= lo && !h; v--) { snprintf(n, sizeof n, "%s.so.%d", base, v); h = dlopen(n, RTLD_NOW | RTLD_LOCAL); }
    if (!h) { snprintf(n, sizeof n, "%s.so", base); h = dlopen(n, RTLD_NOW | RTLD_LOCAL); }
    return h;
}
static void ff_init(void) {
    void *c = open_first("libavcodec", 70, 58), *u = open_first("libavutil", 68, 56);
    if (!c || !u) { LOG("[RED] movie: no FFmpeg (libavcodec %s, libavutil %s): H.264 gives black pictures, AAC silence", c ? "found" : "missing", u ? "found" : "missing"); return; }
#define S(lib, f, name) *(void **)&F.f = dlsym(lib, name)
    S(c, find, "avcodec_find_decoder_by_name"); S(c, alloc_ctx, "avcodec_alloc_context3"); S(c, open, "avcodec_open2"); S(c, send, "avcodec_send_packet");
    S(c, recv, "avcodec_receive_frame"); S(c, flush, "avcodec_flush_buffers"); S(c, free_ctx, "avcodec_free_context"); S(c, pkt_alloc, "av_packet_alloc");
    S(c, pkt_free, "av_packet_free"); S(c, pkt_new, "av_new_packet"); S(c, pkt_unref, "av_packet_unref");
    S(u, frm_alloc, "av_frame_alloc"); S(u, frm_free, "av_frame_free"); S(u, frm_unref, "av_frame_unref"); S(u, opt_int, "av_opt_set_int");
#undef S
    F.ok = F.find && F.alloc_ctx && F.open && F.send && F.recv && F.flush && F.free_ctx && F.pkt_alloc && F.pkt_free && F.pkt_new && F.pkt_unref && F.frm_alloc && F.frm_free && F.frm_unref && F.opt_int;
    Dl_info di; LOG("[movie] FFmpeg %s (%s)", F.ok ? "loaded" : "[RED] incomplete", dladdr((void *)F.find, &di) && di.dli_fname ? di.dli_fname : "?");
}
static int ff_load(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, ff_init); return F.ok; }
typedef struct { void *ctx, *pkt, *frm; } ffdec;
static int ff_open(ffdec *d, const char *codec, int threads) {
    if (d->ctx) return 1;
    if (!ff_load()) return 0;
    const void *c = F.find(codec); if (!c) { LOG("[RED] movie: FFmpeg has no %s decoder", codec); return 0; }
    if (!(d->ctx = F.alloc_ctx(c))) return 0;
    F.opt_int(d->ctx, "threads", threads, 0);
    if (F.open(d->ctx, c, NULL) < 0) { F.free_ctx(&d->ctx); LOG("[RED] movie: FFmpeg's %s decoder did not open", codec); return 0; }
    d->pkt = F.pkt_alloc(); d->frm = F.frm_alloc();
    return d->pkt && d->frm;
}
static void ff_close(ffdec *d) { if (d->ctx) F.free_ctx(&d->ctx); if (d->pkt) F.pkt_free(&d->pkt); if (d->frm) F.frm_free(&d->frm); }
static int ff_send(ffdec *d, const uint8_t *p, size_t n, int64_t pts) {   // n = 0: end of stream (the decoder gives out what it holds)
    if (!n) return F.send(d->ctx, NULL);
    if (F.pkt_new(d->pkt, (int)n) < 0) return -1;
    avpkt_head *h = d->pkt; memcpy(h->data, p, n); h->pts = h->dts = pts;
    int r = F.send(d->ctx, d->pkt); F.pkt_unref(d->pkt); return r;
}

// ---- H.264 (dm/host/hle_movie_avc.h) ----
#define QMAX 16
struct mavc {
    ffdec d; int eos, rgba, red, nsps, npps, ps_due;
    uint8_t sps[512], pps[512];
    uint8_t *au; size_t aucap;
    int64_t pend[64]; int npend;   // the times of access units sent and not out yet: each picture takes the smallest (the decoder gives them in presentation order)
    struct { uint8_t *nv12; int w, h; int64_t pts; } q[QMAX]; int nq;
};
mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); return a; }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }
int mavc_queued(const mavc *a) { return a->nq; }
int mavc_held(const mavc *a) { return a->nq + (a->d.ctx && !a->eos ? a->npend : 0); }
void mavc_drop(mavc *a) {
    for (int i = 0; i < a->nq; i++) free(a->q[i].nv12);
    a->nq = 0; a->npend = 0;
    if (a->d.ctx) F.flush(a->d.ctx);
    a->eos = 0; a->ps_due = 1;
}
void mavc_free(mavc *a) { if (!a) return; mavc_drop(a); ff_close(&a->d); free(a->au); free(a); }
static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->ps_due = 1;
}
// the NAL units of p (Annex B start codes, or else 4-byte lengths), each without its prefix (as movie_avc.c)
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {   // AVCDecoderConfigurationRecord (the MP4 avcC box)
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}
typedef struct { mavc *a; size_t len; } au_ctx;
static void au_put(au_ctx *c, const uint8_t *nal, size_t n) {
    mavc *a = c->a;
    if (c->len + 4 + n > a->aucap) { a->aucap = (c->len + 4 + n) * 2; if (!(a->au = realloc(a->au, a->aucap))) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + c->len; o[0] = o[1] = o[2] = 0; o[3] = 1; memcpy(o + 4, nal, n); c->len += 4 + n;
}
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(c->a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    au_put(c, nal, n);
}
static void keep_picture(mavc *a) {   // the decoded frame → a compact NV12 picture in the queue, with the smallest pending time
    avframe_head *f = a->d.frm; int w = f->width, h = f->height;
    if (f->format != PIX_YUV420P && f->format != PIX_YUVJ420P) { if (!a->red++) LOG("[RED] movie: H.264 picture format %d not handled, black picture", f->format); }
    size_t cw = (size_t)(w + 1) / 2, ch = (size_t)(h + 1) / 2;
    uint8_t *o = malloc((size_t)w * h + cw * ch * 2); if (!o) die("movie: AVC picture allocation failed");
    if (f->format == PIX_YUV420P || f->format == PIX_YUVJ420P) {
        for (int y = 0; y < h; y++) memcpy(o + (size_t)y * w, f->data[0] + (size_t)y * f->linesize[0], (size_t)w);
        uint8_t *uv = o + (size_t)w * h;
        for (size_t y = 0; y < ch; y++) { const uint8_t *u = f->data[1] + y * f->linesize[1], *v = f->data[2] + y * f->linesize[2];
            for (size_t x = 0; x < cw; x++) { *uv++ = u[x]; *uv++ = v[x]; } }
    } else { memset(o, 16, (size_t)w * h); memset(o + (size_t)w * h, 128, cw * ch * 2); }
    int64_t pts = 0;
    if (a->npend) { int m = 0; for (int i = 1; i < a->npend; i++) if (a->pend[i] < a->pend[m]) m = i; pts = a->pend[m]; a->pend[m] = a->pend[--a->npend]; }
    if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC picture queue full, dropping the oldest picture"); free(a->q[0].nv12); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    int k = a->nq; while (k > 0 && a->q[k - 1].pts > pts) { a->q[k] = a->q[k - 1]; k--; }
    a->q[k].nv12 = o; a->q[k].w = w; a->q[k].h = h; a->q[k].pts = pts; a->nq++;
    F.frm_unref(a->d.frm);
}
static void drain(mavc *a) { while (F.recv(a->d.ctx, a->d.frm) >= 0) keep_picture(a); }
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0};
    if (!a->d.ctx && !ff_open(&a->d, "h264", 2)) return -1;
    if (a->eos) { F.flush(a->d.ctx); a->eos = 0; a->ps_due = 1; }
    size_t mark = 0; each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if (a->ps_due && a->nsps && a->npps) {   // the parameter sets in band, before the access unit
        size_t body = c.len; uint8_t *tmp = malloc(body); if (!tmp) return -1; memcpy(tmp, a->au, body);
        c.len = 0; au_put(&c, a->sps, (size_t)a->nsps); au_put(&c, a->pps, (size_t)a->npps); mark = c.len;
        if (c.len + body > a->aucap) { a->aucap = (c.len + body) * 2; if (!(a->au = realloc(a->au, a->aucap))) die("movie: AVC buffer allocation failed"); }
        memcpy(a->au + c.len, tmp, body); c.len += body; free(tmp); a->ps_due = 0;
    }
    (void)mark;
    if (a->npend == 64) memmove(a->pend, a->pend + 1, --a->npend * sizeof *a->pend);
    a->pend[a->npend++] = pts_us;
    int r = ff_send(&a->d, a->au, c.len, pts_us);
    if (r == AVERR_EAGAIN) { drain(a); r = ff_send(&a->d, a->au, c.len, pts_us); }
    if (r < 0) { a->npend--; if (!a->red++) LOG("[RED] movie: FFmpeg refused an H.264 access unit (%d, first case)", r); return -1; }
    drain(a);
    return 0;
}
// NV12 (video range) → RGBA rows of w pixels; BT.709 from 720 lines up, else BT.601 (as movie_avc.c)
static void nv12_rgba(const uint8_t *y, const uint8_t *uv, int pw, int ph, uint8_t *dst, int w, int h) {
    int hd = ph >= 720, cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    int rv = hd ? 459 : 409, gu = hd ? 55 : 100, gv = hd ? 136 : 208, bu = hd ? 541 : 516;
    if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    for (int r = 0; r < chh; r++) {
        const uint8_t *yr = y + (size_t)r * pw, *cr = uv + (size_t)(r / 2) * ((pw + 1) & ~1); uint8_t *o = dst + (size_t)r * w * 4;
        for (int x = 0; x < cw; x++) {
            int c = 298 * (yr[x] - 16), u = cr[x & ~1] - 128, v = cr[(x & ~1) + 1] - 128;
            int R = (c + rv * v + 128) >> 8, G = (c - gu * u - gv * v + 128) >> 8, B = (c + bu * u + 128) >> 8;
            o[4 * x] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R); o[4 * x + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G); o[4 * x + 2] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B); o[4 * x + 3] = 255;
        }
    }
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (flush && a->d.ctx && !a->eos) { ff_send(&a->d, NULL, 0, 0); drain(a); a->eos = 1; a->npend = 0; }
    if (!a->nq) return 0;
    uint8_t *pic = a->q[0].nv12; int pw = a->q[0].w, ph = a->q[0].h; if (pts_us) *pts_us = a->q[0].pts;
    memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    const uint8_t *uv = pic + (size_t)pw * ph;
    if (a->rgba) nv12_rgba(pic, uv, pw, ph, dst, w, h);
    else {
        int cw = pw < w ? pw : w, chh = ph < h ? ph : h;
        if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, pic + (size_t)r * pw, (size_t)cw);
        int cs = (pw + 1) & ~1, copied = (cw + 1) & ~1; if (copied > w) copied = w;
        for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, uv + (size_t)r * cs, (size_t)copied);
    }
    free(pic);
    return 1;
}

// ---- AAC → interleaved 16-bit PCM (AudioConverter, for hle_movie.c md_aac) ----
struct dm_audio_converter {
    AudioStreamBasicDescription in, out;
    uint8_t asc[64]; int nasc, red, eos;
    ffdec d;
    int16_t *pend; size_t npend, cap;   // decoded frames (interleaved, out's channels) not handed out yet
};
OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c) {
    *c = NULL;
    if (in->mFormatID != kAudioFormatMPEG4AAC || out->mFormatID != kAudioFormatLinearPCM || out->mBitsPerChannel != 16 || (out->mFormatFlags & (kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved)) ||
        !out->mChannelsPerFrame || out->mChannelsPerFrame > 8) return kAudio_UnimplementedError;
    if (!ff_load()) return kAudio_UnimplementedError;
    AudioConverterRef a = calloc(1, sizeof *a); if (!a) return -1;
    a->in = *in; a->out = *out; *c = a; return noErr;
}
static const uint8_t *descr(const uint8_t *p, const uint8_t *e, int *tag, size_t *n) {   // one descriptor of an ES descriptor (audioconverter_mediacodec.c)
    if (p >= e) return NULL;
    *tag = *p++; size_t len = 0;
    for (int i = 0; i < 4 && p < e; i++) { uint8_t b = *p++; len = len << 7 | (b & 0x7f); if (!(b & 0x80)) break; }
    if (len > (size_t)(e - p)) return NULL;
    *n = len; return p;
}
OSStatus AudioConverterSetProperty(AudioConverterRef c, AudioConverterPropertyID id, UInt32 size, const void *data) {
    if (id != kAudioConverterDecompressionMagicCookie) return kAudio_UnimplementedError;
    const uint8_t *p = data, *e = p + size; int tag; size_t n;
    if (!(p = descr(p, e, &tag, &n)) || tag != 3 || n < 3) return -50;
    e = p + n; p += 3;
    if (!(p = descr(p, e, &tag, &n)) || tag != 4 || n < 13) return -50;
    e = p + n; p += 13;
    if (!(p = descr(p, e, &tag, &n)) || tag != 5 || !n || n > sizeof c->asc) return -50;
    memcpy(c->asc, p, n); c->nasc = (int)n; return noErr;
}
OSStatus AudioConverterDispose(AudioConverterRef c) { if (c) { ff_close(&c->d); free(c->pend); free(c); } return noErr; }
static void adts(const AudioConverterRef c, uint8_t h[7], size_t len) {   // the ADTS header of one raw AAC frame, from the AudioSpecificConfig (HE-AAC as its core AAC-LC: SBR is implicit)
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    int aot = 2, fi = 3, cc = (int)c->in.mChannelsPerFrame;
    if (c->nasc >= 2) { aot = c->asc[0] >> 3; fi = (c->asc[0] & 7) << 1 | c->asc[1] >> 7; cc = c->asc[1] >> 3 & 15; }
    else for (int i = 0; i < 13; i++) if (rates[i] == (int)c->in.mSampleRate) fi = i;
    if (aot < 1 || aot > 4) aot = 2;
    if (!cc || cc > 7) cc = (int)c->out.mChannelsPerFrame;
    size_t fl = len + 7;
    h[0] = 0xff; h[1] = 0xf1; h[2] = (uint8_t)((aot - 1) << 6 | fi << 2 | cc >> 2); h[3] = (uint8_t)((cc & 3) << 6 | fl >> 11);
    h[4] = (uint8_t)(fl >> 3); h[5] = (uint8_t)((fl & 7) << 5 | 0x1f); h[6] = 0xfc;
}
static void pend_frame(AudioConverterRef c) {   // the decoded frame → interleaved int16 frames at the end of pend
    avframe_head *f = c->d.frm; int n = f->nb_samples, och = (int)c->out.mChannelsPerFrame, planar = f->format == SMP_FLTP || f->format == SMP_S16P || f->format == SMP_S32P;
    int dch = 0; if (planar) while (dch < 8 && f->data[dch]) dch++; else dch = och;
    if (c->npend + (size_t)n > c->cap) { c->cap = (c->npend + (size_t)n) * 2; if (!(c->pend = realloc(c->pend, c->cap * och * sizeof *c->pend))) die("movie: AAC buffer allocation failed"); }
    int16_t *o = c->pend + c->npend * och;
    for (int i = 0; i < n; i++) for (int ch = 0; ch < och; ch++) {
        int s = dch ? (ch < dch ? ch : dch - 1) : 0; float v = 0;
        switch (f->format) {
        case SMP_FLTP: v = ((const float *)f->data[s])[i]; break;
        case SMP_FLT: v = ((const float *)f->data[0])[i * dch + s]; break;
        case SMP_S16P: o[i * och + ch] = ((const int16_t *)f->data[s])[i]; continue;
        case SMP_S16: o[i * och + ch] = ((const int16_t *)f->data[0])[i * dch + s]; continue;
        case SMP_S32P: o[i * och + ch] = (int16_t)(((const int32_t *)f->data[s])[i] >> 16); continue;
        case SMP_S32: o[i * och + ch] = (int16_t)(((const int32_t *)f->data[0])[i * dch + s] >> 16); continue;
        default: if (!c->red++) LOG("[RED] movie: AAC sample format %d not handled, silence", f->format); break;
        }
        v *= 32767.f; o[i * och + ch] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : v);
    }
    c->npend += (size_t)n; F.frm_unref(c->d.frm);
}
static size_t take(AudioConverterRef c, void *dst, size_t frames) {
    size_t k = c->npend < frames ? c->npend : frames, och = c->out.mChannelsPerFrame;
    memcpy(dst, c->pend, k * och * sizeof *c->pend);
    memmove(c->pend, c->pend + k * och, (c->npend - k) * och * sizeof *c->pend); c->npend -= k;
    return k;
}
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc proc, void *ud, UInt32 *packets, AudioBufferList *out, AudioStreamPacketDescription *desc) {
    (void)desc;
    size_t want = *packets, och = c->out.mChannelsPerFrame; OSStatus st = noErr;
    if (out->mNumberBuffers && out->mBuffers[0].mDataByteSize / (och * 2) < want) want = out->mBuffers[0].mDataByteSize / (och * 2);
    if (!c->d.ctx && !ff_open(&c->d, "aac", 1)) { *packets = 0; return kAudio_UnimplementedError; }
    if (c->eos) { F.flush(c->d.ctx); c->eos = 0; }
    while (c->npend < want) {
        UInt32 np = 1; AudioBufferList bl = {1, {{0, 0, NULL}}}; AudioStreamPacketDescription *pd = NULL;
        if ((st = proc(c, &np, &bl, &pd, ud)) || !np || !bl.mBuffers[0].mData) break;
        size_t n = bl.mBuffers[0].mDataByteSize; uint8_t *b = malloc(n + 7); if (!b) break;
        adts(c, b, n); memcpy(b + 7, bl.mBuffers[0].mData, n);
        int r = ff_send(&c->d, b, n + 7, 0); free(b);
        if (r < 0 && !c->red++) LOG("[RED] movie: FFmpeg refused an AAC packet (%d, first case)", r);
        while (F.recv(c->d.ctx, c->d.frm) >= 0) pend_frame(c);
    }
    size_t k = take(c, out->mBuffers[0].mData, want);
    out->mBuffers[0].mDataByteSize = (UInt32)(k * och * 2); *packets = (UInt32)k;
    return k ? noErr : st;
}
// the PCM the decoder still holds once no more packets come (hle_movie.c md_aac_tail)
size_t dm_aac_tail(AudioConverterRef c, void *dst, size_t cap) {
    if (!c || !c->d.ctx) return 0;
    if (!c->eos) { ff_send(&c->d, NULL, 0, 0); while (F.recv(c->d.ctx, c->d.frm) >= 0) pend_frame(c); c->eos = 1; }
    size_t och = c->out.mChannelsPerFrame;
    return take(c, dst, cap / (och * 2)) * och * 2;
}
