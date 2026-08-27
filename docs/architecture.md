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

Layout is layered, not force-directed: dependency depth fixes the row, barycentre
sweeps order within it. A spring simulation was tried first and produced a hairball
that never stopped drifting. Rows run so a package depending on nothing sits at the
bottom and its dependents stack above — impact rises, the way the spec draws it.

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
