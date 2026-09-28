#pragma once

#include "decode.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace itch {

// Message sizes from the ITCH 5.0 spec, indexed by type byte; 0 = unknown type.
inline constexpr std::array<std::uint8_t, 256> kMsgSizes = [] {
    std::array<std::uint8_t, 256> a{};
    a['S'] = 12; a['R'] = 39; a['H'] = 25; a['Y'] = 20; a['L'] = 26; a['V'] = 35;
    a['K'] = 28; a['A'] = 36; a['F'] = 40; a['E'] = 31; a['X'] = 23; a['D'] = 19;
    a['U'] = 35; a['P'] = 44; a['W'] = 12; a['h'] = 21; a['C'] = 36; a['Q'] = 40;
    a['B'] = 19; a['I'] = 50; a['N'] = 20; a['J'] = 35;
    return a;
}();

struct ParseResult {
    bool ok = true;
    std::uint64_t processed = 0; // messages for which dispatch returned true
    const char* error = "";
};

// Called around every dispatch (decode + handler). The default does nothing and compiles
// away; M8's latency probe times each message with it.
struct NoProbe {
    void before() {}
    void after(unsigned char /*type*/, const unsigned char* /*message*/) {}
};

// Walks a length-prefixed ITCH buffer and dispatches every message to `handler`.
// count > 0 stops after that many processed messages; count <= 0 means the whole buffer.
template <typename Handler, typename Probe = NoProbe>
ParseResult parse_buffer(const unsigned char* data, std::size_t size, Handler& handler, long long count = 0,
                         Probe probe = {}) {
    ParseResult r;
    const bool full = count <= 0;
    std::size_t pos = 0;
    while (pos + 2 <= size && (full || r.processed < static_cast<std::uint64_t>(count))) {
        const std::size_t len = (std::size_t{data[pos]} << 8) | data[pos + 1];
        if (len < 1) { r.ok = false; r.error = "Invalid length"; break; }
        const std::size_t start = pos + 2;
        if (start + len > size) { r.ok = false; r.error = "Message length exceeds buffer"; break; }
        const unsigned char* message = data + start;
        const unsigned char type = message[0];
        const std::size_t expected = kMsgSizes[type];
        if (expected == 0) { r.ok = false; r.error = "Unknown message type"; break; }
        if (expected != len) { r.ok = false; r.error = "Size mismatch"; break; }
        probe.before();
        const bool counted = dispatch(type, message, handler);
        probe.after(type, message);
        if (counted) ++r.processed;
        pos = start + len;
    }
    return r;
}

} // namespace itch
