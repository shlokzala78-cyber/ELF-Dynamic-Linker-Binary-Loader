# ELFLab — ELF Dynamic Linker & Binary Loader

A working Linux x86-64 systems project in **C and x86-64 assembly**. ELFLab explains and performs the steps between an ELF file on disk and a callable function in memory: parse program headers, discover dependencies, map segments, resolve symbols, apply relocations, protect pages, and execute the linked code.

The main demonstration loads three freestanding modules and calls a function across their library boundaries. A second demonstration overrides symbols from the root module, changing the result without rebuilding its dependencies.

**Target:** Linux x86-64 with 4 KiB pages. On Windows, use Ubuntu through WSL2 or the optional Docker instructions below.

**Scope:** this is an educational dynamic linker for a documented ELF subset. It executes exported functions from purpose-built ET_DYN modules. It does not start arbitrary Linux applications or replace glibc's dynamic linker. The host CLI uses normal Linux/libc startup; the loaded modules are mapped and linked by this project's C code, with no dlopen/dlsym delegation.

## Start here on Windows / VS Code

If Ubuntu is not installed in WSL, open **PowerShell as Administrator** and run:

    wsl --install -d Ubuntu

Restart if prompted, launch Ubuntu, and finish creating its Linux user account.

In VS Code, install Microsoft's **WSL** and **C/C++** extensions. Extract the ZIP, open the **elf-dynamic-loader** folder, then use **Ctrl+Shift+P → WSL: Reopen Folder in WSL**. The bottom-left status indicator should show WSL/Ubuntu.

Open **Terminal → New Terminal** in that WSL window. Run:

    uname -m
    sudo apt update
    sudo apt install -y build-essential binutils python3 gdb
    make
    make demo
    make test

The architecture should be **x86_64**. These are Linux terminal commands. This project does not use npm or a browser server.

You can also open an Ubuntu terminal, navigate to the extracted folder, and run **code .** to open that folder in VS Code through WSL. Paths on the Windows C: drive appear under /mnt/c in Ubuntu.

See [Microsoft's WSL installation guide](https://learn.microsoft.com/en-us/windows/wsl/install) and [VS Code's WSL guide](https://code.visualstudio.com/docs/remote/wsl) if WSL is not available.

## Start on Linux

Install GCC, GNU Make, binutils, and Python 3 using your distribution's package manager. On Ubuntu/Debian:

    sudo apt update
    sudo apt install -y build-essential binutils python3
    make
    make demo
    make test

To run only the main example:

    ./build/elflab run build/fixtures/app.so

Expected output with the supplied toolchain/fixtures:

    RESULT demo_entry(5, 7) = 49
    Linked 3 objects; applied 16 relocations.

Relocation counts and addresses can vary with the compiler/linker. The arithmetic result must remain 49.

## Optional: run with Docker Desktop

From the project folder in PowerShell or a Linux terminal:

    docker build --platform linux/amd64 -t elflab .
    docker run --rm --platform linux/amd64 elflab
    docker run --rm --platform linux/amd64 elflab make test

Docker runs the demonstration by default. The image uses a non-root runtime user. This is an alternative toolchain setup, not an isolation guarantee for arbitrary modules. The Docker recipe is included but was not executed in the build environment.

## Commands

| Command | Example | Behavior |
| --- | --- | --- |
| inspect | ./build/elflab inspect build/fixtures/app.so | Validate headers; list segments and direct dependencies |
| symbols | ./build/elflab symbols build/fixtures/libmath.so | Print dynamic symbols, bindings, visibility, and definitions |
| relocs | ./build/elflab relocs build/fixtures/libmath.so | Print RELA and PLT relocation records |
| deps | ./build/elflab deps build/fixtures/app.so | Build the dependency graph without running module code |
| maps | ./build/elflab maps build/fixtures/app.so | Map, relocate, and protect the graph without calling constructors |
| run | ./build/elflab run build/fixtures/app.so --trace | Initialize dependencies, call the entry function, finalize, and unload |

Options:

- **-L DIR**: additional dependency search directory, repeatable up to 16 times.
- **--entry NAME**: exported function to call; default is demo_entry.
- **--arg1 N / --arg2 N**: signed long arguments; defaults are 5 and 7.
- **--trace**: mapping, relocation, and initialization details on stderr.
- **--dot**: Graphviz dependency output for the deps command.
- **--help**: command summary.

The function ABI is **long entry(long first, long second)**. ELFLab does not use e_entry as a process entry point and does not construct argc, argv, envp, or an auxiliary vector. The assembly bridge preserves the System V AMD64 call alignment when entering a module function.

Return codes: **0** for success, **1** for parse/link/load errors, **2** for invalid CLI usage. The module's return value is printed rather than used as the CLI exit status.

## Demonstrations

### Follow the complete loading process

    ./build/elflab run build/fixtures/app.so --trace

The trace shows the app, math library, and base library being parsed and mapped; then their relocations are resolved before any constructor runs. Constructors execute in dependency order. Finalizers execute in reverse initialization order.

### Change arguments

    ./build/elflab run build/fixtures/app.so --arg1 10 --arg2 2

Expected result: **59**.

### Demonstrate symbol interposition

    ./build/elflab run build/fixtures/override.so --trace

Expected result: **161**. The root module supplies a different base_bias and an optional_hook that was an unresolved weak reference in the first demo.

### Inspect a system executable without running it

    ./build/elflab inspect /bin/ls

The inspector can describe ordinary x86-64 ELF executables within its parser limits, even when their execution requires unsupported features such as symbol versioning. Dynamic symbol inspection is provided; the separate debugging/static symbol table is not parsed.

### Export the dependency graph

    ./build/elflab deps build/fixtures/app.so --dot > build/dependencies.dot

If Graphviz is installed, you can render that file with:

    dot -Tsvg build/dependencies.dot -o build/dependencies.svg

### Compare relocation diagnostics

    ./build/elflab relocs build/fixtures/libmath.so
    readelf -Wr build/fixtures/libmath.so

## Implemented capabilities

| Area | Implementation |
| --- | --- |
| ELF parsing | ELF64 little-endian x86-64 header and program-header validation |
| Virtual-address translation | Translate virtual addresses through PT_LOAD into file bytes or mapped memory |
| Segment mapping | Anonymous mmap reservation, copied file contents, zero-filled BSS, inaccessible holes |
| Memory permissions | Temporary non-executable writable pages; final mprotect permissions; W^X rejection |
| GNU_RELRO | Remove write permission from the page-aligned RELRO range after eager relocation |
| Dependencies | DT_NEEDED traversal, canonical-path deduplication, bounded breadth-first graph |
| Symbols | Global/weak references, root-first interposition, owner-local/protected binding, absolute symbols |
| Hash metadata | SysV and GNU hash formats used to determine bounded dynamic-symbol counts |
| Relocations | NONE, RELATIVE, 64, GLOB_DAT, JUMP_SLOT, PC32, 32, 32S |
| Lifecycle | DT_INIT, DT_FINI, constructor/destructor arrays; cycle-aware dependency ordering |
| Diagnostics | Headers, symbols, relocations, dependency/DOT graphs, final memory maps, trace logs |
| Testing | Real module execution, kernel permission inspection, malformed fixtures, sanitizer build |

Dynamic symbol lookup is a straightforward bounded linear scan over the breadth-first scope. Hash metadata support does not imply accelerated hash-based lookup.

Supported 32-bit relocation forms have explicit signed/unsigned overflow checks. All relocations write only inside originally writable PT_LOAD byte ranges. Unsupported relocation types fail before initialization.

For permissions, ELFLab combines flags for segments sharing a page and rejects any page requiring both writing and execution. Whole-page RELRO protection follows a rounded-down start/end policy; a trailing partial page remains under its segment permissions.

## Dependency search and limits

Dependencies are searched in the provided -L directories, in order, then the root module's directory. Only basename DT_NEEDED entries are accepted. Identity is based on canonical path, so hard-link aliases are not inode-deduplicated.

ELFLab does not use LD_LIBRARY_PATH, the system loader cache, default /lib paths, RPATH/RUNPATH expansion, or symbol versions. Supply the provided freestanding dependencies together, or use -L explicitly.

Limits are deliberately bounded:

- 32 objects per dependency graph and 32 dependencies per object.
- 128 program headers and 512 dynamic-table entries per object.
- 65,536 dynamic symbols and 131,072 entries per relocation table.
- 64 MiB input file / mapped image per object; 256 MiB total mapped image space.
- Segment alignment no larger than the system page size.

Run the sample modules you build from this repository. **run executes native code in the same process and is not a sandbox.** It can crash or perform system calls with the caller's privileges. Inspection and graph commands do not invoke module functions.

## Features deliberately outside this version

- Executing ET_EXEC binaries at fixed addresses or normal PT_INTERP/libc-based applications.
- Linux process startup stacks, auxiliary vectors, TLS, IFUNC/IRELATIVE, COPY relocations, symbol versions, and GNU UNIQUE binding.
- REL or packed RELR relocations, text relocations, lazy PLT binding, C++ runtime/exception integration, audit hooks, and full glibc compatibility.
- Extended header numbering, overlapping PT_LOAD byte ranges, arbitrary large alignment, or a complete hardened parser for hostile inputs.

Known unsupported execution features produce an error. Unknown metadata that is not needed for this supported subset may be ignored. Successful inspection is not a promise that a file can be executed by ELFLab.

## Tests and verification

    make test
    make sanitize

The normal build treats compiler warnings as errors. The sanitizer build enables AddressSanitizer and UndefinedBehaviorSanitizer for the host loader and C tests; the freestanding fixtures stay uninstrumented because they intentionally have no sanitizer-runtime dependency.

At handoff, on Linux x86-64 / GCC 13.3:

- **32 Python integration tests passed**, including an 80-input deterministic header-mutation smoke test.
- **1,154 C assertions passed**, including 1,024 BSS zero checks, relocation arithmetic, real cross-library calls, initialization/finalization state, and actual /proc/self/maps permissions.
- The same tests passed with **ASan and UBSan** enabled.
- LeakSanitizer could not inspect process/thread metadata in the build environment. The successful local sanitizer run used **ASAN_OPTIONS=detect_leaks=0 make sanitize**. Leak checking is therefore **not verified at handoff**; the default make sanitize command and GitHub workflow leave it enabled.
- /bin/ls inspection and the complete demo were run successfully.
- The Docker recipe and interactive VS Code/GDB setup were not run here.

The tests compare relocation names with GNU readelf and exercise malformed headers, strings, symbol indices, relocation targets, initializer pointers, segment alignment, unsupported features, and dependency cycles. The mutation test is a small regression check, not a comprehensive fuzzing campaign or security certification.

## Main code and project structure

| File | Responsibility |
| --- | --- |
| src/main.c | CLI entry point and command workflow |
| src/linker.c | Dependency graph, symbol resolution, relocation application, constructors/finalizers |
| src/elf.c | File snapshot, headers, address translation, loadability checks |
| src/dynamic.c | Dynamic tags, strings, symbol bounds, hash metadata, RELA table parsing |
| src/memory.c | mmap, BSS, page flags, mprotect, RELRO |
| src/relocation.c | Relocation formulas and overflow checks |
| src/call.S | Assembly function-call bridge |
| src/inspect.c | Human-readable and DOT diagnostics |
| include/elflab.h | Shared types, limits, and interfaces |
| examples/ | Freestanding modules, assembly example, and negative fixtures |
| tests/ | C assertions and Python CLI/malformed-input tests |
| docs/ | Architecture, supported ABI, and interview/demo guidance |
| .vscode/ | Optional build/test tasks, IntelliSense, and GDB launch configuration |
| .github/workflows/ | Automated Linux build and tests |

For debugging, open src/linker.c, set a breakpoint in lab_link or lab_resolve, and run **Debug ELF loader (Linux / WSL)**. The provided GDB configuration debugs the host loader. Symbols for custom-mapped modules are not automatically registered with GDB.

## Add your own example

Write a C function with the documented two-long argument contract. Avoid printf, malloc, libc startup, TLS, and compiler-generated runtime helpers. A standalone example:

    long demo_entry(long first, long second) {
        return first * first + second;
    }

Save it as examples/custom.c and compile:

    gcc -O1 -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib -shared -Wl,--hash-style=both,-z,relro,-z,now,-z,noexecstack examples/custom.c -o build/fixtures/custom.so
    ./build/elflab run build/fixtures/custom.so --arg1 5 --arg2 7

Expected result: **32**. A custom module can depend on other freestanding modules using ordinary -L/-l link options; make sure those dependencies are present when ELFLab loads it.

## GitHub upload

Extract the ZIP and upload the **contents of elf-dynamic-loader**, so README.md and Makefile are at the repository root. Keep src, include, examples, tests, docs, configuration, and license files.

Do not upload build or build-sanitize. These contain generated executables, object files, and fixture binaries, and are excluded by .gitignore. The examples folder contains the source required to rebuild those fixtures, so keep it.

The .vscode files contain shared Linux/WSL settings and are safe to include. The GitHub Actions workflow runs compilation, tests, and sanitizers on Ubuntu after a push. No account secrets or API keys are needed.

## Troubleshooting

| Problem | Fix |
| --- | --- |
| make/gcc is not recognized in PowerShell | Reopen the folder in WSL and use its Linux terminal |
| Missing elf.h or compiler | Install build-essential in Ubuntu |
| Exec format error | Confirm uname -m is x86_64; build inside Linux or use linux/amd64 Docker |
| Missing dependency | Build all fixtures with make, or provide -L with the library directory |
| Undefined strong symbol | Remove unexpected libc/runtime calls or supply a freestanding library defining that symbol |
| PT_INTERP, TLS, versioning, or unsupported relocation error | Use the provided compiler/linker flags and supported ABI subset |
| Permission denied executing built files under a mounted drive | Copy the project into your Ubuntu home directory and rebuild there |
| Old results after compiler-flag changes | Run make clean, then make test |
| LeakSanitizer fatal /proc or ptrace error | Run outside a debugger/restricted container; use detect_leaks=0 only to separate ASan/UBSan checks from unavailable leak scanning |

## Further reading

- [System V generic ELF ABI: dynamic linking](https://gabi.xinuos.com/v42/elf/08-dynamic.html)
- [x86-64 psABI source and specification](https://gitlab.com/x86-psABIs/x86-64-ABI)
- [Linux ELF structures](https://man7.org/linux/man-pages/man5/elf.5.html)

Read docs/ARCHITECTURE.md for the loading pipeline and docs/DEMO-AND-INTERVIEW.md for a concise explanation of its user value.
