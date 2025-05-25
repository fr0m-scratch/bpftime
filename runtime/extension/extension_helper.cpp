/* SPDX-License-Identifier: MIT
 *
 */

 #include "bpftime_helper_group.hpp"
 #include <atomic>
 #include <cerrno>
#include <cstdio>
 #include <fcntl.h>
 #include <linux/fiemap.h>
 #include <linux/futex.h>
 #include <linux/nvme_ioctl.h>
 #include <mutex>
 #include <pthread.h>
 #include <sched.h>
 #include <spdlog/spdlog.h>
 #include <sys/ioctl.h>
 #include <sys/mman.h>
 #include <sys/stat.h>
 #include <sys/syscall.h>
 #include <unistd.h>
 #include <unordered_map>
 #include <vector>
 #include <filesystem>
 #include <cstring>
 #include "bpftime_ufunc.hpp"
 #include <linux/fs.h>
 #include <fcntl.h> 
 #include <sys/sysmacros.h>   
 #include <string> 
#include <regex> 
#include <fstream>   
#include <sstream>



 
 #if defined(BPFTIME_ENABLE_IOURING_EXT) && __linux__
 #include <liburing.h>
 #endif
 
 #include <linux/if_xdp.h>
 #include <linux/if_link.h>
 using namespace std;
 
 uint64_t dummy_hello(void)
 {
	 SPDLOG_INFO("dummy_hello called");
	 printf("dummy_hello called\n");
	 return 0;
 }
 

 #if BPFTIME_ENABLE_FS_HELPER
 uint64_t bpftime_get_abs_path(const char *filename,const char *buf,uint64_t sz)
 {
	 auto p=std::filesystem::absolute(filename);
	 return (uint64_t)(uintptr_t)strncpy((char*)(uintptr_t)buf,p.c_str(),sz);
 }
 uint64_t bpftime_path_join(const char *a,const char *b,const char *buf,uint64_t sz)
 {
	 auto p=std::filesystem::path(a)/std::filesystem::path(b);
	 return (uint64_t)(uintptr_t)strncpy((char*)(uintptr_t)buf,p.c_str(),sz);
 }
 #endif
 

 #if defined(BPFTIME_ENABLE_IOURING_EXT) && __linux__
 static struct io_uring g_ring;
 struct iovec iv{};
 void *huge;
 
 uint64_t bpftime_io_uring_init(uint32_t entries,uint32_t flags)
 {
	 struct io_uring_params p{}; p.flags=flags;
	 if(io_uring_queue_init_params(entries?entries:1024,&g_ring,&p)==0)
		 return 0;
	 if(io_uring_queue_init(entries?entries:1024,&g_ring,
			 flags|IORING_SETUP_SINGLE_ISSUER)==0)
		 return 0;
	 return 1;
 }
 uint64_t bpftime_io_uring_submit(void){ return io_uring_submit(&g_ring); }
 
uint64_t bpftime_io_uring_wait(uint32_t min_complete)
{
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqes(&g_ring, &cqe, min_complete, nullptr, nullptr);
    int res = cqe->res;
    if (cqe->flags & IORING_CQE_F_BUFFER) {
        unsigned bid = cqe->flags >> 16;
        void* buf_ptr = (char*)huge + bid * 2097152;
        memcpy((char*)cqe->user_data,(char*)buf_ptr,  cqe->res);
        auto sqe = io_uring_get_sqe(&g_ring);
        io_uring_prep_provide_buffers(sqe, buf_ptr, 2097152, 1, 0, bid);
        io_uring_submit(&g_ring);
        io_uring_wait_cqe(&g_ring, &cqe);
        io_uring_cqe_seen(&g_ring, cqe);
    }
    // if (cqe -> user_data == 11326) {
    //     printf("cqe->res=%d\n", cqe->res);
    // }
    // printf("cqe->res=%d, cqe->user_data=%lu\n", cqe->res, cqe->user_data);
    io_uring_cqe_seen(&g_ring, cqe);
    return res;                              
}
 
 #define DEF_PREP_ONE(name,call)                                              \
 uint64_t bpftime_##name(int fd,void*buf,size_t len,off_t off,uint64_t ud)    \
 { auto s=io_uring_get_sqe(&g_ring); if(!s) return 1; call; s->user_data=ud; return 0;}
 
 DEF_PREP_ONE(io_uring_prep_read,  io_uring_prep_read (s,fd,buf,len,off));
 DEF_PREP_ONE(io_uring_prep_write, io_uring_prep_write(s,fd,buf,len,off));
 DEF_PREP_ONE(io_uring_prep_readv, io_uring_prep_readv(s,fd,(iovec*)buf,len,off));
 DEF_PREP_ONE(io_uring_prep_writev,io_uring_prep_writev(s,fd,(iovec*)buf,len,off));
 DEF_PREP_ONE(io_uring_prep_recv,  io_uring_prep_recv (s,fd,buf,len,0));
 DEF_PREP_ONE(io_uring_prep_send_zc,
			  io_uring_prep_send_zc(s,fd,buf,len,MSG_ZEROCOPY,0));
 #endif 
 

 

 #if defined(BPFTIME_ENABLE_IOURING_EXT) && __linux__

 struct lba_extent {
    uint64_t logi;
    uint64_t phys;
    uint64_t len;
};

struct file_info {
    // Store the io_uring index for the character device, initialized to -1
    int char_dev_idx = -1;
    // Store the device ID for the character device
    dev_t char_dev_rdev = 0;
    // Extent mapping cache
    std::vector<lba_extent> ex;
    // File size cache
    off_t size = 0;
};

// Map character device rdev to its registered io_uring index
static unordered_map<dev_t, int> g_dev2idx;
// Map original fd to its file_info containing char device index and extents
static unordered_map<int, file_info> g_fdinfo;
static int g_next_idx = 0; // Next available io_uring fixed file slot index
static mutex g_meta_mtx;   // Mutex protecting global metadata maps

static std::pair<std::string, std::string> fd_to_devpaths(int fd)
{
    char fdlink[64];
    char path[PATH_MAX];

    int link_len = snprintf(fdlink, sizeof(fdlink), "/proc/self/fd/%d", fd);
    if (link_len < 0 || static_cast<size_t>(link_len) >= sizeof(fdlink)) {
        printf("[fd_to_devpaths] snprintf failed for fd %d\n", fd);
        return {};
    }

    ssize_t n = readlink(fdlink, path, sizeof(path) - 1);
    if (n <= 0) {
        printf("[fd_to_devpaths] readlink failed for %s: %s\n", fdlink, strerror(errno));
        return {};
    }
    path[n] = '\0';
    // printf("[fd_to_devpaths] fd %d → path %s\n", fd, path);

    std::string best_blk_dev;
    size_t best_len = 0;

    FILE *mi = fopen("/proc/self/mountinfo", "r");
    if (!mi) {
        printf("[fd_to_devpaths] failed to open mountinfo\n");
        return {};
    }

    char line[4096];
    while (fgets(line, sizeof(line), mi)) {
        char mp[PATH_MAX], fs_type[64], dev_path[PATH_MAX];
        if (sscanf(line, "%*s %*s %*s %*s %s %*[^-]- %s %s", mp, fs_type, dev_path) != 3)
            continue;

        size_t mpl = strlen(mp);
        if (mpl <= best_len || strncmp(path, mp, mpl) != 0)
            continue;

        if (strncmp(dev_path, "/dev/", 5) == 0) {
            best_len = mpl;
            best_blk_dev = dev_path;
            // printf("[fd_to_devpaths] matched mountpoint %s → block device %s\n", mp, dev_path);
        }
    }
    fclose(mi);

    if (best_blk_dev.empty()) {
        printf("[fd_to_devpaths] no block device found for %s\n", path);
        return {};
    }

    // printf("[fd_to_devpaths] best block device: %s\n", best_blk_dev.c_str());

    char nvme_ns[64];
    int nvme_id, ns_id;
    if (sscanf(best_blk_dev.c_str(), "/dev/nvme%dn%d", &nvme_id, &ns_id) == 2) {
        snprintf(nvme_ns, sizeof(nvme_ns), "/dev/ng%dn%d", nvme_id, ns_id);
        // printf("[fd_to_devpaths] derived char device: %s\n", nvme_ns);
        return {best_blk_dev, nvme_ns};
    }

    // printf("[fd_to_devpaths] cannot derive char device from %s\n", best_blk_dev.c_str());
    return {best_blk_dev, ""};
}

int64_t bpftime_get_lba(int fd, uint64_t off, uint64_t *out_lba)
{
    if (!out_lba)
        return -EFAULT;

    std::unique_lock<std::mutex> lk(g_meta_mtx);
    file_info &fi = g_fdinfo[fd];

    if (fi.size == 0) {
        struct stat stf;
        lk.unlock();
        if (fstat(fd, &stf) != 0)
            return -errno;
        lk.lock();
        fi.size = stf.st_size == 0 ? 1 : stf.st_size;
    }
    // printf("[bpftime_get_lba] fd %d, size %lld\n", fd, fi.size);
    if (off >= static_cast<uint64_t>(fi.size))
        return -EINVAL;

    if (fi.char_dev_idx < 0) {
        std::string blk_path, chr_path;
        lk.unlock();
        std::tie(blk_path, chr_path) = fd_to_devpaths(fd);
        // printf("[bpftime_get_lba] blk_path: %s, chr_path: %s\n", blk_path.c_str(), chr_path.c_str());
        if (chr_path.empty())
            return -ENODEV;

        struct stat st_char;
        if (stat(chr_path.c_str(), &st_char) != 0)
            return -errno;
        lk.lock();

        auto it = g_dev2idx.find(st_char.st_rdev);
        if (it == g_dev2idx.end()) {
            constexpr int FILE_SLOTS = 32;
            if (g_next_idx >= FILE_SLOTS)
                return -EMFILE;

            lk.unlock();
            int char_fd = open(chr_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (char_fd < 0)
                return -errno;

            int update_fd = char_fd;
            int ret = io_uring_register_files_update(&g_ring, g_next_idx, &update_fd, 1);
            if (ret == -EEXIST) {
                close(char_fd);
                lk.lock();
                fi.char_dev_idx = g_dev2idx[st_char.st_rdev];
                fi.char_dev_rdev = st_char.st_rdev;
            } else if (ret < 0) {
                close(char_fd);
                return ret;
            } else {
                lk.lock();
                g_dev2idx[st_char.st_rdev] = g_next_idx;
                fi.char_dev_idx = g_next_idx;
                fi.char_dev_rdev = st_char.st_rdev;
                ++g_next_idx;
                close(char_fd);
            }
        } else {
            fi.char_dev_idx = it->second;
            fi.char_dev_rdev = st_char.st_rdev;
        }
    }
    //printf("[bpftime_get_lba] char_dev_idx: %d, char_dev_rdev: %u\n", fi.char_dev_idx, fi.char_dev_rdev);

    auto find_lba_in_extents = [&](uint64_t logical) -> bool {
        for (const auto &e : fi.ex) {
            if (logical >= e.logi && logical < e.logi + e.len) {
                uint64_t phys = e.phys + (logical - e.logi);
                if (!phys)
                    return false;
                *out_lba = phys >> 9;
                return true;
            }
        }
        return false;
    };
    if (find_lba_in_extents(off))
        return fi.char_dev_idx;

    lk.unlock();
    constexpr uint32_t EXT_MAX = 128;
    size_t sz = sizeof(struct fiemap) + EXT_MAX * sizeof(struct fiemap_extent);
    auto *fm = reinterpret_cast<struct fiemap *>(alloca(sz));
    memset(fm, 0, sz);

    fm->fm_start = off;
    fm->fm_length = FIEMAP_MAX_OFFSET;
    fm->fm_extent_count = EXT_MAX;
    fm->fm_flags = FIEMAP_FLAG_SYNC;

    if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0)
        return -errno;

    std::vector<lba_extent> new_ex;
    new_ex.reserve(fm->fm_mapped_extents);
    for (uint32_t i = 0; i < fm->fm_mapped_extents; ++i) {
        const auto &e = fm->fm_extents[i];
        if (e.fe_length == 0 ||
            (e.fe_flags & (FIEMAP_EXTENT_UNKNOWN |
                           FIEMAP_EXTENT_UNWRITTEN |
                           FIEMAP_EXTENT_ENCODED)))
            continue;
        new_ex.push_back({e.fe_logical, e.fe_physical, e.fe_length});
    }

    lk.lock();
    fi.ex.swap(new_ex);
    if (find_lba_in_extents(off))
        return fi.char_dev_idx;

    return -ENOENT;
}

 /* 3‑2  provide+link (1027) & read_bg (1028) */
 uint64_t bpftime_provide_link(void*addr,int len,uint16_t bg,uint16_t bid)
 {
     auto s=io_uring_get_sqe(&g_ring);
     if(!s) return 1;
     io_uring_prep_provide_buffers(s,addr,len,1,bg,bid);
     s->flags|=IOSQE_IO_LINK;
     return 0;
 }

 int64_t bpftime_read_bg(int fd,size_t len,off_t off,uint16_t bg,uint64_t ud)
 {
     auto s=io_uring_get_sqe(&g_ring); if(!s) return 1;
     // register fd as fixed file
    // int ret = io_uring_register_files_update(&g_ring, fd, &fd, 1);
    // if (ret < 0) {
    //     printf("io_uring_register_files_update failed: %s\n", strerror(-ret));
    //     return ret;
    // }
    // print the actual fd at registered slot id 1

    // printf("fd: %d, slot id: %d\n", fd, g_dev2idx[fd]);

     io_uring_prep_read(s,1,nullptr,len,off);  
     s->flags|=IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE; 
     s->buf_group=0;
     s->user_data=ud;
     return 0;
 }

 
 /* 3‑3  nvme_cmd_rd (1031) */
 #include <linux/nvme_ioctl.h>
 
 uint64_t bpftime_nvme_cmd_rd(int fidx, size_t len, uint64_t lba, void *buf)
{
    constexpr size_t MAX_NVME_IO_SIZE = 128 * 1024; // 128 KiB
    constexpr size_t NVME_SECTOR_SIZE = 512;

    if (len == 0 || (len & (NVME_SECTOR_SIZE - 1)))
        return -EINVAL;

    uint8_t *cur_buf = static_cast<uint8_t*>(buf);
    uint64_t cur_lba = lba;
    size_t remaining = len;

    while (remaining > 0) {
        size_t io_size = (remaining > MAX_NVME_IO_SIZE) ? MAX_NVME_IO_SIZE : remaining;

        struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
        if (!sqe)
            return -EBUSY;

        auto *cmd = (struct nvme_uring_cmd *)&sqe->cmd;
        memset(cmd, 0, sizeof(*cmd));

        cmd->opcode   = 0x02;                   // NVMe READ opcode
        cmd->nsid     = 1;                      // Adjust as needed
        cmd->addr     = (uint64_t)cur_buf;       // Buffer address
        cmd->data_len = io_size;
        cmd->cdw10    = (uint32_t)cur_lba;
        cmd->cdw11    = (uint32_t)(cur_lba >> 32);
        cmd->cdw12    = (io_size / NVME_SECTOR_SIZE) - 1;

        sqe->opcode          = IORING_OP_URING_CMD;
        sqe->cmd_op          = NVME_URING_CMD_IO;
        sqe->fd              = fidx;
        sqe->flags           = IOSQE_FIXED_FILE;
        sqe->uring_cmd_flags = 0;               
        sqe->buf_index       = 0;
        sqe->len             = 0;
        sqe->user_data       = 11326;
        cur_buf += io_size;
        cur_lba += io_size / NVME_SECTOR_SIZE;
        remaining -= io_size;
    }

    return 0;
}


 #ifndef MAP_HUGE_2MB
 #define MAP_HUGE_SHIFT 26
 #define MAP_HUGE_2MB (21ULL<<MAP_HUGE_SHIFT)
 #endif
 static void fut_wake(uint32_t*a){ syscall(SYS_futex,a,FUTEX_WAKE_PRIVATE,1,nullptr,nullptr,0); }
 
 struct pend { void*ub; size_t len; uint32_t*fu; ssize_t res; };
 static unordered_map<uint64_t,pend> g_pend; static mutex g_pend_m;
 static volatile bool cq_stop=false;
 
 uint64_t bpftime_io_uring_init_extreme_preset(void)
 {
     static bool done=false; if(done) return 0;
     huge=mmap(nullptr,1ULL<<30,PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB|MAP_HUGE_2MB,-1,0);
     if(huge==MAP_FAILED) return 1;
     struct io_uring_params p{}; p.flags=IORING_SETUP_SQPOLL| 
                                     IORING_SETUP_CQSIZE|
                                     IORING_SETUP_SQE128 | IORING_SETUP_CQE32;
     p.cq_entries=8192; p.sq_thread_idle=10000; p.sq_thread_cpu=3;
     if(io_uring_queue_init_params(4096,&g_ring,&p)) return 2;
    const int FILE_SLOTS = 32;
    { 
       std::vector<int> empty(FILE_SLOTS, -1);
       if (io_uring_register_files(&g_ring, empty.data(), FILE_SLOTS))
          return 31;
    }

    // Register the second slot with /mnt/nvme0/testfile_4g
    int fd = open("/mnt/nvme0/testfile_4g", O_RDWR | O_DIRECT | O_CLOEXEC);
    if (fd < 0) {
       printf("Failed to open /mnt/nvme0/testfile_4g: %s\n", strerror(errno));
       return 32;
    }

    if (io_uring_register_files_update(&g_ring, 1, &fd, 1) < 0) {
       printf("Failed to register file to slot 1: %s\n", strerror(errno));
       close(fd);
       return 33;
    }

    close(fd);
    iv.iov_base = huge;
    iv.iov_len = 1ULL << 30;
    //  if(io_uring_register_buffers(&g_ring,&iv,1)) return 3;
     auto sqe=io_uring_get_sqe(&g_ring);
     io_uring_prep_provide_buffers(sqe, huge, 2097152, 512, 0, 0);
     sqe->flags = 0;
     int res = io_uring_submit(&g_ring);
     if (res < 0) {
         printf("io_uring_submit failed: %s\n", strerror(-res));
         return 4;
     }
     
     // use uring wait for the first buffer to be provided
    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(&g_ring, &cqe)) {
        printf("io_uring_wait_cqe failed: %s\n", strerror(-res));
        return 4;
    }
    if (cqe->res < 0) {
        printf("init io_uring_wait_cqe: cqe->res=%d\n", cqe->res);
        io_uring_cqe_seen(&g_ring, cqe);
        return 5;
    }
    io_uring_cqe_seen(&g_ring, cqe);
    //  bpftime_xdp_setup("ens2f0np0",0,4096,512);
    //  pthread_t t; pthread_create(&t,nullptr,cq_loop,nullptr); pthread_detach(t);
     done=true; return 0;
 }
 __attribute__((destructor)) static void fini(){ cq_stop=true; }

 /* ──────────────────────────────────────────
 * futex helpers  (wait / wake)
 * ──────────────────────────────────────────*/
static uint64_t bpftime_futex_wait(uint32_t *addr,
	uint32_t expected,
	uint64_t timeout_ns,
	uint32_t flags)
{
	struct timespec ts, *tsp = nullptr;
	if (timeout_ns) {
	ts.tv_sec  = timeout_ns / 1'000'000'000ULL;
	ts.tv_nsec = timeout_ns % 1'000'000'000ULL;
	tsp = &ts;
	}

	int ret = syscall(SYS_futex,
	addr,
	FUTEX_WAIT_PRIVATE | flags,
	expected,
	tsp,
	nullptr,
	0);
	return ret == 0 ? 0 : (uint64_t)(-errno);
}

static uint64_t bpftime_futex_wake(uint32_t *addr,
	uint32_t nr_wake,
	uint32_t flags)
{
	int ret = syscall(SYS_futex,
	addr,
	FUTEX_WAKE_PRIVATE | flags,
	nr_wake,
	nullptr,
	nullptr,
	0);
	return ret >= 0 ? (uint64_t)ret : (uint64_t)(-errno);
}

 #endif
 

 #define ADD_HELPER(ID,FN) { ID,{ .index=ID,.name=#FN,.fn=(void*)(FN) } }
 
 namespace bpftime {
 extern const bpftime_helper_group extesion_group = { {

	 ADD_HELPER(9999,dummy_hello),
 
 #if BPFTIME_ENABLE_FS_HELPER
	 ADD_HELPER(EXTENDED_HELPER_GET_ABS_PATH_ID,bpftime_get_abs_path),
	 ADD_HELPER(EXTENDED_HELPER_PATH_JOIN_ID,   bpftime_path_join),
 #endif
 
 #if defined(BPFTIME_ENABLE_IOURING_EXT) && __linux__
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_INIT,          bpftime_io_uring_init),
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_SUBMIT,        bpftime_io_uring_submit),
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_WAIT_AND_SEEN, bpftime_io_uring_wait),
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_READ,          bpftime_io_uring_prep_read),
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_WRITE,         bpftime_io_uring_prep_write),
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_READV,         bpftime_io_uring_prep_readv),
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_WRITEV,        bpftime_io_uring_prep_writev),
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_RECV,          bpftime_io_uring_prep_recv),
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_SEND_ZC,       bpftime_io_uring_prep_send_zc),

	 ADD_HELPER(EXTENDED_UFUNC_FUTEX_WAIT, bpftime_futex_wait),
	 ADD_HELPER(EXTENDED_UFUNC_FUTEX_WAKE, bpftime_futex_wake),
	

	 ADD_HELPER(EXTENDED_UFUNC_IOURING_PROVIDE_LINK,  bpftime_provide_link),
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_READ_BG,       bpftime_read_bg),
	 ADD_HELPER(EXTENDED_UFUNC_IOURING_CMD_NVME_RD,   bpftime_nvme_cmd_rd),
	 ADD_HELPER(EXTENDED_UFUNC_GET_LBA,               bpftime_get_lba),


	 ADD_HELPER(EXT_UFUNC_IOURING_INIT_EXTREME, bpftime_io_uring_init_extreme_preset),
 #endif 


 } };
 } // namespace bpftime
 