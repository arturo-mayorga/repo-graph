// Fixture codec. nlohmann/json is deliberately confined to the .cpp so the contract
// headers stay cheap to include from every translation unit in the app.
#pragma once

#include "rgv/contract/Event.h"
#include "rgv/contract/Graph.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace rgv::fixture {

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& what) : std::runtime_error(what) {}
};

// Unrecognized enum tokens and missing required fields raise ParseError with the
// offending path, so a typo in a fixture fails loudly at load instead of quietly
// rendering the wrong thing.
Snapshot parse_snapshot(const std::string& text, const std::string& origin);
Snapshot load_snapshot(const std::string& path);

// Scenarios are JSON Lines: one event per line, blank lines and `//` comments allowed
// so a scenario can be annotated by whoever authors it.
std::vector<Event> parse_scenario(const std::string& text, const std::string& origin);
std::vector<Event> load_scenario(const std::string& path);

std::string read_file(const std::string& path);

} // namespace rgv::fixture
