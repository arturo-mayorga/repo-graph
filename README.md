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

Three suites run: the contract and scene tests, fixture validation, and the UI layout
tests. None of them need a display.

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
| drag a node | pin it where you put it |
| double click | pin / unpin |
| `F` | fit to view |
| `space` / `.` / `R` | play-pause / step one event / restart |
| `Esc` | clear selection |

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

The **relevance** slider in the toolbar filters on that score. On the synthetic fixture
it takes 178 impacted packages down to 16, and the 162 it mutes are exactly the ones
whose only claim to being impacted is that they depend on `core`, `util`, or `types`.

```sh
./build/bin/rgv-replay fixtures/large-synthetic --scenario 0 | grep "package impact"
# package impact: 179 node(s) from 1 seed(s); 16 above relevance 0.35
```

**The inverse case matters more.** Specificity says a hub is uninformative *as an
explanation*. It says nothing about importance — and when a hub is the thing that
**changed**, the blast radius is real and enormous. So:

- a seed is never discounted by its own score, and the relevance filter can never mute
  what the agent actually touched;
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

`fixtures/large-synthetic` — 240 packages / 2880 files / 5.5k edges with a deliberately
power-law degree distribution: three hub packages (`core`, `util`, `types`) that 100–137
packages depend on, sitting on a small shared foundation. Changing the foundation
impacts 178 packages, 162 of them only via a hub. The scale probe, and the case that
makes the relevance filter worth having.

Editing a scenario and pressing **Reload** in the app re-reads it from disk.

## Architecture

```
IGraphSource ──poll──▶ GraphStore ──dirty set──▶ Scene (EnTT) ──▶ GL renderer
  fixture │ live         single writer            components         3 draw calls
                         of graph state           + systems          + ImGui panels
```

`GraphStore` is the only writer of graph state and exposes **dirty sets**, so ECS sync
and layout touch only what moved — this is what satisfies the spec's "a file save must
not trigger a full global layout".

Layout is layered, not force-directed: dependency depth fixes the row, barycentre
sweeps order within it. A spring simulation was tried first and produced a hairball
that never stopped drifting. Rows run so a package depending on nothing sits at the
bottom and its dependents stack above — impact rises, the way the spec draws it.

Text is drawn through ImGui's draw list rather than the GL renderer: a glyph atlas is a
subsystem, and ImGui already ships one. Nodes, edges, and arrowheads are three
instanced draw calls with rounded-rectangle SDFs in the fragment shader.

Graph labels are drawn in world space, so they scale with zoom and can never overflow
the box that was sized to hold them. That single fact — the on-screen size of a label —
is what drives semantic zoom, and it is why the layout footprint a node reserves is
kept separate from the size it is drawn at: zooming must never reflow the graph.

## What is not here yet

No backend. No live source. No symbol level. The temporal-compare view (FR-34) is
represented in the contract (`valid_to` on edges) but has no dedicated view mode.
Open questions on the contract itself are listed at the end of
`docs/frontend-contract.md`.
