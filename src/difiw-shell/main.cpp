// Difiw Shell entry point: banner, prompt, input loop.
// SPDX-License-Identifier: MIT
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "commands.hpp"
#include "parser.hpp"

namespace {

constexpr std::size_t kMaxLine = 4095;

// Runs one line. Returns exit status or difiw::kExitShell.
int run_line(const std::string& line) {
    const difiw::ParseResult pr = difiw::parse_line(line);
    if (!pr.ok) {
        std::fprintf(stderr, "difiw: syntax error: %s\n", pr.error.c_str());
        return 2;
    }
    return difiw::run_command(pr.args);
}

void ignore_signals() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
    sigaction(SIGTSTP, &sa, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--version")) {
        std::printf("Difiw Shell %s (Difiw OS %s)\n", difiw::kVersion, difiw::kEdition);
        return 0;
    }
    difiw::init_state();
    if (argc == 3 && !std::strcmp(argv[1], "-c")) {
        const int rc = run_line(argv[2]);
        return rc == difiw::kExitShell ? 0 : rc;
    }
    if (argc != 1) {
        std::fprintf(stderr, "usage: difiw-shell [--version | -c \"command\"]\n");
        return 2;
    }

    ignore_signals();
    const bool tty = isatty(STDIN_FILENO) != 0;
    if (tty) difiw::print_banner();

    char buf[kMaxLine + 2];
    int status = 0;
    for (;;) {
        if (tty) {
            std::printf("difiw:%s> ", difiw::current_dir().c_str());
            std::fflush(stdout);
        }
        if (!std::fgets(buf, sizeof buf, stdin)) {
            if (tty) std::printf("\nexit\n");  // Ctrl+D
            break;
        }
        std::size_t len = std::strlen(buf);
        if (len > 0 && buf[len - 1] != '\n' && !std::feof(stdin)) {
            // Over-long line: discard the rest of it and report.
            int c;
            while ((c = std::fgetc(stdin)) != EOF && c != '\n') {
            }
            std::fprintf(stderr, "difiw: input too long (max %zu characters)\n", kMaxLine);
            status = 2;
            continue;
        }
        status = run_line(buf);
        if (status == difiw::kExitShell) { status = 0; break; }
        std::fflush(stdout);
    }
    return status;
}
