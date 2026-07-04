#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "include/engine_constants.h"
#include "include/alsa_device.h"
#include "include/clk2.h"

_Atomic int64_t  g_aoip_frames    = 0;
_Atomic int64_t  g_aoip_hts_ns    = 0;
_Atomic uint32_t g_aoip_seq       = 0;
_Atomic int64_t  g_ravenna_frames = 0;
_Atomic int64_t  g_ravenna_hts_ns = 0;
_Atomic uint32_t g_ravenna_seq    = 0;

volatile int    g_clk2_report       = 1;
_Atomic double  g_ravenna_ratio_hint = 1.0;
_Atomic int     g_clk_ready          = 0;
_Atomic int     g_ptp_locked         = 0;
_Atomic uint32_t g_ptp_resync_gen    = 0;

/* seqlock reader: writer의 (frames, hts_ns) 페어를 일관성 있게 스냅샷 */
static inline void clk2_reader_snapshot(_Atomic uint32_t *seq,
                                        _Atomic int64_t  *frames,
                                        _Atomic int64_t  *hts_ns,
                                        int64_t *out_fr, int64_t *out_hts)
{
    uint32_t s1, s2;
    for (int spin = 0; spin < 1024; spin++) {
        s1 = atomic_load_explicit(seq, memory_order_acquire);
        if (s1 & 1u) continue;                       /* writer mid-update */
        *out_fr  = atomic_load_explicit(frames, memory_order_relaxed);
        *out_hts = atomic_load_explicit(hts_ns, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        s2 = atomic_load_explicit(seq, memory_order_acquire);
        if (s1 == s2) return;                        /* consistent pair */
    }
    /* spin 한계 초과 — 마지막 값 그대로 반환 (다음 주기에 재시도) */
}

extern int g_period_frames;

/* 안정화 판정: |drift_med| < STABLE_PPM_ABS AND |delta| < STABLE_PPM_DELTA 가 STABLE_COUNT회 연속.
 * delta 만 보면 median 이 outlier 값에 stuck 인 상태(+18ppm 유지)도 "안정"으로 오인 → 절대값 게이트 필수. */
#define STABLE_PPM_ABS   3.0
#define STABLE_PPM_DELTA 2.0
#define STABLE_COUNT     5
#define FAST_INTERVAL_S  5LL
#define SLOW_INTERVAL_S  30LL
/* PTP 재동기화 감지 후 ratio_hint 갱신을 막는 동결 시간.
 * PTP 끊김→재락 시 ravenna_rate 가 점프하면서 DSP SRC 가 한 사이클 동안
 * 실 피치 시프트를 일으키는 현상 방지. 10s 정도면 측정 윈도우가 새 정상 상태로 채워짐. */
#define PTP_RESYNC_FREEZE_NS 10000000000LL

/* ── ppb 트래커 ──────────────────────────────────────────────────────────
 * pll_audio_core 미세 조정 (ptp-i2s-sync 모듈 sysfs).
 * alsa_open hook 이 부팅 시 PPB_INIT (11000) 로 베이스라인 적용 → 이후 여기서
 * drift_ppm 을 P-only 컨트롤러로 최소화. 적분/누적 없음(이전 폭주 이력).
 *   step      = -drift_ppm × 1000 × P_GAIN, 한 cycle 당 ±PPB_MAX_STEP 클램프
 *   누적 범위  = PPB_INIT ± PPB_RANGE (외부 환경 변동 흡수 여유)
 *   deadband  = |drift| < DEADBAND_PPM → 무조정 (헌팅 방지) */
#define PPB_SYSFS_PATH "/sys/kernel/ptp_i2s_sync/freq_ppb"
#define PPB_INIT       22000L
#define PPB_RANGE      5000L
#define PPB_MAX_STEP   2000L
#define PPB_P_GAIN     0.5
#define DEADBAND_PPM   0.3
/* SRC ready → precal 실 적용 까지의 대기 시간.
 * 즉시 적용 시 라이브 DMA 가 clk_set_rate(pll_audio_core) transient 로 폭주.
 * SRC ratio_hint 가 어느 정도 흡수한 뒤에 1회 step → 5초부터 시작해서 늘려간다. */
#define PRECAL_DELAY_NS 0LL

static long tracker_ppb     = 0;
static int  tracker_inited  = 0;
static int     precal_armed   = 0;
static int     precal_done    = 0;
static int64_t precal_armed_ns = 0;

static long ppb_read_sysfs(void)
{
    int fd = open(PPB_SYSFS_PATH, O_RDONLY);
    if (fd < 0) return PPB_INIT;
    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return PPB_INIT;
    return strtol(buf, NULL, 10);
}

/* hw:aoip 캡처 스레드 첫 readi 성공 시점 호출 — clk_ready 신호만 set.
 * 실제 precal 적용은 RAVENNA SRC ready 직후 clk2_apply_precal 에서 수행. */
void clk2_apply_initial_ppb(void)
{
    if (tracker_inited) return;
    tracker_inited = 1;
    tracker_ppb    = ppb_read_sysfs();
    atomic_store_explicit(&g_clk_ready, 1, memory_order_release);
    fprintf(stderr, "[aoip_engine] clk2: clk_ready=1 (current sysfs ppb=%ld)\n", tracker_ppb);
}

/* RAVENNA RTP 로딩 완료(SRC ready) 직후 1회 호출 — 실 적용은 PRECAL_DELAY_NS 후
 * clk2_report() 에서 수행. 여기서는 timestamp 만 무장. */
void clk2_apply_precal(void)
{
    if (precal_armed || precal_done) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    precal_armed_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    precal_armed    = 1;
    fprintf(stderr, "[aoip_engine] clk2: precal armed (delay=%lldms after SRC ready)\n",
            (long long)(PRECAL_DELAY_NS / 1000000LL));
}

/* clk2_report 진입부에서 호출 — armed 후 PRECAL_DELAY_NS 경과 시 1회 sysfs 적용. */
static void clk2_precal_tick(int64_t now_ns)
{
    if (!precal_armed || precal_done) return;
    if (now_ns - precal_armed_ns < PRECAL_DELAY_NS) return;
    precal_done = 1;

    int fd = open(PPB_SYSFS_PATH, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[aoip_engine] clk2: precal open(%s) failed: %s\n",
                PPB_SYSFS_PATH, strerror(errno));
        return;
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld", PPB_INIT);
    if (write(fd, buf, n) > 0) {
        tracker_ppb = PPB_INIT;
        fprintf(stderr, "[aoip_engine] clk2: precal ppb=%ld applied (%.1fs after SRC ready)\n",
                PPB_INIT, (double)(now_ns - precal_armed_ns) / 1e9);
    } else {
        fprintf(stderr, "[aoip_engine] clk2: precal write failed: %s\n", strerror(errno));
    }
    close(fd);
}

/* 5-sample median 필터 — 측정창에 끼는 일시적 이상치 제거용.
 * seqlock으로 torn read는 막았지만, ALSA htstamp 자체가 한 period 만큼
 * 늦게 갱신되는 경우가 있어 표시값/제어값을 한 번 더 부드럽게 한다. */
#define MED_N 5
static double clk2_median5(const double *src)
{
    double a[MED_N];
    for (int i = 0; i < MED_N; i++) a[i] = src[i];
    for (int i = 0; i < MED_N; i++)
        for (int j = i + 1; j < MED_N; j++)
            if (a[j] < a[i]) { double t = a[i]; a[i] = a[j]; a[j] = t; }
    return a[MED_N / 2];
}

/* hw:aoip ↔ RAVENNA 클럭 drift 보고
 * 초기: 1s 주기로 빠르게 측정 → 안정화 후 30s 주기로 전환 */
void clk2_report(int64_t *pa_fr,  int64_t *pa_hts,
                 int64_t *pr_fr,  int64_t *pr_hts,
                 int64_t *next_ns)
{
    static int    stable_cnt  = 0;
    static int    stabilized  = 0;
    static double prev_ppm    = 0.0;

    static double med_ppm[MED_N]   = {0};
    static double med_ratio[MED_N] = {0};
    static int    med_idx          = 0;
    static int    med_filled       = 0;

    static uint32_t last_resync_gen     = 0;
    static int64_t  resync_freeze_until = 0;

    struct timespec _now;
    clock_gettime(CLOCK_MONOTONIC, &_now);
    int64_t now_ns = (int64_t)_now.tv_sec * 1000000000LL + _now.tv_nsec;

    /* precal 지연 적용 — next_ns 5s 게이트보다 위에서 체크해 정확한 타이밍 보장 */
    clk2_precal_tick(now_ns);

    /* PTP 재동기화 감지 — gen 변화 시 freeze 윈도우 시작, median/stab 리셋.
     * prev 스냅샷도 무효화: 그대로 두면 다음 측정이 unlock 갭(ravenna frame 정지)을
     * 포함한 윈도우를 봐서 ravenna_rate가 37kHz 같은 쓰레기 값으로 측정 → median 5칸
     * 오염 → 수십 초간 ratio_hint/안정화 판정 망가짐. next_ns도 FAST로 재설정. */
    uint32_t cur_gen = atomic_load_explicit(&g_ptp_resync_gen, memory_order_acquire);
    if (cur_gen != last_resync_gen) {
        last_resync_gen     = cur_gen;
        resync_freeze_until = now_ns + PTP_RESYNC_FREEZE_NS;
        med_idx = 0; med_filled = 0;
        stable_cnt = 0; stabilized = 0; prev_ppm = 0.0;
        *pa_fr = 0; *pa_hts = 0; *pr_fr = 0; *pr_hts = 0;
        *next_ns = now_ns + FAST_INTERVAL_S * 1000000000LL;
        fprintf(stderr, "[aoip_engine] clk2: PTP resync gen=%u, ratio_hint frozen for %lldms\n",
                cur_gen, (long long)(PTP_RESYNC_FREEZE_NS / 1000000LL));
    }
    int frozen = (now_ns < resync_freeze_until);

    if (!g_clk2_report) {
        *next_ns = now_ns + SLOW_INTERVAL_S * 1000000000LL;
        return;
    }

    if (now_ns < *next_ns) return;

    /* 초기 베이스라인은 i2s 캡처 스레드(첫 readi 성공 시점)가 clk2_apply_initial_ppb 로 적용.
     * 여기서는 그대로 진행만 한다. */

    int64_t a_fr = 0, a_hts = 0, r_fr = 0, r_hts = 0;
    clk2_reader_snapshot(&g_aoip_seq,    &g_aoip_frames,    &g_aoip_hts_ns,    &a_fr, &a_hts);
    clk2_reader_snapshot(&g_ravenna_seq, &g_ravenna_frames, &g_ravenna_hts_ns, &r_fr, &r_hts);

    int64_t interval_s = stabilized ? SLOW_INTERVAL_S : FAST_INTERVAL_S;

    if (*pa_hts > 0 && *pr_hts > 0 &&
        a_hts > *pa_hts && r_hts > *pr_hts) {

        int64_t da_fr  = a_fr  - *pa_fr;
        int64_t da_hts = a_hts - *pa_hts;
        int64_t dr_fr  = r_fr  - *pr_fr;
        int64_t dr_hts = r_hts - *pr_hts;

        double aoip_rate    = (double)da_fr * 1e9 / (double)da_hts;
        double ravenna_rate = (double)dr_fr * 1e9 / (double)dr_hts;
        double drift_ppm    = (aoip_rate - ravenna_rate) / SAMPLE_RATE * 1e6;
        double elapsed_s    = (double)da_hts / 1e9;
        double dsp_tick_ms  = (double)g_period_frames / aoip_rate * 1000.0;
        double ratio        = ravenna_rate / aoip_rate;

        /* sanity gate: ravenna_rate가 ±5% 벗어나면(PTP unlock 잔재/packet 정지 등)
         * median 버퍼에 넣지 않음 — 단 한 샘플 오염으로 25초간 ratio_hint가 망가지는
         * 회귀를 막는다. prev 스냅샷은 갱신해서 다음 측정은 정상 윈도우로. */
        const double RATE_MIN = SAMPLE_RATE * 0.95;
        const double RATE_MAX = SAMPLE_RATE * 1.05;
        int sample_valid = (ravenna_rate > RATE_MIN && ravenna_rate < RATE_MAX &&
                            aoip_rate    > RATE_MIN && aoip_rate    < RATE_MAX);
        if (!sample_valid) {
            fprintf(stderr, "[aoip_engine] clk2: rejecting outlier sample"
                    " aoip=%.1fHz ravenna=%.1fHz (likely PTP transient)\n",
                    aoip_rate, ravenna_rate);
        }

        /* 5-sample median 필터 — 이상치 억제 */
        if (sample_valid) {
            med_ppm[med_idx]   = drift_ppm;
            med_ratio[med_idx] = ratio;
            med_idx = (med_idx + 1) % MED_N;
            if (med_filled < MED_N) med_filled++;
        }

        double drift_ppm_med = (med_filled == MED_N) ? clk2_median5(med_ppm)   : drift_ppm;
        double ratio_med     = (med_filled == MED_N) ? clk2_median5(med_ratio) : ratio;

        /* ratio_hint는 median 값으로 갱신 — 일시적 이상치가 DSP 보간 비율에 새지 않도록.
         * PTP 재동기화 freeze 중에는 갱신 스킵 (마지막 정상 ratio_hint 유지). */
        if (!frozen && ratio_med > RATIO_MIN && ratio_med < RATIO_MAX)
            atomic_store_explicit(&g_ravenna_ratio_hint, ratio_med, memory_order_relaxed);

        /* 안정화 판정도 median 기준으로 (초기 빠른 수렴 구간에서만).
         * 절대값(|med| < ABS) + delta(|med-prev| < DELTA) 동시 충족이어야 함 —
         * delta-only 는 outlier 값에 median 이 머무는 동안에도 통과해서 오동작. */
        if (!stabilized) {
            if (fabs(drift_ppm_med) < STABLE_PPM_ABS &&
                fabs(drift_ppm_med - prev_ppm) < STABLE_PPM_DELTA)
                stable_cnt++;
            else
                stable_cnt = 0;
            prev_ppm = drift_ppm_med;
            if (stable_cnt >= STABLE_COUNT) {
                stabilized = 1;
                fprintf(stderr, "[aoip_engine] clk2: stabilized at drift_ppm=%+.3f"
                        " ratio=%.7f → switching to %llds interval\n",
                        drift_ppm_med, ratio_med, (long long)SLOW_INTERVAL_S);
            }
        }

        /* 라이브 보정 비활성화 — 라이브 DMA 중 pll_audio_core clk_set_rate 가 xrun 폭주 유발.
         * 잔여 drift 는 DSP SRC 가 ratio_hint 로 흡수. precal 1회로 충분. */

        printf("clk2 aoip_rate=%.3f ravenna_rate=%.3f drift_ppm=%+.3f elapsed=%.0f"
               " dsp_period=%d dsp_tick_ms=%.3f ratio_hint=%.7f ppb=%ld%s%s\n",
               aoip_rate, ravenna_rate, drift_ppm_med, elapsed_s,
               g_period_frames, dsp_tick_ms, ratio_med, tracker_ppb,
               stabilized ? "" : " [fast]",
               frozen ? " [freeze]" : "");
        fflush(stdout);
    }

    *pa_fr  = a_fr;  *pa_hts = a_hts;
    *pr_fr  = r_fr;  *pr_hts = r_hts;
    *next_ns = now_ns + interval_s * 1000000000LL;
}
