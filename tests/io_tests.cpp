// Input modes: whole-buffer, mmap and chunked reading must deliver exactly the same
// messages in the same order, whatever the chunk size and wherever a message is split
// across a chunk boundary. A file that ends inside a message must be reported, not
// silently dropped.

#include "itch_io.h"
#include "core/message.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << what << '\n';
    }
}

// Order-sensitive fingerprint of every message the parser delivers.
struct Recorder {
    std::uint64_t hash = 1469598103934665603ULL;
    std::uint64_t n = 0;
    void mix(std::uint64_t v) {
        hash = (hash ^ v) * 1099511628211ULL;
        ++n;
    }
    void on(const StockDirectoryMessage& m) { mix(m.stockLocate); }
    void on(const AddOrderMessage& m) { mix(m.orderRef ^ m.price ^ m.timeStamp); }
    void on(const AddOrderMPIDMessage& m) { mix(m.orderRef ^ m.price); }
    void on(const OrderExecutedMessage& m) { mix(m.orderRef ^ m.executedShares); }
    void on(const OrderExecutedWithPriceMessage& m) { mix(m.orderRef ^ m.price); }
    void on(const OrderCancelMessage& m) { mix(m.orderRef ^ m.cancelledShares); }
    void on(const OrderDeleteMessage& m) { mix(m.orderRef); }
    void on(const OrderReplaceMessage& m) { mix(m.origRef ^ m.newRef); }
};

void put(std::vector<unsigned char>& b, std::uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

// A stream of valid A / X / D / S messages with varied lengths (36, 23, 19, 12 bytes).
std::vector<unsigned char> make_stream(int messages) {
    std::vector<unsigned char> b;
    for (int i = 0; i < messages; ++i) {
        const auto ref = static_cast<std::uint64_t>(i + 1);
        switch (i % 4) {
            case 0: put(b, 36, 2); b.push_back('A'); put(b, 1, 2); put(b, 0, 2); put(b, ref * 1000, 6);
                    put(b, ref, 8); b.push_back('B'); put(b, 100, 4); for (int k = 0; k < 8; ++k) b.push_back(' ');
                    put(b, 1'000'000 + ref, 4); break;
            case 1: put(b, 23, 2); b.push_back('X'); put(b, 1, 2); put(b, 0, 2); put(b, ref, 6); put(b, ref, 8); put(b, 7, 4); break;
            case 2: put(b, 19, 2); b.push_back('D'); put(b, 1, 2); put(b, 0, 2); put(b, ref, 6); put(b, ref, 8); break;
            default: put(b, 12, 2); b.push_back('S'); put(b, 0, 2); put(b, 0, 2); put(b, ref, 6); b.push_back('O'); break;
        }
    }
    return b;
}

std::filesystem::path write_temp(const std::vector<unsigned char>& b, const char* name) {
    const auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return p;
}

} // namespace

int main() {
    const auto stream = make_stream(20'000);
    const auto path = write_temp(stream, "itch_io_test.bin");

    Recorder ref;
    const auto r0 = itch::parse_buffer(stream.data(), stream.size(), ref);
    check(r0.ok && r0.trailing == 0 && r0.consumed == stream.size(), "whole buffer parses cleanly");

    for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{37}, std::size_t{4096}, std::size_t{1} << 20}) {
        Recorder c;
        std::string err;
        const auto r = itch::parse_chunked(path, c, 0, err, chunk);
        check(r.ok && r.trailing == 0, "chunked " + std::to_string(chunk) + ": ok, nothing left over");
        check(c.n == ref.n && c.hash == ref.hash, "chunked " + std::to_string(chunk) + ": same messages in the same order");
        check(r.processed == r0.processed, "chunked " + std::to_string(chunk) + ": same processed count");
    }

    {
        itch::MappedFile m;
        std::string err;
        check(m.open(path, err) && m.size() == stream.size(), "mmap opens the whole file");
        Recorder c;
        const auto r = itch::parse_buffer(m.data(), m.size(), c);
        check(r.ok && c.hash == ref.hash && c.n == ref.n, "mmap: same messages in the same order");
    }

    {   // count limit stops at the same place in every mode
        Recorder a, b;
        std::string err;
        const auto ra = itch::parse_buffer(stream.data(), stream.size(), a, 1234);
        const auto rb = itch::parse_chunked(path, b, 1234, err, 37);
        check(ra.processed == 1234 && rb.processed == 1234 && a.hash == b.hash, "count limit honoured across chunks");
    }

    {   // truncated file: last message cut short
        auto cut = stream;
        cut.resize(cut.size() - 5);
        const auto tp = write_temp(cut, "itch_io_test_cut.bin");
        Recorder a, b;
        std::string err;
        const auto ra = itch::parse_buffer(cut.data(), cut.size(), a);
        const auto rb = itch::parse_chunked(tp, b, 0, err, 37);
        check(ra.ok && ra.trailing > 0, "buffer: truncated tail is reported");
        check(rb.ok && rb.trailing == ra.trailing, "chunked: same truncated tail reported");
        // the last message is a 14-byte 'S' (2 length + 12 body); cutting 5 bytes leaves 9 behind
        check(ra.consumed == stream.size() - 14 && ra.trailing == 9, "truncated: stops exactly before the cut message");
        check(a.hash == b.hash && a.n == b.n, "truncated: chunked delivers the same messages as buffer");
        std::filesystem::remove(tp);
    }

    std::filesystem::remove(path);
    std::cout << (failures ? "FAILED" : "OK") << " io tests\n";
    return failures ? 1 : 0;
}
