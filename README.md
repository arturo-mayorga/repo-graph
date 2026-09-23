# Live Repository Impact Graph — native frontend

A local-first view of what a coding agent just did to a repository's dependency
structure. Not "which lines changed" — **which relationships changed, and what can
break as a result**.

The product spec is [`live_repository_impact_graph_spec_v0.1.md`](live_repository_impact_graph_spec_v0.1.md).
This repository currently contains the **frontend**: a C++20 / OpenGL 3.3 application
with an EnTT entity-component-system at its core, driven entirely by fixtures so the
interaction can be designed and argued about before any backend exists.

```
docs/frontend-contract.md   the interface the frontend consumes  <- read this first
fixtures/                   authored snapshots + scenario timelines
frontend/                   the application
```

## Why fixtures first

The backend in the spec is a large system: Watchman, Tree-sitter, SCIP, DuckDB,
package adapters. None of it is needed to answer the question that actually decides
the product — *can a person look at this and immediately understand the blast radius?*

So the frontend consumes one interface, `IGraphSource`, and two things implement it: a
fixture player (today) and a WebSocket client (later). No frontend code branches on
which is attached. Scenarios are hand-editable JSON timelines, replayed with a
scrubber, so a reviewer can step through "the agent redirected an import" event by
event and argue about what the screen should be doing at each beat.

## Build

Needs a C++20 compiler, CMake ≥ 3.20, and an OpenGL 3.3 driver. GLFW and Dear ImGui
are fetched at configure time and pinned to a tag; EnTT and nlohmann/json are vendored
under `frontend/third_party/`.

```sh
# Arch: sudo pacman -S --needed cmake ninja
cmake -S frontend -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Four suites run and none of them need a display:

| Target | Covers |
|---|---|
| `rgv-tests` | contract, store, scene, layout, settings, specificity, and the ECS itself |
| `rgv-ui-tests` | panel layout invariants, driven through real ImGui frames with no window |
| `rgv-replay <dir> --check` | fixture validation, once per fixture set |

What CI should run — no GL, no ImGui, no network fetch, and warnings are errors:

```sh
cmake -S frontend -B build-headless -G Ninja -DRGV_BUILD_APP=OFF -DRGV_WARNINGS_AS_ERRORS=ON
cmake --build build-headless && ctest --test-dir build-headless
```

Useful options:

| Option | Effect |
|---|---|
| `-DRGV_BUILD_APP=OFF` | headless build: core, replay tool, tests. No GL, no network fetch. |
| `-DRGV_OFFLINE=ON` | never fetch; use an installed GLFW and a local ImGui checkout. |
| `-DRGV_WAYLAND=ON` | build GLFW's Wayland backend as well as X11. |
| `-DRGV_WARNINGS_AS_ERRORS=ON` | for CI. |

## Run

```sh
./build/bin/rgv                                   # smallest fixture, windowed
./build/bin/rgv --fullscreen                      # fills the primary monitor
./build/bin/rgv --fixture monorepo-ts --scenario 2 --at 1200 --select pkg:api
./build/bin/rgv --hover pkg:api                    # opens with a node's card showing
./build/bin/rgv --text-settings                   # opens with the text size window up
./build/bin/rgv --fixture large-synthetic --relevance 0.5
```

### Watching a real checkout

```sh
./build/bin/rgv --watch .                          # watch this repo, live
./build/bin/rgv --watch ~/code/some-project
./build/bin/rgv --watch . --provider /path/to/other-provider
./build/bin/rgv --watch ~/code/app --select file:src/app/systems/movement.py
```

`rgv-watch` is found next to the `rgv` binary, so nothing needs to be on `PATH`. Pass
`--provider` to point at a different one; a name with a slash in it is used as given.

`--watch` swaps the fixture player for `rgv-watch`, a provider process that walks the
directory, emits it as a contract snapshot, and then streams deltas as files are created,
modified and deleted. Saving a file marks it changed in the session panel and bumps the
generation, live. Saving a Python file also re-reads its imports, moves any edge that
changed, and re-computes the blast radius: the files that import it light up in the File
graph view, and the package that owns it seeds the Architecture view.

The provider is a separate program that shares nothing with the frontend but the wire
format — newline-delimited JSON on stdout, `docs/frontend-contract.md` §6 — so you can
watch what it produces without a UI at all:

```sh
./build/bin/rgv-watch --root . | head -3
```

The provider has five adapters. The **filesystem** adapter reports containment: which
files and directories exist and when they change.

What it walks is the repository minus what the repository says is derived: the root
`.gitignore`, plus a built-in list of directories that cost more to watch than they can
be worth (`node_modules`, `.venv`, `__pycache__`, `build`, …) for repositories that say
nothing. This matters more than it sounds — a derived tree is not noise in the graph, it
is a second graph, and this repository's own `build-headless/` was contributing 24
packages that were temp checkouts written by the test suite. It is patterns and names,
never a prefix: `builder/` and `buildings/` are source directories with an unlucky
spelling, and hiding source is a worse failure than showing a build tree. The supported
subset of gitignore syntax, and the cases deliberately left out, are documented at the
top of `provider/watch/Ignore.h`.

The **manifest** adapter finds packages and their declared dependencies, which is what
populates the Architecture view:

| Ecosystem | Manifest | Reads |
|---|---|---|
| npm / pnpm / yarn | `package.json` | `name`, `dependencies`, `peerDependencies`, `devDependencies` |
| Python | `pyproject.toml` | PEP 621 `[project]` and Poetry `[tool.poetry]` |

A package node *replaces* the directory node at its path, and everything inside reparents
onto it — so walking up the containment tree answers "which package owns this file"
(FR-11). A dependency naming a package in the repo becomes a `depends_on` edge between
them; anything else becomes an `external_package`, which the `External` filter hides by
default. Every edge carries the manifest path, line number and snippet that declared it,
which is what the provenance inspector shows.

Declared dependencies, not used ones. A manifest states what a package is *allowed* to
depend on — that is `confidence: "exact"` about the declaration and says nothing about
whether any code imports it. Import-level truth needs a reader per language, and the
other three adapters are those.

The **python-imports** adapter reads every `.py` file and turns its `import` and
`from … import` statements into `imports` edges between files, which is what populates
the File graph view. It also makes every directory with an `__init__.py` a package node,
nested under whatever contains it, and aggregates the imports that cross package
boundaries into one `depends_on` edge per pair, carrying the first crossing import as
evidence. That is what makes the Architecture view of a single-distribution repository —
one `pyproject.toml`, a dozen packages — an architecture rather than one box. A Python
package replaces the directory node at its path exactly as a manifest package does, so
"which package owns this file" is still a walk up the containment tree, and it lands on
the innermost one.

A distribution and its top-level package usually share a name — `elevators` the
`pyproject.toml`, `elevators` the directory under `src/` — so they are drawn as one
node, and **that node stands where the code stands, not where the manifest does.** A
`src` layout declares `src/elevators` from a file at the repository root; seating the
package on the manifest's directory would make it the repository wearing a package's
name, with `tests/`, `docs/` and `tools/` all inside it. On this repository that
attributed 132 of 158 architecture imports to the test suite and invented three cycles
in a graph that has none. Package-level impact is seeded there and runs over the aggregated
edges together with the declared ones. An import resolves against the source roots — the repository root,
each package directory, and any `src/` under either, so both the flat and the src
layout work — to `module.py` or `module/__init__.py`. `from pkg import name` tries
`pkg/name.py` first and falls back to `pkg/__init__.py`, which is what the interpreter
does. Relative imports resolve from the importing file. Every edge carries the line and
the statement that declared it.

What it will not do is guess. An import it cannot place in the repository — the
standard library, a third-party package, a module built with `importlib` — produces no
edge at all, because an edge to a node that does not exist is worse than a missing one
and mapping a module name to a distribution (`yaml` is `PyYAML`) is a different problem.
An import two source roots could satisfy resolves to the first and is marked
`heuristic`, which the traversal skips by default. Imports inside strings and comments
are not imports; imports inside functions and `if TYPE_CHECKING:` blocks are.

The **ts-imports** adapter does the same for `.ts`, `.tsx`, `.js`, `.jsx`, `.mts`,
`.cts`, `.mjs` and `.cjs`, and the whole of the difference is resolution. It takes the
specifier out of every static `import`, every re-export (`export … from`), every
`require()` and every dynamic `import()`, `import type` included — erased at run time,
still a dependency, since changing the type stops the importer compiling.

Then it resolves the way Node and TypeScript do. A specifier is extensionless, so
`./util` is tried as `.ts`, `.tsx`, `.mts`, `.cts`, `.d.ts` and then the JavaScript
extensions, source before anything compiled beside it; a directory resolves to the
`index` in it. A TypeScript file compiled for ESM writes `./util.js` and means the
`./util.ts` beside it, so that mapping is applied — after looking for the literal file,
so a specifier that meant what it said still gets it. A bare specifier is matched
against the packages this repository declares, longest name first so `@acme/auth-core`
is not mistaken for a subpath of `@acme/auth`: an exact match resolves through the
manifest's `source`, `module` or `main`, falling back to the conventional `index` and
`src/index`, because `main` usually points into a build directory nobody checks in. A
subpath resolves inside the package directory. Everything else — `react`, `node:fs`, a
path from a `tsconfig.json` alias — resolves to nothing, for the same reason as in
Python: an edge to a node that does not exist is worse than a missing one.

The **cpp-imports** adapter reads `.cpp`, `.cc`, `.cxx`, `.c` and every header beside
them, and turns `#include` into the same file-level edges. A quoted include is looked up
beside the including file first, which is what the standard says and what makes
`Impact.cpp` → `Impact.h` unambiguous; after that, and for an angled include from the
start, it goes to the source roots — the repository root, every directory named
`include`, every directory named `src`. C++ has no packaging to read an include path
out of; the real answer lives in a build system's include path, and these are the
conventions a repository is laid out by. `<vector>` and `<entt/entt.hpp>` resolve to
nothing and produce nothing, and an include two roots could satisfy resolves to the
first and is marked `heuristic`. A header and its implementation are two nodes and
`foo.cpp → foo.h` is a real edge, not a pair to be folded away. What it will not read is
the include directories the build declares, so an include that only
`target_include_directories` makes findable produces no edge rather than a guessed one.

And because C++ has no packages, the unit above the file is the **build target**:
`add_library(...)` and `add_executable(...)` out of `CMakeLists.txt`, which is the only
place a C++ repository says what it is made of. A target owns the sources it lists and,
transitively, the headers those sources include — a .cpp never includes another .cpp, so
without the second half the architecture view would be boxes with no lines between them,
every line ending in an `include/` tree no build file mentions. That ownership is the
containment parent, because containment is the only thing the architecture view folds an
import along. A target is not a directory, though: its sources come from several and one
file can be listed in two targets, so a file has one parent and every further claim is an
`owns` edge (contract §3.1), never a second parent. Where a whole directory belongs to
one target the directory moves rather than each file in it, which keeps the filesystem
view's tree recognisable. Ties are broken the way a C++ programmer would: a target that
*lists* a file beats one that merely reaches it, a library beats an executable (a header
several targets compile belongs to the library among them), then the target whose sources
are least scattered, then the name. Impact is emitted at the `build target` level too —
change a header, and the targets that have to be rebuilt light up.

On this repository that is 307 `imports` edges and 6 targets: `rgv`, `rgv-tests`,
`rgv-replay` and `rgv-ui-tests` all depending on `rgv_core`, and `rgv-watch` depending on
nothing, which is what a provider that shares only the wire format should look like.

No symbols for TypeScript or C++ yet, so `reads` and `writes` edges are Python-only.
TypeScript's export forms are varied enough that a line scanner would start guessing, and
a C++ definition is spread over a header and a translation unit, which is a question for
a parser. A wrong symbol edge is worse than a missing one.

All three readers are line scanners, not parsers, and they are meant to stay that way: an
import is a statement at the start of a logical line, and the cases a scanner cannot see are the ones a
dependency graph should not claim to.

**Symbols.** The same adapter extracts every top-level class and function and follows
each file's import bindings to where the names it uses are defined — through re-exports
too, so `from ..components import CarState` lands on `components/car.py` even though
`components/__init__.py` is what the import names. A symbol something *else* uses
becomes a `symbol` node under its file; one nobody imports is a detail of the file. How
a file uses a symbol is the edge: constructing or invoking it (`CarState(...)`,
`queries.load(...)`) is a `calls` edge, every other mention — a query by type, an
annotation, an argument — is a `references` edge. In an entity-component codebase that
is write versus read: the system that builds a component owns it, the systems that ask
for it consume it, and two systems are related by the component between them without
importing each other. The architecture view draws those as coloured edges between the
modules; symbol-level impact is seeded at the symbols a changed file defines and reaches
the files that use them.

What a name scan cannot see: a component fetched and then mutated in place
(`position.floor = 3`) is a read of `CarPosition` here, not a write. Calling that a
write needs dataflow, which is a parser's job and a later provider's.

The blast radius is the provider's too. The frontend renders `impact.updated`; it does
not derive one from a live stream (contract §6.4). So after any save, or any edge that
moves, `rgv-watch` walks the `imports` edges in reverse from every file changed since
the baseline and emits the file-level result, with one shortest path per hit, and the
package-level result seeded by the packages that own those files, and the
build-target-level result seeded by the targets that build them. The end-to-end test
checks the provider's answer against the frontend's own traversal over the same store,
which is the same agreement `rgv-replay --check` demands of a fixture.

Startup picks a view that has something in it — a baseline with no dependency edges opens
on Filesystem — so you land somewhere useful without passing `--view`.

Namespace packages — a directory of modules with no `__init__.py` — are not package
nodes; their files are owned by the nearest package above them. A distribution and its
top-level package usually share a name (`elevators` the pyproject, `elevators` the
directory), so the view shows two boxes with that label: the one with a path under it is
the code, the one without is the manifest, which owns `tests/` and anything else outside
the package.

Adding an ecosystem is adding a reader in `provider/watch/Packages.cpp`; adding a
language is another `PythonImports`-shaped pair of functions — parse one file, resolve
against the tree — and the loop in `main.cpp` does not change. Cargo and Go are not read
yet. CMake is, but only for what it builds: a `target_link_libraries` is a declared
dependency this does not report, the same distinction as a manifest against an import.

`--scenario N --at MS --select NODE --hover NODE --text-settings` reproduce an exact
on-screen state,
which is what makes a screenshot or a bug report about the UX worth anything. Live
input always wins over `--hover`, so pointing at something else just works.

| Input | Action |
|---|---|
| drag / wheel | pan / zoom |
| hover a node | lights every edge it has: what it reads, writes, and imports |
| hover a node | fades in a card: what it is, what changed, why it is impacted |
| drag a node | move it; it settles back into place on release |
| double click a file | open it with whatever this desktop opens files with |
| double click anything else | pin / unpin in place |
| `F` | fit to view |
| `Tab` | hide the side and bottom panels; the graph takes the window |
| `space` / `.` / `R` | play-pause / step one event / restart |
| `Esc` | clear selection |

**`Tab` gives the graph the window.** The session, inspector and scenario panels are
about 45% of a 1920-wide screen between them, and a view whose job is to be looked at
should be able to have that back. The toolbar stays: it is the way back, it holds the
controls that change what the graph shows, and it is 64 pixels against their 880. The
**Panels** checkbox on it does the same thing for anyone who has not read this.

Hiding them widens the free viewport, so the labels recompute against the room they
actually have and a good many more of them fit. `--no-panels` opens that way.

### Verifying a change you can see

The app is interactive, so reproducible states are flags rather than clicks — that is
what the `--scenario`/`--at`/`--select`/`--hover`/`--text-settings` options are for.
`--fullscreen` is real GLFW fullscreen, so demoing never depends on the window manager.

Screenshot with whatever your compositor provides (`grim` on Wayland) and read the
image back. One trap worth knowing: **if a capture comes back blank or the screenshot
tool hangs, check whether the screen is locked before suspecting the code.** Draw calls
being issued while the capture shows nothing means the capture is lying, not the
renderer.

### The filesystem view

Laid out radially, inspired by [Gource](https://gource.io). The repository sits at the
centre; directories are discs that grow gently with the files they hold; those files
orbit the directory that owns them, clear of it, coloured by extension; child subtrees
fan outward.

Containment runs from the container to what it holds, so the arrowheads spread outward
from the repository rather than converging on it. That is the opposite of a dependency
arrow, and deliberately: one says "this is inside me", the other says "I need this".

Node sizes step by the **golden ratio**: a file, a directory, and the largest a
directory grows to are `r`, `r·φ`, `r·φ²`. Three sizes on one geometric scale read as a
family — clearly different, obviously related — where an arbitrary ratio reads either as
two unrelated shapes or as no difference at all.

A directory's **drawn size** and the **orbit its files sit on** are separate. Conflating
them puts every file dot exactly on its directory's edge, half-occluding it, and makes
the size ratio between a file and a directory the ratio of a dot to a whole orbit.

Files are packed onto **successive orbits** rather than one ring, which is what Gource
does and for the same reason. A single orbit seating 57 icons needs a radius of 160
around a disc of radius 16: a vast empty annulus with most of the screen wasted.
Filling orbits outward keeps a wide directory compact.

Gource pushes nodes apart with a force simulation. This does it by construction: every
subtree is laid out in its own frame first, so its enclosing radius is exact rather than
estimated, and a parent packs those subtrees as rigid discs. Nothing overlaps, nothing
settles, and the same repository always draws identically.

**Each child comes in as close as it can sit.** They used to share one ring whose radius
was set by the largest of them, so a directory holding a single file was flung as far
out as one holding sixty, and the annulus between them went to waste. Seating each on
its own terms is what lets a small subtree tuck into the gap between two big ones.

**A subtree's enclosing disc is measured where it actually is, not assumed to be centred
on the subtree's root.** This is the whole ball game for a deep tree. A parent holding
one child of radius `R` is `R` plus a constant, not `2R`: assuming the parent sat at the
centre made every level of nesting double, so six levels of `var/state/platform/…` cost
`2⁶`. A 723-file repository came out twenty thousand units across with three thousandths
of a percent of it covered in anything. It is now about a thousand, and the on-screen ink
roughly triples once the camera fits it.

A subtree is then **turned so its bulk faces away from its parent**, which puts its root
on the near side with everything it holds blooming outward — the reason the tree reads
outward from the repository, and the reason a parent is nearer the centre than its
children.

**A directory's disc is sized by what it holds directly**, and saturates. It has to stay
clear of its own file ring, so growing it with everything underneath would shove that
subtree outward — a factor of two per level of nesting. Subtree mass is therefore not
encoded in node size at all: it shows in how much room a subtree takes up, which is
where it belongs. Gource's answer is the opposite — size the directory by the mass
beneath it and draw only a bloom, never a disc — and we tried the hybrid, disc plus
bloom. It read as haze rather than structure, so the bloom is gone.

Impact colouring still wins over the extension palette. The repository looks like
Gource; the blast radius lights up on top of it.

**Hovering a node curves its dependencies over the tree.** Amber for what it depends on,
cyan for what depends on it, an arrowhead on the dependency end either way. A directory
answers for everything inside it, so "what does this folder need" is a hover rather than
a query, and a dependency that stays inside what you are pointing at is not a crossing
and is not drawn. Symbols are not in this view, so a use of one answers as the file that
defines it.

They are curved on purpose. Every line the layout itself draws is a straight radial stub
from a child to the disc that holds it, so a bowed line cannot be mistaken for one. They
bow toward the hub the tree grows from, which makes them follow the structure they are
drawn over instead of cutting across it.

How far they bow is capped at a fraction of the distance they span, so a curve never
swings wider than the gap it covers. Pulling every control point a fixed fraction of the
way to the hub makes a short hop between two neighbours arc right across the view, since
the distance to the hub has nothing to do with how far apart the two nodes are. Nearby
links stay gentle, long ones still bundle.

And they exist only at draw time, produced from the store rather than as entities, so
the layout never learns they exist: hovering moves nothing. That is what lets the most
readable picture the tool draws stay a containment tree while still answering the
dependency question. `--hover NODE` holds until the pointer actually moves, so a
screenshot of one is reproducible.

**Zooming in morphs circles into labelled boxes**, with the name rendered inside the
box — shrunk to fit it, so a half-morphed node never draws a rectangle with its name
floating outside. Boxes appear only where there is room for one. That caveat is not a shortcut — dense radial packing and full text boxes are in
direct conflict. Files on an orbit sit ~18 world units apart while a filename box is
~120 wide, so letting every node grow to its label produces an unreadable stack. The
morph is gated on room × zoom: the repository and its packages become proper boxes with
the name inside; files and cramped directories stay circles and are named from outside.

Names appear as you zoom in, because a label drawn *beside* a node holds a constant
screen size while the graph spreads out beneath it. World-scaled text grows in step with
the spacing and never uncrowds, however far you zoom. Gource makes the same split.

That constant size is also the problem: at any zoom there is a fixed amount of room and
more names than room, so something has to choose. **Labels beside a node are sorted by
priority and taken in order, and one is drawn only if it clears every label already
drawn.** Priority is how much is attached to the node — its dependency degree in the
repository plus what it holds on screen — so the names that survive are the ones the
most of the picture connects to. Both halves of that sum are needed: a directory does
not import anything, and the filesystem view is unreadable without directory names,
while the only drawn edge a file has there is the one to its parent, which would make
the module everything imports no more nameable than the module nothing imports.

Attention outranks all of it, as a ladder rather than a sum: the node under the pointer,
then the selection, then the nodes a hover curve arrives at, then the nodes on the
dependency path being explained, then everything else. A sum would let a node that is
both selected and reached by a curve climb over the one being pointed at. Degree orders
nodes within a rung and never between them, so a promoted group keeps its own order.

The curves matter most here. They are the answer to the question a hover asked, and an
answer nobody can read is not one, so the file at the far end of one is named even
though the shape of the graph would never have chosen it. Labels fade in and out over
250ms rather than appearing and vanishing, so panning does not strobe. The choice is greedy rather than optimal on purpose: an optimal packing
reshuffles wholesale when one node moves, and a label that jumps to a different node is
worse than a label that is missing.

It is a system rather than something the panel works out as it draws, because the choice
depends on every other label on screen and the fade has to remember what it was last
frame — `LabelSystem` decides, `SideLabel` carries the answer, and the panel only draws
it.

**Dragging runs a live relaxation.** The layout itself has no forces — it is structural
packing, deliberately, so nothing drifts — but a drag wants the graph to give way. So a
drag switches on a spring-and-repulsion simulation *seeded from the packing*: every
containment edge remembers the length the packing gave it, and that becomes its rest
length. The equilibrium of the simulation is the layout it started from.

Only the node under the cursor is moved directly. Its children trail on their springs
and settle; whatever the cluster runs into is pushed aside. The relaxation is
position-based, with no velocity — velocity is what makes a force layout oscillate and
drift, and drift is what the structural layout exists to avoid.

**Releasing does not pin.** The relaxation keeps running until the graph falls quiet, so
a dropped node travels on to a position its neighbours agree with rather than freezing
wherever the cursor left it. A drag that pinned turned every node the user had ever
touched into a fixed point; after a few of those the relaxation had nothing left to move
and the graph became a static picture that stopped reacting to itself. Pinning is still
available, explicitly, on double click -- or from the inspector, which is where a file
pins now that its double click belongs to the desktop.

**If a file does not open, set `open_command`.** `~/.config/rgv/settings.json` takes an
`open_command` — `"kitty -e nvim {}"`, `"code -g {}"` — and a `{}` in it is replaced by
the absolute path; without one the path is appended. It is split on whitespace and run
directly, never through a shell, so a space in a filename is part of the name.

You need it when your desktop's handler for a source file is a **`Terminal=true`**
entry, which is the common case for `nvim`, `helix` and `emacs -nw`. The launcher is
reparented so it outlives the session, which means it has no terminal to run in, and a
terminal editor started that way dies without drawing anything. Nothing we can detect
from here — so the escape hatch is explicit rather than guessed. When the launcher
cannot start at all, that is now reported on stderr rather than silently swallowed.

**Double-clicking a file opens it** with whatever this desktop already opens that kind
of file with: `xdg-open` on Linux, `open` on macOS. Not a viewer of our own. The user
has already chosen their editor, the desktop already knows the association, and a
built-in viewer would be a worse editor than the one they have plus another thing to
keep current with the graph.

The path it will hand over is bounded: it has to exist and to resolve, symlinks and
all, to somewhere inside the repository being watched. A path arrives over the wire from
a provider, which is a separate process speaking a wire format, so it is input rather
than something this program wrote -- and handing a path to a desktop launcher is handing
it a program to run. There is no shell anywhere in the path either, so a space or a
quote in a filename is part of the name and nothing more.

The layered views have no containment to relax, so a drag there stays rigid and the row
structure is not shaken apart.

### The architecture view

Concentric, not a packed tree. Every module and every package is on screen from the
start, and nothing has to take part in a dependency to be drawn. A ring says how far out
a node sits; ordering within a ring is solved by barycentre sweeps so dependency lines
run roughly radially instead of chording across the middle.

**Clicking a node turns the graph to face it.** The selection becomes the centre ring,
and every other node is re-seated by how many hops it is from the question being asked.
Nothing enters or leaves — the node set is identical before and after — and positions
ease, so the graph is seen to turn rather than to be replaced. Let the selection go and
it settles back to the resting reading, where the ring is *reach*: what the most of the
repository is ultimately built on sits in the middle and the consumers end up on the rim.

That is one scalar changing. Same data, same topology, same renderer — only the key that
decides the ring. It is why this is a re-seat and never a filter, which matters because
the view's job is to be watched while an agent edits: anything hidden is a change that
could land unseen.

**Distance is also brightness.** From the selection outward, each hop is a fixed fraction
dimmer than the one inside it — geometric, so the first two or three rings separate
sharply, which is where the question is, and the far field compresses rather than
marching evenly into the background. It is floored, and the floor is the design: the
dimmest thing on screen is still findable, readable and clickable. This says *further
away*, never *not here*.

Two exemptions, and they are the same rule twice: what the agent changed is never dimmed,
and neither is a line on the explained path. A change is what the view exists to report,
and muting it because the reader happens to be looking elsewhere is exactly what the
relevance filter is forbidden to do.

**The unit is the module: a file something builds or packages.** Everything else a
repository holds — tests, fixtures, shaders, documentation — sits under a plain
directory, and the containment tree already says which is which. A package's own
`__init__.py` is never drawn beside it: that file *is* the package.

The owner may be a package or a build target, because the relation is the point and
each language expresses it differently — Python with `__init__.py`, C++ with an
`add_library`. Keying on packages alone would give a C++ checkout its target boxes and
none of the modules that do the work, which is the same thing folding a Python
package's modules away does: on `elevators` the eleven systems that are the
application's functionality collapsed into a single box called `systems`.

**The tree is the import graph.** A module that imports nothing is on the first ring;
each ring outward is code built on the ring inside it; and a node's orbiting children
are the modules that import *it*. So a disc's size is how much is built on it, the same
way a directory's size in the filesystem view is how much it holds. The tree is spanned
breadth first, which is also what makes it a tree at all: an import graph has cycles,
and visiting each node once turns any back edge into a cross-link the layout ignores
rather than a knot it has to resolve.

**Imports, and only imports.** The layout is a tree of them, so a line that is not one
is a line the arrangement cannot account for. An arrow runs from a module to the one it
imports, which is the contract's rule for every dependency edge and means the arrowheads
converge on whatever the most code is built on. Parallel edges between the same pair still
collapse into one line carrying a count.

**The repository is not a module.** It is the centre the layout grows from, and a
dependency never folds onto it. Anything outside every package -- a test suite, a tools
directory -- has it as the nearest drawn ancestor, so without that rule every such
import arrives as a line from the one node that means "here is the middle": on
`elevators` 188 test imports arriving as 17 lines out of 66.

**Cycles are named, not swallowed.** The layout spans its tree breadth first, which
turns a back edge into a cross-link it ignores -- fine as an arrangement, fatal as a
reading, because an entanglement is the thing a reviewer most needs told and the thing
the picture is least able to show. So it is computed instead: every group of modules
that all reach each other is drawn in its own colour, a legend row appears that a
healthy repository never sees, and selecting a member lists the others it is tangled
with. The finding is the whole group, never "the edge that closes the ring" -- which
edge closes it depends on where the walk started, and naming one of three would be an
arbitrary accusation. The altitude decides the question: two packages can be entangled
while none of their files is, and a ring between two modules inside one package is that
package's own business.

One consequence worth knowing: a declared dependency from a manifest is not an import,
so it is not drawn. A repository with `package.json` files and no language provider gets
its packages as nodes with nothing between them. Import-level truth needs a reader per
language, and Python is the only one so far.

### Hiding by pattern

The toolbar's **hide** box takes a regular expression; enter adds it as a chip, and the
chip's `x` removes it. A node whose name or path matches is hidden along with everything
it holds, case-insensitively, and its edges are *gone* -- not moved up to its package,
which is what "hide the tests" has to mean or the package they belong to grows a fan of
edges that used to be theirs. `test_` hides a test suite; `^pkg:demo$` hides one package
and its modules. An invalid pattern stays in the toolbar marked invalid and hides
nothing, so a typo is visible rather than silently ignored.

A hidden node stays hidden when it changes. The relevance filter spares what the agent
touched, because that is a heuristic; a pattern the user typed is a decision.

```sh
./build/bin/rgv --watch ~/code/app --hide 'test_' --hide '^conftest'
```

### Semantic zoom

Nodes are labelled boxes when you are close enough to read them and collapse to small
constant-size dots when you are not. A label smeared across three pixels is worse than
an honest dot, and at monorepo scale text is what kills both the frame rate and the
picture. The dots keep a fixed screen size, so an overview stays a readable
constellation — changed nodes largest, then direct dependents, then transitive, with
unaffected context smallest — rather than fading into nothing.

Hovering fills the gap that leaves: a card fades in after a short dwell with the node's
identity, change state, impact distance, and its reason chain. It is anchored to the
node rather than the cursor, so it does not jitter or sit under what you are aiming at.
Picking uses exactly the same size function the renderer does, with a screen-space
floor, so a collapsed dot stays clickable.

### Architectural specificity

Every real monorepo has a `logger`, a `utils`, a `types` — packages nearly everything
depends on. They wreck a blast radius: change something they sit on and 178 of 240
packages light up, every one of them "impacted", none of it telling you anything.

The fix is borrowed from information retrieval. Inverse document frequency (Spärck
Jones, 1972) says a word in nearly every document discriminates nothing; the same
arithmetic says a package nearly everything depends on explains nothing:

```
specificity(n) = log(N / df(n)) / log(N)        in [0, 1]
```

`df(n)` is the number of distinct dependents, `N` the node count at that level. A hub
scores near 0; a package with one dependent scores 1. An impacted node's **relevance**
is then the weakest specificity along its explanation — one hop through a hub makes the
whole chain unremarkable, because "everything depends on the hub" was already known.

The **relevance** slider in the toolbar filters on that score, and does two things:

- **Hides the hubs themselves.** Above the threshold a low-specificity package leaves
  the view entirely — literal stop-word removal. On `monorepo-ts` at 0.50, `logger`
  (0.06) and `database` (0.33) disappear and the architecture underneath becomes
  legible.
- **Mutes impact that only routes through one.** On the synthetic fixture it takes 178
  impacted packages down to 16; the 162 it mutes are the ones whose only claim to being
  impacted is that they depend on `core`, `util`, or `types`.

```sh
./build/bin/rgv-replay fixtures/large-synthetic --scenario 0 | grep "package impact"
# package impact: 179 node(s) from 1 seed(s); 16 above relevance 0.35
```

**The inverse case matters more.** Specificity says a hub is uninformative *as an
explanation*. It says nothing about importance — and when a hub is the thing that
**changed**, the blast radius is real and enormous. So:

- a seed is never discounted by its own score, and the filter can neither mute nor hide
  what the agent actually touched, nor an impact seed;
- a changed hub raises a **HUB CHANGE** banner naming it, its dependent count, and its
  reach, plus expanding rings on the node itself. `07-hub-change.jsonl` is that case:
  `@acme/logger` moves, 7 of 8 packages are impacted, and every individual result
  scores 0.06 — so the hub is the headline and the list underneath is noise.

This is a frontend-derived heuristic, not part of the contract. It is correct only
while the frontend holds the whole graph at the level being scored — see the open
questions in `docs/frontend-contract.md`.

### Text size

Behind the toolbar's **Text** button, both persisted to
`$XDG_CONFIG_HOME/rgv/settings.json` (or `~/.config/rgv/settings.json`):

| Setting | Controls | Trade-off |
|---|---|---|
| `ui_text_scale` | panels, inspector, event log — and the panels' own size | bigger text, less room for the graph |
| `graph_text_scale` | node labels, and therefore node boxes | bigger text, fewer nodes on screen, labels survive further out before collapsing to dots |

They are separate because they solve different problems: one is legibility, the other
is density. On a large monorepo, big panel text with small graph text is a reasonable
thing to want. A corrupt or hand-edited file falls back to defaults and clamps out-of-
range values rather than producing an unusable UI; a file written before the split
carries a single `text_scale`, which is migrated to both.

They live in their own window rather than on the toolbar for a specific reason: a
control that edits the UI scale, while being scaled by it, resizes and moves under the
cursor as you drag it, which makes hitting a particular value impossible. That window
cancels the global font scale for itself, so its controls stay at exactly the same
size and position while everything behind them updates live. It also carries `-`/`+`
steppers, and ctrl+click on a slider types an exact value.

`frontend/tests/test_ui_layout.cpp` pins this down by running real ImGui frames with
no window and no GPU, asserting the window comes out identical at 1.0x and 2.2x. It
includes a negative control — an uncorrected window, which does change size — so the
assertion cannot pass vacuously.

## Headless tools

`rgv-replay` runs the same store and the same fixture player with the renderer
removed. `--check` validates fixtures and needs no display, so it belongs in CI:

```sh
./build/bin/rgv-replay fixtures/monorepo-ts --check
./build/bin/rgv-replay fixtures/monorepo-ts --scenario 0 --log
```

It verifies that edge endpoints resolve, the containment tree is sound, every impact
path is a real connected chain that reaches a seed, and — the useful one — that each
scenario's **authored** impact result agrees with the traversal the frontend would
perform. A fixture that lies is worse than no fixture.

## Fixtures

`fixtures/monorepo-ts` — an 8-package TypeScript workspace, six scenarios:

| Scenario | What it is for |
|---|---|
| `01-import-redirect` | Spec Appendix B. Package topology moves only after *both* files change, so file edges cannot just mirror package edges. |
| `02-manifest-dependency-added` | A blast radius produced by one `package.json` line, with no source change at all. |
| `03-invalid-intermediate` | The parse fails mid-edit. Old facts must stay on screen marked **stale**, never vanish. |
| `04-rename-reconcile` | A rename arriving as delete + create; a heuristic edge upgraded to exact by a git reconcile. |
| `05-wide-burst` | Every file changes at once — where "changed" stops being a useful signal. |
| `06-new-package` | A package that did not exist at baseline appears mid-session. |
| `07-hub-change` | `@acme/logger` changes. 7 of 8 packages impacted, every result scoring 0.06 — the case the relevance filter must *not* quieten. |

`fixtures/wide-tree` — a component library whose `packages/icons/src` holds 57 files.
The shape real repositories actually have — icon sets, generated clients, migrations —
and the case a radial layout has to handle without the ring swallowing its own
directory.

`fixtures/large-synthetic` — 240 packages / 2880 files / 5.5k edges with a deliberately
power-law degree distribution: three hub packages (`core`, `util`, `types`) that 100–137
packages depend on, sitting on a small shared foundation. Changing the foundation
impacts 178 packages, 162 of them only via a hub. The scale probe, and the case that
makes the relevance filter worth having.

Editing a scenario and pressing **Reload** in the app re-reads it from disk.

## Architecture

```
IGraphSource ──poll──▶ GraphStore ──dirty set──▶ World (EnTT) ──▶ GL renderer
  fixture │ live         single writer            components          3 draw calls
                         of graph state           + 19 systems        + ImGui panels
```

An entity-component-system: resources are the state there is exactly one of, components
are per-entity data with a single owning system each, and systems are all the behaviour.
Six phases order the frame — `rgv --schedule` prints it. `main.cpp` is the world, the
schedule, and the loop; adding a capability is adding a system, and attaching live data
is constructing a different `IGraphSource`.

**[docs/architecture.md](docs/architecture.md)** has the design in full: the schedule,
the rules it enforces and why, layout and rendering, and how the pipeline is tested.

## What is not here yet

Two languages. `rgv-watch` extracts imports for Python and TypeScript/JavaScript, and
symbols for Python only, so a C++ or Go checkout gives you a live Filesystem view and an
Architecture view holding whatever its manifests declare, with nothing between the
nodes. `tsconfig.json` path aliases and `package.json` `exports` maps are not read, so a
repository leaning on either will lose the imports that go through them. No git
reconciliation in the live path either: a rename arrives as a delete and a create.

The temporal-compare view (FR-34) is represented in the contract (`valid_to` on edges) but
has no view mode.

Two gaps worth knowing before building on this:

- **The fixture generators are not committed.** The JSON is the artefact; the scripts
  that produced it were throwaway. Regenerating a fixture means rewriting the generator.
  Deliberate — a fixture you cannot diff is a fixture you cannot trust — but it makes
  large edits to `large-synthetic` more expensive than they look.
- **Architectural specificity assumes the whole graph is present.** It is derived
  frontend-side from in-degree, so if the backend ever streams a subgraph the scores
  skew silently: a hub looks specific because most of its dependents were not sent.

Open questions on the contract itself are at the end of `docs/frontend-contract.md`.
