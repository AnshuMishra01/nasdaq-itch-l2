// Counts ITCH messages by type in fixed time buckets inside a window.
// Built to answer: at the 09:30 opening cross, what removes the crossed orders from the
// book? Q carries no order refs, so whatever type spikes (D or E/C) is the answer.
//
// Streams the file in chunks (a full-day file is ~10 GB) and stops once past the window.
//
// usage: itch_window <file> [start=09:29:59] [end=09:30:02] [bucket_ms=1000] [out=metrics.txt]
//   end is inclusive of that whole second: 09:30:02 means up to 09:30:02.999999999

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kNsPerMs = 1'000'000ULL;
constexpr std::uint64_t kNsPerSec = 1'000'000'000ULL;

// "HH:MM:SS" or "HH:MM:SS.mmm" -> ns since midnight
bool parse_time(const std::string& s, std::uint64_t& out) {
    unsigned h = 0, m = 0, sec = 0, ms = 0;
    const int got = std::sscanf(s.c_str(), "%u:%u:%u.%u", &h, &m, &sec, &ms);
    if (got < 3) return false;
    out = (std::uint64_t{h} * 3600 + std::uint64_t{m} * 60 + sec) * kNsPerSec + std::uint64_t{ms} * kNsPerMs;
    return true;
}

std::string fmt_time(std::uint64_t ns) {
    const std::uint64_t s = ns / kNsPerSec;
    std::ostringstream o;
    o << std::setfill('0') << std::setw(2) << s / 3600 << ':' << std::setw(2) << s / 60 % 60 << ':'
      << std::setw(2) << s % 60 << '.' << std::setw(3) << ns % kNsPerSec / kNsPerMs;
    return o.str();
}

// Every ITCH 5.0 message starts: type(1) locate(2) tracking(2) timestamp(6).
std::uint64_t read_ts(const unsigned char* msg) {
    std::uint64_t ts = 0;
    for (int i = 5; i < 11; ++i) ts = (ts << 8) | msg[i];
    return ts;
}

const char* type_name(char t) {
    switch (t) {
        case 'S': return "system event";      case 'R': return "stock directory";
        case 'H': return "trading action";    case 'A': return "add order";
        case 'F': return "add order MPID";    case 'E': return "order executed";
        case 'C': return "executed w/ price"; case 'X': return "order cancel";
        case 'D': return "order delete";      case 'U': return "order replace";
        case 'P': return "trade (non-cross)"; case 'Q': return "cross trade";
        case 'B': return "broken trade";      case 'I': return "NOII";
        default:  return "other";
    }
}

using Counts = std::array<std::uint64_t, 256>;

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "usage: " << argv[0]
                  << " <file> [start=09:29:59] [end=09:30:02] [bucket_ms=1000] [out=metrics.txt]\n";
        return 1;
    }
    const std::string path = argv[1];
    std::uint64_t start = 0, end = 0;
    if (!parse_time(argc > 2 ? argv[2] : "09:29:59", start) ||
        !parse_time(argc > 3 ? argv[3] : "09:30:02", end)) {
        std::cout << "bad time, expected HH:MM:SS[.mmm]\n";
        return 1;
    }
    end += kNsPerSec; // make the end second inclusive
    const std::uint64_t bucket_ns = (argc > 4 ? std::stoull(argv[4]) : 1000ULL) * kNsPerMs;
    const std::string outpath = argc > 5 ? argv[5] : "metrics.txt";
    if (bucket_ns == 0 || end <= start) {
        std::cout << "need bucket_ms > 0 and end after start\n";
        return 1;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) { std::cout << "cannot open " << path << '\n'; return 1; }

    std::map<std::uint64_t, Counts> buckets; // bucket start ns -> per-type counts
    Counts total{};
    std::uint64_t first_ts = 0, last_ts = 0, scanned = 0;
    bool seen_any = false, past_end = false;

    // Chunked read; a partial message at the end of a chunk is carried to the next one.
    std::vector<unsigned char> buf(64u << 20);
    std::size_t have = 0;
    while (!past_end) {
        in.read(reinterpret_cast<char*>(buf.data() + have), static_cast<std::streamsize>(buf.size() - have));
        const std::size_t got = static_cast<std::size_t>(in.gcount());
        if (in.bad()) { std::cout << "read error in " << path << " after " << scanned << " messages\n"; return 1; }
        if (got == 0) break;
        have += got;

        std::size_t pos = 0;
        while (pos + 2 <= have) {
            const std::size_t len = (std::size_t{buf[pos]} << 8) | buf[pos + 1];
            if (len < 11) { std::cout << "corrupt length " << len << " at message " << scanned << '\n'; return 1; }
            if (pos + 2 + len > have) break; // partial: finish on the next chunk
            const unsigned char* msg = buf.data() + pos + 2;
            const std::uint64_t ts = read_ts(msg);
            if (!seen_any) { first_ts = ts; seen_any = true; }
            last_ts = ts;
            ++scanned;
            pos += 2 + len;

            if (ts < start) continue;
            if (ts >= end) { past_end = true; break; }
            const std::uint64_t b = start + (ts - start) / bucket_ns * bucket_ns;
            buckets[b][msg[0]]++;
            total[msg[0]]++;
        }
        std::copy(buf.begin() + static_cast<std::ptrdiff_t>(pos), buf.begin() + static_cast<std::ptrdiff_t>(have), buf.begin());
        have -= pos;
    }

    std::ofstream out(outpath);
    if (!out) { std::cout << "cannot write " << outpath << '\n'; return 1; }

    out << "ITCH message counts by type\n"
        << "  file:    " << path << '\n'
        << "  window:  " << fmt_time(start) << " to " << fmt_time(end) << " (end exclusive), "
        << bucket_ns / kNsPerMs << " ms buckets\n"
        << "  scanned: " << scanned << " messages, file time " << fmt_time(first_ts) << " to " << fmt_time(last_ts)
        << (past_end ? " (stopped after window)" : " (end of file)") << "\n\n";

    if (buckets.empty()) {
        out << "NO MESSAGES IN WINDOW.\n";
        if (last_ts < start) out << "The file ends at " << fmt_time(last_ts) << ", before the window starts. Use a full-day file.\n";
        out.close();
        if (!out) { std::cout << "failed writing " << outpath << '\n'; return 1; }
        std::cout << "No messages in window; see " << outpath << '\n';
        return 0;
    }

    // columns: only types seen in the window, in spec order
    const std::string order = "SRHYLVKAFECXDUPQBINJhW";
    std::vector<unsigned char> cols;
    for (char c : order) if (total[static_cast<unsigned char>(c)]) cols.push_back(static_cast<unsigned char>(c));
    for (int c = 0; c < 256; ++c)
        if (total[static_cast<std::size_t>(c)] && order.find(static_cast<char>(c)) == std::string::npos)
            cols.push_back(static_cast<unsigned char>(c));

    auto row = [&](const std::string& label, const Counts& c) {
        std::uint64_t sum = 0;
        for (auto t : cols) sum += c[t];
        out << std::left << std::setw(14) << label << std::right << std::setw(10) << sum;
        for (auto t : cols) out << std::setw(9) << c[t];
        out << '\n';
    };
    out << std::left << std::setw(14) << "bucket" << std::right << std::setw(10) << "total";
    for (auto t : cols) out << std::setw(9) << static_cast<char>(t);
    out << '\n' << std::string(24 + 9 * cols.size(), '-') << '\n';
    for (const auto& [b, c] : buckets) row(fmt_time(b), c);
    out << std::string(24 + 9 * cols.size(), '-') << '\n';
    row("TOTAL", total);

    // The question: which event removes the crossed orders?
    out << "\nBook-removal candidates in window:\n";
    const std::string candidates = "DECX";
    char winner = 'D';
    for (char t : candidates) {
        const auto ut = static_cast<unsigned char>(t);
        std::uint64_t peak = 0, peak_at = 0;
        for (const auto& [b, c] : buckets) if (c[ut] > peak) { peak = c[ut]; peak_at = b; }
        out << "  " << t << "  " << std::left << std::setw(18) << type_name(t) << std::right
            << "total " << std::setw(9) << total[ut];
        if (peak) out << "   peak bucket " << fmt_time(peak_at) << " (" << peak << ")";
        out << '\n';
        if (total[ut] > total[static_cast<unsigned char>(winner)]) winner = t;
    }
    out << "\n  Most frequent: " << winner << " (" << type_name(winner) << ")\n";
    out << "  Q (cross trade) messages in window: " << total[static_cast<unsigned char>('Q')] << '\n';

    out.close();
    if (!out) { std::cout << "failed writing " << outpath << '\n'; return 1; }
    std::cout << "Wrote " << outpath << " (most frequent removal type: " << winner << ")\n";
    return 0;
}
