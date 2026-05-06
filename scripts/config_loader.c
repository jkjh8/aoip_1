#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "include/config_loader.h"
#include "include/engine_globals.h"
#include "include/engine_constants.h"
#include "include/clk2.h"

static int json_bool(const char *json, const char *key, int def)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return def;
    p = strchr(p + strlen(pat), ':');
    if (!p) return def;
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true",  4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return def;
}

static int json_int(const char *json, const char *key, int def)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return def;
    return atoi(p);
}

void load_config_prios(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char buf[8192] = "";
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char *sec = strstr(buf, "\"engine\"");
    if (!sec) return;
    const char *start = strchr(sec, '{');
    if (!start) return;
    const char *end = strchr(start, '}');
    if (!end) return;

    char section[512] = "";
    size_t len = (size_t)(end - start + 1);
    if (len >= sizeof(section)) len = sizeof(section) - 1;
    memcpy(section, start, len);
    section[len] = '\0';

    int v;
    if ((v = json_int(section, "dspPrio",     0)) > 0) g_prio_dsp     = v;
    if ((v = json_int(section, "alsaPrio",    0)) > 0) g_prio_alsa    = v;
    if ((v = json_int(section, "ravennaPrio", 0)) > 0) g_prio_ravenna = v;
    if ((v = json_int(section, "rtpPrio",     0)) > 0) g_prio_rtp     = v;
    if ((v = json_int(section, "periodFrames", 0)) > 0 && v <= MAX_PERIOD_FRAMES)
        g_period_frames = v;
    g_lvl_report  = json_bool(section, "lvlReport", 1);
    g_clk2_report = g_lvl_report;
    if ((v = json_int(section, "ravennaFillFrames", 0)) > 0)
        g_ravenna_fill_target = v;

    int min_fill = g_period_frames + 4;
    if (g_ravenna_fill_target < min_fill) {
        fprintf(stderr, "[aoip_engine] ravennaFillFrames %d < minimum %d, clamping\n",
                g_ravenna_fill_target, min_fill);
        g_ravenna_fill_target = min_fill;
    }

    fprintf(stderr, "[aoip_engine] config: dsp=%d alsa=%d ravenna=%d rtp=%d period=%d "
            "lvl=%d clk2=%d ravennaFillFrames=%d\n",
            g_prio_dsp, g_prio_alsa, g_prio_ravenna, g_prio_rtp,
            g_period_frames, g_lvl_report, g_clk2_report, g_ravenna_fill_target);
}
