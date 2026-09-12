// Handing a file to whatever the user already opens files with.
//
// Deliberately not a viewer of our own. The desktop already knows which program owns a
// `.py` or a `.png`, the user has already chosen it, and every editor and file manager
// on the machine agrees on how to ask. So this asks: `xdg-open` on Linux, `open` on
// macOS. A built-in viewer would be a worse editor than the one they have, and another
// thing to keep current with the graph.
#pragma once

#include <string>

namespace rgv::platform {

// The absolute path a node's repo-relative path refers to, or empty when there is no
// such file, or when it would leave the repository.
//
// The bound matters. A path arrives over the wire from a provider, which is a separate
// process the frontend does not control (contract section 6), so it is input rather
// than something this program wrote. Handing an arbitrary path to the desktop's
// launcher is handing it a program to run, and "somewhere under the repository the user
// asked to watch" is the whole of what this feature needs.
std::string openable_path(const std::string& repo_root, const std::string& rel);

// Opens `absolute` with whatever the desktop uses for it. Returns false and fills
// `error` when the launcher could not be started; a launcher that starts and then fails
// reports to the user itself, which is the point of using theirs.
//
// The child is detached and double-forked, so a viewer outlives the session, never
// becomes a zombie, and cannot scribble on the streams the provider is using.
bool open_in_default_app(const std::string& absolute, std::string* error);

} // namespace rgv::platform
