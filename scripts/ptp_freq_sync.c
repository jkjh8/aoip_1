/*
 * ptp_freq_sync.c — PTP 주파수 전용 동기화 데몬
 *
 * /dev/ptp0 주파수를 CLOCK_MONOTONIC_RAW 기준으로 측정하고
 * adjtimex(ADJ_FREQUENCY)로 CLOCK_REALTIME 주파수만 보정.
 * 절대 시간(시각)은 절대 변경하지 않음.
 *
 * 용도: Dante 등 시간 정보 없이 클럭만 제공하는 PTP 마스터의
 *       슬레이브로 동작할 때 CLOCK_REALTIME을 PTP 속도에 맞춤.
 *
 * Build: gcc -O2 -o ptp_freq_sync ptp_freq_sync.c -lm
 * Run:   sudo ./ptp_freq_sync [/dev/ptp0] [측정간격(초)]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/timex.h>
#include <errno.h>
#include <math.h>

/* POSIX dynamic clock ID from fd */
#define CLOCKFD        3
#define FD_TO_CLOCKID(fd)  ((~(clockid_t)(fd) << 3) | CLOCKFD)

/* adjtimex freq 단위: 2^-16 ppm */
#define PPM_TO_FREQ(ppm)  ((long)((ppm) * 65536.0))

/* PI 튜닝 */
#define KP          0.60
#define KI          0.02
#define INTEG_MAX   480.0   /* ±480 PPM clamp (커널 한계 500 PPM) */
#define PPM_SANITY  800.0   /* 이 이상이면 PTP 스텝 이벤트로 간주, skip */

static volatile int g_quit = 0;
static void sig_handler(int s) { (void)s; g_quit = 1; }

static long long ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000000000LL
         + (b->tv_nsec - a->tv_nsec);
}

/* CLOCK_REALTIME 주파수만 조정 — 시간(초 단위)은 건드리지 않음 */
static void apply_freq_ppm(double ppm)
{
    if (ppm >  INTEG_MAX) ppm =  INTEG_MAX;
    if (ppm < -INTEG_MAX) ppm = -INTEG_MAX;

    struct timex tx;
    memset(&tx, 0, sizeof(tx));
    tx.modes = ADJ_FREQUENCY;
    tx.freq  = PPM_TO_FREQ(ppm);
    if (adjtimex(&tx) == TIME_ERROR)
        fprintf(stderr, "[ptp_freq_sync] adjtimex warning (TIME_ERROR)\n");
}

int main(int argc, char *argv[])
{
    const char *dev     = argc > 1 ? argv[1] : "/dev/ptp0";
    double interval_sec = argc > 2 ? atof(argv[2]) : 4.0;

    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[ptp_freq_sync] open %s: %s\n", dev, strerror(errno));
        return 1;
    }
    clockid_t ptp_clkid = FD_TO_CLOCKID(fd);

    /* clock_gettime 가능한지 확인 */
    struct timespec probe;
    if (clock_gettime(ptp_clkid, &probe) < 0) {
        fprintf(stderr, "[ptp_freq_sync] clock_gettime(%s): %s\n",
                dev, strerror(errno));
        close(fd);
        return 1;
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    fprintf(stderr,
            "[ptp_freq_sync] started  dev=%s  interval=%.1fs\n"
            "[ptp_freq_sync] 절대 시간 변경 없음 — 주파수(ADJ_FREQUENCY)만 조정\n",
            dev, interval_sec);

    double integ    = 0.0;
    int    n_sample = 0;
    int    converged = 0;

    struct timespec mono1, ptp1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &mono1);
    clock_gettime(ptp_clkid,           &ptp1);

    while (!g_quit) {
        /* 측정 간격 대기 */
        struct timespec req = {
            .tv_sec  = (time_t)interval_sec,
            .tv_nsec = (long)((interval_sec - (long)interval_sec) * 1e9)
        };
        while (nanosleep(&req, &req) == -1 && errno == EINTR && !g_quit)
            ;
        if (g_quit) break;

        struct timespec mono2, ptp2;
        clock_gettime(CLOCK_MONOTONIC_RAW, &mono2);
        clock_gettime(ptp_clkid,           &ptp2);

        long long mono_ns = ts_diff_ns(&mono1, &mono2);
        long long ptp_ns  = ts_diff_ns(&ptp1,  &ptp2);

        /* 측정값 sanity check: PTP 스텝(시간 점프) 감지 */
        if (mono_ns <= 0 || ptp_ns <= 0) {
            fprintf(stderr, "[ptp_freq_sync] negative delta, reset\n");
            mono1 = mono2; ptp1 = ptp2;
            continue;
        }

        double ptp_ppm = ((double)ptp_ns / (double)mono_ns - 1.0) * 1e6;

        if (fabs(ptp_ppm) > PPM_SANITY) {
            fprintf(stderr,
                    "[ptp_freq_sync] ptp_ppm=%.1f 이상 — PTP 스텝 감지, skip\n",
                    ptp_ppm);
            /* 스텝 후 재기준 */
            mono1 = mono2; ptp1 = ptp2;
            integ = 0.0;
            continue;
        }

        n_sample++;

        /* PI 서보 */
        if (n_sample == 1) integ = ptp_ppm;  /* 첫 샘플: 인테그랄 초기화 */
        integ += ptp_ppm * KI;
        if (integ >  INTEG_MAX) integ =  INTEG_MAX;
        if (integ < -INTEG_MAX) integ = -INTEG_MAX;

        double correction = ptp_ppm * KP + integ;

        apply_freq_ppm(correction);

        /* 수렴 판단: 3회 연속 |ptp_ppm| < 1.0 */
        if (!converged && fabs(ptp_ppm) < 1.0 && n_sample >= 5) {
            converged = 1;
            fprintf(stderr, "[ptp_freq_sync] CONVERGED\n");
        }

        fprintf(stderr,
                "[ptp_freq_sync] [%4d] ptp=%+7.3f PPM  corr=%+7.3f PPM"
                "  integ=%+7.3f%s\n",
                n_sample, ptp_ppm, correction, integ,
                converged ? "  [locked]" : "");

        /* 슬라이딩 윈도우 */
        mono1 = mono2;
        ptp1  = ptp2;
    }

    /* 종료 시 주파수 보정 해제 */
    apply_freq_ppm(0.0);
    fprintf(stderr, "[ptp_freq_sync] stopped — freq reset to 0 PPM\n");

    close(fd);
    return 0;
}
