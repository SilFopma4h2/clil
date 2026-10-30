# Agent Instructions: clil

## Build & Test
- **Build**: `make` (generates `./clil`)
- **Test**: `make test` (unit tests for JSON, HTTP, and tools)

## Code Layout
- `src/`: Core implementation (C++17/C11)
  - `main.cpp`: Entry point & REPL
  - `agent.cpp`: Turn loop & context management
  - `tools.cpp`: `read`, `write`, `edit`, `bash` tools
  - `lmstudio.cpp`: LM Studio API client
  - `http.cpp`: Custom HTTP/1.1 client (no libcurl)
  - `json.cpp`: Custom JSON parser/serializer
  - `commands.cpp`: Slash command logic
  - `term.cpp`: Terminal UI & line editor
- `tests/`: Unit tests
- `build/`: Build artifacts

## Conventions
- **Zero Dependencies**: Use only standard libraries (no `libcurl`, `nlohmann/json`, `readline`).
- **Language**: C++17 (main), C11 (shared state in `src/state.c`).
- **Agent Safety**:
  - `edit`: Replace exact, unique code snippets.
  - `bash`: Non-interactive with a timeout.
  - `tools`: Strip ANSI colors/progress bars from output sent to LLM.
- **Memory**: Keep footprint small (target <200KB binary, ~10MB RAM).
- **Context**: Trim old tool output when context is ~75% full.
