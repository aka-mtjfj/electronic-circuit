#include <rtthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/*
 * MSH 网络测试命令：
 * udp_test <ip> <port> [msg]
 *
 * 它只做一件事：向指定 IP:端口 发一个 UDP 包。
 * 适合用来确认 ESP8266/SAL/socket 这条网络链路是否已经能发数据。
 */
static void udp_test(int argc, char **argv)
{
    int sock;
    int ret;
    int port;
    struct sockaddr_in dst;
    const char *msg = "hello from rt-thread";

    if (argc < 3)
    {
        rt_kprintf("usage: udp_test <ip> <port> [msg]\n");
        return;
    }

    port = atoi(argv[2]);
    if (port <= 0 || port > 65535)
    {
        rt_kprintf("invalid port: %s\n", argv[2]);
        return;
    }

    if (argc >= 4)
    {
        msg = argv[3];
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        rt_kprintf("create udp socket failed\n");
        return;
    }

    rt_memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = inet_addr(argv[1]);

    ret = sendto(sock, msg, strlen(msg), 0,
                 (struct sockaddr *)&dst, sizeof(dst));
    if (ret < 0)
    {
        rt_kprintf("udp send failed\n");
    }
    else
    {
        rt_kprintf("udp send %d bytes to %s:%d\n", ret, argv[1], port);
    }

    closesocket(sock);
}
MSH_CMD_EXPORT(udp_test, send udp packet: udp_test <ip> <port> [msg]);
