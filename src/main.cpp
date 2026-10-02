#include "core/message.h"
#include "decode.h"
#include "parse.h"
#include "itch_io.h"
#include "measure.h"
#include "message_decode.h"
#include "struct_sizes.h"
#include "handlers/book_handler.h"
#include "handlers/combined_handler.h"
#include "handlers/verify_handler.h"
#include "handlers/bbo_handler.h"

#include <cstdio>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>



int main(int argc, char* argv[]) {
    // --io=load|mmap|chunked may appear anywhere; strip it so the positional arguments
    // keep their meaning.
    itch::IoMode io_mode = itch::IoMode::Chunked; // chosen in results/io/summary.md
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--io=", 0) == 0) {
            if (!itch::parse_io_mode(a.substr(5), io_mode)) {
                std::cout << "unknown --io mode '" << a.substr(5) << "' (load, mmap, chunked)\n";
                return 1;
            }
            continue;
        }
        args.push_back(argv[i]);
    }
    argc = static_cast<int>(args.size());
    argv = args.data();

    // support a quick dump of struct sizes and exit
    if (argc >= 2) {
        std::string a1 = argv[1];
        if (a1 == "--dump-structs") {
            dump_struct_sizes("struct.txt");
            return 0;
        }
        const std::string prefix = "--dump-structs=";
        if (a1.rfind(prefix, 0) == 0) {
            dump_struct_sizes(a1.substr(prefix.size()));
            return 0;
        }
    }

    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <file> <count> [out] [mode] [--io=load|mmap|chunked]\n"
                  << "       count <= 0: whole file; out '-': stdout; default io: chunked\n";
        std::cout << "       " << argv[0] << " --dump-structs [or --dump-structs=path]\n";
        return 1;
    }

    try {
        const long long count = std::stoll(argv[2]);
        const std::filesystem::path path{argv[1]};
        std::cout << "file='" << argv[1] << "' count=" << count << " io=" << itch::io_name(io_mode) << '\n';

        // Setup, outside the timed parse: load reads the whole file now; mmap only maps it
        // (bytes arrive later, as page faults inside the parse); chunked opens it per run.
        std::vector<unsigned char> loaded;
        itch::MappedFile mapped;
        const unsigned char* data = nullptr;
        std::size_t size = 0;
        const auto setup_t0 = std::chrono::steady_clock::now();
        std::string io_err;
        if (io_mode == itch::IoMode::Load) {
            if (!itch::load_file(path, loaded, io_err)) { std::cout << io_err << '\n'; return 1; }
            data = loaded.data();
            size = loaded.size();
        } else if (io_mode == itch::IoMode::Mmap) {
            if (!mapped.open(path, io_err)) { std::cout << io_err << '\n'; return 1; }
            data = mapped.data();
            size = mapped.size();
        }
        const auto setup_dur = std::chrono::steady_clock::now() - setup_t0;

        // framing loop lives in parse.h, shared with every tool
        std::size_t trailing = 0;
        auto parse_file = [&](auto &handler, long long &processed, std::string &error_msg) -> bool {
            const itch::ParseResult r = io_mode == itch::IoMode::Chunked
                                            ? itch::parse_chunked(path, handler, count, io_err)
                                            : itch::parse_buffer(data, size, handler, count);
            processed = static_cast<long long>(r.processed);
            trailing = r.trailing;
            if (r.ok && r.trailing)
                std::cout << "WARNING: file ends inside a message (" << r.trailing
                          << " bytes left over): truncated file? Everything before it was processed.\n";
            if (!r.ok) error_msg = io_err.empty() ? r.error : io_err;
            return r.ok;
        };

        // helper: timed runner that executes a callable and returns ok + duration
        auto timed_run = [](auto&& fn) {
            const auto t0 = std::chrono::steady_clock::now();
            bool ok = fn();
            const auto t1 = std::chrono::steady_clock::now();
            return std::make_pair(ok, std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0));
        };

        auto print_timing = [&](std::ostream &out, const std::chrono::nanoseconds &dur, long long processed) {
            const double secs = std::chrono::duration_cast<std::chrono::duration<double>>(dur).count();
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
            const double msgs_per_sec = (secs > 0.0) ? (static_cast<double>(processed) / secs) : 0.0;
            const double ns_per_msg = (processed > 0) ? (static_cast<double>(dur.count()) / static_cast<double>(processed)) : 0.0;
            out << "Timing:\n";
            out << "  elapsed_ms=" << ms << " ms\n";
            out << "  messages=" << processed << "\n";
            out << std::fixed << std::setprecision(2);
            out << "  msgs_per_sec=" << msgs_per_sec << "\n";
            out << "  ns_per_msg=" << ns_per_msg << "\n";
            // I/O: setup is outside the timed parse (load reads everything there; mmap only
            // maps). With mmap the disk reads happen inside the parse as page faults, and
            // with chunked the reads happen inside it too, so compare total_ms across modes.
            const auto setup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(setup_dur).count();
            const itch::ProcessMemory mem = itch::process_memory();
            out << "IO:\n";
            out << "  io=" << itch::io_name(io_mode) << "\n";
            out << "  setup_ms=" << setup_ms << "  (load: read whole file; mmap: map only; chunked: none)\n";
            out << "  total_ms=" << setup_ms + ms << "\n";
            out << "  total_ns_per_msg=" << (processed > 0 ? static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(setup_dur).count() + dur.count()) / static_cast<double>(processed) : 0.0) << "\n";
            out << "  peak_working_set_mb=" << static_cast<double>(mem.peak_working_set) / (1024.0 * 1024.0)
                << "  (includes mapped file pages)\n";
            out << "  peak_private_mb=" << static_cast<double>(mem.peak_private) / (1024.0 * 1024.0) << "\n";
            out << "  page_faults=" << mem.page_faults << "\n";
            if (trailing) out << "  truncated_tail_bytes=" << trailing << "\n";
            out << std::resetiosflags(std::ios_base::fixed);
        };

        // choose mode: optional argv[4] is mode: book, stats, print, both
        std::string mode = "both";
        if (argc >= 5) mode = argv[4];

        long long processed = 0;
        std::string error_msg;
        bool ok = false;

        if (mode == "both") {
            itch::CombinedHandler handler{};
            auto res = timed_run([&]() { return parse_file(handler, processed, error_msg); });
            ok = res.first;
            const auto dur = res.second;
            if (!ok) { std::cout << "Stop reason: error: " << error_msg << '\n'; return 1; }

            // write metrics using combined handler (stats + book)
            std::string outpath = "metrics.txt";
            if (argc >= 4) outpath = argv[3];
            if (outpath == "-") {
                itch::print_stats(handler.stats.stats, std::cout);
                itch::BookHandler::print_anomalies(handler.book, std::cout);
                print_timing(std::cout, dur, processed);
            } else {
                std::ofstream outf(outpath);
                if (!outf) { std::cout << "Failed to open output file: " << outpath << '\n'; return 1; }
                itch::print_stats(handler.stats.stats, outf);
                itch::BookHandler::print_anomalies(handler.book, outf);
                print_timing(outf, dur, processed);
            }

        } else if (mode == "stats") {
            itch::StatsHandler handler{};
            ok = parse_file(handler, processed, error_msg);
            if (!ok) { std::cout << "Stop reason: error: " << error_msg << '\n'; return 1; }
            std::string outpath = "metrics.txt";
            if (argc >= 4) outpath = argv[3];
            if (outpath == "-") {
                itch::print_stats(handler.stats, std::cout);
            } else {
                std::ofstream outf(outpath);
                if (!outf) { std::cout << "Failed to open output file: " << outpath << '\n'; return 1; }
                itch::print_stats(handler.stats, outf);
            }
        } else if (mode == "print") {
            itch::PrintHandler handler{};
            auto res = timed_run([&]() { return parse_file(handler, processed, error_msg); });
            if (!res.first) { std::cout << "Stop reason: error: " << error_msg << '\n'; return 1; }
            const auto dur = res.second;
            print_timing(std::cout, dur, processed);
        } else if (mode == "book") {
            itch::BookHandler handler{};
            auto res = timed_run([&]() { return parse_file(handler, processed, error_msg); });
            if (!res.first) { std::cout << "Stop reason: error: " << error_msg << '\n'; return 1; }
            const auto dur = res.second;
            std::string outpath = "metrics.txt";
            if (argc >= 4) outpath = argv[3];
            if (outpath == "-") {
                itch::BookHandler::print_anomalies(handler, std::cout);
                handler.print_top("AAPL", std::cout);
                print_timing(std::cout, dur, processed);
            } else {
                std::ofstream outf(outpath);
                if (!outf) { std::cout << "Failed to open output file: " << outpath << '\n'; return 1; }
                itch::BookHandler::print_anomalies(handler, outf);
                handler.print_top("AAPL", outf);
                print_timing(outf, dur, processed);
            }
        } else if (mode == "bbo") {
            // A consumer of the book: per-stock BBO changes and quoted spread, plus for one
            // stock a top-5 snapshot at a time and a log of its BBO changes after it.
            // args: [symbol=AAPL] [HH:MM:SS=10:00:00] [window_ms=1000]
            auto handler = std::make_unique<itch::BboHandler>();
            handler->focus = argc >= 6 ? argv[5] : "AAPL";
            unsigned hh = 10, mm = 0, ss = 0;
            if (argc >= 7 && std::sscanf(argv[6], "%u:%u:%u", &hh, &mm, &ss) != 3) { std::cout << "time must be HH:MM:SS\n"; return 1; }
            const std::uint64_t at = (std::uint64_t{hh} * 3600 + mm * 60 + ss) * 1'000'000'000ULL;
            const std::uint64_t window_ms = argc >= 8 ? std::stoull(argv[7]) : 1000;
            handler->snapshot_at = at;
            handler->log_from = at;
            handler->log_to = at + window_ms * 1'000'000ULL;
            auto res = timed_run([&]() { return parse_file(*handler, processed, error_msg); });
            if (!res.first) { std::cout << "Stop reason: error: " << error_msg << '\n'; return 1; }
            std::string outpath = "metrics.txt";
            if (argc >= 4) outpath = argv[3];
            std::ofstream outf;
            if (outpath != "-") {
                outf.open(outpath);
                if (!outf) { std::cout << "Failed to open output file: " << outpath << '\n'; return 1; }
            }
            std::ostream& o = outpath == "-" ? std::cout : outf;
            handler->report(o);
            print_timing(o, res.second, processed);
            if (outpath != "-") std::cout << "Wrote " << outpath << '\n';
        } else if (mode == "verify") {
            // M7: store-vs-book cross-check at checkpoints and at the end, trade prices
            // to compare with published data, and a state hash for determinism.
            // optional argv[5]: expected combined hash (hex); exit code 2 if it differs
            auto verifier = std::make_unique<itch::VerifyHandler>();
            if (!parse_file(*verifier, processed, error_msg)) { std::cout << "Stop reason: error: " << error_msg << '\n'; return 1; }
            const itch::BookHandler& book = verifier->book;

            std::ostringstream out;
            out << "Store vs book cross-check (every " << verifier->every << " messages):\n";
            bool all_ok = true;
            for (const auto& [at, r] : verifier->checkpoints) {
                out << "  after " << at << " msgs:\n";
                itch::BookHandler::print_verify(r, out);
                all_ok = all_ok && r.ok();
            }
            const itch::VerifyResult final_check = book.verify_levels();
            out << "  end of file:\n";
            itch::BookHandler::print_verify(final_check, out);
            all_ok = all_ok && final_check.ok();
            out << "  overall: " << (all_ok ? "PASS" : "FAIL") << "\n\n";

            itch::BookHandler::print_anomalies(book, out);
            out << '\n';
            book.print_trades({"AAPL", "MSFT", "AMZN", "INTC", "CSCO"}, out);
            out << '\n';

            const auto h = book.state_hash();
            out << std::hex << std::setfill('0')
                << "State hash:\n"
                << "  book     = " << std::setw(16) << h.book << '\n'
                << "  store    = " << std::setw(16) << h.store << '\n'
                << "  trades   = " << std::setw(16) << h.trades << '\n'
                << "  combined = " << std::setw(16) << h.combined << '\n'
                << std::dec << std::setfill(' ');

            int rc = all_ok ? 0 : 1;
            if (argc >= 6) {
                const std::uint64_t expected = std::stoull(argv[5], nullptr, 16);
                const bool same = expected == h.combined;
                out << "  expected = " << argv[5] << (same ? "  MATCH\n" : "  MISMATCH\n");
                if (!same) rc = 2;
            }

            std::string outpath = "metrics.txt";
            if (argc >= 4) outpath = argv[3];
            if (outpath == "-") {
                std::cout << out.str();
            } else {
                std::ofstream outf(outpath);
                if (!outf) { std::cout << "Failed to open output file: " << outpath << '\n'; return 1; }
                outf << out.str();
                std::cout << "Wrote " << outpath << " (" << (all_ok ? "PASS" : "FAIL") << ", hash "
                          << std::hex << h.combined << std::dec << ")\n";
            }
            return rc;
        } else {
            std::cout << "Unknown mode: " << mode << " (expected: both, book, stats, print, verify, bbo)\n";
            return 1;
        }
        // end try
    } catch (const std::filesystem::filesystem_error& e) {
        std::cout << "Filesystem error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}