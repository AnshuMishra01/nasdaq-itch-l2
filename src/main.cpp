#include "core/message.h"
#include "decode.h"
#include "parse.h"
#include "measure.h"
#include "message_decode.h"
#include "struct_sizes.h"
#include "handlers/book_handler.h"
#include "handlers/combined_handler.h"
#include "handlers/verify_handler.h"

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
        std::cout << "Usage: " << argv[0] << " <file> <count>  (use count<=0 to process full file)\n";
        std::cout << "       " << argv[0] << " --dump-structs [or --dump-structs=path]\n";
        return 1;
    }

    try {
        int count = std::stoi(argv[2]);
        std::cout << "DEBUG: file='" << argv[1] << "' count=" << count << '\n';
        const std::filesystem::path path{argv[1]};
        const auto fileSize = std::filesystem::file_size(path);

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            std::cout << "Failed to open file: " << argv[1] << '\n';
            return 1;
        }

        std::vector<unsigned char> buffer(static_cast<std::size_t>(fileSize));
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));
        const std::streamsize bytesRead = file.gcount();
        std::cout << "DEBUG: fileSize=" << fileSize << " bytesRead=" << bytesRead << '\n';
        if (bytesRead != static_cast<std::streamsize>(fileSize)) {
            std::cout << "Read " << bytesRead << " bytes, expected " << fileSize << '\n';
            return 1;
        }

        // framing loop lives in parse.h, shared with the M9 benchmark
        auto parse_file = [&](auto &handler, int &processed, std::string &error_msg) -> bool {
            const itch::ParseResult r = itch::parse_buffer(buffer.data(), buffer.size(), handler, count);
            processed = static_cast<int>(r.processed);
            if (!r.ok) error_msg = r.error;
            return r.ok;
        };

        // helper: timed runner that executes a callable and returns ok + duration
        auto timed_run = [](auto&& fn) {
            const auto t0 = std::chrono::steady_clock::now();
            bool ok = fn();
            const auto t1 = std::chrono::steady_clock::now();
            return std::make_pair(ok, std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0));
        };

        auto print_timing = [&](std::ostream &out, const std::chrono::nanoseconds &dur, int processed) {
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
            out << std::resetiosflags(std::ios_base::fixed);
        };

        // choose mode: optional argv[4] is mode: book, stats, print, both
        std::string mode = "both";
        if (argc >= 5) mode = argv[4];

        int processed = 0;
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
            std::cout << "Unknown mode: " << mode << " (expected: both, book, stats, print, verify)\n";
            return 1;
        }
        // end try
    } catch (const std::filesystem::filesystem_error& e) {
        std::cout << "Filesystem error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}