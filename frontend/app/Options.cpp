#include "Options.h"

#include <cstdlib>
#include <iostream>

namespace rgv::app {

bool parse_options(int argc, char** argv, const char* default_root, Options& o) {
    o.root = default_root;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::cout << "usage: rgv [options] [fixture-root]\n"
                         "  --fixture NAME   start on this fixture set\n"
                         "  --scenario N     start on scenario N\n"
                         "  --fullscreen     open fullscreen on the primary monitor\n"
                         "  --at MS          seek to this point in the scenario and pause\n"
                         "  --select NODE    select this node id on startup\n"
                         "  --hover NODE     show this node's hover card on startup\n"
                         "  --relevance F    start with the relevance filter at F (0..1)\n"
                         "  --text-settings  open the text size window on startup\n"
                         "  --schedule       print the system schedule and exit\n";
            return false;
        }
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--text-settings") o.text_settings = true;
        else if (a == "--schedule") o.list_schedule = true;
        else if (a == "--fixture" && i + 1 < argc) o.fixture = argv[++i];
        else if (a == "--scenario" && i + 1 < argc) o.scenario = std::atoi(argv[++i]);
        else if (a == "--at" && i + 1 < argc) o.at = std::atof(argv[++i]);
        else if (a == "--select" && i + 1 < argc) o.select = argv[++i];
        else if (a == "--hover" && i + 1 < argc) o.hover = argv[++i];
        else if (a == "--relevance" && i + 1 < argc) o.relevance = std::atof(argv[++i]);
        else o.root = a;
    }
    return true;
}

} // namespace rgv::app
