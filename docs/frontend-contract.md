# Frontend Data Contract v1

**Status:** Draft v1 · **Scope:** the interface between the Live Repository Impact Graph
backend and the native C++/OpenGL frontend (`rgv`, *repo graph view*).

This document defines *everything the frontend is allowed to know*. The frontend has no
access to Git, Watchman, Tree-sitter, DuckDB, or any adapter. It consumes a baseline
**snapshot** plus an ordered stream of **events**, and it renders the resulting state.

The same contract is served by two implementations:

| Implementation   | Purpose                            | Availability |
|------------------|------------------------------------|--------------|
| `FixtureSource`  | Replays authored JSON scenarios    | Now — drives all UX work |
| `LiveSource`     | Reads NDJSON from a provider process | Now — see §6 |

> **Rule:** no frontend code may branch on which source is attached. If the UX needs a
> capability, it becomes part of this contract and the fixture player grows to satisfy it.

---

## 1. Design constraints from the spec

These properties of the product force specific contract choices. They are not negotiable
by convenience.

| Spec requirement | Contract consequence |
|---|---|
| FR-20/22 versioned facts, provenance | Every edge carries `provider`, `confidence`, `freshness`, `valid_from`. Edges are never silently overwritten. |
| FR-19 partial failure handling | `freshness: "stale"` is a first-class render state. A removed edge and a stale edge look different. |
| FR-26 explainability | `ImpactResult` carries explicit paths as **edge id lists**, not just node sets. |
| FR-30/31 abstraction switch + semantic zoom | Nodes form a containment tree via `parent`. The frontend collapses/expands locally; it never re-queries to change level. |
| NFR-01 interactive updates | Events are deltas, not full snapshots. A file save must not reserialize the graph. |
| NFR-02 semantic latency | `adapter.status` lets the UI show *pending* work without blocking the fast path. |
| §11.2 "a file save must not trigger a full global layout" | Events name exactly which nodes/edges changed so layout can be locally re-solved. |

---

## 2. Vocabulary

All identifiers are opaque UTF-8 strings, stable for the lifetime of a session. The
frontend must treat them as keys, never parse them. Fixtures use a readable
`kind:path` convention purely for human authoring.

### 2.1 Node kinds — `NodeKind`

`repository` · `workspace` · `package` · `build_target` · `directory` · `file` ·
`symbol` · `external_package` · `agent_session`

### 2.2 Edge kinds — `EdgeKind`

| Kind | Traversal role |
|---|---|
| `contains` | structural — projection/navigation only |
| `owns` | structural — projection/navigation only |
| `depends_on` | **dependency** — primary blast-radius edge |
| `imports` | **dependency** — primary file-level blast-radius edge |
| `defines` | structural |
| `references` | **dependency** — semantic, provider-dependent |
| `calls` | **dependency** — semantic, provider-dependent |
| `inherits` | **dependency** — semantic, provider-dependent |
| `generated_from` | **dependency** — optional |

Structural edges render as containment/hierarchy, never as arrows in the impact view.

### 2.3 Freshness — `Freshness`

| Value | Meaning | Frontend obligation |
|---|---|---|
| `current` | reflects the working tree as of `generation` | render normally |
| `stale` | last-known-good; a re-parse/re-index has not yet confirmed it | render de-emphasized + explicit marker (NFR-04: never present as current) |
| `pending` | work is queued; no trustworthy value yet | render as in-flight |
| `invalid` | provider failed on this artifact | render as error state |

### 2.4 Confidence — `Confidence`

`exact` · `high` · `heuristic` · `unresolved`

`heuristic` and `unresolved` edges must be visually distinguishable and must be
excludable by a filter. They are excluded from traversal by default (spec §10.2).

---

## 3. Snapshot

Delivered once at attach time. Establishes the baseline the whole session is measured
against.

```jsonc
{
  "schema": "rgv.snapshot/1",
  "repo": {
    "root": "/home/dev/acme-monorepo",
    "name": "acme-monorepo",
    "head": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0",
    "branch": "main",
    "detached": false
  },
  "session": {
    "id": "sess-01",
    "name": "agent session 01",
    "baseline_generation": 100,
    "started_at_ms": 0
  },
  "generation": 100,
  "nodes": [ /* Node */ ],
  "edges": [ /* Edge */ ]
}
```

### 3.1 `Node`

```jsonc
{
  "id": "pkg:auth",
  "kind": "package",
  "name": "auth",                  // display label
  "path": "packages/auth",         // repo-relative; "" for synthetic nodes
  "parent": "repo:root",           // containment parent, or null for the root
  "language": "typescript",        // optional
  "freshness": "current",
  "attrs": { "version": "1.4.0" }  // optional, string->string, rendered in inspector
}
```

`parent` defines the containment tree used for collapse/expand and for projecting a node
to its owning package (FR-11). It is a *tree*, so exactly one parent. Multi-owner cases
(FR-11 allows several) are expressed as additional `owns` edges, not as extra parents.

### 3.2 `Edge`

```jsonc
{
  "id": "e:api->auth",
  "kind": "depends_on",
  "from": "pkg:api",               // dependent
  "to": "pkg:auth",                // dependency
  "provider": "npm",               // git|watchman|tree-sitter|scip|lsp:<n>|npm|python-imports|bazel|...
  "provider_version": "10.8.2",
  "confidence": "exact",
  "freshness": "current",
  "valid_from": 100,
  "valid_to": null,                // non-null => historical, retained for temporal compare
  "evidence": {                    // optional, shown in the provenance inspector
    "artifact": "packages/api/package.json",
    "line": 14,
    "snippet": "\"@acme/auth\": \"workspace:*\""
  }
}
```

Direction is always **dependent → dependency**. Blast radius traverses these edges
*in reverse*.

---

## 4. Events

An ordered stream. Each event carries the `generation` it produces. The frontend applies
events in order and must tolerate gaps only by requesting a resync (`source.resync`).

| Type | Purpose |
|---|---|
| `session.started` | session identity + baseline generation |
| `file.changed` | T0 change truth — a path changed in the working tree |
| `graph.updated` | node/edge add/remove/update delta |
| `impact.updated` | recomputed blast radius |
| `adapter.status` | provider lifecycle + queue depth |
| `reconcile.checkpoint` | this generation is Git-authoritative (spec T3) |

Common envelope:

```jsonc
{ "t_ms": 420, "type": "graph.updated", "generation": 102, /* type-specific fields */ }
```

`t_ms` is **fixture-only**: it is the scheduled offset from scenario start and is what
makes a scenario a reproducible timeline. `LiveSource` sets it to wall-clock arrival and
the frontend must not depend on it for correctness — only for the scrubber.

### 4.1 `file.changed`

```jsonc
{
  "t_ms": 0, "type": "file.changed", "generation": 101,
  "path": "packages/auth/src/token.ts",
  "node_id": "file:packages/auth/src/token.ts",
  "change": "modified",            // created|modified|deleted|renamed
  "from_path": null,               // set when change == "renamed"
  "processing": "pending"          // pending|structural|semantic|settled
}
```

`processing` drives the per-file progress affordance. It is the frontend's only signal
that work is still in flight for a specific file.

### 4.2 `graph.updated`

```jsonc
{
  "t_ms": 180, "type": "graph.updated", "generation": 102,
  "added_nodes":   [ /* Node */ ],
  "removed_nodes": [ "file:..." ],
  "updated_nodes": [ /* Node — full replacement by id */ ],
  "added_edges":   [ /* Edge */ ],
  "removed_edges": [ "e:auth->database" ],
  "updated_edges": [ /* Edge — full replacement by id */ ],
  "note": "token.ts import redirected"   // optional, surfaced in the event log
}
```

Removal semantics matter (FR-19). A relationship that *disappeared from the source* is a
`removed_edges` entry. A relationship whose evidence merely went stale is an
`updated_edges` entry with `freshness: "stale"`. The frontend renders these differently:
removed edges animate out and appear in temporal-compare as deletions; stale edges remain
on screen, de-emphasized.

### 4.3 `impact.updated`

Mirrors spec §10.3.

```jsonc
{
  "t_ms": 420, "type": "impact.updated", "generation": 102,
  "level": "package",              // package|build_target|file|symbol
  "baseline_generation": 100,
  "seed_nodes": [ "pkg:auth" ],
  "filters": { "max_depth": 8, "edge_kinds": ["depends_on"], "include_heuristic": false },
  "impacted_nodes": [
    {
      "node_id": "pkg:api",
      "min_distance": 1,
      "direct": true,
      "changed": false,
      "freshness": "current",
      "cause": "implementation",   // implementation|dependency_added|dependency_removed
      "paths": [ { "edges": ["e:api->auth"] } ]
    }
  ]
}
```

A path is an **ordered list of edge ids** from the impacted node toward a seed. The
frontend resolves edge ids to endpoints itself, so a path renders as a highlighted chain
without any extra round-trip. `cause` implements FR-29.

`impact.updated` fully replaces the previous impact result for its `level`. Results for
different levels coexist, so switching abstraction (FR-30) is instant and lossless.

### 4.4 `adapter.status`

```jsonc
{
  "t_ms": 60, "type": "adapter.status", "generation": 101,
  "adapter": "tree-sitter",
  "state": "running",              // idle|running|degraded|failed
  "queue_depth": 3,
  "last_success_generation": 100,
  "message": "parsing 3 files"
}
```

### 4.5 `reconcile.checkpoint`

```jsonc
{
  "t_ms": 5200, "type": "reconcile.checkpoint", "generation": 105,
  "corrections": 2,
  "message": "git reconcile: 1 missed delete, 1 rename remapped"
}
```

---

## 5. The C++ interface

```cpp
namespace rgv {

// A source pushes events into a sink. Sources never own frontend state.
struct EventSink {
    virtual ~EventSink() = default;
    virtual void on_event(const Event& e) = 0;
};

struct SourceStatus {
    bool        attached      = false;
    bool        ended         = false;   // fixture reached end of scenario
    Generation  generation    = 0;
    double      position_ms   = 0.0;
    double      duration_ms   = 0.0;     // 0 for live sources
    std::string description;
};

// Timeline is non-null only for replayable sources. Its presence is what tells the
// UI to draw a scrubber; nothing else in the frontend distinguishes fixture from live.
struct Timeline {
    virtual ~Timeline() = default;
    virtual void   play()               = 0;
    virtual void   pause()              = 0;
    virtual bool   playing() const      = 0;
    virtual void   set_rate(double)     = 0;   // 0.25x .. 8x
    virtual double rate() const         = 0;
    virtual void   seek_ms(double)      = 0;   // rewinds + re-applies from baseline
    virtual void   step_event()         = 0;   // advance exactly one event
    virtual void   restart()            = 0;
};

class IGraphSource {
public:
    virtual ~IGraphSource() = default;

    // Baseline graph. Valid from attach until detach; never mutated by events.
    virtual const Snapshot& baseline() const = 0;

    // Drain all events that have become due. Called once per frame with the frame delta.
    // Must be non-blocking. Returns the number of events emitted.
    virtual int poll(double dt_seconds, EventSink& sink) = 0;

    virtual SourceStatus status() const = 0;

    // Non-null => scrubbable. Live sources return nullptr.
    virtual Timeline* timeline() { return nullptr; }
};

} // namespace rgv
```

### 5.1 Frame loop contract

```
per frame:
    source.poll(dt, store)          // store applies events, marks dirty sets
    store.drain_dirty(ecs_sync)     // create/destroy/patch entities for changed ids only
    systems: layout -> impact style -> picking -> camera -> render -> ui
    store.clear_dirty()
```

`GraphStore` is the single writer of graph state and exposes **dirty sets**
(`dirty_nodes`, `dirty_edges`, `impact_dirty`, `topology_dirty`) so ECS sync and layout
touch only what moved. This is what satisfies "a file save must not trigger a full global
layout".

---

## 6. The live transport

`LiveSource` attaches to a **provider process**: a child process that writes the contract
to its stdout as newline-delimited JSON, one message per line, UTF-8, no trailing commas.
The frontend spawns it, reads it non-blockingly, and never writes to it except to close
its stdin on shutdown.

A local subprocess rather than a WebSocket, deliberately. This is a single-user tool
watching a checkout on the same machine; a socket server would add a dependency, a port,
and a lifetime to manage in exchange for a capability nothing currently asks for. The
framing below is transport-agnostic, so moving to a socket later changes how bytes are
delivered and nothing about what they mean.

### 6.1 Framing

The first line is the baseline:

```jsonc
{ "type": "snapshot", "snapshot": { /* §3 */ } }
```

Every subsequent line is one event in the `§4` envelope:

```jsonc
{ "t_ms": 0, "type": "file.changed", "generation": 101, /* ... */ }
```

Rules:

- **One message per line.** A message must not contain a raw newline. Readers split on
  `\n` and parse each line independently, so a partial line at the end of a read buffer
  is held until its terminator arrives.
- **`t_ms` is ignored.** It is scheduling information for a replayable source. A live
  provider should emit `0`; a frontend must not delay a live event by it.
- **Ordering is the stream order.** There is no reordering buffer and no sequence number
  beyond `generation`.
- **`generation` is monotonic.** A provider that cannot guarantee that must emit
  `source.resync` (see below) rather than going backwards.
- **stderr is diagnostics**, never protocol. The frontend may surface it, and must not
  parse it.
- **Exit is end-of-stream.** `SourceStatus::ended` goes true; the graph is retained and
  clearly marked as no longer live. A provider crash is not a frontend crash.

### 6.2 Provider identity

A provider announces itself with `adapter.status` before or with its first graph event,
so the inspector can attribute edges and the UI can show what is running:

```jsonc
{ "t_ms": 0, "type": "adapter.status", "generation": 100,
  "adapter": "filesystem", "state": "ready", "queue_depth": 0 }
```

The `provider` field on every edge (§3.2) must match an announced `adapter`. This is what
lets several providers feed one stream — a filesystem walker and a language extractor —
with different freshness and confidence per edge, and lets the UI say which one is behind.

### 6.3 Mapping to a remote API

The contract stays isomorphic to spec §12.2, so a socket or HTTP transport is a reframing
with no translation logic:

| Contract element | Remote equivalent |
|---|---|
| first `snapshot` line | `GET /repo` + `GET /graph?generation=<baseline>` |
| subsequent lines | `WS /events` |
| `impact.updated` | `GET /impact` (also pushed) |
| `Edge.evidence` | `GET /explain/edge/:id` (fixtures inline it; live may lazy-load) |
| `reconcile.checkpoint` | `POST /reconcile` response echoed onto the stream |

The one intentional difference: fixtures inline `evidence` on every edge, while a live
source may omit it. The frontend therefore treats `evidence` as optional and shows a
loading state in the inspector when absent.

---

### 6.4 Language providers

A language provider contributes file-level `imports` edges and, because nothing else in
the live path computes it, the blast radius those edges imply. Concretely, for the files
it understands it must:

- Emit one `imports` edge per resolved import, `from` the importing file `to` the file
  it resolves to, with `evidence` pointing at the import statement. `confidence` is
  `exact` when the resolution is unambiguous and `heuristic` when more than one source
  root could satisfy it.
- Emit **no edge** for an import it cannot resolve to a file in the repository. Standard
  library and third-party imports fall out here. An `unresolved` edge to a node that does
  not exist is worse than a missing one, and mapping a module name to a distribution name
  (`yaml` → `PyYAML`) is a different problem with its own provider.
- Emit a `package` node for every unit of the language's own packaging (a Python
  package is a directory with `__init__.py`), nested under whatever contains it and
  replacing the `directory` node at its path, and aggregate the `imports` that cross
  package boundaries into `depends_on` edges between those packages, with the first
  crossing import as `evidence`. A manifest names a distribution; the architecture of
  the code is the packages inside it, and the Architecture view draws `package` nodes.
- Re-parse a file when it changes and report the difference as `graph.updated`
  `added_edges` / `removed_edges` — never as a full replacement. A surviving edge whose
  evidence or confidence changed is an `updated_edges` entry.
- Set `attrs.module_file` on a `package` node to the repo-relative file that *is* that
  package, when the language has one (`__init__.py` for Python). The frontend folds a
  package's modules into it at overview and drops dependency edges that only restate
  containment; this attribute is how it keeps the one that does not, a real import of
  the package as a unit.
- Emit a `symbol` node for every top-level definition that another file uses, parented
  to the file that defines it, with `attrs.kind` of `class` or `function`. A file that
  constructs or invokes a symbol has a `calls` edge to it; a file that mentions it in
  any other way has a `references` edge. Both carry the first such use as `evidence`.
  Unused-elsewhere definitions are not nodes: the file stands for them.
- Emit `impact.updated` for the `symbol` level, seeded at the symbols the changed files
  define and traversed over `references` and `calls`. The result mixes kinds -- seeds
  are symbols, hits are files -- and the frontend admits both at that level.
- Emit `impact.updated` for the `file` level (over `imports`) and the `package` level
  (over `depends_on`, seeded by the packages that own the changed files) whenever the
  changed-file set or the dependency edges move. The frontend does not derive impact on
  its own from a live stream; a provider that emits edges without impact produces a graph
  that never lights up.

## 7. Open contract questions

1. **Symbol-level volume.** Symbol nodes (spec Phase 2) could be 10^5–10^6 per repo.
   Should they stream lazily on expand rather than arriving in the snapshot? Current
   contract assumes lazy, but there is no `graph.request_expand` message yet.
2. **Multi-level impact.** `impact.updated` is per-level. Should the backend push all
   levels eagerly, or only the level the UI is displaying? Eager is assumed.
3. **Historical edges.** `valid_to` retains superseded edges for temporal compare.
   Unbounded retention is a memory risk; a retention window belongs in the contract.
4. **Where specificity is computed.** The frontend derives architectural specificity
   (an IDF over in-degree) from the graph it holds, to rank and filter impact — see the
   README. That is only correct while the frontend holds the *whole* graph at the level
   being scored. If the backend ever streams a subgraph, in-degree becomes a local
   sample and the scores silently skew: a hub looks specific because most of its
   dependents were not sent. Either the backend computes and ships `specificity` per
   node, or the contract has to guarantee complete in-edge counts per level.
5. **Resync.** §4 says the frontend may "request a resync (`source.resync`)", but with a
   one-way stream it has no way to ask. Either the provider pushes a fresh `snapshot`
   line when it detects it has fallen out of sync, or the transport needs a back channel.
   Pushed-snapshot is assumed; the frontend must therefore accept a `snapshot` line at
   any point, not only first.
6. **Path count.** `paths[]` is unbounded. Real graphs can have thousands of paths to one
   node. A `paths_truncated: true` flag plus a cap is probably needed.
