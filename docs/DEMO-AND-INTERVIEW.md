# Demonstration and interview guide

## Explain the capability and its use first

“ELFLab is a tool for understanding how compiled Linux code becomes executable in memory. It can inspect a binary's dependencies, symbols, relocations, and memory layout. For supported modules, it then performs the linking work itself and runs an exported function. That makes it useful for learning and debugging the relationship between compiled code, shared libraries, and virtual memory.”

Describe this as a systems learning and diagnostic project. Do not claim production customers, replacement of the system linker, or compatibility with arbitrary Linux programs.

## A three-minute demonstration

1. Run **make demo** once before the interview to confirm your environment.
2. Show **inspect** and point to the separate executable and writable segments.
3. Show **deps**: app.so needs libmath.so, which needs libbase.so.
4. Show **relocs** on libmath.so. Explain that imported symbols and stored pointers need runtime addresses.
5. Run the normal example with **--trace**. Point out that all relocations occur before the dependency constructors.
6. Show the result **49**.
7. Run override.so and show **161**. Explain how the root supplies replacement definitions used by its dependencies.
8. Show **maps** and identify read-only, writable, and executable pages. Mention that tests verify those permissions through the Linux kernel's reported mappings.

## Follow-up questions to prepare for

**Why not use dlopen?** The goal is to implement the loading/linking mechanisms. The module path is parsed, mapped, relocated, and resolved by this code. The host CLI itself still uses normal Linux and libc startup.

**Why program headers instead of section headers?** Program headers describe the segments needed in a running image. Dynamic-table pointers supply the symbol, string, and relocation data. The tests remove section-header metadata and still execute the module.

**What is load bias?** It is the difference between where a module was mapped and its linked virtual-address base. A module virtual address becomes a real pointer after applying that translation.

**How do you handle BSS?** Anonymous mmap provides zero-filled pages. The loader copies only p_filesz bytes, leaving the remainder up to p_memsz zeroed.

**Why are some pages writable at first?** The loader must copy segment data and fill relocated pointers. These pages are non-executable during that stage. It then applies the final permissions and makes the RELRO range read-only.

**What is the difference between RELATIVE and JUMP_SLOT?** RELATIVE adjusts a value using the module's load bias. JUMP_SLOT installs the resolved function address for an external call. This loader resolves those calls eagerly.

**How do you choose a symbol definition?** External lookup walks the root-first breadth-first object scope. Local/protected definitions have owner-specific rules. An unresolved weak reference becomes zero; an unresolved strong reference is an error.

**What was a difficult correctness problem?** Pick something you personally studied, such as page-sharing permissions, validating virtual addresses separately from file offsets, or rejecting stale/malformed symbol indices. Show the related test and explain what would go wrong without the check.

**What are the limits?** This version calls functions from freestanding ET_DYN modules. It does not construct a normal process startup environment or implement libc integration, TLS, symbol versions, IFUNCs, or every relocation type.

**How did you test it?** Explain the actual module calls, malformed-fixture tests, comparison with readelf, and kernel permission checks. At handoff, 32 integration cases and 1,154 C assertions passed normally and with ASan/UBSan. Leak checking was blocked by the development environment and remains unverified there.

## Understand the project before presenting it

Start with src/main.c, then read src/linker.c, src/memory.c, and src/relocation.c. Change the arithmetic in an example, rebuild it, and predict the new result. Add a deliberately missing symbol and explain the resulting diagnostic. Be accurate about development assistance and the parts you can personally maintain.
