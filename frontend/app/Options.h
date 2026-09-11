// Command line parsing, kept out of main so that file stays a description of the
// application rather than a script.
#pragma once

#include <string>
#include <vector>

namespace rgv::app {

struct Options {
    std::string root;            // fixture root directory
    std::string watch;           // directory to watch live, instead of a fixture
    std::string provider = "rgv-watch";   // provider command used by --watch
    std::string fixture;         // fixture set to start on
    std::string select;          // node selected on startup
    std::string hover;           // node whose hover card is shown on startup
    std::string view;            // architecture | filesystem | file-graph
    std::string filter;          // initial name/path filter text
    std::vector<std::string> hide;   // --hide REGEX, repeatable
    float       zoom = -1.0f;    // camera zoom to hold, instead of fitting
    int         scenario      = 0;
    double      at            = -1.0;   // seek here and pause
    float       relevance     = -1.0f;
    bool        fullscreen    = false;
    bool        text_settings = false;
    bool        list_schedule = false;
};

// Returns false when the program should exit without running (help, or a bad flag).
bool parse_options(int argc, char** argv, const char* default_root, Options& out);

} // namespace rgv::app
