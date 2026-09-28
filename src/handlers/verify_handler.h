#pragma once

#include "handlers/book_handler.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace itch {

// Runs a BookHandler and cross-checks store against books (M7 layer 1) every
// `every` messages, so drift is caught near where it starts, not only at the end.
struct VerifyHandler {
    BookHandler book;
    std::uint64_t every = 500'000;
    std::uint64_t seen = 0;
    std::vector<std::pair<std::uint64_t, VerifyResult>> checkpoints; // (message #, result)

    template <typename Message>
    void on(const Message& m) {
        book.on(m);
        if (++seen % every == 0) checkpoints.emplace_back(seen, book.verify_levels());
    }
};

} // namespace itch
