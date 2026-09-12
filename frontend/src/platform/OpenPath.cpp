#include "rgv/platform/OpenPath.h"

#include <filesystem>
#include <system_error>

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

bool open_in_default_app(const std::string& absolute, std::string* error) {
    if (absolute.empty()) {
        if (error) *error = "no path";
        return false;
    }
#if defined(_WIN32)
    if (error) *error = "not implemented on Windows";
    return false;
#else
#if defined(__APPLE__)
    const char* launcher = "open";
#else
    const char* launcher = "xdg-open";
#endif
    const pid_t child = ::fork();
    if (child < 0) {
        if (error) *error = "fork failed";
        return false;
    }
    if (child == 0) {
        // Double fork: the launcher is reparented away from us, so it outlives the
        // session and never has to be reaped.
        const pid_t grandchild = ::fork();
        if (grandchild == 0) {
            ::setsid();
            // A viewer inherits our terminal otherwise, and writes over the diagnostics
            // the provider is putting on it.
            const int null = ::open("/dev/null", O_RDWR);
            if (null >= 0) {
                ::dup2(null, 0);
                ::dup2(null, 1);
                ::dup2(null, 2);
                if (null > 2) ::close(null);
            }
            // No shell anywhere in this path, so nothing in a filename can be read as
            // one: a space or a quote is part of the name and nothing more.
            ::execlp(launcher, launcher, absolute.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::_exit(grandchild < 0 ? 1 : 0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);   // the intermediate child only; it exits at once
    return true;
#endif
}

} // namespace rgv::platform
