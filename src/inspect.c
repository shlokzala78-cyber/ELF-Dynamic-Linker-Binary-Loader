#include "elflab.h"
#include <inttypes.h>

static const char *segment_name(uint32_t type) {
    switch (type) {
        case PT_LOAD: return "LOAD";
        case PT_DYNAMIC: return "DYNAMIC";
        case PT_INTERP: return "INTERP";
        case PT_NOTE: return "NOTE";
        case PT_PHDR: return "PHDR";
        case PT_TLS: return "TLS";
        case PT_GNU_STACK: return "GNU_STACK";
        case PT_GNU_RELRO: return "GNU_RELRO";
        case PT_GNU_EH_FRAME: return "GNU_EH_FRAME";
        default: return "OTHER";
    }
}

void lab_inspect(const LabObject *obj) {
    fputs("ELF file: ", stdout); lab_quoted(stdout, obj->path); putchar('\n');
    printf("Format: ELF64 / little-endian / x86-64 / %s\n", obj->eh.e_type == ET_DYN ? "ET_DYN" : "ET_EXEC");
    printf("Entry virtual address: 0x%" PRIx64 "\n", obj->eh.e_entry);
    printf("Program headers: %u | Dynamic symbols: %zu | RELA entries: %zu\n",
           obj->eh.e_phnum, obj->symbol_count, obj->rela_count + obj->plt_count);
    puts("\n #  TYPE           OFFSET       VADDR        FILESZ       MEMSZ        FLAGS");
    for (size_t i = 0; i < obj->eh.e_phnum; ++i) {
        const Elf64_Phdr *p = &obj->ph[i];
        char flags[4]; lab_permissions(p->p_flags, flags);
        printf("%2zu  %-13s  %010" PRIx64 "   %010" PRIx64 "   %010" PRIx64 "   %010" PRIx64 "   %s\n",
               i, segment_name(p->p_type), p->p_offset, p->p_vaddr, p->p_filesz, p->p_memsz, flags);
    }
    if (obj->soname) { fputs("\nSONAME: ", stdout); lab_quoted(stdout, obj->soname); putchar('\n'); }
    puts("\nDirect dependencies:");
    for (size_t i = 0; i < obj->needed_count; ++i) { fputs("  ", stdout); lab_quoted(stdout, obj->needed[i]); putchar('\n'); }
    if (!obj->needed_count) puts("  (none)");
    if (obj->unsupported[0]) printf("\nExecution restriction: %s\n", obj->unsupported);
    puts("\nInspection does not execute the input.");
}

void lab_print_symbols(const LabObject *obj) {
    puts("INDEX  VALUE             SIZE     BIND    TYPE     VIS  STATE  NAME");
    for (size_t i = 0; i < obj->symbol_count; ++i) {
        const Elf64_Sym *s = &obj->symbols[i];
        unsigned b = ELF64_ST_BIND(s->st_info), t = ELF64_ST_TYPE(s->st_info);
        const char *bind = b == STB_GLOBAL ? "GLOBAL" : b == STB_WEAK ? "WEAK" : b == STB_LOCAL ? "LOCAL" : "OTHER";
        const char *type = t == STT_FUNC ? "FUNC" : t == STT_OBJECT ? "OBJECT" : t == STT_TLS ? "TLS" : t == STT_GNU_IFUNC ? "IFUNC" : "NOTYPE";
        printf("%5zu  %016" PRIx64 "  %-7" PRIu64 "  %-6s  %-7s  %u    %-5s  ",
               i, s->st_value, s->st_size, bind, type, ELF64_ST_VISIBILITY(s->st_other),
               s->st_shndx == SHN_UNDEF ? "UND" : s->st_shndx == SHN_ABS ? "ABS" : "DEF");
        lab_quoted(stdout, lab_string(obj, s->st_name)); putchar('\n');
    }
}

static void print_table(const LabObject *obj, const Elf64_Rela *table, size_t count, const char *kind) {
    for (size_t i = 0; i < count; ++i) {
        const Elf64_Rela *r = &table[i];
        unsigned type = ELF64_R_TYPE(r->r_info);
        size_t symbol = ELF64_R_SYM(r->r_info);
        printf("%-4s  %016" PRIx64 "  %-24s (%3u)  %12" PRId64 "  ",
               kind, r->r_offset, lab_reloc_name(type), type, r->r_addend);
        lab_quoted(stdout, lab_string(obj, obj->symbols[symbol].st_name)); putchar('\n');
    }
}

void lab_print_relocations(const LabObject *obj) {
    puts("TABLE OFFSET            TYPE                              ADDEND  SYMBOL");
    print_table(obj, obj->rela, obj->rela_count, "DYN");
    print_table(obj, obj->plt, obj->plt_count, "PLT");
}

void lab_print_graph(const LabContext *ctx, bool dot) {
    if (dot) puts("digraph dependencies {\n  rankdir=LR;");
    else puts("Dependency graph (global symbol scope is breadth-first):");
    for (size_t i = 0; i < ctx->count; ++i) {
        const LabObject *obj = ctx->objects[i];
        if (dot) { printf("  n%zu [label=", i); lab_quoted(stdout, lab_basename(obj->path)); puts("];"); }
        else { printf("[%zu] ", i); lab_quoted(stdout, lab_basename(obj->path)); putchar('\n'); }
        for (size_t j = 0; j < obj->needed_count; ++j) {
            if (dot) printf("  n%zu -> n%zu;\n", i, obj->deps[j]);
            else { printf("    -> [%zu] ", obj->deps[j]); lab_quoted(stdout, obj->needed[j]); putchar('\n'); }
        }
    }
    if (dot) puts("}");
}

void lab_print_maps(const LabContext *ctx) {
    puts("Final mapped pages (permissions after relocations and GNU_RELRO):");
    for (size_t i = 0; i < ctx->count; ++i) {
        const LabObject *obj = ctx->objects[i];
        printf("\n[%zu] ", i); lab_quoted(stdout, lab_basename(obj->path));
        printf("  bias=0x%" PRIx64 "\n", obj->bias);
        for (size_t first = 0; first < obj->page_count;) {
            size_t end = first + 1;
            while (end < obj->page_count && obj->permissions[end] == obj->permissions[first]) ++end;
            char flags[4]; lab_permissions(obj->permissions[first], flags);
            printf("  0x%016" PRIxPTR "-0x%016" PRIxPTR "  %s  vaddr=0x%08" PRIx64 "  %zu bytes%s\n",
                   (uintptr_t)(obj->mapping + first * obj->page_size),
                   (uintptr_t)(obj->mapping + end * obj->page_size), flags,
                   obj->low + first * obj->page_size, (end - first) * obj->page_size,
                   obj->permissions[first] ? "" : " (reserved hole)");
            first = end;
        }
    }
    printf("\nLinked %zu objects; applied %zu relocations; mapped %zu bytes.\n",
           ctx->count, ctx->relocation_count, ctx->mapped_total);
}
