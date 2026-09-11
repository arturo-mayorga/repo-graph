# Architecture

The frontend is an entity-component-system, taken seriously. This document is the
design; `README.md` is how to run it and `docs/frontend-contract.md` is what it
consumes.

An entity-component-system, taken seriously. Three kinds of thing, and nothing else:

**Entities** — two archetypes sharing one registry. A node carries `NodeRef`; an edge
carries `EdgeRef` and `Endpoints`. Those are the tags, and systems name them explicitly
in their queries rather than relying on which components an archetype happens to lack.

**Components** — per-entity data, iterated in bulk. Every one names the single system
that owns its value, and no other system writes it. `SceneSyncSystem` creates entities
and so attaches whole archetypes, but constructing is not owning.

**Resources** — the state there is exactly one of: the graph store, the camera, the
filters, the selection. Held in the registry's context, so a system's dependencies are
visible in its body instead of reached through an owner object.

**Systems** — all the behaviour, all with the same shape: given the world and the
frame, read and write components and resources. Six phases order the frame; insertion
order orders within a phase. `rgv --schedule` prints it:

```
Input/WindowSystem          the only system that touches the OS
Input/PickingSystem         resolves what the pointer is over, once
Input/NavigationSystem      pan, zoom, drag, framing
Input/TransportSystem       play / step / restart, if the source has a timeline
Ingest/SourceSystem         <- the seam: fixture player today, watcher or socket later
Sync/CommandSystem          drains the command queue
Sync/SpecificitySystem      IDF over in-degree, plus hub alerts
Sync/SceneSyncSystem        store deltas -> entities. The only creator/destroyer.
Simulate/ImpactStateSystem  Changed / Impacted / HubSeed
Simulate/SelectionSystem    Selected / Hovered / OnExplainedPath
Simulate/LayoutSystem       depth, row ordering, easing
Simulate/StyleSystem        derives Style. The renderer reads it verbatim.
Render/UiSystem             panels first: they decide how much room the graph gets
Render/GraphRenderSystem    three instanced draw calls
Render/OverlaySystem        labels, legend, hover card
Present/PresentSystem       swap
```

`main.cpp` is the world, the schedule, and the loop. Adding a capability is adding a
system; attaching live data is constructing a different `IGraphSource`. Neither touches
that file.

### Rules the design enforces

**One writer per component.** `Style` used to be written by the styling pass and then
overridden again at draw time — two places computing the same thing, the second
silently winning. Now `StyleSystem` is the only writer and the renderer is dumb.

**No duplicated state.** Selection lives in one resource; `Selected`, `Hovered`, and
`OnExplainedPath` are derived from it by one system. Previously the id and the
component were maintained side by side at three call sites, and one of them — selecting
from the inspector — set the id but not the component, so the canvas showed no outline.
That bug is now unrepresentable, and `test_scene.cpp` holds the line.

**Panels never mutate.** They read resources and push commands. `CommandSystem` is the
single place anything is applied, so there is one order in which things happen.

**Nothing derived is cached without a reason.** Most Simulate systems run every frame
as linear passes with no allocation after warm-up. Only `SpecificitySystem`, the one
that is genuinely superlinear in the graph, is gated on its inputs changing.

### Layout and rendering

Layout has three strategies. The architecture view is force-directed over containment: a
package is a node linked to what it holds, containment attracts, everything repels
(siblings gently, strangers harder), and each node is held to its dependency rank's row
so the picture has a reading direction. Position-based with no velocity, seeded by id,
cooled, and stopped when quiet -- `LayoutSystem::force_place` settles it off screen and
the scene eases in. A non-drag relaxation is gentle and moves only newly seated nodes,
which is what keeps a save from reshuffling the picture.

What that view draws is decided in `SceneSyncSystem`. It opens at package level and a
package's modules appear only when it is expanded (FR-31), because the file-level graph
is past Euler's planarity bound and cannot be drawn without crossings by any layout.
Edges are drawn between whatever stands for their endpoints on screen, the most specific
edge per pair with the rest collapsed into its weight, filtered to one relation at a
time, and a package edge its modules already explain is not drawn at all --
`choose_drawn_edges` is where all of that is decided. The other two are radial. The filesystem view is a Gource-inspired
tree: discs sized by file count, files on the rim, children packed into shells inside
their parent's wedge. The dependency views are concentric: the ring is **reach** — how
much of the repository transitively depends on a node — so the core sits in the middle
and consumers end up on the rim, and angle is ordered by circular barycentre so
dependency lines run roughly radially instead of chording across the middle.

Reach, not direct dependents, and not depth. A package imported by one adapter that half
the repository sits behind has a direct count of 1, and putting it on the rim would exile
the actual core. Depth is still computed and still cycle-safe, but it no longer decides
position. Reach is heavily skewed — a small core, a wide middle, a rim of leaves — so the
buckets it produces are compacted before placement; without that, a graph scoring 63, 62,
5, 0 lands on rings 0, 0, 4, 6 and the empty rings in between are just a moat. Ring count
is bounded by population as well as by the spread of reach, because eight packages over
seven rings puts one node on each, and a ring of one is a point on a line: the whole graph
comes out as a single radial spoke.

Neither layout relaxes into place; both are computed. A spring simulation was tried first
for the dependency views and produced a hairball that never stopped drifting, and Gource's
own force-based spreading was replaced with structural packing for the same reason.

Node size means the same thing in every view: how much depends on this. The radial tree
says it with a disc radius, the dependency views with a `Prominence` multiplier on the
golden ratio (1, φ, φ²) applied to the footprint and to the label inside it. Without it a
box view sizes a node by the length of its name, so a package six others import is drawn
like one nothing imports. It scales up only — shrinking below what the text needs would
trade legibility for a distinction colour and layout already carry — and it is off in the
filesystem view, whose discs already carry it.

Layout runs in full only when the user asks for it — the `Re-layout` button, a view-mode
change, or a fresh source. Everything else is a delta. A filter moving is a `revisit`: the
visibility predicate is re-tested against every node and only the difference is applied,
so entities that stay visible keep their positions. A node that arrives is tagged
`Unplaced`, and layout seats it on the ring its reach earns at the circular mean of the
neighbours already on screen, then leaves it to the relaxation. A node that leaves moves
nothing at all.

This matters because the controls that change visibility are *dragged*. Every filter used
to set `rebuild`, which clears the registry and reseeds every position from scratch, so
the relevance slider tore the graph down and rebuilt it on every frame it moved. Even a
plain relayout is too much: ring membership and radii are global, so one node appearing
moves every other node. Resizing text is the same shape of problem and gets the same
answer — `resettle` wakes the relaxation to push newly-overlapping neighbours apart rather
than rearranging anything.

A drag switches on a live relaxation and releasing does not switch it off: it runs until
the graph is quiet, so a dropped node travels somewhere that belongs instead of freezing
under the cursor. Dropping never pins. The two layouts relax differently because they mean
different things. A containment tree has no privileged direction, so the radial tree
relaxes freely in both axes, springs along containment holding the distances the packing
chose. The concentric layout does have one — the ring *is* the reach reading — so
`relax_rings` springs the radius home and leaves the angle alone: neighbours slide apart
along the arc to reopen the gap a drop closed, and the angle the user chose is kept,
because springing that home too would simply undo the drag.

Text is drawn through ImGui's draw list rather than the GL renderer: a glyph atlas is a
subsystem, and ImGui already ships one. Nodes, edges, and arrowheads are three
instanced draw calls with rounded-rectangle SDFs in the fragment shader.

Graph labels are drawn in world space, so they scale with zoom and can never overflow
the box that was sized to hold them. That single fact — the on-screen size of a label —
drives semantic zoom, and it is why the layout footprint a node reserves is kept
separate from the size it is drawn at: zooming must never reflow the graph.

### Testing the pipeline, not a stand-in

`tests/Harness.h` builds a real world and a real schedule with everything except the
platform, the GPU, and the data source. Tests tick it and assert on components, so they
exercise the systems that ship. `test_schedule.cpp` covers the ECS itself — phase
ordering, resource identity, teardown order — and `test_ui_layout.cpp` drives real
ImGui frames with no window at all.
