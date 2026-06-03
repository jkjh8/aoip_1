/*
 * ptp-tai-sync.c
 *
 * PHC → CLOCK_TAI 절대값 동기화 + ptp4l freq → adjtimex + I2S clk 동기화
 *
 * 동작:
 *  1. 시작 시 tick=10000µs 복구 (phc2sys 잔재 제거)
 *  2. PHC 시각을 읽어 CLOCK_TAI를 맞춤 (tai_offset만 조정, CLOCK_REALTIME 불변)
 *  3. journalctl -f ptp4l 로그에서 freq(ppb) 파싱
 *  4. EMA 필터 후 adjtimex(ADJ_FREQUENCY) 적용
 *  5. ptp-i2s-sync 커널 모듈 sysfs로 I2S 하드웨어 클럭도 동시 보정
 *  6. TAI_RESYNC_S마다 PHC 재동기 (crystal drift 보정)
 *  7. SIGTERM/SIGINT 시 freq=0 복귀
 *
 * 빌드:
 *   gcc -O2 -o ptp-tai-sync ptp-tai-sync.c -lm
 *
 * 사용:
 *   ptp-tai-sync [/dev/ptp0]
 *
 * 의존:
 *   ptp-i2s-sync.ko 커널 모듈 로드 시 I2S 클럭 동기화 활성화
 *   모듈 없어도 동작 (adjtimex만 적용)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/timex.h>

/* ── 상수 ──────────────────────────────────────────────────────────────── */
#define CLOCK_TAI_ID    ((clockid_t)11)

/* FD_TO_CLOCKID: PHC 파일 디스크립터 → POSIX clock ID 변환 */
#define CLOCKFD         3
#define FD_TO_CLOCKID(fd) ((clockid_t)((((unsigned int)~(fd)) << 3) | CLOCKFD))

#define ADJ_FREQUENCY   0x0002
#define ADJ_TICK        0x4000
#define ADJ_SETOFFSET   0x0100
#define ADJ_NANO        0x2000

#define TICK_NOMINAL    10000       /* USER_HZ=100 기준 정상값 (µs) */
#define MAX_PPM         500.0       /* adjtimex freq 클램핑 한계 */
#define APPLY_MIN_PPM   0.5         /* 이 미만 변화는 adjtimex 재호출 생략 */
#define EMA_ALPHA       0.95        /* leaky integrator (시상수 ~20샘플) */
#define WARMUP_N        5           /* 초기 N 샘플은 raw 그대로 (빠른 수렴) */

/* ptp-i2s-sync 커널 모듈 sysfs 경로 */
#define I2S_SYSFS_PPB   "/sys/kernel/ptp_i2s_sync/freq_ppb"
#define I2S_SYSFS_RATE  "/sys/kernel/ptp_i2s_sync/rate_hz"
#define OUTLIER_PPM     50.0        /* 이 폭 이상이면 α를 크게 해 천천히 흡수 */
#define TIMEOUT_S       30          /* ptp4l 로그 없으면 freq=0 복귀 */
#define TAI_RESYNC_S    60          /* PHC → CLOCK_TAI 재동기 주기 (s) */

/* ── 전역 ──────────────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ── adjtimex 헬퍼 ─────────────────────────────────────────────────────── */
static void reset_tick(void)
{
    struct timex tx = { .modes = ADJ_TICK, .tick = TICK_NOMINAL };
    adjtimex(&tx);

    struct timex tx2 = { 0 };
    adjtimex(&tx2);
    fprintf(stderr, "ptp-tai-sync: tick reset → %ld µs\n", tx2.tick);
}

static void reset_freq(void)
{
    struct timex tx = { .modes = ADJ_FREQUENCY, .freq = 0 };
    adjtimex(&tx);
}

/* ppb → adjtimex ADJ_FREQUENCY 적용, 클램핑 후 실제 ppm 반환 */
static double set_freq_ppb(double ppb)
{
    double ppm = ppb / 1000.0;
    if (ppm >  MAX_PPM) ppm =  MAX_PPM;
    if (ppm < -MAX_PPM) ppm = -MAX_PPM;

    struct timex tx = {
        .modes = ADJ_FREQUENCY,
        .freq  = (long)(ppm * 65536.0),
    };
    adjtimex(&tx);
    return ppm;
}

/* ppb → ptp-i2s-sync 커널 모듈 sysfs 경유로 I2S 하드웨어 클럭 보정.
 * 모듈이 로드되지 않은 경우 조용히 무시. */
static void set_i2s_ppb(double ppb)
{
    char buf[32];
    int fd, n;

    fd = open(I2S_SYSFS_PPB, O_WRONLY);
    if (fd < 0) return;   /* 모듈 미로드 시 무시 */

    n = snprintf(buf, sizeof(buf), "%ld", (long)ppb);
    if (write(fd, buf, n) < 0)
        fprintf(stderr, "ptp-tai-sync: I2S sysfs write failed: %s\n",
                strerror(errno));
    close(fd);
}

/* 현재 I2S 실제 클럭 주파수 읽기 (로그용) */
static unsigned long get_i2s_rate_hz(void)
{
    char buf[32];
    int fd;
    ssize_t n;

    fd = open(I2S_SYSFS_RATE, O_RDONLY);
    if (fd < 0) return 0;

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    buf[n] = '\0';
    return (unsigned long)strtoul(buf, NULL, 10);
}

/* ── PHC → CLOCK_TAI 동기화 ───────────────────────────────────────────── */
/*
 * PHC 시각을 읽어 CLOCK_TAI에 ADJ_SETOFFSET으로 반영.
 * ADJ_SETOFFSET on CLOCK_TAI → tai_offset 필드만 갱신,
 * CLOCK_REALTIME / CLOCK_MONOTONIC 불변.
 */
static int sync_tai_to_phc(const char *dev)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ptp-tai-sync: open %s: %s\n", dev, strerror(errno));
        return -1;
    }

    clockid_t phc_clk = FD_TO_CLOCKID(fd);
    struct timespec phc_ts;
    if (clock_gettime(phc_clk, &phc_ts) < 0) {
        fprintf(stderr, "ptp-tai-sync: clock_gettime PHC: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    struct timespec tai_ts;
    clock_gettime(CLOCK_TAI_ID, &tai_ts);

    long long off_ns = (long long)(phc_ts.tv_sec - tai_ts.tv_sec) * 1000000000LL
                     + (phc_ts.tv_nsec - tai_ts.tv_nsec);

    /* 1ms 미만이면 재동기 불필요 */
    if (llabs(off_ns) < 1000000LL) {
        fprintf(stderr, "ptp-tai-sync: TAI already in sync (offset=%lld ns)\n", off_ns);
        return 0;
    }

    /* clock_adjtime(CLOCK_TAI, ADJ_SETOFFSET) — tai_offset 갱신 */
    struct timex tx = {
        .modes  = ADJ_SETOFFSET | ADJ_NANO,
        .time   = {
            .tv_sec  = (time_t)(off_ns / 1000000000LL),
            .tv_usec = (long)(off_ns % 1000000000LL),   /* tv_usec = nsec (ADJ_NANO) */
        },
    };
    if (clock_adjtime(CLOCK_TAI_ID, &tx) < 0) {
        fprintf(stderr, "ptp-tai-sync: clock_adjtime CLOCK_TAI: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr, "ptp-tai-sync: TAI sync offset=%lld ns "
                    "(PHC=%ld.%09ld)\n",
            off_ns, phc_ts.tv_sec, phc_ts.tv_nsec);
    return 0;
}

/* ── 메인 ──────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *phc_dev = (argc > 1) ? argv[1] : "/dev/ptp0";

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    reset_tick();
    sync_tai_to_phc(phc_dev);

    FILE *jctl = popen(
        "journalctl -f -u ptp4l -o cat --no-hostname 2>/dev/null",
        "r");
    if (!jctl) {
        perror("ptp-tai-sync: popen journalctl");
        return 1;
    }

    int jfd = fileno(jctl);
    double current_ppm = 0.0;
    double ema_ppm     = 0.0;
    int    ema_valid   = 0;
    int    sample_cnt  = 0;
    time_t last_freq   = time(NULL);
    time_t last_resync = time(NULL);

    char line[512];

    while (!g_quit) {
        /* 1초 타임아웃으로 select — blocking 없이 periodic 작업 처리 */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(jfd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(jfd + 1, &rfds, NULL, NULL, &tv);

        time_t now = time(NULL);

        /* 주기적 TAI 재동기 */
        if (now - last_resync >= TAI_RESYNC_S) {
            sync_tai_to_phc(phc_dev);
            last_resync = now;
        }

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* select timeout — ptp4l 로그 없는 경우 timeout 체크 */
        if (ret == 0) {
            if (now - last_freq > TIMEOUT_S && current_ppm != 0.0) {
                fprintf(stderr,
                    "ptp-tai-sync: %ds ptp4l 로그 없음 — freq=0 복귀\n",
                    TIMEOUT_S);
                reset_freq();
                current_ppm = 0.0;
                ema_valid   = 0;
                sample_cnt  = 0;
            }
            continue;
        }

        if (!fgets(line, sizeof(line), jctl)) break;

        /* freq 파싱: "… freq -13122 …" */
        char *p = strstr(line, "freq ");
        if (!p) continue;

        long ppb;
        if (sscanf(p + 5, "%ld", &ppb) != 1) continue;

        double raw_ppm = (double)ppb / 1000.0;
        last_freq = now;

        /* EMA 갱신 */
        if (!ema_valid || sample_cnt < WARMUP_N) {
            ema_ppm   = raw_ppm;
            ema_valid = 1;
        } else {
            double alpha = (fabs(raw_ppm - ema_ppm) > OUTLIER_PPM)
                         ? 0.98 : EMA_ALPHA;
            ema_ppm = alpha * ema_ppm + (1.0 - alpha) * raw_ppm;
        }
        sample_cnt++;

        if (fabs(ema_ppm - current_ppm) < APPLY_MIN_PPM) continue;

        double applied = set_freq_ppb(ema_ppm * 1000.0);
        set_i2s_ppb(ema_ppm * 1000.0);

        unsigned long i2s_hz = get_i2s_rate_hz();
        if (i2s_hz)
            fprintf(stderr,
                "ptp-tai-sync: freq=%+ldppb ema=%+.3fppm → adjtimex %+.3fppm  i2s=%luHz\n",
                ppb, ema_ppm, applied, i2s_hz);
        else
            fprintf(stderr,
                "ptp-tai-sync: freq=%+ldppb ema=%+.3fppm → adjtimex %+.3fppm\n",
                ppb, ema_ppm, applied);
        current_ppm = applied;
    }

    reset_freq();
    pclose(jctl);
    fprintf(stderr, "ptp-tai-sync: 종료\n");
    return 0;
}
