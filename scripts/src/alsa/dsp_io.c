#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#include "include/dsp_io.h"
#include "include/engine_globals.h"
#include "include/dsp_neon.h"
#include "include/dsp_src.h"
#include "include/ring_buf.h"
#include "include/shm_ring.h"
#include "include/clk2.h"

/* ── DSP 틱 대기 (eventfd 기반) ──────────────────────────────────── */
int dsp_wait_tick(long long period_ns)
{
    struct pollfd pfd = { .fd = g_dsp_clock_fd, .events = POLLIN };
    int timeout_ms = (int)(period_ns * 3 / 1000000LL);
    if (timeout_ms < 4) timeout_ms = 4;
    int pr = poll(&pfd, 1, timeout_ms);
    if (atomic_load_explicit(&g_quit, memory_order_relaxed)) return 0;
    if (pr < 0) { if (errno != EINTR) return 0; return 1; }
    if (pfd.revents & POLLIN) {
        uint64_t val;
        (void)read(g_dsp_clock_fd, &val, sizeof(val));

        /* [DEBUG] catch-up burst 검증: 즉시 비-블로킹 poll로 잔여 신호 측정 */
        static uint64_t s_total_ticks = 0;
        static uint64_t s_burst_extra = 0;
        static uint64_t s_burst_events = 0;
        static int      s_burst_max = 0;
        static time_t   s_last_report = 0;
        s_total_ticks++;
        int extra = 0;
        while (1) {
            struct pollfd p2 = { .fd = g_dsp_clock_fd, .events = POLLIN };
            if (poll(&p2, 1, 0) <= 0) break;
            if (!(p2.revents & POLLIN)) break;
            uint64_t v2;
            if (read(g_dsp_clock_fd, &v2, sizeof(v2)) <= 0) break;
            extra++;
            if (extra > 256) break; /* safety */
        }
        if (extra > 0) {
            s_burst_extra += extra;
            s_burst_events++;
            if (extra > s_burst_max) s_burst_max = extra;
        }
        time_t now = time(NULL);
        if (s_last_report == 0) s_last_report = now;
        if (now - s_last_report >= 60) {
            fprintf(stderr,
                "[aoip_engine] dsp burst stat: events=%llu extra=%llu max=%d / ticks=%llu (%.4f%%)\n",
                (unsigned long long)s_burst_events,
                (unsigned long long)s_burst_extra,
                s_burst_max,
                (unsigned long long)s_total_ticks,
                100.0 * (double)s_burst_events / (double)(s_total_ticks ? s_total_ticks : 1));
            s_burst_events = 0;
            s_burst_extra = 0;
            s_burst_max = 0;
            s_total_ticks = 0;
            s_last_report = now;
        }
    }
    return 1;
}

/* ── ALSA 입력 읽기 ──────────────────────────────────────────────── */
static void read_alsa_master(Device *d)
{
    float *rptrs[MAX_CH];
    if (slot_ring_acquire_read(&d->i2s_in_ring, rptrs)) {
        d->i2s_in_acquired = 1;
        for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
            g_in_ptr[d->ch_start+c] = rptrs[c];
        if (d->cap_underrun > 0) {
            fprintf(stderr, "[aoip_engine] alsa '%s': capture recovered after %u ticks\n",
                    d->name, d->cap_underrun);
            d->cap_underrun = 0;
        }
    } else {
        d->i2s_in_acquired = 0;
        d->cap_underrun++;
        if (d->cap_underrun == 10 || (d->cap_underrun > 10 && d->cap_underrun % 500 == 0))
            fprintf(stderr, "[aoip_engine] alsa '%s': capture underrun %u ticks (avail=%d)\n",
                    d->name, d->cap_underrun, slot_ring_avail(&d->i2s_in_ring));
        for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++) {
            g_in_ptr[d->ch_start+c] = g_in_buf_static[d->ch_start+c];
            memset(g_in_ptr[d->ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
        }
    }
}

static void read_alsa_device(Device *d)
{
    if (d->is_ravenna) {
        for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
            g_in_ptr[d->ch_start+c] = g_in_buf_static[d->ch_start+c];
        if (!atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_acquire)) {
            for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                memset(g_in_ptr[d->ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
            d->cap_prebuf_ready = 0;
            return;
        }
        if (!d->cap_prebuf_ready) {
            if (rb_avail(&d->in_ring) < g_ravenna_fill_target) {
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    memset(g_in_ptr[d->ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
                return;
            }
            src_reset(d->cap_src);
            pi_reset(&d->cap_pi);
            d->cap_prebuf_ready = 1;
            fprintf(stderr, "[aoip_engine] ravenna '%s': cap prebuffer done (fill=%d), SRC ready\n",
                    d->name, rb_avail(&d->in_ring));
            /* RAVENNA RTP 로딩 완료 — I2S 베이스라인 보정 1회 적용 */
            clk2_apply_precal();
        }
        if (ring_capture_src(d->cap_src, &d->cap_pi, &d->in_ring,
                             d->tmp_cap_in, d->tmp_cap_out,
                             g_ravenna_fill_target, d->channels, d->ch_start) == 0) {
            d->cap_underrun++;
            if (d->cap_underrun >= 100) {
                src_reset(d->cap_src);
                pi_reset(&d->cap_pi);
                d->cap_prebuf_ready = 0;
                d->cap_underrun = 0;
            }
        } else {
            d->cap_underrun = 0;
        }
    } else {
        read_alsa_master(d);
    }
}

void dsp_read_inputs(void)
{
    for (int ch = 0; ch < g_n_in; ch++)
        g_in_ptr[ch] = g_in_buf_static[ch];
    for (int ch = 0; ch < g_n_out; ch++)
        g_out_ptr[ch] = g_out_buf_static[ch];

    for (int di = 0; di < g_n_dev; di++) {
        Device *d = &g_dev[di];
        if (!d->enabled) continue;
        if (d->is_i2s && d->mode != 1) {
            float *wptrs[MAX_CH];
            if (slot_ring_acquire_write(&d->i2s_out_ring, wptrs)) {
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    g_out_ptr[d->ch_start+c] = wptrs[c];
                d->i2s_out_acquired = 1;
                if (d->play_overflow > 0) {
                    fprintf(stderr, "[aoip_engine] alsa '%s': playback recovered after %u ticks\n",
                            d->name, d->play_overflow);
                    d->play_overflow = 0;
                }
            } else {
                d->i2s_out_acquired = 0;
                d->play_overflow++;
                if (d->play_overflow == 10 || (d->play_overflow > 10 && d->play_overflow % 500 == 0))
                    fprintf(stderr, "[aoip_engine] alsa '%s': playback overflow %u ticks (free=%d)\n",
                            d->name, d->play_overflow, slot_ring_free_slots(&d->i2s_out_ring));
            }
        }
        if (d->mode != 2) read_alsa_device(d);
    }
    for (int ri = 0; ri < g_n_rtp_in; ri++) {
        RtpStream *r = &g_rtp_in[ri];
        for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
            g_in_ptr[r->ch_start+c] = g_in_buf_static[r->ch_start+c];
        if (!r->enabled || !r->ring.buf || !r->rtp_src) {
            for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                memset(g_in_ptr[r->ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
            continue;
        }
        if (r->prebuffering) {
            if (rb_avail(&r->ring) >= r->fill_target / 2) {
                r->prebuffering = 0;
                fprintf(stderr, "[aoip_engine] rtp_in '%s': prebuffer done (fill=%d, target=%d)\n",
                        r->name, rb_avail(&r->ring), r->fill_target);
            } else {
                for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                    memset(g_in_ptr[r->ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
                continue;
            }
        }
        if (rb_avail(&r->ring) > r->fill_target * 2) r->overrun_total++;
        int used = ring_capture_src(r->rtp_src, &r->rtp_pi, &r->ring,
                                    r->rtp_in_buf, r->rtp_out_buf,
                                    r->fill_target, r->channels, r->ch_start);
        if (used > 0) {
            r->rtp_underrun = 0;
        } else {
            r->rtp_underrun++;
            r->underrun_total++;
            if (r->rtp_underrun == 10 || (r->rtp_underrun > 10 && r->rtp_underrun % 500 == 0))
                fprintf(stderr, "[aoip_engine] rtp_in '%s': underrun %d ticks (fill=%d), re-prebuffering\n",
                        r->name, r->rtp_underrun, rb_avail(&r->ring));
            if (r->rtp_underrun == 10) {
                src_reset(r->rtp_src);
                pi_reset(&r->rtp_pi);
                r->prebuffering = 1;
            }
        }
    }
}

/* ── 출력 쓰기 ───────────────────────────────────────────────────── */
static void interleave_buf(int ch_start, int channels, float *tmp)
{
    if (channels == 2 && ch_start + 1 < MAX_CH) {
        interleave_2ch_neon(g_out_ptr[ch_start], g_out_ptr[ch_start + 1], tmp, g_period_frames);
        return;
    }
    memset(tmp, 0, (size_t)(g_period_frames * channels) * sizeof(float));
    for (int f = 0; f < g_period_frames; f++)
        for (int c = 0; c < channels && (ch_start+c) < MAX_CH; c++)
            tmp[f * channels + c] = g_out_ptr[ch_start+c][f];
}

void dsp_write_outputs(void)
{
    for (int di = 0; di < g_n_dev; di++) {
        Device *d = &g_dev[di];
        if (!d->enabled) continue;
        if (d->is_i2s) {
            if (d->mode != 2 && d->i2s_in_acquired)
                slot_ring_consume_read(&d->i2s_in_ring);
            if (d->mode != 1 && d->i2s_out_acquired)
                slot_ring_commit_write(&d->i2s_out_ring);
            continue;
        }
        if (d->mode == 1) continue;
        alsa_playback_src(d);
    }
    for (int ri = 0; ri < g_n_rtp_out; ri++) {
        RtpStream *r = &g_rtp_out[ri];
        if (!r->enabled || !r->ring.buf) continue;
        interleave_buf(r->ch_start, r->channels, g_interleave_tmp);
        if (rb_write(&r->ring, g_interleave_tmp, g_period_frames) < g_period_frames)
            r->overrun_total++;
    }
}
