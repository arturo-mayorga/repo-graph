// The live half of the contract: an IGraphSource fed by a provider process.
//
// Contract §6. The provider is a child process that writes newline-delimited JSON to its
// stdout -- one message per line, the first a `snapshot`, the rest events. This class
// spawns it, reads it without ever blocking the frame, and hands events to the sink.
//
// It knows nothing about what the provider watches or what language it understands. That
// is the whole point: a filesystem walker and a language extractor are the same thing
// from here, distinguishable only by the `provider` field on the edges they emit.
#pragma once

#include "rgv/contract/IGraphSource.h"

#include <memory>
#include <string>
#include <vector>

namespace rgv::live {

class SpawnError : public std::runtime_error {
public:
    explicit SpawnError(const std::string& what) : std::runtime_error(what) {}
};

class LiveSource final : public IGraphSource {
public:
    // Spawns `argv[0]` with `argv` and blocks until the baseline snapshot line arrives
    // or `startup_timeout_ms` elapses. Blocking here and nowhere else is deliberate:
    // `baseline()` is contractually valid from attach, so there is no correct way to
    // return a source that does not have one yet. Throws SpawnError on either failure.
    LiveSource(std::vector<std::string> argv, double startup_timeout_ms = 5000.0);
    ~LiveSource() override;

    LiveSource(const LiveSource&)            = delete;
    LiveSource& operator=(const LiveSource&) = delete;

    const Snapshot& baseline() const override;
    int             poll(double dt_seconds, EventSink& sink) override;
    SourceStatus    status() const override;

    // No timeline. A live stream cannot be scrubbed, and the null return is what tells
    // the UI not to draw a scrubber -- a capability check, not a type check.
    Timeline* timeline() override { return nullptr; }

    // Diagnostics the provider wrote to stderr, newest last. Never parsed as protocol.
    const std::vector<std::string>& log() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Splits a buffer into complete lines, leaving any trailing partial line in `carry` for
// the next read. Exposed because it is the one piece of the transport worth testing
// without a process: a message must never be parsed until its terminator has arrived.
std::vector<std::string> take_lines(std::string& carry, std::string_view chunk);

} // namespace rgv::live
