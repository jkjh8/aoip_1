/*
 * ravenna_ctl — RAVENNA/AES67 ALSA LKM control via netlink
 * Communicates with the RAVENNA kernel module using the same protocol as
 * aes67-linux-daemon (Merging Technologies netlink IDs 31/29).
 *
 * stdin protocol (one command per line, space-separated):
 *   init <iface> <ptp_domain> <ptp_dscp> <playout_delay_samples>
 *   add_src  <id> <name> <src_ip> <dst_ip> <port> <rate> <ch> <codec> <ssrc> <pt> <ttl> <dscp>
 *   add_sink <id> <name> <src_ip> <dst_ip> <port> <rate> <ch> <codec> <ssrc> <pt> <delay_samples>
 *   remove   <handle>
 *   ptp_status
 *   ptp_config <domain> <dscp>
 *   stream_status <handle>
 *   quit
 *
 * stdout:
 *   ok [value]      — success, optional value (e.g. handle as decimal u64)
 *   error <message> — failure
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <sys/time.h>

/* ── RAVENNA kernel module constants ──────────────────────────── */

#define NETLINK_U2K_ID  31
#define NETLINK_K2U_ID  29
#define MAX_PAYLOAD     1024

enum MT_ALSA_msg_id {
    MT_ALSA_Msg_Start = 0,
    MT_ALSA_Msg_Stop,
    MT_ALSA_Msg_Reset,
    MT_ALSA_Msg_StartIO,
    MT_ALSA_Msg_StopIO,
    MT_ALSA_Msg_SetSampleRate,
    MT_ALSA_Msg_GetSampleRate,
    MT_ALSA_Msg_GetAudioMode,
    MT_ALSA_Msg_SetDSDAudioMode,
    MT_ALSA_Msg_SetTICFrameSizeAt1FS,
    MT_ALSA_Msg_SetMaxTICFrameSize,
    MT_ALSA_Msg_SetNumberOfInputs,
    MT_ALSA_Msg_SetNumberOfOutputs,
    MT_ALSA_Msg_GetNumberOfInputs,
    MT_ALSA_Msg_GetNumberOfOutputs,
    MT_ALSA_Msg_SetInterfaceName,
    MT_ALSA_Msg_Add_RTPStream,
    MT_ALSA_Msg_Remove_RTPStream,
    MT_ALSA_Msg_Update_RTPStream_Name,
    MT_ALSA_Msg_GetPTPInfo,
    MT_ALSA_Msg_Hello,
    MT_ALSA_Msg_Bye,
    MT_ALSA_Msg_Ping,
    MT_ALSA_Msg_SetMasterOutputVolume,
    MT_ALSA_Msg_SetMasterOutputSwitch,
    MT_ALSA_Msg_GetMasterOutputVolume,
    MT_ALSA_Msg_GetMasterOutputSwitch,
    MT_ALSA_Msg_SetPlayoutDelay,
    MT_ALSA_Msg_SetCaptureDelay,
    MT_ALSA_Msg_GetRTPStreamStatus,
    MT_ALSA_Msg_SetPTPConfig,
    MT_ALSA_Msg_GetPTPConfig,
    MT_ALSA_Msg_GetPTPStatus,
};

struct MT_ALSA_msg {
    int   id;
    int   errCode;
    int   dataSize;
    void *data;   /* pointer — data bytes follow inline in netlink payload */
};

/* ── RAVENNA stream info struct (packed, same layout as kernel) ── */

#pragma pack(push, 1)

#define MAX_STREAM_NAME_SIZE        64
#define MAX_CODEC_NAME_SIZE         10
#define MAX_CHANNELS_BY_RTP_STREAM  64

typedef struct {
    uint32_t  m_ui32CRTP_stream_info_sizeof;
    int8_t    m_b802_1Q;
    int16_t   m_ui16VLAN_Id;
    uint32_t  m_uiIfPortId;
    char      m_cName[MAX_STREAM_NAME_SIZE];
    uint32_t  m_ui32PlayOutDelay;
    uint32_t  m_ui32FrameSize;
    uint32_t  m_ui32MaxSamplesPerPacket;
    uint8_t   m_ui8DestMAC[6];
    uint8_t   m_ucDSCP;
    uint32_t  m_ui32RTCPSrcIP;
    uint32_t  m_ui32SrcIP;
    uint32_t  m_ui32DestIP;
    uint8_t   m_byTTL;
    uint16_t  m_usSrcPort;
    uint16_t  m_usDestPort;
    uint16_t  m_usRTCPSrcPort;
    uint16_t  m_usRTCPDestPort;
    uint8_t   m_byPayloadType;
    uint32_t  m_ui32SSRC;
    int8_t    m_bSSRCInitialized;
    uint32_t  m_ui32RTPTimestampOffset;
    uint32_t  m_ui32SamplingRate;
    char      m_cCodec[MAX_CODEC_NAME_SIZE];
    uint8_t   m_byWordLength;
    uint8_t   m_byNbOfChannels;
    int8_t    m_bSource;
    uint32_t  m_uiId;
    uint8_t   m_bIsPrimaryPort;
    uint32_t  m_aui32Routing[MAX_CHANNELS_BY_RTP_STREAM];
} TRTP_stream_info;

typedef struct {
    uint8_t ui8Domain;
    uint8_t ui8DSCP;
} TPTPConfig;

typedef struct {
    uint32_t nPTPLockStatus;   /* 0=unlocked 1=locking 2=locked */
    uint64_t ui64GMID[2];
    int32_t  i32GMIDStats[2];
    int32_t  i32NetworkJitter;
    int32_t  i32ClockJitter;
} TPTPStatus;
typedef struct {
    union {
        struct {
            uint32_t seq_id_error    : 1;
            uint32_t ssrc_error      : 1;
            uint32_t pt_error        : 1;
            uint32_t sac_error       : 1;
            uint32_t receiving       : 1;
            uint32_t muted           : 1;
            uint32_t some_muted      : 1;
            uint32_t all_muted       : 1;
        } bits;
        uint32_t flags;
    } u;
    int32_t sink_min_time;
} TRTP_stream_status;

#pragma pack(pop)

/* ── Netlink helper ───────────────────────────────────────────── */

static int nl_sock = -1;

static int nl_open(void) {
    nl_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_U2K_ID);
    if (nl_sock < 0) return -1;

    struct sockaddr_nl sa = {0};
    sa.nl_family = AF_NETLINK;
    sa.nl_pid    = getpid();
    if (bind(nl_sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(nl_sock);
        nl_sock = -1;
        return -1;
    }
    return 0;
}

/* send a command and wait for reply; reply data copied to reply_buf (up to reply_max) */
static int nl_cmd(enum MT_ALSA_msg_id id,
                  const void *data, size_t data_size,
                  void *reply_buf, size_t reply_max)
{
    /* Build netlink message */
    size_t msg_len = NLMSG_SPACE(sizeof(struct MT_ALSA_msg) + data_size);
    uint8_t *buf = calloc(1, msg_len);
    if (!buf) return -1;

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_len   = NLMSG_LENGTH(sizeof(struct MT_ALSA_msg) + data_size);
    nlh->nlmsg_type  = NLMSG_DONE;
    nlh->nlmsg_flags = 0;
    nlh->nlmsg_seq   = 0;
    nlh->nlmsg_pid   = getpid();

    struct MT_ALSA_msg *msg = (struct MT_ALSA_msg *)NLMSG_DATA(nlh);
    msg->id       = id;
    msg->errCode  = 0;
    msg->dataSize = (int)data_size;
    msg->data     = NULL;
    if (data && data_size > 0)
        memcpy((uint8_t *)msg + sizeof(struct MT_ALSA_msg), data, data_size);

    struct sockaddr_nl dst = {0};
    dst.nl_family = AF_NETLINK;
    dst.nl_pid    = 0;   /* kernel */

    if (sendto(nl_sock, buf, nlh->nlmsg_len, 0,
               (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        free(buf);
        return -1;
    }
    free(buf);

    /* Wait for reply (timeout 2s) */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(nl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t rbuf[NLMSG_SPACE(MAX_PAYLOAD + sizeof(struct MT_ALSA_msg))];
    ssize_t n = recv(nl_sock, rbuf, sizeof(rbuf), 0);
    if (n < 0) return -1;

    struct nlmsghdr *rnlh = (struct nlmsghdr *)rbuf;
    if (!NLMSG_OK(rnlh, (unsigned)n)) return -1;

    struct MT_ALSA_msg *rmsg = (struct MT_ALSA_msg *)NLMSG_DATA(rnlh);
    if (rmsg->errCode != 0) return rmsg->errCode;

    if (reply_buf && rmsg->dataSize > 0) {
        size_t copy = rmsg->dataSize < (int)reply_max ? rmsg->dataSize : reply_max;
        memcpy(reply_buf, (uint8_t *)rmsg + sizeof(struct MT_ALSA_msg), copy);
    }
    return 0;
}

/* ── Codec word length helper ─────────────────────────────────── */

static uint8_t codec_word_len(const char *codec) {
    if (!strcmp(codec, "L16"))  return 2;
    if (!strcmp(codec, "L24"))  return 3;
    if (!strcmp(codec, "AM824")) return 4;
    return 3;
}

/* ── IP string → uint32 (host byte order) ────────────────────── */
static uint32_t ip4(const char *s) {
    struct in_addr a;
    inet_aton(s, &a);
    return ntohl(a.s_addr);
}

/* ── Multicast MAC from IPv4 multicast address ────────────────── */
static void mcast_mac(uint32_t dest_ip_host, uint8_t mac[6]) {
    /* RFC 1112: 01:00:5E:XX:YY:ZZ where XX = ip[1]&0x7F, YY = ip[2], ZZ = ip[3] */
    mac[0] = 0x01; mac[1] = 0x00; mac[2] = 0x5E;
    mac[3] = (dest_ip_host >> 16) & 0x7F;
    mac[4] = (dest_ip_host >>  8) & 0xFF;
    mac[5] =  dest_ip_host        & 0xFF;
}

#define IN_MULTICAST_HE(ip) (((ip) & 0xF0000000) == 0xE0000000)

/* ── Command handlers ─────────────────────────────────────────── */

/* init <iface> <ptp_domain> <ptp_dscp> <playout_delay>
 *      <capture_delay> <sample_rate> <tic_frame_size> <max_tic_frame_size>
 *      <num_inputs> <num_outputs>
 */
static void cmd_init(char *iface, int domain, int dscp,
                     int playout, int capture,
                     uint32_t rate, uint64_t tic, uint64_t max_tic,
                     int n_in, int n_out)
{
    if (nl_cmd(MT_ALSA_Msg_Hello, NULL, 0, NULL, 0) < 0) {
        puts("error hello failed — kernel module not loaded?");
        return;
    }
    nl_cmd(MT_ALSA_Msg_Start, NULL, 0, NULL, 0);
    nl_cmd(MT_ALSA_Msg_Reset, NULL, 0, NULL, 0);

    nl_cmd(MT_ALSA_Msg_SetInterfaceName, iface, strlen(iface) + 1, NULL, 0);

    TPTPConfig ptpcfg = { (uint8_t)domain, (uint8_t)dscp };
    nl_cmd(MT_ALSA_Msg_SetPTPConfig, &ptpcfg, sizeof(ptpcfg), NULL, 0);

    /* clock */
    nl_cmd(MT_ALSA_Msg_SetSampleRate, &rate, sizeof(rate), NULL, 0);

    /* I/O channel counts */
    int32_t ni = n_in,  no = n_out;
    nl_cmd(MT_ALSA_Msg_SetNumberOfInputs,  &ni, sizeof(ni), NULL, 0);
    nl_cmd(MT_ALSA_Msg_SetNumberOfOutputs, &no, sizeof(no), NULL, 0);

    /* frame / buffer sizes */
    nl_cmd(MT_ALSA_Msg_SetTICFrameSizeAt1FS, &tic,     sizeof(tic),     NULL, 0);
    nl_cmd(MT_ALSA_Msg_SetMaxTICFrameSize,   &max_tic, sizeof(max_tic), NULL, 0);

    /* output + input playout delay */
    int32_t pd = playout, cd = capture;
    nl_cmd(MT_ALSA_Msg_SetPlayoutDelay, &pd, sizeof(pd), NULL, 0);
    nl_cmd(MT_ALSA_Msg_SetCaptureDelay, &cd, sizeof(cd), NULL, 0);

    /* start ALSA I/O — makes hw:RAVENNA accessible to applications */
    nl_cmd(MT_ALSA_Msg_StartIO, NULL, 0, NULL, 0);

    puts("ok");
}

static void cmd_set_sample_rate(uint32_t rate) {
    int rc = nl_cmd(MT_ALSA_Msg_SetSampleRate, &rate, sizeof(rate), NULL, 0);
    if (rc != 0) printf("error set_sample_rate rc=%d\n", rc);
    else         puts("ok");
}

static void cmd_set_playout_delay(int32_t delay) {
    int rc = nl_cmd(MT_ALSA_Msg_SetPlayoutDelay, &delay, sizeof(delay), NULL, 0);
    if (rc != 0) printf("error set_playout_delay rc=%d\n", rc);
    else         puts("ok");
}

static void cmd_set_capture_delay(int32_t delay) {
    int rc = nl_cmd(MT_ALSA_Msg_SetCaptureDelay, &delay, sizeof(delay), NULL, 0);
    if (rc != 0) printf("error set_capture_delay rc=%d\n", rc);
    else         puts("ok");
}

static void cmd_set_tic_frame_size(uint64_t size) {
    int rc = nl_cmd(MT_ALSA_Msg_SetTICFrameSizeAt1FS, &size, sizeof(size), NULL, 0);
    if (rc != 0) printf("error set_tic_frame_size rc=%d\n", rc);
    else         puts("ok");
}

static void cmd_set_max_tic_frame_size(uint64_t size) {
    int rc = nl_cmd(MT_ALSA_Msg_SetMaxTICFrameSize, &size, sizeof(size), NULL, 0);
    if (rc != 0) printf("error set_max_tic_frame_size rc=%d\n", rc);
    else         puts("ok");
}

static void cmd_set_num_inputs(int32_t n) {
    int rc = nl_cmd(MT_ALSA_Msg_SetNumberOfInputs, &n, sizeof(n), NULL, 0);
    if (rc != 0) printf("error set_num_inputs rc=%d\n", rc);
    else         puts("ok");
}

static void cmd_set_num_outputs(int32_t n) {
    int rc = nl_cmd(MT_ALSA_Msg_SetNumberOfOutputs, &n, sizeof(n), NULL, 0);
    if (rc != 0) printf("error set_num_outputs rc=%d\n", rc);
    else         puts("ok");
}

static void cmd_add_stream(int is_src, uint32_t id, const char *name,
                           const char *src_ip, const char *dst_ip,
                           int port, uint32_t rate, int ch,
                           const char *codec, uint32_t ssrc, int pt,
                           int ttl_or_delay, int dscp)
{
    TRTP_stream_info info;
    memset(&info, 0, sizeof(info));
    info.m_ui32CRTP_stream_info_sizeof = sizeof(TRTP_stream_info);
    info.m_ui32SamplingRate   = rate;
    info.m_byNbOfChannels     = (uint8_t)ch;
    info.m_byPayloadType      = (uint8_t)pt;
    info.m_ui32SSRC           = ssrc;
    info.m_bSSRCInitialized   = ssrc ? 1 : 0;
    info.m_ucDSCP             = (uint8_t)dscp;
    info.m_uiId               = id;
    info.m_bSource            = is_src ? 1 : 0;
    info.m_bIsPrimaryPort     = 1;
    info.m_usDestPort         = (uint16_t)port;
    info.m_ui32DestIP         = ip4(dst_ip);
    info.m_ui32SrcIP          = ip4(src_ip);
    /* 1ms packet time: samples = rate/1000, e.g. 48000→48, 96000→96 */
    uint32_t spp = rate / 1000;
    info.m_ui32MaxSamplesPerPacket = spp;
    info.m_ui32FrameSize           = spp;

    /* RTCPSrcIP must equal the local source IP — kernel is_valid() rejects 0 */
    info.m_ui32RTCPSrcIP = ip4(src_ip);
    info.m_usSrcPort     = (uint16_t)port;

    /* multicast destinations need the derived MAC address */
    if (IN_MULTICAST_HE(info.m_ui32DestIP))
        mcast_mac(info.m_ui32DestIP, info.m_ui8DestMAC);

    if (is_src) {
        info.m_byTTL = (uint8_t)ttl_or_delay;
    } else {
        info.m_ui32PlayOutDelay = (uint32_t)ttl_or_delay;
    }

    strncpy(info.m_cName, name, MAX_STREAM_NAME_SIZE - 1);
    strncpy(info.m_cCodec, codec, MAX_CODEC_NAME_SIZE - 1);
    info.m_byWordLength = codec_word_len(codec);

    /* default routing: channel n → physical channel n */
    for (int i = 0; i < ch && i < MAX_CHANNELS_BY_RTP_STREAM; i++)
        info.m_aui32Routing[i] = i;
    for (int i = ch; i < MAX_CHANNELS_BY_RTP_STREAM; i++)
        info.m_aui32Routing[i] = 0xFFFFFFFF;

    uint64_t handle = 0;
    int rc = nl_cmd(MT_ALSA_Msg_Add_RTPStream, &info, sizeof(info),
                    &handle, sizeof(handle));
    if (rc != 0)
        printf("error add_stream rc=%d\n", rc);
    else
        printf("ok %llu\n", (unsigned long long)handle);
}

static void cmd_remove(uint64_t handle) {
    int rc = nl_cmd(MT_ALSA_Msg_Remove_RTPStream, &handle, sizeof(handle), NULL, 0);
    if (rc != 0) printf("error remove rc=%d\n", rc);
    else         puts("ok");
}

static void cmd_ptp_status(void) {
    TPTPStatus st;
    memset(&st, 0, sizeof(st));
    int rc = nl_cmd(MT_ALSA_Msg_GetPTPStatus, NULL, 0, &st, sizeof(st));
    if (rc != 0) {
        printf("error ptp_status rc=%d\n", rc);
        return;
    }
    /* Format GMID as EUI-64 hex string from first uint64 (big-endian bytes) */
    uint64_t gmid = st.ui64GMID[0];
    char gmid_str[24];
    snprintf(gmid_str, sizeof(gmid_str),
             "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             (uint8_t)(gmid >> 56), (uint8_t)(gmid >> 48),
             (uint8_t)(gmid >> 40), (uint8_t)(gmid >> 32),
             (uint8_t)(gmid >> 24), (uint8_t)(gmid >> 16),
             (uint8_t)(gmid >>  8), (uint8_t)(gmid));
    printf("ok %d %d %d %s\n",
           st.nPTPLockStatus,
           st.i32ClockJitter,
           st.i32NetworkJitter,
           gmid_str);
}

static void cmd_get_ptp_config(void) {
    TPTPConfig cfg = {0};
    int rc = nl_cmd(MT_ALSA_Msg_GetPTPConfig, NULL, 0, &cfg, sizeof(cfg));
    if (rc != 0) printf("error get_ptp_config rc=%d\n", rc);
    else         printf("ok %d %d\n", cfg.ui8Domain, cfg.ui8DSCP);
}

static void cmd_ptp_config(int domain, int dscp) {
    TPTPConfig cfg = { (uint8_t)domain, (uint8_t)dscp };
    int rc = nl_cmd(MT_ALSA_Msg_SetPTPConfig, &cfg, sizeof(cfg), NULL, 0);
    if (rc != 0) printf("error ptp_config rc=%d\n", rc);
    else         puts("ok");
}

static void cmd_get_sample_rate(void) {
    uint32_t rate = 0;
    int rc = nl_cmd(MT_ALSA_Msg_GetSampleRate, NULL, 0, &rate, sizeof(rate));
    if (rc != 0) printf("error get_sample_rate rc=%d\n", rc);
    else         printf("ok %u\n", rate);
}

static void cmd_stream_status(uint64_t handle) {
    TRTP_stream_status st;
    memset(&st, 0, sizeof(st));
    int rc = nl_cmd(MT_ALSA_Msg_GetRTPStreamStatus, &handle, sizeof(handle),
                    &st, sizeof(st));
    if (rc != 0) {
        printf("error stream_status rc=%d\n", rc);
        return;
    }
    printf("ok %u %d\n", st.u.flags, st.sink_min_time);
}

/* ── Main loop ────────────────────────────────────────────────── */

int main(void) {
    if (nl_open() < 0) {
        fprintf(stderr, "[ravenna_ctl] netlink open failed: %s\n", strerror(errno));
        /* don't exit — commands will report errors individually */
    }

    /* line-buffered stdout so Node.js gets replies immediately */
    setvbuf(stdout, NULL, _IOLBF, 0);

    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        /* strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;

        char cmd[64] = {0};
        sscanf(line, "%63s", cmd);

        if (!strcmp(cmd, "quit")) {
            nl_cmd(MT_ALSA_Msg_StopIO, NULL, 0, NULL, 0);
            nl_cmd(MT_ALSA_Msg_Bye,    NULL, 0, NULL, 0);
            break;

        } else if (!strcmp(cmd, "init")) {
            char iface[64] = "eth0";
            int domain = 0, dscp = 46, playout = 576, capture = 576;
            unsigned rate = 48000;
            unsigned long long tic = 48, max_tic = 192;
            int n_in = 8, n_out = 8;
            sscanf(line, "%*s %63s %d %d %d %d %u %llu %llu %d %d",
                   iface, &domain, &dscp, &playout, &capture,
                   &rate, &tic, &max_tic, &n_in, &n_out);
            cmd_init(iface, domain, dscp, playout, capture,
                     (uint32_t)rate, (uint64_t)tic, (uint64_t)max_tic,
                     n_in, n_out);

        } else if (!strcmp(cmd, "add_src") || !strcmp(cmd, "add_sink")) {
            int is_src = (cmd[4] == 's');
            uint32_t id = 0, ssrc = 0, rate = 48000;
            int port = 5004, ch = 2, pt = 98, extra = 0, dscp = 34;
            char name[64]="", src_ip[32]="", dst_ip[32]="", codec[16]="L24";
            sscanf(line, "%*s %u %63s %31s %31s %d %u %d %15s %u %d %d %d",
                   &id, name, src_ip, dst_ip, &port, &rate, &ch, codec,
                   &ssrc, &pt, &extra, &dscp);
            cmd_add_stream(is_src, id, name, src_ip, dst_ip,
                           port, rate, ch, codec, ssrc, pt, extra, dscp);

        } else if (!strcmp(cmd, "remove")) {
            uint64_t handle = 0;
            sscanf(line, "%*s %llu", (unsigned long long *)&handle);
            cmd_remove(handle);

        } else if (!strcmp(cmd, "ptp_status")) {
            cmd_ptp_status();

        } else if (!strcmp(cmd, "get_ptp_config")) {
            cmd_get_ptp_config();

        } else if (!strcmp(cmd, "ptp_config")) {
            int domain = 0, dscp = 46;
            sscanf(line, "%*s %d %d", &domain, &dscp);
            cmd_ptp_config(domain, dscp);

        } else if (!strcmp(cmd, "get_sample_rate")) {
            cmd_get_sample_rate();

        } else if (!strcmp(cmd, "set_sample_rate")) {
            unsigned rate = 48000;
            sscanf(line, "%*s %u", &rate);
            cmd_set_sample_rate((uint32_t)rate);

        } else if (!strcmp(cmd, "set_playout_delay")) {
            int d = 576;
            sscanf(line, "%*s %d", &d);
            cmd_set_playout_delay((int32_t)d);

        } else if (!strcmp(cmd, "set_capture_delay")) {
            int d = 576;
            sscanf(line, "%*s %d", &d);
            cmd_set_capture_delay((int32_t)d);

        } else if (!strcmp(cmd, "set_tic_frame_size")) {
            unsigned long long s = 48;
            sscanf(line, "%*s %llu", &s);
            cmd_set_tic_frame_size((uint64_t)s);

        } else if (!strcmp(cmd, "set_max_tic_frame_size")) {
            unsigned long long s = 192;
            sscanf(line, "%*s %llu", &s);
            cmd_set_max_tic_frame_size((uint64_t)s);

        } else if (!strcmp(cmd, "set_num_inputs")) {
            int n = 8;
            sscanf(line, "%*s %d", &n);
            cmd_set_num_inputs((int32_t)n);

        } else if (!strcmp(cmd, "set_num_outputs")) {
            int n = 8;
            sscanf(line, "%*s %d", &n);
            cmd_set_num_outputs((int32_t)n);

        } else if (!strcmp(cmd, "stream_status")) {
            uint64_t handle = 0;
            sscanf(line, "%*s %llu", (unsigned long long *)&handle);
            cmd_stream_status(handle);

        } else {
            printf("error unknown command: %s\n", cmd);
        }
    }

    if (nl_sock >= 0) close(nl_sock);
    return 0;
}
