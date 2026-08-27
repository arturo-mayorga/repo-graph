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

`--scenario N --at MS --select NODE --hover NODE --text-settings` reproduce an exact
on-screen state,
which is what makes a screenshot or a bug report about the UX worth anything. Live
input always wins over `--hover`, so pointing at something else just works.

| Input | Action |
|---|---|
| drag / wheel | pan / zoom |
| hover a node | fades in a card: what it is, what changed, why it is impacted |
| drag a node | move it; it settles back into place on release |
| double click | pin / unpin in place |
| `F` | fit to view |
| `space` / `.` / `R` | play-pause / step one event / restart |
| `Esc` | clear selection |

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

Node sizes step by the **golden ratio**: a file, a directory, and the largest a
directory grows to are `r`, `r·φ`, `r·φ²`. Three sizes on one geometric scale read as a
family — clearly different, obviously related — where an arbitrary ratio reads either as
two unrelated shapes or as no difference at all.

A directory's **drawn size** and the **orbit its files sit on** are separate. Conflating
them puts every file dot exactly on its directory's edge, half-occluding it, and makes
the size ratio between a file and a directory the ratio of a dot to a whole orbit.

Children — files and subtrees alike — are packed onto **successive orbits** rather than
one ring, which is what Gource does and for the same reason. A single orbit seating 57
icons needs a radius of 160 around a disc of radius 16: a vast empty annulus with most
of the screen wasted. Filling orbits outward keeps a wide directory compact.

Gource pushes nodes apart with a force simulation. This does it by construction: every
subtree is laid out in its own frame first, so its enclosing radius is exact rather than
estimated, and a parent packs those subtrees as rigid discs. Nothing overlaps, nothing
settles, and the same repository always draws identically.

Where a parent's children sit decides whether it reads as a flower or a comet. They go
on one ring when that ring is a sane size relative to the children, and spill into
further shells when it is not. Starting as tight as possible seats a few and flings the
rest into a distant second shell; starting wide enough to seat all of them degenerates
into an annulus with a void in the middle once there are hundreds.

Impact colouring still wins over the extension palette. The repository looks like
Gource; the blast radius lights up on top of it.

**Zooming in morphs circles into labelled boxes**, with the name rendered inside the
box — shrunk to fit it, so a half-morphed node never draws a rectangle with its name
floating outside. The directory bloom fades out as the node becomes a box, where a halo
reads as a second misaligned rectangle rather than a glow. Boxes appear only where there
is room for one. That caveat is not a shortcut — dense radial packing and full text boxes are in
direct conflict. Files on an orbit sit ~18 world units apart while a filename box is
~120 wide, so letting every node grow to its label produces an unreadable stack. The
morph is gated on room × zoom: the repository and its packages become proper boxes with
the name inside; files and cramped directories stay circles and are named from outside.

File names appear as you zoom in. That works because labels drawn *beside* a node hold
a constant screen size while the graph spreads out beneath them — world-scaled text
grows in step with the spacing and never uncrowds, however far you zoom. A label
*inside* a box scales with the box, so it always fits. Gource makes the same split.

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
available, explicitly, on double click.

The layered views have no containment to relax, so a drag there stays rigid and the row
structure is not shaken apart.

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
                         of graph state           + 16 systems        + ImGui panels
```

An entity-component-system: resources are the state there is exactly one of, components
are per-entity data with a single owning system each, and systems are all the behaviour.
Six phases order the frame — `rgv --schedule` prints it. `main.cpp` is the world, the
schedule, and the loop; adding a capability is adding a system, and attaching live data
is constructing a different `IGraphSource`.

**[docs/architecture.md](docs/architecture.md)** has the design in full: the schedule,
the rules it enforces and why, layout and rendering, and how the pipeline is tested.

## What is not here yet

No backend, no live source, no symbol level. The temporal-compare view (FR-34) is
represented in the contract (`valid_to` on edges) but has no view mode.

Two gaps worth knowing before building on this:

- **The fixture generators are not committed.** The JSON is the artefact; the scripts
  that produced it were throwaway. Regenerating a fixture means rewriting the generator.
  Deliberate — a fixture you cannot diff is a fixture you cannot trust — but it makes
  large edits to `large-synthetic` more expensive than they look.
- **Architectural specificity assumes the whole graph is present.** It is derived
  frontend-side from in-degree, so if the backend ever streams a subgraph the scores
  skew silently: a hub looks specific because most of its dependents were not sent.

Open questions on the contract itself are at the end of `docs/frontend-contract.md`.
