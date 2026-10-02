#pragma once

// Getting file bytes to the parser, three ways:
//   Load     read the whole file into one vector (fine for ~100 MB, not for 8-9 GB)
//   Mmap     map the file into the address space; the OS pages it in on first touch, so
//            disk reads happen inside the parse loop as page faults
//   Chunked  read 64 MB at a time into one reused buffer, carrying a partial message
//            across the boundary; disk reads happen between chunks, at points we choose
// Load and Mmap hand parse_buffer one contiguous span; Chunked calls it once per chunk.

#include "parse.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace itch {

enum class IoMode { Load, Mmap, Chunked };

inline const char* io_name(IoMode m) {
    switch (m) {
        case IoMode::Load: return "load";
        case IoMode::Mmap: return "mmap";
        case IoMode::Chunked: return "chunked";
    }
    return "?";
}

inline bool parse_io_mode(const std::string& s, IoMode& out) {
    if (s == "load") out = IoMode::Load;
    else if (s == "mmap") out = IoMode::Mmap;
    else if (s == "chunked") out = IoMode::Chunked;
    else return false;
    return true;
}

// ---- Load ---------------------------------------------------------------------------
inline bool load_file(const std::filesystem::path& path, std::vector<unsigned char>& buf, std::string& err) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { err = "cannot stat " + path.string(); return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path.string(); return false; }
    buf.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    if (static_cast<std::uintmax_t>(in.gcount()) != size) { err = "short read on " + path.string(); return false; }
    return true;
}

// ---- Mmap ---------------------------------------------------------------------------
// Read-only view of a whole file. Move-only; unmaps on destruction.
class MappedFile {
public:
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() { close(); }

    bool open(const std::filesystem::path& path, std::string& err) {
        close();
#ifdef _WIN32
        file_ = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) { err = "cannot open " + path.string(); return false; }
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file_, &sz)) { err = "cannot size " + path.string(); close(); return false; }
        size_ = static_cast<std::size_t>(sz.QuadPart);
        if (size_ == 0) return true; // mapping an empty file is an error on Windows
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) { err = "CreateFileMapping failed for " + path.string(); close(); return false; }
        data_ = static_cast<const unsigned char*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (!data_) { err = "MapViewOfFile failed for " + path.string(); close(); return false; }
#else
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) { err = "cannot open " + path.string(); return false; }
        struct stat st{};
        if (fstat(fd_, &st) != 0) { err = "cannot size " + path.string(); close(); return false; }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ == 0) return true;
        void* p = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (p == MAP_FAILED) { err = "mmap failed for " + path.string(); close(); return false; }
        madvise(p, size_, MADV_SEQUENTIAL);
        data_ = static_cast<const unsigned char*>(p);
#endif
        return true;
    }

    void close() {
#ifdef _WIN32
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        mapping_ = nullptr;
        file_ = INVALID_HANDLE_VALUE;
#else
        if (data_) munmap(const_cast<unsigned char*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
#endif
        data_ = nullptr;
        size_ = 0;
    }

    const unsigned char* data() const { return data_; }
    std::size_t size() const { return size_; }

private:
    const unsigned char* data_ = nullptr;
    std::size_t size_ = 0;
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// Where a chunked run's time goes: waiting on read() vs parsing (decode + handler).
struct IoStats {
    std::uint64_t read_ns = 0;
    std::uint64_t parse_ns = 0;
    std::uint64_t bytes = 0;
    std::uint64_t reads = 0;
};

// ---- Chunked ------------------------------------------------------------------------
// Reads `chunk_bytes` at a time into one buffer and parses each chunk. An incomplete
// message at the end of a chunk is moved to the front of the buffer and completed by the
// next read. Memory stays at one buffer whatever the file size. Returns the combined result;
// `trailing` is non-zero only if the file itself ends inside a message.
template <typename Handler, typename Probe = NoProbe>
ParseResult parse_chunked(const std::filesystem::path& path, Handler& handler, long long count, std::string& err,
                          std::size_t chunk_bytes = std::size_t{64} << 20, Probe probe = {},
                          IoStats* stats = nullptr) {
    using clk = std::chrono::steady_clock;
    auto ns_since = [](clk::time_point t) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t).count());
    };
    ParseResult total;
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path.string(); total.ok = false; total.error = "cannot open file"; return total; }

    std::vector<unsigned char> buf(chunk_bytes + 65536); // room for one carried message
    std::size_t have = 0;
    for (;;) {
        const auto t_read = clk::now();
        in.read(reinterpret_cast<char*>(buf.data() + have), static_cast<std::streamsize>(buf.size() - have));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (stats) { stats->read_ns += ns_since(t_read); stats->bytes += got; ++stats->reads; }
        if (in.bad()) { err = "read error on " + path.string(); total.ok = false; total.error = "read error"; return total; }
        have += got;
        const bool eof = got == 0 || in.eof();

        const long long left = count > 0 ? count - static_cast<long long>(total.processed) : 0;
        const auto t_parse = clk::now();
        const ParseResult r = parse_buffer(buf.data(), have, handler, left, probe);
        if (stats) stats->parse_ns += ns_since(t_parse);
        total.processed += r.processed;
        total.consumed += r.consumed;
        if (!r.ok) { total.ok = false; total.error = r.error; return total; }
        if (count > 0 && static_cast<long long>(total.processed) >= count) return total;

        std::memmove(buf.data(), buf.data() + r.consumed, have - r.consumed);
        have -= r.consumed;
        if (eof) {
            total.trailing = have;
            return total;
        }
    }
}

// ---- memory / page-fault counters ---------------------------------------------------
struct ProcessMemory {
    std::uint64_t peak_working_set = 0; // peak RAM resident, including mapped file pages
    std::uint64_t peak_private = 0;     // peak memory the process owns (heap, vectors); excludes the mapped file
    std::uint64_t page_faults = 0;      // soft + hard since process start (Windows); major faults on POSIX
};

inline ProcessMemory process_memory() {
    ProcessMemory m;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        m.peak_working_set = pmc.PeakWorkingSetSize;
        m.peak_private = pmc.PeakPagefileUsage;
        m.page_faults = pmc.PageFaultCount;
    }
#else
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        m.peak_working_set = static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;
        m.page_faults = static_cast<std::uint64_t>(ru.ru_majflt);
    }
#endif
    return m;
}

} // namespace itch
