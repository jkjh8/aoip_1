/*
 * usb_jack_bridge.c — USB UAC2 Gadget ↔ JACK 브리지
 *
 * 시작: /usr/local/bin/usb_gadget_uac2.sh
 * 종료: /usr/local/bin/usb_gadget_uac2_stop.sh
 *
 * JACK 포트:
 *   <name>:out_1, out_2  — USB 수신(S24_3LE) → JACK 출력
 *   <name>:in_1,  in_2   — JACK 입력 → USB 송신(S24_3LE)
 *
 * ALSA 장치가 없거나(USB 미연결) 오류 시 JACK 포트는 유지하면서
 * 2초마다 재연결을 시도합니다.
 *
 * 사용법:
 *   ./usb_jack_bridge [alsa_device [jack_client_name]]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>

/* ── 설정 ────────────────────────────────────────────────────── */

#define CHANNELS            2
#define SAMPLE_RATE         48000
#define PERIOD_FRAMES       512
#define RINGBUF_SIZE        (1 << 18)   /* 채널당 256 KB */
#define ALSA_RETRY_SEC      2

/* ── 전역 ────────────────────────────────────────────────────── */

static jack_client_t     *g_jack_client;
static jack_port_t       *g_jack_out[CHANNELS]; /* USB→JACK */
static jack_port_t       *g_jack_in[CHANNELS];  /* JACK→USB */
static jack_ringbuffer_t *g_rb_cap[CHANNELS];   /* ALSA 캡처 → JACK */
static jack_ringbuffer_t *g_rb_play[CHANNELS];  /* JACK → ALSA 재생 */

static volatile int       g_running = 1;

/* ALSA 장치 이름 (스레드에 전달) */
static const char        *g_alsa_device;

/* ── 변환: S24_3LE ↔ float32 ─────────────────────────────────── */

static inline float s24_3le_to_float(const uint8_t *p)
{
    int32_t v = (int32_t)p[0]
              | ((int32_t)p[1] << 8)
              | ((int32_t)p[2] << 16);
    if (v & 0x800000)
        v |= (int32_t)0xFF000000;
    return (float)v * (1.0f / 8388608.0f);
}

static inline void float_to_s24_3le(float f, uint8_t *p)
{
    if      (f >  1.0f) f =  1.0f;
    else if (f < -1.0f) f = -1.0f;
    int32_t v = (int32_t)(f * 8388607.0f);
    p[0] = (uint8_t)( v        & 0xFF);
    p[1] = (uint8_t)((v >>  8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}

/* ── JACK 프로세스 콜백 ──────────────────────────────────────── */

static int jack_process(jack_nframes_t nframes, void *arg)
{
    (void)arg;
    const size_t bytes = (size_t)nframes * sizeof(float);

    /* USB→JACK: rb_cap → 출력 포트 */
    for (int ch = 0; ch < CHANNELS; ch++) {
        float *out   = (float *)jack_port_get_buffer(g_jack_out[ch], nframes);
        size_t avail = jack_ringbuffer_read_space(g_rb_cap[ch]);

        if (avail >= bytes) {
            jack_ringbuffer_read(g_rb_cap[ch], (char *)out, bytes);
        } else {
            memset(out, 0, bytes);
            if (avail > 0)
                jack_ringbuffer_read_advance(g_rb_cap[ch], avail);
        }
    }

    /* JACK→USB: 입력 포트 → rb_play */
    for (int ch = 0; ch < CHANNELS; ch++) {
        const float *in = (const float *)jack_port_get_buffer(g_jack_in[ch], nframes);
        if (jack_ringbuffer_write_space(g_rb_play[ch]) >= bytes)
            jack_ringbuffer_write(g_rb_play[ch], (const char *)in, bytes);
    }

    return 0;
}

/* ── ALSA 설정 헬퍼 ──────────────────────────────────────────── */

static int alsa_open(snd_pcm_t **pcm, snd_pcm_stream_t stream)
{
    int err;
    if ((err = snd_pcm_open(pcm, g_alsa_device, stream, 0)) < 0)
        return err;

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(*pcm, hw);
    snd_pcm_hw_params_set_access(*pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(*pcm, hw, SND_PCM_FORMAT_S24_3LE);
    snd_pcm_hw_params_set_channels(*pcm, hw, CHANNELS);

    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(*pcm, hw, &rate, 0);

    snd_pcm_uframes_t period = PERIOD_FRAMES;
    snd_pcm_hw_params_set_period_size_near(*pcm, hw, &period, 0);

    snd_pcm_uframes_t bufsize = period * 4;
    snd_pcm_hw_params_set_buffer_size_near(*pcm, hw, &bufsize);

    if ((err = snd_pcm_hw_params(*pcm, hw)) < 0) {
        snd_pcm_close(*pcm); *pcm = NULL;
        return err;
    }
    if ((err = snd_pcm_prepare(*pcm)) < 0) {
        snd_pcm_close(*pcm); *pcm = NULL;
        return err;
    }
    return 0;
}

/* ── ALSA 캡처 스레드 (USB→JACK) ────────────────────────────── */
/*
 * ALSA 장치가 없으면(USB 미연결) 재시도 루프를 돌며 대기한다.
 * 장치가 생기면 정상 캡처를 시작한다.
 */
static void *alsa_cap_thread(void *arg)
{
    (void)arg;
    const int bpf = CHANNELS * 3;
    uint8_t  *buf = (uint8_t *)malloc((size_t)PERIOD_FRAMES * bpf);
    float     tmp[CHANNELS][PERIOD_FRAMES];

    while (g_running) {
        /* ALSA 장치 열기 시도 */
        snd_pcm_t *pcm = NULL;
        int err = alsa_open(&pcm, SND_PCM_STREAM_CAPTURE);
        if (err < 0) {
            fprintf(stderr, "[usb_jack] capture open failed (%s): %s — retry in %ds\n",
                    g_alsa_device, snd_strerror(err), ALSA_RETRY_SEC);
            sleep(ALSA_RETRY_SEC);
            continue;
        }
        fprintf(stderr, "[usb_jack] capture opened: %s\n", g_alsa_device);

        /* 캡처 루프 */
        while (g_running) {
            snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, PERIOD_FRAMES);
            if (n == -EPIPE) { snd_pcm_prepare(pcm); continue; }
            if (n < 0) {
                if (snd_pcm_recover(pcm, (int)n, 0) < 0) break; /* 재연결 필요 */
                continue;
            }

            for (snd_pcm_sframes_t i = 0; i < n; i++)
                for (int ch = 0; ch < CHANNELS; ch++)
                    tmp[ch][i] = s24_3le_to_float(buf + i * bpf + ch * 3);

            for (int ch = 0; ch < CHANNELS; ch++) {
                size_t bytes = (size_t)n * sizeof(float);
                if (jack_ringbuffer_write_space(g_rb_cap[ch]) >= bytes)
                    jack_ringbuffer_write(g_rb_cap[ch], (const char *)tmp[ch], bytes);
            }
        }

        snd_pcm_close(pcm);
        if (g_running) {
            fprintf(stderr, "[usb_jack] capture disconnected — retry in %ds\n",
                    ALSA_RETRY_SEC);
            sleep(ALSA_RETRY_SEC);
        }
    }

    free(buf);
    return NULL;
}

/* ── ALSA 재생 스레드 (JACK→USB) ────────────────────────────── */

static void *alsa_play_thread(void *arg)
{
    (void)arg;
    const int bpf = CHANNELS * 3;
    float     tmp[CHANNELS][PERIOD_FRAMES];
    uint8_t  *buf = (uint8_t *)malloc((size_t)PERIOD_FRAMES * bpf);

    while (g_running) {
        /* ALSA 장치 열기 시도 */
        snd_pcm_t *pcm = NULL;
        int err = alsa_open(&pcm, SND_PCM_STREAM_PLAYBACK);
        if (err < 0) {
            fprintf(stderr, "[usb_jack] playback open failed (%s): %s — retry in %ds\n",
                    g_alsa_device, snd_strerror(err), ALSA_RETRY_SEC);
            sleep(ALSA_RETRY_SEC);
            continue;
        }
        fprintf(stderr, "[usb_jack] playback opened: %s\n", g_alsa_device);

        /* 재생 루프 */
        while (g_running) {
            /* 데이터 대기 (500µs 폴링, 최대 10ms) */
            size_t need = (size_t)PERIOD_FRAMES * sizeof(float);
            for (int t = 0; t < 20 && g_running; t++) {
                int ready = 1;
                for (int ch = 0; ch < CHANNELS; ch++)
                    if (jack_ringbuffer_read_space(g_rb_play[ch]) < need) {
                        ready = 0; break;
                    }
                if (ready) break;
                usleep(500);
            }

            snd_pcm_sframes_t n = PERIOD_FRAMES;
            for (int ch = 0; ch < CHANNELS; ch++) {
                snd_pcm_sframes_t f =
                    (snd_pcm_sframes_t)(jack_ringbuffer_read_space(g_rb_play[ch])
                                        / sizeof(float));
                if (f < n) n = f;
            }

            if (n <= 0) {
                memset(buf, 0, (size_t)PERIOD_FRAMES * bpf);
                snd_pcm_sframes_t r = snd_pcm_writei(pcm, buf, PERIOD_FRAMES);
                if (r == -EPIPE)    { snd_pcm_prepare(pcm); continue; }
                if (r < 0)          { if (snd_pcm_recover(pcm, (int)r, 0) < 0) break; }
                continue;
            }

            for (int ch = 0; ch < CHANNELS; ch++)
                jack_ringbuffer_read(g_rb_play[ch], (char *)tmp[ch],
                                     (size_t)n * sizeof(float));

            for (snd_pcm_sframes_t i = 0; i < n; i++)
                for (int ch = 0; ch < CHANNELS; ch++)
                    float_to_s24_3le(tmp[ch][i], buf + i * bpf + ch * 3);

            snd_pcm_sframes_t r = snd_pcm_writei(pcm, buf, n);
            if (r == -EPIPE)    { snd_pcm_prepare(pcm); continue; }
            if (r < 0)          { if (snd_pcm_recover(pcm, (int)r, 0) < 0) break; }
        }

        snd_pcm_close(pcm);
        if (g_running) {
            fprintf(stderr, "[usb_jack] playback disconnected — retry in %ds\n",
                    ALSA_RETRY_SEC);
            sleep(ALSA_RETRY_SEC);
        }
    }

    free(buf);
    return NULL;
}

/* ── JACK 종료 콜백 ──────────────────────────────────────────── */

static void jack_shutdown(void *arg)
{
    (void)arg;
    fprintf(stderr, "[usb_jack] JACK 서버 종료\n");
    g_running = 0;
}

static void signal_handler(int sig) { (void)sig; g_running = 0; }

/* ── main ────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    g_alsa_device           = (argc > 1) ? argv[1] : "hw:UAC2Gadget";
    const char *jack_name   = (argc > 2) ? argv[2] : "usb_gadget";

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* 1. USB 가젯 활성화 */
    fprintf(stderr, "[usb_jack] USB 가젯 활성화 중...\n");
    if (system("sudo /usr/local/bin/usb_gadget_uac2.sh") != 0)
        fprintf(stderr, "[usb_jack] 경고: 가젯 활성화 스크립트 실패\n");

    /* 2. JACK 클라이언트 */
    jack_status_t jstatus;
    g_jack_client = jack_client_open(jack_name, JackNullOption, &jstatus);
    if (!g_jack_client) {
        fprintf(stderr, "[usb_jack] JACK 클라이언트 열기 실패 (0x%x)\n", jstatus);
        goto cleanup_gadget;
    }
    jack_set_process_callback(g_jack_client, jack_process, NULL);
    jack_on_shutdown(g_jack_client, jack_shutdown, NULL);

    /* 3. 포트 & ringbuffer */
    for (int ch = 0; ch < CHANNELS; ch++) {
        char name[32];

        snprintf(name, sizeof(name), "out_%d", ch + 1);
        g_jack_out[ch] = jack_port_register(g_jack_client, name,
                                             JACK_DEFAULT_AUDIO_TYPE,
                                             JackPortIsOutput, 0);
        snprintf(name, sizeof(name), "in_%d", ch + 1);
        g_jack_in[ch]  = jack_port_register(g_jack_client, name,
                                             JACK_DEFAULT_AUDIO_TYPE,
                                             JackPortIsInput, 0);
        if (!g_jack_out[ch] || !g_jack_in[ch]) {
            fprintf(stderr, "[usb_jack] JACK 포트 등록 실패 (ch=%d)\n", ch);
            goto cleanup_jack;
        }

        g_rb_cap[ch]  = jack_ringbuffer_create(RINGBUF_SIZE);
        g_rb_play[ch] = jack_ringbuffer_create(RINGBUF_SIZE);
        if (!g_rb_cap[ch] || !g_rb_play[ch]) {
            fprintf(stderr, "[usb_jack] ringbuffer 생성 실패 (ch=%d)\n", ch);
            goto cleanup_jack;
        }
        jack_ringbuffer_mlock(g_rb_cap[ch]);
        jack_ringbuffer_mlock(g_rb_play[ch]);
    }

    /* 4. JACK 활성화 */
    if (jack_activate(g_jack_client) != 0) {
        fprintf(stderr, "[usb_jack] JACK activate 실패\n");
        goto cleanup_jack;
    }

    fprintf(stderr, "[usb_jack] 실행 중. 클라이언트: %s\n", jack_name);
    fprintf(stderr, "[usb_jack]   %s:out_1~%d  USB→JACK\n", jack_name, CHANNELS);
    fprintf(stderr, "[usb_jack]   %s:in_1~%d   JACK→USB\n", jack_name, CHANNELS);
    fprintf(stderr, "[usb_jack] ALSA 장치 연결 대기 중: %s\n", g_alsa_device);

    /* 5. ALSA 스레드 시작 (장치 없어도 재시도 루프로 동작) */
    pthread_t tid_cap, tid_play;
    pthread_create(&tid_cap,  NULL, alsa_cap_thread,  NULL);
    pthread_create(&tid_play, NULL, alsa_play_thread, NULL);

    /* 6. 메인 루프 */
    while (g_running)
        sleep(1);

    fprintf(stderr, "[usb_jack] 종료 중...\n");
    pthread_join(tid_cap,  NULL);
    pthread_join(tid_play, NULL);
    jack_deactivate(g_jack_client);

cleanup_jack:
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (g_rb_cap[ch])  jack_ringbuffer_free(g_rb_cap[ch]);
        if (g_rb_play[ch]) jack_ringbuffer_free(g_rb_play[ch]);
    }
    jack_client_close(g_jack_client);

cleanup_gadget:
    fprintf(stderr, "[usb_jack] USB 가젯 비활성화 중...\n");
    system("sudo /usr/local/bin/usb_gadget_uac2_stop.sh");
    fprintf(stderr, "[usb_jack] 완료.\n");
    return 0;
}
