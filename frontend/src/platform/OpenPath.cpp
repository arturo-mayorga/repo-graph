#include "rgv/platform/OpenPath.h"

#include <filesystem>
#include <system_error>
#include <vector>

#include <cerrno>
#include <cstring>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace rgv::platform {

std::string openable_path(const std::string& repo_root, const std::string& rel) {
    if (repo_root.empty() || rel.empty()) return {};

    std::error_code ec;
    const fs::path  root = fs::weakly_canonical(fs::path(repo_root), ec);
    if (ec) return {};
    const fs::path target = fs::weakly_canonical(root / rel, ec);
    if (ec) return {};

    // Inside the repository, after symlinks are resolved. A link pointing out of the
    // tree resolves out of the tree and is refused.
    const std::string root_s = root.generic_string();
    const std::string tgt_s  = target.generic_string();
    if (tgt_s != root_s && tgt_s.rfind(root_s + "/", 0) != 0) return {};

    if (!fs::exists(target, ec) || ec) return {};
    return target.string();
}

std::vector<std::string> opener_argv(const std::string& command, const std::string& absolute) {
#if defined(__APPLE__)
    const char* launcher = "open";
#else
    const char* launcher = "xdg-open";
#endif
    if (command.empty()) return {launcher, absolute};

    std::vector<std::string> argv;
    bool                     placed = false;
    for (std::size_t at = 0; at < command.size();) {
        const std::size_t begin = command.find_first_not_of(" \t", at);
        if (begin == std::string::npos) break;
        std::size_t end = command.find_first_of(" \t", begin);
        if (end == std::string::npos) end = command.size();
        std::string tok = command.substr(begin, end - begin);
        if (tok == "{}") {
            // One slot, whatever is in the name. This is the reason there is no shell
            // in this path: a space or a quote in a filename is part of the name.
            argv.push_back(absolute);
            placed = true;
        } else {
            argv.push_back(std::move(tok));
        }
        at = end;
    }
    if (argv.empty()) return {launcher, absolute};
    if (!placed) argv.push_back(absolute);
    return argv;
}

bool open_in_default_app(const std::string& absolute, std::string* error) {
    return open_with(absolute, std::string{}, error);
}

bool open_with(const std::string& absolute, const std::string& command, std::string* error) {
    if (absolute.empty()) {
        if (error) *error = "no path";
        return false;
    }
#if defined(_WIN32)
    if (error) *error = "not implemented on Windows";
    return false;
#else
    const std::vector<std::string> args = opener_argv(command, absolute);
    std::vector<char*>             argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    // How exec failure gets back here. The launcher is deliberately reparented away so
    // a long-lived editor never has to be reaped, which also means its exit status is
    // gone -- but the one failure worth reporting, "that program is not there", happens
    // before exec succeeds. The pipe is close-on-exec: it shutting empty IS the success
    // signal, and nothing has to be waited for.
    int report[2] = {-1, -1};
    if (::pipe(report) != 0) {
        if (error) *error = "pipe failed";
        return false;
    }
    ::fcntl(report[1], F_SETFD, FD_CLOEXEC);

    const pid_t child = ::fork();
    if (child < 0) {
        if (error) *error = "fork failed";
        return false;
    }
    if (child == 0) {
        ::close(report[0]);
        // Double fork: the launcher is reparented away from us, so it outlives the
        // session and never has to be reaped.
        const pid_t grandchild = ::fork();
        if (grandchild == 0) {
            ::setsid();
            // A viewer inherits our terminal otherwise, and writes over the diagnostics
            // the provider is putting on it.
            // stdin and stdout go nowhere -- a viewer inheriting them writes over the
            // diagnostics the provider is putting on the terminal. stderr is kept: when
            // the launcher refuses a file because nothing is registered for its type,
            // that sentence is the whole diagnosis and swallowing it is why this used to
            // look like nothing happening at all.
            const int null = ::open("/dev/null", O_RDWR);
            if (null >= 0) {
                ::dup2(null, 0);
                ::dup2(null, 1);
                if (null > 2) ::close(null);
            }
            // No shell anywhere in this path, so nothing in a filename can be read as
            // one: a space or a quote is part of the name and nothing more.
            ::execvp(argv[0], argv.data());
            const int err = errno;
            const ssize_t ignored = ::write(report[1], &err, sizeof err);
            (void)ignored;
            ::_exit(127);
        }
        ::_exit(grandchild < 0 ? 1 : 0);
    }
    ::close(report[1]);
    int status = 0;
    ::waitpid(child, &status, 0);   // the intermediate child only; it exits at once

    int          exec_errno = 0;
    const ssize_t got = ::read(report[0], &exec_errno, sizeof exec_errno);
    ::close(report[0]);
    if (got == static_cast<ssize_t>(sizeof exec_errno)) {
        if (error) *error = std::string(args[0]) + ": " + std::strerror(exec_errno);
        return false;
    }
    return true;
#endif
}

} // namespace rgv::platform
