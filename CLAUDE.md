# repo-graph

Native C++20 / OpenGL frontend for the Live Repository Impact Graph.

| Where | What |
|---|---|
| `live_repository_impact_graph_spec_v0.1.md` | product spec — the FR/NFR numbers cited in comments |
| `docs/frontend-contract.md` | the backend↔frontend interface. Change it before changing code |
| `docs/architecture.md` | the ECS design and the rules it enforces |
| `README.md` | build, run, test, fixtures |

## Do

- Use ECS design discipline.
- Implement features using Red/Green TDD.
- Run `ctest --test-dir build` before claiming anything works.
- Encode UX behaviour worth arguing about as a fixture scenario, not a screenshot.
- Verify GUI changes by actually looking at them.

## Don't

- Don't add bloat to this file. Detail belongs in `README.md` or `docs/`.
- Don't let two systems write the same component. One owner, named in `Components.h`.
- Don't mutate state from a panel. Panels read resources and push commands.
- Don't render stale evidence as current (NFR-04), or let the relevance filter mute
  something the agent changed. Both have tests; both are the product's credibility.
- Don't rebuild the scene or re-lay-out the graph because a filter moved. Filters are
  dragged; place what arrives, leave the rest where it is.
- Don't `pkill -f` a pattern that also matches the invoking shell's command line.
