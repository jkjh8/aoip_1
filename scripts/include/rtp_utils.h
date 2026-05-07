#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static inline int rtp_unix_connect(const char *path, int retries, int ms)
{
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    for (int i = 0; i <= retries; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        close(fd);
        if (i < retries) usleep(ms * 1000);
    }
    return -1;
}

static inline int rtp_read_line(int fd, char *buf, int maxlen)
{
    int n = 0; char c;
    while (n < maxlen - 1) {
        if (read(fd, &c, 1) <= 0) break;
        if (c == '\n') break;
        if (c != '\r') buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

static inline int rtp_cfg_int(const char *s, const char *key, int def)
{
    char pat[64]; int v = def;
    snprintf(pat, sizeof(pat), "%s=%%d", key);
    const char *p = strstr(s, key);
    if (p) sscanf(p, pat, &v);
    return v;
}

static inline void rtp_cfg_str(const char *s, const char *key, char *buf, size_t n, const char *def)
{
    strncpy(buf, def, n); buf[n-1] = '\0';
    char pat[64]; snprintf(pat, sizeof(pat), "%s=%%%zus", key, n-1);
    const char *p = strstr(s, key);
    if (p) sscanf(p, pat, buf);
}
