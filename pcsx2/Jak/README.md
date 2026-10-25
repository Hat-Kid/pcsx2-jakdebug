# Jak / GOAL debugging tools

Tools for poking at a running Jak game (mainly Jak X, also Jak 3/2/1) to help with
decompilation in [OpenGOAL](https://github.com/open-goal/jak-project).

## Setup

1. Jak X debug mode: enable *Settings > Advanced > Enable Extended RAM (Dev Console)* (128MB), then
   enable the `TOOL` patch in *Game Properties > Patches*. The patches live in
   `bin/patches/` (from LuminarLight/pcsx2-sanity) and are found next to the executable.
   The tools also work on the retail game without debug mode or extended RAM, see below.
2. Open *Debug > Open Debugger* and use the **GOAL REPL** view (it is in the default R5900
   layout, or add it from the layout's view menu).
3. Load the type information once, it is remembered:
   `types ~/code/C++/jak-project/decompiler/config/jakx/all-types.gc`

## REPL

Expressions use GOAL syntax, anything else is a command. `help` lists everything.

```
(inspect *target*)                    ; runs the game's own inspect method, output is captured
(inspect (-> *target* control trans)) ; structures are inspected with their static type
(-> *target* control trans)           ; field access via all-types.gc
dump *target*                         ; native typed dump, unknown bytes are listed as gaps
dump (process-by-name "target") target 2
(vector-length (-> *target* control trans))   ; call any GOAL function
(call-method *target* 9)              ; methods by id or all-types name
```

Nothing is compiled: values are read natively using the symbol table and all-types.gc,
and calls run existing game functions. A call saves every EE/FPU/VU0 register, the call
scratch area and the scratchpad, runs the function on the interpreter with events and
interrupts held off, then restores everything. `(format #t ...)` is redirected into a
scratch string while the call runs, which is how inspect/print output is captured.

Commands are executed on the CPU thread: immediately when paused, at the next vsync when
running.

## Retail mode (no debug patch, 32MB)

Retail Jak 3 / Jak X builds differ from debug mode in three ways that matter here:

- Symbol names: only a few are kept in memory, the rest point at `*unknown-symbol-name*`. Every
  object file on the disc still links its symbols by id and name, so on first use the names are
  read from the disc's DGO/CGO files (LZO decompressed, about 2s for Jak X) and cached in
  `cache/jak_symbols_<serial>_<crc>.txt`. `symbols` shows the state, `symbols reload` reads the disc
  again and `symbols <dir>` reads extracted files instead (e.g. jak-project's `iso_data/jakx`).
- The debug segment isn't loaded, and it holds nearly every inspect method (2476 of 2487 in
  Jak X). `(inspect obj)` on a type without its own inspect method in memory does a native
  all-types dump instead. Main segment inspects (`process`, `vector`, `matrix`, ...) and all other
  game functions still run normally. Output printed by game code itself still shows
  `*unknown-symbol-name*` for symbols, since the game reads the names from its own memory.
- The global heap covers nearly all of RAM, so calls use `#xc0000-#xe0000` as the scratch area
  (unused memory between the BIOS kernel and the game ELF).

## Finding out types

- `guess <addr> [size]` classifies every word (symbol, basic of type X, float, pointer,
  handle, ...) and shows which all-types field covers it.
- `fieldstats <type>` scans all instances of a type and summarises each offset over all of
  them, suggesting a type when the declared one is generic (`uint32`, `basic`, ...).
- `xref <addr>` finds everything in RAM pointing at an object and names the owning object
  and field (or the global symbol holding it).
- `instances <type>`, `procs`, `methods <type>`, `type <name>`
- `typecheck` compares every type loaded in the game (parent, size, method count) with
  all-types.gc and lists types that are missing.
- `snap name <addr>` / `diff name` shows which fields change over time.

## Call analysis

Records argument and return types of GOAL functions and methods as the game runs and flags
disagreements with all-types.gc.

- `analysis trace <targets...>` hooks only the given functions (`vector-length`), types
  (`target`, all methods it implements), methods (`target:9`, `process-tree:activate`) or
  addresses. Only the entry points and the call sites they return to are instrumented, so
  the rest of the game runs at full speed.
- `analysis on` (or *Tools > Jak Debugging > Enable GOAL Call Analysis*) hooks every
  `jalr ra, t9` call site. This costs a lot of performance, but much less than the original
  tool: one hook per call that only flushes the registers it reads, no locks, no strings
  and no symbol table scans in the hot path.
- `analysis show <target>`, `analysis top`, `analysis dump [path]` (JSON). In either mode a
  JSON file is also written to the data directory (`jak_call_analysis.json`) every 600
  frames, change that with `analysis autodump <frames>`.

## Scripting

*Tools > Jak Debugging > Enable GOAL REPL Server* listens on `127.0.0.1:28012`
(`[JakDebug] ServerPort` in PCSX2.ini). Every request is one line, every reply ends with a
NUL byte. `tools/jakdebug/goal_repl.py` is an interactive client and a small library:

```
python3 tools/jakdebug/goal_repl.py "fieldstats collide-shape-moving"
```

## Layout notes

The runtime layout comes from jak-project's kernel (`common/symbols.h`, `kscheme.cpp`).
The Jak X kernel in OpenGOAL is still mostly the Jak 3 one, so the Jak X layout assumes the
same symbol table and type layout as Jak 3; s7, `#t` and the type of `type` are validated
at runtime before anything is trusted. Use `game <jak1|jak2|jak3|jakx|auto>` to override
the detection.
