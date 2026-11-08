# docs

| File | Answers |
|---|---|
| [design.md](design.md) | How hitchtrace works: frames, per-task state, hooks, causes, chains, stall groups, the record and its JSON |
| [evaluation.md](evaluation.md) | What the paper claims, what each claim needs, and every result measured so far |
| [todo.md](todo.md) | Everything still open: the tool's gaps, runs, the paper, citations to check, repository housekeeping |
| [history.md](history.md) | How the design got here: each change and what found it, plus where the deleted September documents are in git |
| [bare-metal-plan.md](bare-metal-plan.md) | The plan for real hardware and real games: the machine, Deck-like limits, Proton specifics, controls |
| [real-world-cases.md](real-world-cases.md) | About 30 documented Linux gaming stutters, mapped to hitchtrace's causes; `cases/` reproduces their mechanisms |

How to build and run each part is in the README next to it: `src/layer/`,
`src/hitchtracegui/`, `cases/`, `cases/hitchlab/`, `tools/overhead/`,
`tools/catch/` (with the rare-hitch protocol), `paper/`.
