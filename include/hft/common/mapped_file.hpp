#pragma once
// Read-only memory-mapped file.
//
// Why mmap instead of read() into a buffer?
//  * Zero copies: the parser reads straight out of the kernel page cache.
//  * No buffer-boundary logic: a message never straddles two reads.
//  * madvise(MADV_SEQUENTIAL) tells the kernel to read ahead aggressively
//    and drop pages behind us, so a 12 GB file doesn't evict everything else.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hft {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("cannot open " + path);

        struct stat st{};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            throw std::runtime_error("cannot stat " + path);
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ == 0) return;  // empty file: nothing to map

        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (p == MAP_FAILED) {
            ::close(fd_);
            throw std::runtime_error("cannot mmap " + path);
        }
        ::madvise(p, size_, MADV_SEQUENTIAL);
        data_ = static_cast<const std::uint8_t*>(p);
    }

    ~MappedFile() {
        if (data_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    int fd_ = -1;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace hft
