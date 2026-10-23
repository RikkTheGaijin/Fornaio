# Code graph (graphify)

This repo has a local knowledge graph of the code in `graphify-out/graph.json` (git hooks rebuild it on commit/checkout).

- For questions about architecture, where something lives, what calls what, or the impact of a change, query the graph before grepping or reading files one by one:
  - `graphify query "<question>"` - scoped subgraph for a plain-language question
  - `graphify explain "<Symbol>"` - a node and its connections
  - `graphify path "<A>" "<B>"` - how two symbols connect
  - `graphify affected "<Symbol>"` - what depends on a symbol
- Then read the specific files/lines the graph points to. The graph locates code; it does not replace reading it before editing.
- If `graphify-out/graph.json` is missing or the command fails, run `graphify update .` (local, no LLM) and retry, or fall back to normal search.
- The graph excludes `third_party/`, `build/`, `artifacts/` (see `.graphifyignore`).
