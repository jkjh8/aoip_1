/*
 * ptp_mcast_loop.c — LD_PRELOAD shim for ptp4l
 *
 * ptp4l은 setsockopt(IP_MULTICAST_LOOP, 0)으로 멀티캐스트 루프백을 꺼버린다.
 * 그러면 같은 호스트의 aes67-daemon이 ptp4l의 PTP 패킷을 수신할 수 없다.
 * 이 shim은 해당 setsockopt 호출을 가로채서 루프백을 항상 켠 상태로 유지한다.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <dlfcn.h>

static int (*_real_setsockopt)(int, int, int, const void *, socklen_t);

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen)
{
    if (!_real_setsockopt)
        _real_setsockopt = dlsym(RTLD_NEXT, "setsockopt");

    /* IP_MULTICAST_LOOP / IPV6_MULTICAST_LOOP 을 끄려는 시도를 무시 */
    if ((level == IPPROTO_IP   && optname == IP_MULTICAST_LOOP) ||
        (level == IPPROTO_IPV6 && optname == IPV6_MULTICAST_LOOP)) {
        const int on = 1;
        return _real_setsockopt(sockfd, level, optname, &on, sizeof(on));
    }

    return _real_setsockopt(sockfd, level, optname, optval, optlen);
}
