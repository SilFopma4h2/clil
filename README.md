# clil — a tiny terminal coding agent for LM Studio

`clil` is a coding agent for your terminal that runs entirely on local models
served by [LM Studio](https://lmstudio.ai). It is written in plain C++17 with
no dependencies (no libcurl, no JSON library, no readline), so it stays out of
the way of the model that needs your laptop's RAM and GPU.

```
› add a --verbose flag to main.cpp and make sure it still builds
● read  src/main.cpp
  └ 212 lines
● edit  src/main.cpp
  - static bool quiet = false;
  + static bool quiet = false, verbose = false;
  allow edit? [Y]es / [n]o / [a]lways, or type instructions: y
  └ line 14: -1 +1
● bash  make
  └ exit 0 · 3 lines
Added `--verbose` (sets `verbose`), build passes.
  412 tok · 38.6 tok/s · ctx 3.1k/8k
```

## Why it is light

**On your Mac**
- One binary under 200 KB. It uses about 10 MB of RAM and no CPU while it waits.
- It talks plain HTTP to LM Studio over raw sockets and streams tokens straight
  to the terminal.
- No runtime, no interpreter and no background processes.

**On the model**
- Four short tools (`bash`, `read`, `write`, `edit`) and a short system prompt
  keep the fixed prompt to about 500 tokens.
- Tool output is cut down before the model sees it: ANSI colours and progress
  bars are stripped, and long output keeps only its head and tail. Each result
  is capped at about 20% of the context window.
- The history is append-only, so LM Studio's prompt cache keeps working and
  nothing gets re-processed between turns. When the context gets about 75%
  full, clil trims old tool output in one go. If that is not enough, it has
  the model summarize the conversation.
- Reasoning (`<think>…</think>` / `reasoning_content`) is shown to you, but
  never sent back to the model.

## Install (macOS)

```sh
xcode-select --install        # once, for the C++ compiler
make                          # builds ./clil
sudo make install             # → /usr/local/bin/clil   (PREFIX=… to change)
make universal                # optional: one binary for Apple Silicon + Intel
```

Linux works too (`make` with g++ or clang++).

## Start

1. Start the LM Studio server. In the app, open **Developer** and turn on
   **Start server**, or run `lms server start`. You can also do this from clil
   with `/server start`.
2. Run `clil`. It picks the model that is already loaded. If none is loaded,
   use `/models` and `/load <name>`; Tab completes model names.

```sh
clil                                   # interactive session
clil -m qwen3-coder -c 16384           # pick a model (fuzzy) and context length for loading
clil "why does make fail?"             # one-shot
git diff | clil "write a commit message"
cat main.c | clil --no-tools "review this, bugs only"
clil /models                           # any slash command works one-shot too
```

## Commands

| command | |
|---|---|
| `/models` | downloaded models (● = loaded), with size, quantization, max context, tool support |
| `/ps` | loaded models and their context length |
| `/model [name]` | show or switch the chat model (fuzzy match, Tab completes) |
| `/load <name> [ctx]` | load a model into LM Studio, optionally with a context length |
| `/unload [name\|all]` | unload a model (default: the current one) |
| `/get <name\|url>` | download a model (LM Studio catalog id or Hugging Face link) with progress |
| `/server [start\|stop\|status]` | control the LM Studio server through `lms` |
| `/lms <args>` | run any `lms` command, e.g. `/lms log stream` |
| `/clear` | new conversation |
| `/compact` | have the model summarize the conversation to free context |
| `/ctx` | context usage |
| `/system [text\|off]` | extra system instructions |
| `/temp [x\|off]` | temperature |
| `/tools` | toggle tools; off means plain chat with the smallest prompt |
| `/yes` | toggle auto-approve for `bash` / `write` / `edit` |
| `/think` | show or hide the model's reasoning |
| `/save [file]` | save the conversation as Markdown |
| `/exit` | quit (Ctrl-D also works) |
| `!cmd` | run a shell command yourself |
| `@path` | attach a file to your message (Tab completes paths) |

If a model is selected but not loaded, clil loads it through LM Studio's REST
API when you send your next message. It uses `-c` as the context length when
you set one.

## Tools and approval

| tool | does | asks first |
|---|---|---|
| `read` | read a file (with `offset` / `limit`) or list a directory | no |
| `write` | create or overwrite a file (creates parent directories) | yes |
| `edit` | replace one exact, unique snippet; tolerates indentation drift | yes |
| `bash` | run `sh -c` in the working directory, non-interactive, with a timeout | yes |

Before asking, clil shows the change: a diff for `edit`, the start of the
file for `write`, the command for `bash`. Your answer can be:

- **Enter** or `y`: allow it.
- `a`: always allow this tool for the rest of the session.
- `n`: deny it and stop.
- Anything else: deny it and pass your text to the model as an instruction.

`-y` / `--yes` (or `/yes`) skips the questions.

In one-shot runs without a terminal, nothing that changes files or runs
commands is allowed unless you pass `-y`.

If `AGENTS.md` or `CLIL.md` exists in the working directory, clil adds it to
the system prompt as project instructions.

## Keys

Enter sends. Ctrl-J or a trailing `\` starts a new line (Option-Enter too,
when Option acts as Meta), and pasting multi-line text just works. ↑/↓ browse history (`~/.clil_history`)
and Tab completes. Ctrl-A/E, Ctrl-W/U/K and Option-←/→ edit the line.
Ctrl-C stops a reply or a running command. Press Ctrl-C twice, or Ctrl-D, to
quit.

## Options

```
-m, --model NAME     model (env LMSTUDIO_MODEL; default: the loaded one)
-H, --host HOST:PORT server (env LMSTUDIO_HOST; default 127.0.0.1:1234)
-c, --ctx N          context length when clil loads a model
-s, --system TEXT    extra system instructions
-t, --temp X         temperature
-n, --max-tokens N   cap each reply
-y, --yes            run bash/write/edit without asking
    --no-tools       plain chat
    --no-think       hide reasoning (shows a spinner with a token count instead)
    --timeout SEC    limit per shell command (default 180)
    --max-steps N    tool rounds per message (default 40)
-q, --quiet          no status line or tool output previews
    --no-color       no colours (NO_COLOR is honoured too)
```

`LM_API_TOKEN` is sent as a bearer token if you have enabled authentication
in LM Studio.

## LM Studio compatibility

- **Chat** uses the OpenAI-compatible `/v1/chat/completions` endpoint with
  streaming and tool calls. Any model works. Models that LM Studio marks as
  tool-trained (Qwen, Llama 3.x, Mistral, gpt-oss, …) handle tools best.
- **Model management** uses the native REST API (`/api/v1/models`,
  `/models/load`, `/models/unload`, `/models/download`) from LM Studio 0.4.
  On older versions clil falls back to `/api/v0/models` for listing and to the
  `lms` CLI for loading and unloading.

## Development

```sh
make test     # unit tests: JSON, streaming over chunked HTTP (local fake server), tools
```

The code is in `src/`:

| file | contents |
|---|---|
| `json` | JSON value, parser and serializer |
| `http` | socket HTTP/1.1 client with chunked decoding |
| `lmstudio` | REST and streaming chat client |
| `tools` | the four tools |
| `agent` | the turn loop, context budget and streaming printer |
| `commands` | slash commands |
| `term` | line editor and spinner |
| `main` | options and the REPL |

## License

MIT, see [LICENSE](LICENSE).
