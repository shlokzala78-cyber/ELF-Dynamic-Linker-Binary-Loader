#include "elflab.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__linux__) || !defined(__x86_64__)
#error "ELFLab requires Linux x86-64. On Windows, use Ubuntu under WSL2."
#endif

static void usage(FILE *out) {
    fputs(
        "ELFLab — ELF Dynamic Linker & Binary Loader\n\n"
        "Usage: elflab COMMAND FILE [options]\n\n"
        "  inspect   Validate headers and show segments/dependencies\n"
        "  symbols   Print the dynamic symbol table\n"
        "  relocs    Print dynamic RELA and PLT relocations\n"
        "  deps      Resolve dependency graph without executing code\n"
        "  maps      Map, eagerly link, and protect pages; do not run code\n"
        "  run       Initialize and call a freestanding exported function\n\n"
        "Options:\n"
        "  -L DIR          Search directory; repeat up to 16 times\n"
        "  --entry NAME    Function name (default: demo_entry)\n"
        "  --arg1 INTEGER  First signed long argument (default: 5)\n"
        "  --arg2 INTEGER  Second signed long argument (default: 7)\n"
        "  --trace         Explain mapping, relocations, init/fini on stderr\n"
        "  --dot           Graphviz output for the deps command\n\n"
        "Function contract: long entry(long first, long second).\n"
        "run executes module code in this process; it is not a sandbox.\n", out);
}

static bool integer(const char *text, long *value) {
    char *end; errno = 0;
    *value = strtol(text, &end, 0);
    return !errno && text[0] && !*end;
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { usage(stdout); return 0; }
    if (argc < 3) { usage(stderr); return 2; }
    const char *command = argv[1], *path = argv[2], *entry = "demo_entry";
    bool single = !strcmp(command, "inspect") || !strcmp(command, "symbols") || !strcmp(command, "relocs");
    bool deps = !strcmp(command, "deps"), maps = !strcmp(command, "maps"), run = !strcmp(command, "run");
    if (!single && !deps && !maps && !run) { fprintf(stderr, "Unknown command: %s\n", command); return 2; }
    LabContext ctx = {0}; LabError err = {{0}};
    bool dot = false, ok = true;
    long first = 5, second = 7;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--trace")) ctx.trace = true;
        else if (!strcmp(argv[i], "--dot") && deps) dot = true;
        else if (!strcmp(argv[i], "-L") && i + 1 < argc && ctx.directory_count < 16) ctx.directories[ctx.directory_count++] = argv[++i];
        else if (!strcmp(argv[i], "--entry") && i + 1 < argc && run) entry = argv[++i];
        else if (!strcmp(argv[i], "--arg1") && i + 1 < argc && run) {
            if (!integer(argv[++i], &first)) { fputs("Invalid --arg1 integer\n", stderr); return 2; }
        } else if (!strcmp(argv[i], "--arg2") && i + 1 < argc && run) {
            if (!integer(argv[++i], &second)) { fputs("Invalid --arg2 integer\n", stderr); return 2; }
        } else { fprintf(stderr, "Unknown, incomplete, or misplaced option: %s\n", argv[i]); return 2; }
    }
    if (single) {
        LabObject *obj = calloc(1, sizeof(*obj));
        if (!obj) return 1;
        ok = lab_object_read(obj, path, &err);
        if (ok) {
            if (!strcmp(command, "inspect")) lab_inspect(obj);
            else if (!strcmp(command, "symbols")) lab_print_symbols(obj);
            else lab_print_relocations(obj);
        }
        lab_object_destroy(obj);
    } else {
        ok = lab_graph(&ctx, path, &err);
        if (ok && deps) lab_print_graph(&ctx, dot);
        if (ok && (maps || run)) ok = lab_link(&ctx, &err);
        if (ok && maps) lab_print_maps(&ctx);
        if (ok && run) {
            uint64_t function; unsigned type;
            ok = lab_symbol(&ctx, entry, &function, &type, &err);
            if (ok && (type != STT_FUNC || !lab_executable(&ctx, function)))
                ok = lab_fail(&err, "Entry must be an STT_FUNC symbol in executable module memory");
            if (ok) ok = lab_initialize(&ctx, &err);
            if (ok) {
                long result = lab_call2(function, first, second);
                lab_finalize(&ctx);
                printf("RESULT %s(%ld, %ld) = %ld\n", entry, first, second, result);
                printf("Linked %zu objects; applied %zu relocations.\n", ctx.count, ctx.relocation_count);
            }
        }
        lab_context_destroy(&ctx);
    }
    if (!ok) { fprintf(stderr, "elflab: %s\n", err.text[0] ? err.text : "operation failed"); return 1; }
    return 0;
}
