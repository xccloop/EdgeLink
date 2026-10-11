/*
    cansetup —— 在开发板上配置 SocketCAN 接口。

    为什么需要它：

    真实 CAN 控制器在 up 之前必须先设好波特率，否则内核直接报
        ip: SIOCSIFFLAGS: Invalid argument
    而开发板上的 ip 是 BusyBox 版本，不支持
        ip link set can0 type can bitrate 500000
    这条命令（iproute2 才有）。板上装不了 iproute2，也没有编译器。

    这个程序用 rtnetlink 做与上面那条命令等价的事，不依赖任何外部工具：

        cansetup can0 500000

    用法：
        cansetup <接口名> <波特率>     例如  cansetup can0 500000
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <net/if.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/can/netlink.h>

/* 往 nlmsghdr 末尾追加一个属性 */
static void addattr(struct nlmsghdr *nh, int type, const void *data, int len)
{
    struct rtattr *rta =
        (struct rtattr *)((char *)nh + NLMSG_ALIGN(nh->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len  = RTA_LENGTH(len);
    if (len > 0) {
        memcpy(RTA_DATA(rta), data, len);
    }
    nh->nlmsg_len = NLMSG_ALIGN(nh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
}

/* 开始一个嵌套属性（RTA_LENGTH(0)），返回句柄给 nest_end 收尾 */
static struct rtattr *nest_start(struct nlmsghdr *nh, int type)
{
    struct rtattr *rta =
        (struct rtattr *)((char *)nh + NLMSG_ALIGN(nh->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len  = RTA_LENGTH(0);
    nh->nlmsg_len = NLMSG_ALIGN(nh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    return rta;
}

static void nest_end(struct nlmsghdr *nh, struct rtattr *rta)
{
    rta->rta_len = (char *)nh + NLMSG_ALIGN(nh->nlmsg_len) - (char *)rta;
}

/* 发送一条 netlink 请求并等 ACK；成功返回 0，失败打印原因返回 -1 */
static int nl_talk(int fd, struct nlmsghdr *nh)
{
    struct sockaddr_nl sa;
    struct iovec iov;
    struct msghdr msg;
    char buf[4096];
    int len;
    struct nlmsghdr *rh;

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    nh->nlmsg_seq = 1;
    nh->nlmsg_pid = 0;

    iov.iov_base = nh;
    iov.iov_len  = nh->nlmsg_len;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name    = &sa;
    msg.msg_namelen = sizeof(sa);
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;

    if (sendmsg(fd, &msg, 0) < 0) {
        perror("sendmsg");
        return -1;
    }

    len = recv(fd, buf, sizeof(buf), 0);
    if (len < 0) {
        perror("recv");
        return -1;
    }

    for (rh = (struct nlmsghdr *)buf; NLMSG_OK(rh, len);
         rh = NLMSG_NEXT(rh, len)) {
        if (rh->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(rh);
            if (e->error != 0) {
                errno = -e->error;
                perror("netlink");
                return -1;
            }
            return 0;
        }
    }
    return 0;
}

/* 第一步：设置 CAN 波特率（等价于 ip link set can0 type can bitrate N）
 *
 * 属性必须这样嵌套，少一层内核就找不到 can 的 changelink：
 *
 *   IFLA_LINKINFO
 *     IFLA_INFO_KIND   = "can"
 *     IFLA_INFO_DATA
 *       IFLA_CAN_BITTIMING = { bitrate }
 *
 * 注意：IFLA_INFO_KIND / IFLA_INFO_DATA 是 IFLA_LINKINFO 的子属性，
 * 不是顶层的。放到顶层会被当成 IFLA_UNSPEC / IFLA_ADDRESS。
 */
static int set_bitrate(int fd, int ifindex, unsigned int bitrate)
{
    char req[512];
    struct nlmsghdr *nh = (struct nlmsghdr *)req;
    struct ifinfomsg *ifi;
    struct rtattr *linkinfo;
    struct rtattr *info_data;
    struct can_bittiming bt;

    memset(req, 0, sizeof(req));
    nh->nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nh->nlmsg_type  = RTM_NEWLINK;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    ifi = (struct ifinfomsg *)NLMSG_DATA(nh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index  = ifindex;

    linkinfo  = nest_start(nh, IFLA_LINKINFO);
    addattr(nh, IFLA_INFO_KIND, "can", 4);
    info_data = nest_start(nh, IFLA_INFO_DATA);
    memset(&bt, 0, sizeof(bt));
    bt.bitrate = bitrate;   /* sample_point = 0 让内核自己算 */
    addattr(nh, IFLA_CAN_BITTIMING, &bt, sizeof(bt));
    nest_end(nh, info_data);
    nest_end(nh, linkinfo);

    return nl_talk(fd, nh);
}

/* 第二步：把接口拉起来（等价于 ip link set can0 up） */
static int set_up(int fd, int ifindex)
{
    char req[256];
    struct nlmsghdr *nh = (struct nlmsghdr *)req;
    struct ifinfomsg *ifi;

    memset(req, 0, sizeof(req));
    nh->nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nh->nlmsg_type  = RTM_NEWLINK;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    ifi = (struct ifinfomsg *)NLMSG_DATA(nh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index  = ifindex;
    ifi->ifi_flags  = IFF_UP;
    ifi->ifi_change = IFF_UP;   /* 只改 UP 这一位 */

    return nl_talk(fd, nh);
}

int main(int argc, char **argv)
{
    const char *ifname;
    unsigned int bitrate;
    int ifindex;
    int fd;

    if (argc != 3) {
        fprintf(stderr, "用法: %s <接口名> <波特率>\n", argv[0]);
        fprintf(stderr, "例如: %s can0 500000\n", argv[0]);
        return 2;
    }

    ifname  = argv[1];
    bitrate = (unsigned int)strtoul(argv[2], NULL, 0);

    ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        fprintf(stderr, "找不到网络接口 %s\n", ifname);
        return 1;
    }

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        perror("socket(AF_NETLINK)");
        return 1;
    }

    if (set_bitrate(fd, ifindex, bitrate) != 0) {
        fprintf(stderr, "设置 %s 波特率 %u 失败\n", ifname, bitrate);
        close(fd);
        return 1;
    }
    printf("%s: 波特率已设为 %u\n", ifname, bitrate);

    if (set_up(fd, ifindex) != 0) {
        fprintf(stderr, "启动 %s 失败\n", ifname);
        close(fd);
        return 1;
    }
    printf("%s: 已启动\n", ifname);

    close(fd);
    return 0;
}
