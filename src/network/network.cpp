#include "virtualization/network/network.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace virtualization::network {

#pragma pack(push, 1)
struct EthHdr  { uint8_t  dst[6], src[6]; uint16_t type; };
struct ArpPkt  {
    uint16_t htype, ptype; uint8_t hlen, plen; uint16_t op;
    uint8_t  sha[6]; uint32_t spa;
    uint8_t  tha[6]; uint32_t tpa;
};
struct Ip4Hdr  {
    uint8_t  ver_ihl, tos; uint16_t tot_len, id, frag_off;
    uint8_t  ttl, proto; uint16_t cksum; uint32_t src, dst;
};
struct TcpHdr  {
    uint16_t sport, dport; uint32_t seq, ack_seq;
    uint8_t  doff, flags; uint16_t window, cksum, urg;
};
struct UdpHdr  { uint16_t sport, dport, len, cksum; };
struct IcmpHdr { uint8_t  type, code; uint16_t cksum; uint32_t rest; };
#pragma pack(pop)

static constexpr uint8_t IP_ICMP=1, IP_TCP=6, IP_UDP=17;
static constexpr uint8_t TF_FIN=0x01, TF_SYN=0x02, TF_RST=0x04,
                          TF_PSH=0x08, TF_ACK=0x10;
static constexpr size_t  kFrameMax = 2048;

static uint16_t ip_cksum(const void* p, size_t n) {
    uint32_t s = 0;
    const uint16_t* w = static_cast<const uint16_t*>(p);
    while (n >= 2) { s += *w++; n -= 2; }
    if (n) s += *reinterpret_cast<const uint8_t*>(w);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return static_cast<uint16_t>(~s);
}

static uint16_t transport_cksum(uint32_t sip, uint32_t dip,
                                 uint8_t proto,
                                 const void* seg, uint16_t seg_len) {
#pragma pack(push, 1)
    struct { uint32_t s, d; uint8_t z, p; uint16_t l; }
        ph { sip, dip, 0, proto, htons(seg_len) };
#pragma pack(pop)
    uint32_t s = 0;
    const uint16_t* p = reinterpret_cast<const uint16_t*>(&ph);
    for (size_t i = 0; i < sizeof(ph) / 2; ++i) s += p[i];
    const uint16_t* q = static_cast<const uint16_t*>(seg);
    size_t n = seg_len;
    while (n >= 2) { s += *q++; n -= 2; }
    if (n) s += *reinterpret_cast<const uint8_t*>(q);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return static_cast<uint16_t>(~s);
}

struct ConnKey {
    uint32_t sip, dip;
    uint16_t sp, dp;
    bool operator==(const ConnKey& o) const noexcept {
        return sip==o.sip && dip==o.dip && sp==o.sp && dp==o.dp;
    }
};
struct ConnKeyHash {
    size_t operator()(const ConnKey& k) const noexcept {
        size_t h = k.sip;
        h = h * 2654435761u ^ k.dip;
        h = h * 2654435761u ^ ((uint32_t)k.sp << 16 | k.dp);
        return h;
    }
};

struct TcpConn {
    uint32_t stk_src_ip, stk_dst_ip;
    uint16_t stk_src_port, stk_dst_port;
    uint8_t  guest_mac[6];

    std::atomic<uint32_t> g_nxt{0};
    std::atomic<uint32_t> h_nxt{0};

    int fd{-1};

    enum class State : uint8_t { CONNECTING, SYNACK_SENT, ESTABLISHED };
    std::atomic<State>    state{State::CONNECTING};
    std::atomic<bool>     stop{false};

    ~TcpConn() {
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            fd = -1;
        }
    }
};

struct Stack::Impl {
    Config                cfg;
    TxFn                  tx;
    std::atomic<bool>     running{true};

    std::mutex            mtx;
    std::unordered_map<ConnKey,
        std::shared_ptr<TcpConn>, ConnKeyHash> conns;
    std::vector<ConnKey>  done_queue;

    std::vector<std::thread> pf_threads;
    std::atomic<uint16_t> next_eport{49152};

    void emit_ip(const uint8_t* dst_mac,
                 uint32_t sip_h, uint32_t dip_h,
                 uint8_t proto,
                 const void* payload, size_t plen) {
        const uint16_t ip_total = static_cast<uint16_t>(sizeof(Ip4Hdr) + plen);
        uint8_t buf[kFrameMax];

        auto* eth = reinterpret_cast<EthHdr*>(buf);
        std::memcpy(eth->dst, dst_mac, 6);
        std::memcpy(eth->src, cfg.gw_mac, 6);
        eth->type = htons(0x0800);

        auto* ip = reinterpret_cast<Ip4Hdr*>(buf + sizeof(EthHdr));
        ip->ver_ihl  = 0x45;
        ip->tos      = 0;
        ip->tot_len  = htons(ip_total);
        ip->id       = 0;
        ip->frag_off = 0;
        ip->ttl      = 64;
        ip->proto    = proto;
        ip->cksum    = 0;
        ip->src      = htonl(sip_h);
        ip->dst      = htonl(dip_h);
        ip->cksum    = ip_cksum(ip, sizeof(Ip4Hdr));

        std::memcpy(buf + sizeof(EthHdr) + sizeof(Ip4Hdr), payload, plen);
        tx(buf, sizeof(EthHdr) + ip_total);
    }

    void send_tcp(const uint8_t* dst_mac,
                  uint32_t sip, uint16_t sp,
                  uint32_t dip, uint16_t dp,
                  uint32_t seq, uint32_t ack,
                  uint8_t flags,
                  const void* data = nullptr, size_t dlen = 0) {
        const uint16_t tcp_len = static_cast<uint16_t>(sizeof(TcpHdr) + dlen);
        uint8_t seg[sizeof(TcpHdr) + 8192];

        auto* tcp = reinterpret_cast<TcpHdr*>(seg);
        tcp->sport   = htons(sp);
        tcp->dport   = htons(dp);
        tcp->seq     = htonl(seq);
        tcp->ack_seq = htonl(ack);
        tcp->doff    = (sizeof(TcpHdr) / 4) << 4;
        tcp->flags   = flags;
        tcp->window  = htons(65535);
        tcp->cksum   = 0;
        tcp->urg     = 0;
        if (data && dlen)
            std::memcpy(seg + sizeof(TcpHdr), data, dlen);
        tcp->cksum = transport_cksum(htonl(sip), htonl(dip),
                                     IP_TCP, seg, tcp_len);
        emit_ip(dst_mac, sip, dip, IP_TCP, seg, tcp_len);
    }

    void send_udp(const uint8_t* dst_mac,
                  uint32_t sip, uint16_t sp,
                  uint32_t dip, uint16_t dp,
                  const void* data, size_t dlen) {
        const uint16_t udp_len = static_cast<uint16_t>(sizeof(UdpHdr) + dlen);
        uint8_t seg[sizeof(UdpHdr) + 1500];

        auto* udp = reinterpret_cast<UdpHdr*>(seg);
        udp->sport = htons(sp);
        udp->dport = htons(dp);
        udp->len   = htons(udp_len);
        udp->cksum = 0;
        std::memcpy(seg + sizeof(UdpHdr), data, dlen);
        udp->cksum = transport_cksum(htonl(sip), htonl(dip),
                                     IP_UDP, seg, udp_len);
        if (udp->cksum == 0) udp->cksum = 0xffff;

        emit_ip(dst_mac, sip, dip, IP_UDP, seg, udp_len);
    }

    void handle_arp(const uint8_t* eth_src, const uint8_t* payload, size_t len) {
        if (len < sizeof(ArpPkt)) return;
        const auto* arp = reinterpret_cast<const ArpPkt*>(payload);
        if (ntohs(arp->op) != 1) return;

        const uint32_t tpa = ntohl(arp->tpa);
        if (tpa != cfg.gw_ip && tpa != cfg.dns_ip) return;

        uint8_t buf[sizeof(EthHdr) + sizeof(ArpPkt)];
        auto* eth = reinterpret_cast<EthHdr*>(buf);
        std::memcpy(eth->dst, eth_src, 6);
        std::memcpy(eth->src, cfg.gw_mac, 6);
        eth->type = htons(0x0806);

        auto* rep = reinterpret_cast<ArpPkt*>(buf + sizeof(EthHdr));
        rep->htype = htons(1);
        rep->ptype = htons(0x0800);
        rep->hlen  = 6;
        rep->plen  = 4;
        rep->op    = htons(2);
        std::memcpy(rep->sha, cfg.gw_mac, 6);
        rep->spa   = htonl(tpa);
        std::memcpy(rep->tha, arp->sha, 6);
        rep->tpa   = arp->spa;
        tx(buf, sizeof(buf));
    }

    void handle_icmp(const uint8_t* eth_src, const Ip4Hdr* ip,
                     const uint8_t* payload, size_t plen) {
        if (plen < sizeof(IcmpHdr)) return;
        if (payload[0] != 8) return;

        std::vector<uint8_t> rep(payload, payload + plen);
        rep[0] = 0;
        rep[2] = rep[3] = 0;
        const uint16_t ck = ip_cksum(rep.data(), rep.size());
        rep[2] = ck & 0xff;
        rep[3] = ck >> 8;

        emit_ip(eth_src, ntohl(ip->dst), ntohl(ip->src), IP_ICMP,
                rep.data(), rep.size());
    }

    void handle_dhcp(const uint8_t* eth_src, const uint8_t* payload, size_t len) {
        if (len < 240) return;

        const uint32_t xid    = *reinterpret_cast<const uint32_t*>(payload + 4);
        const uint8_t* chaddr = payload + 28;

        uint8_t msg_type = 0;
        const uint8_t* opt     = payload + 240;
        size_t         opt_rem = len - 240;

        for (size_t i = 0; i < opt_rem; ) {
            if (opt[i] == 255) break;
            if (opt[i] == 0)  { ++i; continue; }
            if (i + 1 >= opt_rem) break;
            const uint8_t olen = opt[i + 1];
            if (opt[i] == 53 && olen >= 1) msg_type = opt[i + 2];
            i += 2 + olen;
        }

        if (msg_type != 1 && msg_type != 3) return;

        uint8_t reply[548]{};
        reply[0] = 2; reply[1] = 1; reply[2] = 6;
        std::memcpy(reply + 4, &xid, 4);
        const uint32_t yip = htonl(cfg.guest_ip);
        const uint32_t sip = htonl(cfg.gw_ip);
        std::memcpy(reply + 16, &yip, 4);
        std::memcpy(reply + 20, &sip, 4);
        std::memcpy(reply + 28, chaddr, 6);
        reply[236]=99; reply[237]=130; reply[238]=83; reply[239]=99;

        size_t p = 240;
        auto opt8  = [&](uint8_t tag, uint8_t v) {
            reply[p++]=tag; reply[p++]=1; reply[p++]=v;
        };
        auto opt32 = [&](uint8_t tag, uint32_t v) {
            reply[p++]=tag; reply[p++]=4;
            v = htonl(v);
            std::memcpy(reply+p, &v, 4); p+=4;
        };

        opt8 (53, msg_type == 1 ? 2 : 5);
        opt32(54, cfg.gw_ip);
        opt32(51, 86400);
        opt32(1,  0xFFFFFF00u);
        opt32(3,  cfg.gw_ip);
        opt32(6,  cfg.dns_ip);
        reply[p++] = 255;

        const size_t    rlen  = std::max(p, size_t(300));
        const uint8_t   bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
        send_udp(bcast, cfg.gw_ip, 67, 0xFFFFFFFFu, 68, reply, rlen);
    }

    void handle_dns(const uint8_t* eth_src, const Ip4Hdr* ip,
                    uint16_t src_port, const uint8_t* q, size_t qlen) {
        const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return;

        struct timeval tv{2, 0};
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(53);
        sa.sin_addr.s_addr = htonl(cfg.dns_fwd_ip);

        if (::sendto(sock, q, qlen, 0,
                     reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) > 0) {
            uint8_t   resp[512];
            const ssize_t n = ::recv(sock, resp, sizeof(resp), 0);
            if (n > 0)
                send_udp(eth_src, cfg.dns_ip, 53,
                         ntohl(ip->src), src_port, resp, n);
        }
        ::close(sock);
    }

    void handle_udp(const uint8_t* eth_src, const Ip4Hdr* ip,
                    const uint8_t* payload, size_t plen) {
        if (plen < sizeof(UdpHdr)) return;
        const auto* udp  = reinterpret_cast<const UdpHdr*>(payload);
        const uint16_t sp   = ntohs(udp->sport);
        const uint16_t dp   = ntohs(udp->dport);
        const uint8_t* data = payload + sizeof(UdpHdr);
        const size_t   dlen = plen   - sizeof(UdpHdr);

        if (dp == 67) { handle_dhcp(eth_src, data, dlen); return; }
        if (dp == 53) { handle_dns(eth_src, ip, sp, data, dlen); return; }

        const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return;
        struct timeval tv{2, 0};
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(dp);
        sa.sin_addr.s_addr = ip->dst;
        if (::sendto(sock, data, dlen, 0,
                     reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) > 0) {
            uint8_t       resp[1500];
            const ssize_t n = ::recv(sock, resp, sizeof(resp), 0);
            if (n > 0)
                send_udp(eth_src, ntohl(ip->dst), dp,
                         ntohl(ip->src), sp, resp, n);
        }
        ::close(sock);
    }

    void launch_io_thread(std::shared_ptr<TcpConn> conn, ConnKey key) {
        std::thread([this, conn = std::move(conn), key]() mutable {
            uint8_t buf[1024];

            while (!conn->stop.load(std::memory_order_relaxed)) {
                struct pollfd pfd{conn->fd, POLLIN, 0};
                if (::poll(&pfd, 1, 100) <= 0) continue;
                if (!(pfd.revents & POLLIN)) break;

                const ssize_t n = ::read(conn->fd, buf, sizeof(buf));
                if (n <= 0) break;

                const uint32_t seq = conn->h_nxt.fetch_add(
                    static_cast<uint32_t>(n), std::memory_order_relaxed);
                const uint32_t ack = conn->g_nxt.load(std::memory_order_relaxed);

                send_tcp(conn->guest_mac,
                         conn->stk_src_ip, conn->stk_src_port,
                         conn->stk_dst_ip, conn->stk_dst_port,
                         seq, ack, TF_PSH|TF_ACK, buf, n);
            }

            if (!conn->stop.load(std::memory_order_relaxed)) {
                const uint32_t seq = conn->h_nxt.fetch_add(1,
                    std::memory_order_relaxed);
                send_tcp(conn->guest_mac,
                         conn->stk_src_ip, conn->stk_src_port,
                         conn->stk_dst_ip, conn->stk_dst_port,
                         seq, conn->g_nxt.load(), TF_FIN|TF_ACK);
            }

            std::unique_lock<std::mutex> lk(mtx);
            done_queue.push_back(key);
        }).detach();
    }

    void handle_tcp(const uint8_t* eth_src, const Ip4Hdr* ip,
                    const uint8_t* payload, size_t plen) {
        if (plen < sizeof(TcpHdr)) return;
        const auto* tcp  = reinterpret_cast<const TcpHdr*>(payload);
        const uint16_t sp    = ntohs(tcp->sport);
        const uint16_t dp    = ntohs(tcp->dport);
        const uint32_t seq   = ntohl(tcp->seq);
        const uint32_t ack_v = ntohl(tcp->ack_seq);
        const uint8_t  flags = tcp->flags;
        const size_t   hlen  = (tcp->doff >> 4) * 4;
        const uint8_t* data  = payload + hlen;
        const size_t   dlen  = plen > hlen ? plen - hlen : 0;

        const ConnKey key{ ntohl(ip->src), ntohl(ip->dst), sp, dp };

        std::unique_lock<std::mutex> lk(mtx);

        for (const auto& k : done_queue) conns.erase(k);
        done_queue.clear();

        if ((flags & TF_SYN) && !(flags & TF_ACK)) {
            if (conns.count(key)) return;

            auto conn = std::make_shared<TcpConn>();
            conn->stk_src_ip   = ntohl(ip->dst);
            conn->stk_src_port = dp;
            conn->stk_dst_ip   = ntohl(ip->src);
            conn->stk_dst_port = sp;
            std::memcpy(conn->guest_mac, eth_src, 6);
            conn->g_nxt.store(seq + 1, std::memory_order_relaxed);
            conn->h_nxt.store(0x40000000u, std::memory_order_relaxed);
            conn->state.store(TcpConn::State::CONNECTING, std::memory_order_relaxed);
            conns.emplace(key, conn);

            const uint32_t dst_ip_n = ip->dst;
            std::thread([this, conn, key, dst_ip_n, dp]() {
                const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) {
                    std::lock_guard<std::mutex> lk(mtx);
                    conns.erase(key);
                    return;
                }

                ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
                struct sockaddr_in sa{};
                sa.sin_family      = AF_INET;
                sa.sin_addr.s_addr = dst_ip_n;
                sa.sin_port        = htons(dp);
                ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));

                struct pollfd pfd{fd, POLLOUT, 0};
                if (::poll(&pfd, 1, 5000) > 0) {
                    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
                    conn->fd = fd;
                    conn->state.store(TcpConn::State::SYNACK_SENT, std::memory_order_relaxed);
                    const uint32_t isn = conn->h_nxt.fetch_add(1, std::memory_order_relaxed);
                    send_tcp(conn->guest_mac,
                             conn->stk_src_ip, conn->stk_src_port,
                             conn->stk_dst_ip, conn->stk_dst_port,
                             isn, conn->g_nxt.load(), TF_SYN|TF_ACK);
                } else {
                    ::close(fd);
                    std::lock_guard<std::mutex> lk(mtx);
                    conns.erase(key);
                }
            }).detach();
            return;
        }

        auto it = conns.find(key);
        if (it == conns.end()) {
            if (!(flags & TF_RST)) {
                lk.unlock();
                send_tcp(eth_src, ntohl(ip->dst), dp,
                         ntohl(ip->src), sp, ack_v, 0, TF_RST);
            }
            return;
        }

        auto conn = it->second;

        if (flags & TF_RST) {
            conn->stop.store(true);
            conns.erase(it);
            return;
        }

        if (conn->state.load() == TcpConn::State::SYNACK_SENT) {
            if ((flags & TF_SYN) && (flags & TF_ACK)) {
                conn->g_nxt.store(seq + 1, std::memory_order_relaxed);
                send_tcp(conn->guest_mac,
                         conn->stk_src_ip, conn->stk_src_port,
                         conn->stk_dst_ip, conn->stk_dst_port,
                         conn->h_nxt.load(), conn->g_nxt.load(), TF_ACK);
                conn->state.store(TcpConn::State::ESTABLISHED, std::memory_order_relaxed);
                launch_io_thread(conn, key);
                return;
            } else if (flags & TF_ACK) {
                conn->state.store(TcpConn::State::ESTABLISHED, std::memory_order_relaxed);
                launch_io_thread(conn, key);
                return;
            }
        }

        if (conn->state.load() != TcpConn::State::ESTABLISHED) return;

        std::vector<uint8_t> to_write;
        bool should_ack = false;

        if (dlen > 0) {
            if (seq == conn->g_nxt.load()) {
                to_write.assign(data, data + dlen);
                conn->g_nxt.fetch_add(static_cast<uint32_t>(dlen),
                                      std::memory_order_relaxed);
            }
            should_ack = true;
        }

        bool should_fin = false;
        if (flags & TF_FIN) {
            conn->g_nxt.fetch_add(1, std::memory_order_relaxed);
            conn->stop.store(true);
            if (conn->fd >= 0) ::shutdown(conn->fd, SHUT_WR);
            should_fin = true;
            conns.erase(it);
        }

        uint32_t ack_seq = conn->g_nxt.load();
        uint32_t h_seq   = conn->h_nxt.load();
        if (should_fin)
            h_seq = conn->h_nxt.fetch_add(1, std::memory_order_relaxed);

        lk.unlock();

        if (!to_write.empty() && conn->fd >= 0) {
            const uint8_t* p = to_write.data();
            size_t rem = to_write.size();
            while (rem > 0) {
                const ssize_t w = ::write(conn->fd, p, rem);
                if (w <= 0) break;
                p += w; rem -= static_cast<size_t>(w);
            }
        }

        if (should_fin) {
            send_tcp(conn->guest_mac,
                     conn->stk_src_ip, conn->stk_src_port,
                     conn->stk_dst_ip, conn->stk_dst_port,
                     h_seq, ack_seq, TF_FIN|TF_ACK);
        } else if (should_ack) {
            send_tcp(conn->guest_mac,
                     conn->stk_src_ip, conn->stk_src_port,
                     conn->stk_dst_ip, conn->stk_dst_port,
                     h_seq, ack_seq, TF_ACK);
        }
    }

    void inbound_fwd(int host_fd, uint16_t guest_port) {
        const uint16_t eport   = next_eport.fetch_add(1, std::memory_order_relaxed);
        const uint32_t our_isn = 0x50000000u | (eport << 8u);

        const ConnKey key{cfg.guest_ip, cfg.gw_ip, guest_port, eport};

        auto conn = std::make_shared<TcpConn>();
        conn->stk_src_ip   = cfg.gw_ip;
        conn->stk_src_port = eport;
        conn->stk_dst_ip   = cfg.guest_ip;
        conn->stk_dst_port = guest_port;
        std::memcpy(conn->guest_mac, cfg.guest_mac, 6);
        conn->h_nxt.store(our_isn + 1, std::memory_order_relaxed);
        conn->fd    = host_fd;
        conn->state.store(TcpConn::State::SYNACK_SENT, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk(mtx);
            conns.emplace(key, conn);
        }

        for (int attempt = 0; attempt < 5; ++attempt) {
            send_tcp(cfg.guest_mac,
                     cfg.gw_ip, eport, cfg.guest_ip, guest_port,
                     our_isn, 0, TF_SYN);

            std::this_thread::sleep_for(
                std::chrono::milliseconds(200 << attempt));

            std::unique_lock<std::mutex> lk(mtx);
            auto it = conns.find(key);
            if (it == conns.end()) return;
            if (it->second->state.load() != TcpConn::State::SYNACK_SENT) return;
        }

        std::unique_lock<std::mutex> lk(mtx);
        conns.erase(key);
    }

    void port_fwd_listen(PortForward pf) {
        const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) return;

        const int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in la{};
        la.sin_family      = AF_INET;
        la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        la.sin_port        = htons(pf.host_port);

        if (::bind(lfd, reinterpret_cast<sockaddr*>(&la), sizeof(la)) < 0 ||
            ::listen(lfd, 8) < 0) {
            ::close(lfd);
            return;
        }

        struct pollfd pfd{lfd, POLLIN, 0};
        while (running.load(std::memory_order_relaxed)) {
            if (::poll(&pfd, 1, 200) <= 0) continue;
            const int cfd = ::accept(lfd, nullptr, nullptr);
            if (cfd < 0) continue;
            std::thread([this, cfd, pf]() {
                inbound_fwd(cfd, pf.guest_port);
            }).detach();
        }
        ::close(lfd);
    }

    void handle_frame(const uint8_t* frame, size_t len) {
        if (len < sizeof(EthHdr)) return;
        const auto*    eth     = reinterpret_cast<const EthHdr*>(frame);
        const uint8_t* payload = frame + sizeof(EthHdr);
        const size_t   plen    = len   - sizeof(EthHdr);
        const uint16_t etype   = ntohs(eth->type);

        if (etype == 0x0806) {
            std::lock_guard<std::mutex> lk(mtx);
            handle_arp(eth->src, payload, plen);
            return;
        }
        if (etype != 0x0800) return;
        if (plen < sizeof(Ip4Hdr)) return;

        const auto*    ip    = reinterpret_cast<const Ip4Hdr*>(payload);
        const size_t   ihl   = (ip->ver_ihl & 0x0f) * 4;
        const uint16_t iplen = ntohs(ip->tot_len);
        if (plen < ihl || iplen < ihl) return;

        const uint8_t* ipp  = payload + ihl;
        const size_t   ippl = iplen   - ihl;

        if      (ip->proto == IP_ICMP) handle_icmp(eth->src, ip, ipp, ippl);
        else if (ip->proto == IP_UDP)  handle_udp (eth->src, ip, ipp, ippl);
        else if (ip->proto == IP_TCP)  handle_tcp (eth->src, ip, ipp, ippl);
    }
};

Stack::Stack(Config cfg, TxFn tx)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
    impl_->tx  = std::move(tx);

    for (const auto& pf : impl_->cfg.port_forwards)
        impl_->pf_threads.emplace_back(&Impl::port_fwd_listen,
                                        impl_.get(), pf);
}

Stack::~Stack() {
    impl_->running.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        for (auto& [k, c] : impl_->conns)
            c->stop.store(true, std::memory_order_relaxed);
        impl_->conns.clear();
    }

    for (auto& t : impl_->pf_threads)
        if (t.joinable()) t.join();
}

void Stack::guest_tx(const uint8_t* frame, size_t len) {
    impl_->handle_frame(frame, len);
}

} // namespace virtualization::network