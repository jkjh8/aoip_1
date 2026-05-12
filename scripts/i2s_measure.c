/*
 * i2s_measure.c — I2S 크리스탈 주파수 측정
 * CLOCK_MONOTONIC(≈PTP) 기준으로 실제 Hz 및 PPM 계산
 *
 * gcc -o i2s_measure i2s_measure.c -lasound
 * ./i2s_measure hw:Device [측정시간(초)]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <alsa/asoundlib.h>

int main(int argc, char *argv[])
{
    const char *dev   = argc > 1 ? argv[1] : "hw:0";
    int nominal       = 48000;
    int measure_sec   = argc > 2 ? atoi(argv[2]) : 60;
    int period_frames = 480;
    int channels      = 2;

    snd_pcm_t *pcm = NULL;
    int err;
    if ((err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "open %s: %s\n", dev, snd_strerror(err));
        return 1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, (unsigned)channels);
    unsigned r = (unsigned)nominal;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0);
    snd_pcm_uframes_t p = (snd_pcm_uframes_t)period_frames;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &p, 0);
    snd_pcm_uframes_t bufsz = p * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufsz);
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "hw_params: %s\n", snd_strerror(err));
        return 1;
    }
    snd_pcm_prepare(pcm);

    int32_t *buf = malloc((size_t)(period_frames * channels) * sizeof(int32_t));
    if (!buf) { perror("malloc"); return 1; }

    int total_periods = (int)((long long)nominal * measure_sec / period_frames);
    printf("[i2s_measure] dev=%s  nominal=48000Hz  측정=%ds (%d periods)\n",
           dev, measure_sec, total_periods);
    fflush(stdout);

    /* 워밍업 1 period — ALSA 스타트 레이턴시 제거 */
    snd_pcm_readi(pcm, buf, (snd_pcm_uframes_t)period_frames);

    struct timespec t1, t2;
    long long total_frames = 0;

    clock_gettime(CLOCK_MONOTONIC, &t1);

    for (int i = 0; i < total_periods; i++) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf,
                                (snd_pcm_uframes_t)period_frames);
        if (n < 0) { snd_pcm_recover(pcm, (int)n, 1); continue; }
        total_frames += n;

        /* 10초마다 중간 보고 */
        if (i > 0 && i % (nominal / period_frames * 10) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t2);
            long long el = (t2.tv_sec - t1.tv_sec) * 1000000000LL
                         + (t2.tv_nsec - t1.tv_nsec);
            double rate = (double)total_frames / el * 1e9;
            printf("  [%3ds] %.4f Hz  (%+.3f PPM)\n",
                   (int)(el / 1000000000LL),
                   rate, (rate - nominal) / nominal * 1e6);
            fflush(stdout);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t2);
    long long elapsed_ns = (t2.tv_sec - t1.tv_sec) * 1000000000LL
                         + (t2.tv_nsec - t1.tv_nsec);

    double actual_rate = (double)total_frames / (double)elapsed_ns * 1e9;
    double ppm         = (actual_rate - nominal) / nominal * 1e6;
    double drift_fps   = actual_rate - nominal;
    if (drift_fps < 0) drift_fps = -drift_fps;

    printf("\n=== 결과 ===\n");
    printf("측정 시간:       %.3f 초\n", elapsed_ns / 1e9);
    printf("캡처 프레임:     %lld\n", total_frames);
    printf("실제 샘플레이트: %.6f Hz\n", actual_rate);
    printf("PPM 오차:        %+.3f PPM\n", ppm);
    if (drift_fps > 0.0001)
        printf("72frame 슬랙 기준 slip 주기: %.1f 초\n", 72.0 / drift_fps);
    else
        printf("드리프트 없음\n");

    free(buf);
    snd_pcm_close(pcm);
    return 0;
}
