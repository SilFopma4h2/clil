# clil — the smallest Ollama coding CLI

A single-file, dependency-free C client for [Ollama](https://ollama.com) that
streams responses token-by-token straight to your terminal. No libcurl, no
JSON library, no heap growth during streaming — just raw POSIX sockets and
pointer arithmetic.

- **Striped binary ≈ 14 KB**, `.text` ≈ 8 KB
- Streaming uses a **fixed 2048-byte stack buffer** — zero `malloc`/`realloc`
  in the hot path (the only allocations are the one-shot request body)
- One `.c` file, builds with plain `cc` / `make`

## Build

```sh
make            # cc -O2 -Os -Wall -Wextra, then strip
./clil -h
```

## Usage

```sh
# prompt as an argument
clil "write a quicksort in C"

# or pipe your code in and ask about it (great for review)
clil -m qwen2.5-coder:14b "review this, explain bugs only" < code.c
cat main.go | clil -s "expert Go reviewer, be terse" "what's wrong?"

# force the tool answer in one go
cat file | clil --once ...
```

Real flags:

```
clil [-m MODEL] [-s SYSTEM] [-t TEMP] [-H HOST:PORT] [PROMPT]
  -m MODEL      model to use      (env OLLAMA_MODEL, default: codellama)
  -s SYSTEM     system prompt
  -t TEMP       temperature (0.0–1.0, optional)
  -H HOST:PORT  server address    (env OLLAMA_HOST, default: localhost:11434)
  -h            help

prompt comes from args; if absent it is read from stdin.
```

## Examples

```sh
# connect to Ollama on a remote box
OLLAMA_HOST=192.168.1.50:11434 clil "say hi"

# a tighter coding persona
clil -m deepseek-coder:v2 -s "You are a senior systems programmer." \
     -t 0.1 "explain this syscall wrapper" < wrapper.c
```

## How it stays light

- Raw `getaddrinfo`/`socket`/`connect`/`read`/`write`. No HTTP library.
- One `POST /api/generate` with `"stream":true`; the server replies with NDJSON.
- The reader scans the byte stream with `strstr` + a couple of running
  pointers, decoding `"response":"..."` values **in place** and printing each
  token the moment its closing quote arrives. Headers are skipped with a
  single `memmove`.
- Only the small incomplete tail line is carried between reads.
- The rest of the world: a tiny JSON escaper and a stack-allocated request
  body.

## Requirements

- POSIX-ish system with `cc` and `connect` (Linux, macOS, *BSD, WSL2).
- A reachable Ollama server (start one with `ollama serve`).

## Building for ARM

`clil.c` is portable C — no x86-specific code — so just recompile for your
ARM target. The binary needs a normal userspace (`getaddrinfo`, sockets), so
use whichever Linux runs Ollama; it is not bare-metal firmware.

Native build on an ARM box (Raspberry Pi, Pine64, Apple Silicon Linux, …):

```sh
make        # uses the on-device compiler
```

Cross-compile from an x86 machine (Debian/Ubuntu):

```sh
# ARM64 (Raspberry Pi 3/4/5 64-bit, most SBCs and phones)
sudo apt install gcc-aarch64-linux-gnu
aarch64-linux-gnu-gcc -O2 -Os -Wall -Wextra -o clil clil.c
aarch64-linux-gnu-strip clil

# 32-bit ARMv7 (Raspberry Pi 2/3/4 in 32-bit mode, older boards)
sudo apt install gcc-arm-linux-gnueabihf
arm-linux-gnueabihf-gcc -O2 -Os -Wall -Wextra -o clil clil.c

# statically-linked, drops the libc dependency at runtime:
aarch64-linux-gnu-gcc -O2 -Os -static -o clil clil.c
```

Then copy `clil` to the ARM device and run it there.

## License

MIT — see [LICENSE](LICENSE).