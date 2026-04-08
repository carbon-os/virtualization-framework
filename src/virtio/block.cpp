#include "virtualization/virtio/block.hpp"
#include "virtualization/firmware/arm64/gic.hpp"
#include "virtualization/vm_context.hpp"
#include "virtualization/logger.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace virtualization::virtio {

namespace Reg {
    inline constexpr uint32_t Magic         = 0x000;
    inline constexpr uint32_t Version       = 0x004;
    inline constexpr uint32_t DeviceId      = 0x008;
    inline constexpr uint32_t VendorId      = 0x00C;
    inline constexpr uint32_t DeviceFeat    = 0x010;
    inline constexpr uint32_t DeviceFeatSel = 0x014;
    inline constexpr uint32_t DriverFeat    = 0x020;
    inline constexpr uint32_t DriverFeatSel = 0x024;
    inline constexpr uint32_t QueueSel      = 0x030;
    inline constexpr uint32_t QueueNumMax   = 0x034;
    inline constexpr uint32_t QueueNum      = 0x038;
    inline constexpr uint32_t QueueReady    = 0x044;
    inline constexpr uint32_t QueueNotify   = 0x050;
    inline constexpr uint32_t IrqStatus     = 0x060;
    inline constexpr uint32_t IrqAck        = 0x064;
    inline constexpr uint32_t Status        = 0x070;
    inline constexpr uint32_t QueueDescLo   = 0x080;
    inline constexpr uint32_t QueueDescHi   = 0x084;
    inline constexpr uint32_t QueueAvailLo  = 0x090;
    inline constexpr uint32_t QueueAvailHi  = 0x094;
    inline constexpr uint32_t QueueUsedLo   = 0x0A0;
    inline constexpr uint32_t QueueUsedHi   = 0x0A4;
    inline constexpr uint32_t ConfigGen     = 0x0FC;
    inline constexpr uint32_t ConfigBase    = 0x100;
}

inline constexpr uint64_t kVirtioF_Version1 = 1ULL << 32;
inline constexpr uint64_t kVirtioBlkF_Flush = 1ULL << 9;

inline constexpr uint32_t VIRTIO_BLK_T_IN     = 0;
inline constexpr uint32_t VIRTIO_BLK_T_OUT    = 1;
inline constexpr uint32_t VIRTIO_BLK_T_FLUSH  = 4;
inline constexpr uint32_t VIRTIO_BLK_T_GET_ID = 8;

inline constexpr uint8_t  VIRTIO_BLK_S_OK     = 0;
inline constexpr uint8_t  VIRTIO_BLK_S_IOERR  = 1;
inline constexpr uint8_t  VIRTIO_BLK_S_UNSUPP = 2;

inline constexpr uint16_t VRING_DESC_F_NEXT   = 1;

struct BlkReqHdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

// ── Constructor / Destructor ──────────────────────────────────────────────────

Block::Block(VMContext& ctx, uint64_t /*gpa*/, const char* image_path) noexcept
    : ctx_(ctx)
    , features_(kVirtioF_Version1 | kVirtioBlkF_Flush)
{
    fd_ = ::open(image_path, O_RDWR);
    if (fd_ < 0) {
        logger::log("[blk] O_RDWR failed on %s: %s\n",
                    image_path, strerror(errno));
        fd_ = ::open(image_path, O_RDONLY);
        if (fd_ >= 0)
            logger::log("[blk] WARNING: opened READ-ONLY — "
                        "all guest writes will fail with IOERR\n");
    } else {
        const int flags = ::fcntl(fd_, F_GETFL);
        logger::log("[blk] opened READ-WRITE (flags=0x%x)\n", flags);
    }

    if (fd_ < 0) {
        logger::log("[blk] fatal: cannot open %s: %s\n",
                    image_path, strerror(errno));
        return;
    }

    struct stat st{};
    if (::fstat(fd_, &st) == 0)
        capacity_ = static_cast<uint64_t>(st.st_size) / 512;

    logger::log("[blk] %s: %llu sectors (%llu MiB)\n",
                image_path,
                static_cast<unsigned long long>(capacity_),
                static_cast<unsigned long long>(capacity_ / 2048));
}

Block::~Block() {
    if (fd_ >= 0) ::close(fd_);
}

// ── IRQ ───────────────────────────────────────────────────────────────────────

void Block::notify_irq() { raise_spi(kSpiBlock); }

// ── Request processing ────────────────────────────────────────────────────────

void Block::process_rq() {
    if (fd_ < 0) return;
    Queue& q = queue_;
    if (!q.ready || !q.desc_gpa || !q.avail_gpa || !q.used_gpa) return;

    auto* desc  = static_cast<VDesc*> (ctx_.gpa_to_hva(q.desc_gpa));
    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    if (!desc || !avail || !used) return;

    while (q.last_avail != avail->idx) {
        const uint16_t head = avail->ring[q.last_avail % kQueueSize];

        static constexpr int kMaxChain = 64;
        uint16_t chain[kMaxChain];
        int      nchain = 0;
        {
            uint16_t c = head;
            while (nchain < kMaxChain) {
                chain[nchain++] = c;
                if (!(desc[c].flags & VRING_DESC_F_NEXT)) break;
                c = desc[c].next;
            }
        }

        uint8_t  status      = VIRTIO_BLK_S_OK;
        uint32_t total_bytes = 0;

        if (nchain < 2) {
            logger::log("[blk] bad chain len=%d\n", nchain);
            status = VIRTIO_BLK_S_IOERR;
            goto commit;
        }

        {
            void* hdr_ptr = ctx_.gpa_to_hva(desc[chain[0]].addr);
            if (!hdr_ptr || desc[chain[0]].len < sizeof(BlkReqHdr)) {
                status = VIRTIO_BLK_S_IOERR;
                goto commit;
            }

            BlkReqHdr hdr{};
            std::memcpy(&hdr, hdr_ptr, sizeof(hdr));

            if (hdr.type == VIRTIO_BLK_T_IN) {
                if (nchain < 3) { status = VIRTIO_BLK_S_IOERR; goto commit; }
                uint64_t sector = hdr.sector;
                for (int i = 1; i < nchain - 1 && status == VIRTIO_BLK_S_OK; i++) {
                    void*    buf = ctx_.gpa_to_hva(desc[chain[i]].addr);
                    uint32_t len = desc[chain[i]].len;
                    if (!buf) { status = VIRTIO_BLK_S_IOERR; break; }
                    const ssize_t n =
                        ::pread(fd_, buf, len, static_cast<off_t>(sector) * 512);
                    if (n != static_cast<ssize_t>(len)) {
                        logger::log("[blk] pread FAIL sector=%llu errno=%d\n",
                                    static_cast<unsigned long long>(sector), errno);
                        status = VIRTIO_BLK_S_IOERR; break;
                    }
                    sector      += len / 512;
                    total_bytes += len;
                }

            } else if (hdr.type == VIRTIO_BLK_T_OUT) {
                if (nchain < 3) { status = VIRTIO_BLK_S_IOERR; goto commit; }
                uint64_t sector = hdr.sector;
                for (int i = 1; i < nchain - 1 && status == VIRTIO_BLK_S_OK; i++) {
                    void*    buf = ctx_.gpa_to_hva(desc[chain[i]].addr);
                    uint32_t len = desc[chain[i]].len;
                    if (!buf) { status = VIRTIO_BLK_S_IOERR; break; }
                    const ssize_t n =
                        ::pwrite(fd_, buf, len, static_cast<off_t>(sector) * 512);
                    if (n != static_cast<ssize_t>(len)) {
                        logger::log("[blk] pwrite FAIL sector=%llu errno=%d\n",
                                    static_cast<unsigned long long>(sector), errno);
                        status = VIRTIO_BLK_S_IOERR; break;
                    }
                    sector      += len / 512;
                    total_bytes += len;
                }

            } else if (hdr.type == VIRTIO_BLK_T_FLUSH) {
                if (::fsync(fd_) != 0) {
                    logger::log("[blk] fsync FAIL errno=%d\n", errno);
                    status = VIRTIO_BLK_S_IOERR;
                }

            } else if (hdr.type == VIRTIO_BLK_T_GET_ID) {
                if (nchain >= 3) {
                    void*    buf = ctx_.gpa_to_hva(desc[chain[1]].addr);
                    uint32_t len = desc[chain[1]].len;
                    if (buf && len) {
                        const char id[] = "virtualization-blk-0";
                        std::memset(buf, 0, len);
                        std::memcpy(buf, id,
                                    std::min(static_cast<uint32_t>(sizeof(id)), len));
                        total_bytes = len;
                    }
                }

            } else {
                logger::log("[blk] unsupported request type=0x%x\n", hdr.type);
                status = VIRTIO_BLK_S_UNSUPP;
            }
        }

    commit:
        {
            void* st_ptr = ctx_.gpa_to_hva(desc[chain[nchain - 1]].addr);
            if (st_ptr) {
                *static_cast<uint8_t*>(st_ptr) = status;
                if (status != VIRTIO_BLK_S_OK)
                    logger::log("[blk] head=%u status=%u\n", head, status);
            }
        }
        {
            const uint16_t ui = used->idx % kQueueSize;
            used->ring[ui].id  = head;
            used->ring[ui].len = total_bytes + 1;
            __atomic_store_n(&used->idx,
                             static_cast<uint16_t>(used->idx + 1),
                             __ATOMIC_RELEASE);
        }
        ++q.last_avail;
    }

    irq_status_ |= 1;
    notify_irq();
}

// ── MMIO ─────────────────────────────────────────────────────────────────────

uint32_t Block::read(uint32_t off, uint32_t /*len*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    switch (off) {
        case Reg::Magic:          return 0x74726976U;
        case Reg::Version:        return 2;
        case Reg::DeviceId:       return 2; // virtio-blk
        case Reg::VendorId:       return 0x554D4551U;
        case Reg::DeviceFeat:
            if (dev_feat_sel_ == 0) return static_cast<uint32_t>(features_);
            if (dev_feat_sel_ == 1) return static_cast<uint32_t>(features_ >> 32);
            return 0;
        case Reg::QueueNumMax:    return kQueueSize;
        case Reg::QueueReady:     return queue_.ready ? 1u : 0u;
        case Reg::IrqStatus:      return irq_status_;
        case Reg::Status:         return status_;
        case Reg::ConfigGen:      return 0;
        case Reg::ConfigBase + 0: return static_cast<uint32_t>(capacity_);
        case Reg::ConfigBase + 4: return static_cast<uint32_t>(capacity_ >> 32);
        default:                  return 0;
    }
}

void Block::write(uint32_t off, uint32_t val, uint32_t /*len*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    Queue* q = &queue_;

#define LO(f, v) q->f = (q->f & 0xFFFFFFFF00000000ULL) | (v)
#define HI(f, v) q->f = (q->f & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(v) << 32)

    switch (off) {
        case Reg::DeviceFeatSel: dev_feat_sel_ = val; break;
        case Reg::DriverFeatSel: drv_feat_sel_ = val; break;
        case Reg::DriverFeat:
            if (drv_feat_sel_ == 0)
                features_ = (features_ & 0xFFFFFFFF00000000ULL) | val;
            else if (drv_feat_sel_ == 1)
                features_ = (features_ & 0x00000000FFFFFFFFULL)
                            | (static_cast<uint64_t>(val) << 32);
            break;
        case Reg::QueueSel:    break; // single queue; ignore selector
        case Reg::QueueNum:    q->num = std::min(val, kQueueSize); break;
        case Reg::QueueReady:  q->ready = (val != 0); break;
        case Reg::QueueNotify: if (val == 0) process_rq(); break;
        case Reg::IrqAck:      irq_status_ &= ~val; break;
        case Reg::Status:
            status_ = val;
            if (val == 0) { queue_ = Queue{}; irq_status_ = 0; }
            break;
        case Reg::QueueDescLo:  LO(desc_gpa,  val); break;
        case Reg::QueueDescHi:  HI(desc_gpa,  val); break;
        case Reg::QueueAvailLo: LO(avail_gpa, val); break;
        case Reg::QueueAvailHi: HI(avail_gpa, val); break;
        case Reg::QueueUsedLo:  LO(used_gpa,  val); break;
        case Reg::QueueUsedHi:  HI(used_gpa,  val); break;
        default: break;
    }

#undef LO
#undef HI
}

} // namespace virtualization::virtio