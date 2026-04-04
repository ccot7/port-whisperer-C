# port-whisperer

**A beautiful CLI tool to see what's running on your ports — bare-metal C edition.**

Inspired by [LarsenCundric/port-whisperer](https://github.com/LarsenCundric/port-whisperer), rewritten from scratch in pure C with zero runtime dependencies.  
Works on **Linux** (Fedora/Wayland and other distros) and **macOS** (Intel & Apple Silicon).

```
$ ports

 ┌─────────────────────────────────────┐
 │  Port Whisperer                     │
 │  listening to your ports...         │
 └─────────────────────────────────────┘

┌───────┬─────────┬───────┬──────────────────────┬────────────┬────────┬───────────┐
│ PORT  │ PROCESS │ PID   │ PROJECT              │ FRAMEWORK  │ UPTIME │ STATUS    │
├───────┼─────────┼───────┼──────────────────────┼────────────┼────────┼───────────┤
│ :3000 │ node    │ 42872 │ frontend             │ Next.js    │ 1d 9h  │ ● healthy │
├───────┼─────────┼───────┼──────────────────────┼────────────┼────────┼───────────┤
│ :5432 │ docker  │ 58351 │ backend-postgres-1   │ PostgreSQL │ 10d 3h │ ● healthy │
└───────┴─────────┴───────┴──────────────────────┴────────────┴────────┴───────────┘

  2 ports active  ·  Run ports <number> for details  ·  --all to show everything
```

---

## Features

| Feature | Description |
|---|---|
| Port table | Color-coded table of every listening TCP port |
| Framework detection | Reads `package.json`, `requirements.txt`, `Gemfile`, `go.mod`, `Cargo.toml`, `pom.xml` |
| Docker awareness | Maps host ports → container names / images (PostgreSQL, Redis, MongoDB, …) |
| Process details | Uptime, memory, CPU%, git branch, full cmdline |
| Interactive kill | `ports <number>` gives a detail view with a kill prompt |
| Process view | `ports ps` — beautiful `ps aux` for developers |
| Watch mode | `ports watch` — real-time port open/close notifications |
| Orphan cleanup | `ports clean` — kill zombie/orphaned dev servers |
| Status colours | `● healthy` (green) · `● orphaned` (yellow) · `● zombie` (red) |

---

## Requirements

### Linux
- GCC ≥ 7 or Clang ≥ 6  
- `lsof` (available in most distros: `sudo dnf install lsof` on Fedora)  
- `docker` CLI (optional — only needed for Docker container mapping)  
- No other dependencies — reads `/proc` directly

### macOS
- Xcode Command Line Tools (`xcode-select --install`)  
- `lsof` (pre-installed)  
- `ps`, `git` (pre-installed)  
- `docker` CLI (optional)

---

## Build & Install

```bash
# Clone
git clone https://github.com/yourname/port-whisperer.git
cd port-whisperer

# Build
make

# Install to ~/.local/bin  (make sure it's in your PATH)
make install

# Or install to /usr/local/bin  (system-wide)
sudo make install PREFIX=/usr/local
```

The binary is named **`ports`** to match the original tool's UX.

### Add to PATH (if needed)

**Bash / Zsh:**
```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc   # or ~/.zshrc
source ~/.bashrc
```

**Fish:**
```fish
fish_add_path ~/.local/bin
```

---

## Usage

### Show dev / docker ports (default)
```
ports
```
Filters out system noise (Spotify, browsers, desktop apps). Shows only dev servers, Docker containers, and databases.

### Show all listening ports
```
ports --all
```

### Inspect a specific port
```
ports 3000
ports 8080
```
Shows: process, project, framework, working directory, git branch, uptime, memory, CPU, cmdline.  
Ends with an interactive prompt to send `SIGTERM` to the process.

### Show all dev processes (not just port-bound)
```
ports ps
ports ps --all
```
Equivalent to a developer-friendly `ps aux`. Docker processes are collapsed into a single summary row.

### Watch for port changes (real-time)
```
ports watch
```
Polls every 2 seconds. Prints timestamped lines when ports open (`+`) or close (`-`). Press `Ctrl-C` to stop.

### Clean up orphaned dev processes
```
ports clean
```
Finds processes whose working directory no longer exists or that are in zombie state, and sends `SIGTERM`. Only targets dev runtimes (node, python, ruby, etc.) — never touches desktop apps.

### Help
```
ports --help
```

---

## How it works

### Linux
1. **`lsof -iTCP -sTCP:LISTEN -nP`** — finds all TCP listening ports and their PIDs
2. **`/proc/<pid>/cmdline`** — reads the full command line directly from procfs (no `ps` call needed)
3. **`/proc/<pid>/cwd`** — symlink read to get the process working directory
4. **`/proc/<pid>/stat`** — parses process state and start time for uptime calculation
5. **`/proc/<pid>/status`** — reads `VmRSS` for memory usage
6. **`docker ps`** (optional) — maps host ports to container names and images
7. **Framework detection** — reads project files in the process CWD

### macOS
1. **`lsof -iTCP -sTCP:LISTEN -nP`** — same as Linux
2. **`ps -p <pid> -o etimes=,rss=,%cpu=`** — single batched call per process
3. **`lsof -p <pid> -d cwd`** — resolves working directory
4. **`docker ps`** (optional) — container mapping

### Framework detection
Reads project files in the process's working directory:

| File | Detected frameworks |
|---|---|
| `package.json` | Next.js, Nuxt, Vite, Angular, React, Svelte, Remix, Astro, Express, Fastify, NestJS, Node.js |
| `requirements.txt` | Django, FastAPI, Flask, Python |
| `pyproject.toml` | Python |
| `Gemfile` | Rails, Ruby |
| `go.mod` | Go |
| `Cargo.toml` | Rust |
| `build.gradle` | Gradle/JVM |
| `pom.xml` | Maven/JVM |

Docker images are matched by name: `postgres` → PostgreSQL, `redis` → Redis, `mongo` → MongoDB, `localstack` → LocalStack, etc.

---

## Uninstall

```bash
make uninstall
# or:
rm ~/.local/bin/ports
```

---

## Known limitations

- **`ports ps --all` is slow on macOS** — in `--all` mode every process gets its CWD resolved via `lsof -p <pid>`, meaning hundreds of serial `lsof` calls. Use `ports ps` (dev-filtered) instead; it's fast. Batched CWD resolution is a planned fix.
- **`make` newline warning on macOS clang** — if you see `-Wnewline-eof`, run `echo "" >> src/main.c` once to fix your local copy.

---

## Platform support

| Platform | Status |
|---|---|
| Linux (Fedora, Ubuntu, Arch, …) | ✅ Full support |
| macOS (Intel + Apple Silicon) | ✅ Full support |
| Windows | ✗ Not planned |

---

## License

MIT — see [LICENSE](LICENSE).

---

## Acknowledgements

Original concept and design by [LarsenCundric](https://github.com/LarsenCundric/port-whisperer).  
This is an independent bare-metal C reimplementation with no shared code.