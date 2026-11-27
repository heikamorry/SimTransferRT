#include "v2/Config.h"
#include <arpa/inet.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rt::v2 {
namespace {
// Ask the kernel for the outgoing interface with the same IPv4 endpoints as Asio.
// RTM_GETROUTE is local to the kernel and does not send a packet to the peer.
unsigned routeInterface(const Config &c, bool &local) {
    struct {
        nlmsghdr header;
        rtmsg route;
        struct Attribute {
            rtattr header;
            in_addr address;
        } attributes[2];
    } request{};
    static_assert(sizeof(request.attributes[0]) == RTA_SPACE(sizeof(in_addr)));
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
    request.header.nlmsg_type = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST;
    request.header.nlmsg_seq = 1;
    request.route.rtm_family = AF_INET;
    const auto addAddress = [&](int index, unsigned short type, const std::string &ip) {
        auto &attr = request.attributes[index];
        attr.header.rta_type = type;
        attr.header.rta_len = RTA_LENGTH(sizeof(in_addr));
        request.header.nlmsg_len += sizeof(attr);
        return inet_pton(AF_INET, ip.c_str(), &attr.address) == 1;
    };
    request.route.rtm_dst_len = 32;
    if (!addAddress(0, RTA_DST, c.targetIp))
        return 0;
    if (!c.bindIp.empty() && c.bindIp != "0.0.0.0") {
        request.route.rtm_src_len = 32;
        if (!addAddress(1, RTA_SRC, c.bindIp))
            return 0;
    }
    struct Socket {
        int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        ~Socket() {
            if (fd >= 0)
                close(fd);
        }
    } link;
    const timeval timeout{0, 100000};
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    if (link.fd < 0 || setsockopt(link.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        sendto(link.fd, &request, request.header.nlmsg_len, 0,
               reinterpret_cast<const sockaddr *>(&kernel), sizeof(kernel)) < 0)
        return 0;
    alignas(nlmsghdr) char buffer[8192];
    const auto bytes = recv(link.fd, buffer, sizeof(buffer), 0);
    int remaining = static_cast<int>(bytes);
    for (auto *msg = reinterpret_cast<nlmsghdr *>(buffer); NLMSG_OK(msg, remaining);
         msg = NLMSG_NEXT(msg, remaining)) {
        if (msg->nlmsg_type != RTM_NEWROUTE || msg->nlmsg_seq != request.header.nlmsg_seq ||
            msg->nlmsg_len < NLMSG_LENGTH(sizeof(rtmsg)))
            continue;
        auto *route = static_cast<rtmsg *>(NLMSG_DATA(msg));
        local = route->rtm_type == RTN_LOCAL;
        int size = RTM_PAYLOAD(msg);
        for (auto *attr = RTM_RTA(route); RTA_OK(attr, size); attr = RTA_NEXT(attr, size)) {
            if (attr->rta_type == RTA_OIF && RTA_PAYLOAD(attr) == sizeof(unsigned)) {
                unsigned index;
                std::memcpy(&index, RTA_DATA(attr), sizeof(index));
                return index;
            }
        }
    }
    return 0;
}
} // namespace

void observeLinuxLink(const Config &c, LinkObservation &out) {
    in_addr target{};
    if (inet_pton(AF_INET, c.targetIp.c_str(), &target) != 1) {
        out.reason = "Invalid target IPv4";
        return;
    }
    if ((ntohl(target.s_addr) >> 24) == 127) {
        out.loopback = true;
        out.reason = "Host loopback; no physical link or wire timing evidence";
        return;
    }
    const auto index = routeInterface(c, out.loopback);
    char name[IF_NAMESIZE]{};
    if (!index || !if_indextoname(index, name)) {
        out.reason = "IPv4 route/interface unavailable";
        return;
    }
    out.interfaceName = name;
    if (out.loopback) {
        out.reason = "Host-local route; no physical link or wire timing evidence";
        return;
    }
    const std::string device = "/sys/class/net/" + out.interfaceName + "/";
    int type = 0, carrier = 0;
    std::ifstream(device + "type") >> type;
    std::ifstream(device + "carrier") >> carrier;
    if (carrier != 1) {
        out.reason = "Selected interface has no confirmed carrier";
        return;
    }
    std::error_code error;
    const auto bus = std::filesystem::read_symlink(device + "device/subsystem", error).filename();
    const auto driver = std::filesystem::read_symlink(device + "device/driver", error).filename();
    // Hyper-V, virtio and Xen can all expose Ethernet and an artificial line speed.
    out.physical = type == 1 && (bus == "platform" || bus == "pci" || bus == "usb") &&
                   !driver.empty() && driver != "vmxnet3" && driver != "virtio_net" &&
                   access((device + "device/physfn").c_str(), F_OK) != 0 &&
                   access((device + "wireless").c_str(), F_OK) != 0 &&
                   access((device + "phy80211").c_str(), F_OK) != 0;
    if (!out.physical) {
        out.reason = "Wireless or virtual interface; Ethernet line speed unverified";
        return;
    }
    long long speed = 0;
    if (std::ifstream(device + "speed") >> speed && speed > 0 &&
        speed < std::numeric_limits<unsigned>::max())
        out.mbps = static_cast<unsigned>(speed);
    out.reason = out.mbps ? "Observed local Ethernet hop only; remote path and egress time unverified"
                          : "Local Ethernet interface; negotiated speed unavailable";
}
} // namespace rt::v2
