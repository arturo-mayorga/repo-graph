#include "rgv/live/LiveSource.h"

#include "rgv/fixture/Json.h"

#include <cerrno>
#include <filesystem>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace rgv::live {
namespace {

// Reads whatever is available without blocking. Returns false when the pipe is at EOF,
// which is how a provider exiting reaches us.
bool drain(int fd, std::string& into, bool& eof) {
    char buf[16384];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            into.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) { eof = true; return false; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        eof = true;
        return false;
    }
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

} // namespace

std::string resolve_provider(const std::string& name) {
    if (name.find('/') != std::string::npos) return name;

    std::error_code ec;
    const auto      self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return name;

    const auto sibling = self.parent_path() / name;
    if (std::filesystem::exists(sibling, ec) &&
        (std::filesystem::status(sibling, ec).permissions() & std::filesystem::perms::owner_exec) !=
            std::filesystem::perms::none) {
        return sibling.string();
    }
    return name;   // let PATH have a go, and let the spawn error name it
}

std::vector<std::string> take_lines(std::string& carry, std::string_view chunk) {
    carry.append(chunk);

    std::vector<std::string> lines;
    std::size_t              start = 0;
    for (;;) {
        const std::size_t nl = carry.find('\n', start);
        if (nl == std::string::npos) break;

        std::string line = carry.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();   // tolerate CRLF
        if (!line.empty()) lines.push_back(std::move(line));
        start = nl + 1;
    }
    carry.erase(0, start);
    return lines;
}

struct LiveSource::Impl {
    pid_t       pid       = -1;
    int         out_fd    = -1;
    int         err_fd    = -1;
    std::string out_carry;
    std::string err_carry;

    Snapshot                 baseline;
    bool                     have_baseline = false;
    std::vector<std::string> log;

    // Lines that arrived in the same read as the baseline. A provider that emits its
    // snapshot and its first events without pausing -- which is the normal case, and
    // exactly what a fast walker does -- delivers them in one chunk, and dropping the
    // remainder of that chunk loses events silently.
    std::vector<std::string> pending;

    SourceStatus status;
    double       elapsed_ms = 0.0;

    ~Impl() {
        if (out_fd >= 0) ::close(out_fd);
        if (err_fd >= 0) ::close(err_fd);
        if (pid > 0) {
            // Closing stdout is the polite signal; SIGTERM is the one that always works.
            ::kill(pid, SIGTERM);
            int st = 0;
            ::waitpid(pid, &st, 0);
        }
    }

    void note(std::string text) {
        // Bounded: a provider looping on an error must not grow the UI without limit.
        if (log.size() >= 200) log.erase(log.begin());
        log.push_back(std::move(text));
    }

    // Pulls stderr into the diagnostic log. Never parsed as protocol (contract §6.1).
    void drain_stderr() {
        if (err_fd < 0) return;
        std::string chunk;
        bool        eof = false;
        drain(err_fd, chunk, eof);
        for (auto& line : take_lines(err_carry, chunk)) note(std::move(line));
        if (eof) { ::close(err_fd); err_fd = -1; }
    }
};

LiveSource::LiveSource(std::vector<std::string> argv, double startup_timeout_ms)
    : impl_(std::make_unique<Impl>()) {
    if (argv.empty()) throw SpawnError("no provider command given");

    int out[2] = {-1, -1};
    int err[2] = {-1, -1};
    if (::pipe(out) != 0) throw SpawnError(std::string("pipe: ") + std::strerror(errno));
    if (::pipe(err) != 0) {
        ::close(out[0]);
        ::close(out[1]);
        throw SpawnError(std::string("pipe: ") + std::strerror(errno));
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    posix_spawn_file_actions_addclose(&actions, err[0]);

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (auto& a : argv) cargv.push_back(a.data());
    cargv.push_back(nullptr);

    pid_t     pid = -1;
    const int rc  = ::posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    ::close(err[1]);

    if (rc != 0) {
        ::close(out[0]);
        ::close(err[0]);
        throw SpawnError("could not start '" + argv[0] + "': " + std::strerror(rc) +
                         "\n  (looked next to the frontend, then on PATH)");
    }

    impl_->pid    = pid;
    impl_->out_fd = out[0];
    impl_->err_fd = err[0];
    set_nonblocking(impl_->out_fd);
    set_nonblocking(impl_->err_fd);

    impl_->status.attached    = true;
    impl_->status.description = argv[0];

    // Wait for the baseline. The only blocking call in this class, and it is bounded.
    double waited = 0.0;
    while (!impl_->have_baseline && waited < startup_timeout_ms) {
        struct pollfd pfd { impl_->out_fd, POLLIN, 0 };
        const int     n = ::poll(&pfd, 1, 50);
        waited += 50.0;
        if (n <= 0) continue;

        std::string chunk;
        bool        eof = false;
        drain(impl_->out_fd, chunk, eof);
        for (auto& line : take_lines(impl_->out_carry, chunk)) {
            if (impl_->have_baseline) {
                impl_->pending.push_back(std::move(line));
                continue;
            }
            try {
                auto msg = fixture::parse_live_line(line, "provider");
                if (msg.is_snapshot) {
                    impl_->baseline          = std::move(msg.snapshot);
                    impl_->have_baseline     = true;
                    impl_->status.generation = impl_->baseline.generation;
                    continue;
                }
                // An event before the baseline is a provider bug, not ours. Note it and
                // keep waiting rather than guessing at a graph to apply it to.
                impl_->note("event before snapshot, ignored");
            } catch (const std::exception& ex) {
                impl_->note(std::string("bad line: ") + ex.what());
            }
        }
        if (eof) break;
    }

    impl_->drain_stderr();
    if (!impl_->have_baseline) {
        std::string why = "provider sent no snapshot within " +
                          std::to_string(static_cast<int>(startup_timeout_ms)) + "ms";
        if (!impl_->log.empty()) why += ": " + impl_->log.back();
        throw SpawnError(why);
    }
}

LiveSource::~LiveSource() = default;

const Snapshot& LiveSource::baseline() const { return impl_->baseline; }

const std::vector<std::string>& LiveSource::log() const { return impl_->log; }

int LiveSource::poll(double dt_seconds, EventSink& sink) {
    impl_->elapsed_ms += dt_seconds * 1000.0;
    impl_->drain_stderr();

    std::string chunk;
    bool        eof = false;
    if (impl_->out_fd >= 0) drain(impl_->out_fd, chunk, eof);

    // Whatever the attach read left over comes first, or the ordering the provider
    // wrote is not the ordering the store sees.
    std::vector<std::string> lines;
    lines.swap(impl_->pending);
    for (auto& l : take_lines(impl_->out_carry, chunk)) lines.push_back(std::move(l));

    int emitted = 0;
    for (const auto& line : lines) {
        try {
            auto msg = fixture::parse_live_line(line, "provider");
            if (msg.is_snapshot) {
                // A resync (contract §7.5): the provider is telling us to start over.
                impl_->baseline = std::move(msg.snapshot);
                impl_->note("provider resynced");
                continue;
            }
            // `t_ms` is scheduling information for a replayable source. Live events are
            // due on arrival, and stamping them keeps the event log readable.
            msg.event.t_ms = impl_->elapsed_ms;
            sink.on_event(msg.event);
            impl_->status.generation = msg.event.generation;
            ++emitted;
            ++impl_->status.events_emitted;
        } catch (const std::exception& ex) {
            // One malformed line must not take down the session. Skip it and say so.
            impl_->note(std::string("bad line: ") + ex.what());
        }
    }

    if (eof && impl_->out_fd >= 0) {
        ::close(impl_->out_fd);
        impl_->out_fd       = -1;
        impl_->status.ended = true;
        impl_->note("provider stream ended");
    }
    return emitted;
}

SourceStatus LiveSource::status() const {
    SourceStatus s = impl_->status;
    s.position_ms  = impl_->elapsed_ms;
    return s;   // duration_ms and events_total stay 0: a live stream has no end to know
}

} // namespace rgv::live
