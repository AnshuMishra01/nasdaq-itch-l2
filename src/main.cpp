#include "core/message.h"
#include "decode.h"
#include "measure.h"
#include "message_decode.h"
#include "struct_sizes.h"
#include "handlers/book_handler.h"
#include "handlers/combined_handler.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <iostream>
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

        const std::array<std::uint8_t, 256> msg_sizes = [] {
            std::array<std::uint8_t, 256> a{};
            a[static_cast<unsigned char>('S')] = 12; a[static_cast<unsigned char>('R')] = 39;
            a[static_cast<unsigned char>('H')] = 25; a[static_cast<unsigned char>('Y')] = 20;
            a[static_cast<unsigned char>('L')] = 26; a[static_cast<unsigned char>('V')] = 35;
            a[static_cast<unsigned char>('K')] = 28; a[static_cast<unsigned char>('A')] = 36;
            a[static_cast<unsigned char>('F')] = 40; a[static_cast<unsigned char>('E')] = 31;
            a[static_cast<unsigned char>('X')] = 23; a[static_cast<unsigned char>('D')] = 19;
            a[static_cast<unsigned char>('U')] = 35; a[static_cast<unsigned char>('P')] = 44;
            a[static_cast<unsigned char>('W')] = 12; a[static_cast<unsigned char>('h')] = 21;
            a[static_cast<unsigned char>('C')] = 36; a[static_cast<unsigned char>('Q')] = 40;
            a[static_cast<unsigned char>('B')] = 19; a[static_cast<unsigned char>('I')] = 50;
            a[static_cast<unsigned char>('N')] = 20; a[static_cast<unsigned char>('J')] = 35;
            return a;
        }();

        const std::size_t bufferSize = buffer.size();

        auto parse_file = [&](auto &handler, int &processed, std::string &error_msg) -> bool {
            std::size_t pos = 0;
            bool had_error = false;
            const bool process_full = (count <= 0);
            processed = 0;
            while (pos + 2 <= bufferSize && (process_full || processed < count)) {
                const int len = (static_cast<std::uint8_t>(buffer[pos]) << 8) + static_cast<std::uint8_t>(buffer[pos + 1]);
                if (len < 1) {
                    had_error = true;
                    error_msg = "Invalid length";
                    break;
                }

                const std::size_t msgStart = pos + 2;
                if (msgStart + static_cast<std::size_t>(len) > bufferSize) {
                    had_error = true;
                    error_msg = "Message length exceeds buffer";
                    break;
                }

                unsigned char* message = buffer.data() + msgStart;
                const unsigned char type = message[0];
                const std::uint8_t expected_u = msg_sizes[type];
                if (expected_u == 0) {
                    had_error = true;
                    error_msg = "Unknown message type";
                    break;
                }

                const std::size_t expected = static_cast<std::size_t>(expected_u);
                if (expected != static_cast<std::size_t>(len)) {
                    had_error = true;
                    error_msg = "Size mismatch";
                    break;
                }

                if (itch::dispatch(type, message, handler)) {
                    processed++;
                }

                pos = msgStart + static_cast<std::size_t>(len);
            }
            return !had_error;
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
        } else {
            std::cout << "Unknown mode: " << mode << " (expected: both, book, stats, print)\n";
            return 1;
        }
        // end try
    } catch (const std::filesystem::filesystem_error& e) {
        std::cout << "Filesystem error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}