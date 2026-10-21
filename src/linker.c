#include "elflab.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool add_object(LabContext *ctx, const char *path, size_t *index, LabError *err) {
    char canonical[PATH_MAX];
    if (!realpath(path, canonical)) return lab_fail(err, "Cannot find %s: %s", path, strerror(errno));
    for (size_t i = 0; i < ctx->count; ++i) {
        if (!strcmp(canonical, ctx->objects[i]->path)) { *index = i; return true; }
    }
    if (ctx->count == LAB_MAX_OBJECTS) return lab_fail(err, "Dependency graph exceeds %d objects", LAB_MAX_OBJECTS);
    LabObject *obj = calloc(1, sizeof(*obj));
    if (!obj) return lab_fail(err, "Out of memory creating object");
    *index = ctx->count; ctx->objects[ctx->count++] = obj;
    if (!lab_object_read(obj, canonical, err)) return false;
    lab_trace(ctx, "parsed %s (%zu symbols, %zu dependencies)", canonical, obj->symbol_count, obj->needed_count);
    return true;
}

static bool dependency_path(const LabContext *ctx, const char *name, char path[PATH_MAX], LabError *err) {
    if (!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, ".."))
        return lab_fail(err, "Dependencies must be basenames, not paths: %s", name);
    for (size_t i = 0; i <= ctx->directory_count; ++i) {
        const char *directory = i < ctx->directory_count ? ctx->directories[i] : ctx->root_directory;
        int size = snprintf(path, PATH_MAX, "%s/%s", directory, name);
        if (size < 0 || size >= PATH_MAX) continue;
        if (access(path, R_OK) == 0) return true;
    }
    return lab_fail(err, "Missing dependency %s; provide its directory with -L", name);
}

bool lab_graph(LabContext *ctx, const char *path, LabError *err) {
    size_t root;
    if (!add_object(ctx, path, &root, err)) return false;
    snprintf(ctx->root_directory, sizeof(ctx->root_directory), "%s", ctx->objects[0]->path);
    char *slash = strrchr(ctx->root_directory, '/');
    if (slash == ctx->root_directory) slash[1] = '\0';
    else if (slash) *slash = '\0';
    // Appending while walking the queue creates the global breadth-first lookup scope.
    for (size_t i = 0; i < ctx->count; ++i) {
        LabObject *obj = ctx->objects[i];
        for (size_t j = 0; j < obj->needed_count; ++j) {
            char found[PATH_MAX];
            if (!dependency_path(ctx, obj->needed[j], found, err) ||
                !add_object(ctx, found, &obj->deps[j], err)) return false;
        }
    }
    return true;
}

static bool symbol_address(LabObject *obj, const Elf64_Sym *sym, uint64_t *value, LabError *err) {
    if (sym->st_shndx == SHN_ABS) { *value = sym->st_value; return true; }
    void *ptr = lab_address(obj, sym->st_value, sym->st_size ? sym->st_size : 1);
    if (!ptr) return lab_fail(err, "Symbol %s is outside mapped segments", lab_string(obj, sym->st_name));
    *value = (uintptr_t)ptr;
    return true;
}

static const Elf64_Sym *find_symbol(LabContext *ctx, const char *name, LabObject **owner) {
    for (size_t i = 0; i < ctx->count; ++i) {
        LabObject *obj = ctx->objects[i];
        for (size_t j = 1; j < obj->symbol_count; ++j) {
            const Elf64_Sym *sym = &obj->symbols[j];
            unsigned binding = ELF64_ST_BIND(sym->st_info), visibility = ELF64_ST_VISIBILITY(sym->st_other);
            if (sym->st_shndx == SHN_UNDEF || (binding != STB_GLOBAL && binding != STB_WEAK) ||
                visibility == STV_HIDDEN || visibility == STV_INTERNAL) continue;
            if (!strcmp(name, lab_string(obj, sym->st_name))) { *owner = obj; return sym; }
        }
    }
    return NULL;
}

bool lab_symbol(LabContext *ctx, const char *name, uint64_t *value, unsigned *type, LabError *err) {
    LabObject *owner = NULL;
    const Elf64_Sym *sym = find_symbol(ctx, name, &owner);
    if (!sym) return lab_fail(err, "Undefined symbol: %s", name);
    *type = ELF64_ST_TYPE(sym->st_info);
    return symbol_address(owner, sym, value, err);
}

bool lab_resolve(LabContext *ctx, LabObject *requester, size_t index, uint64_t *value, LabError *err) {
    if (!index) { *value = 0; return true; }
    if (index >= requester->symbol_count) return lab_fail(err, "Symbol index out of bounds");
    const Elf64_Sym *sym = &requester->symbols[index];
    unsigned binding = ELF64_ST_BIND(sym->st_info), visibility = ELF64_ST_VISIBILITY(sym->st_other);
    if (sym->st_shndx != SHN_UNDEF && (binding == STB_LOCAL || visibility == STV_PROTECTED ||
        visibility == STV_HIDDEN || visibility == STV_INTERNAL))
        return symbol_address(requester, sym, value, err);
    if (sym->st_shndx == SHN_UNDEF && (binding == STB_LOCAL || visibility != STV_DEFAULT))
        return lab_fail(err, "Undefined local or non-default-visibility symbol");
    const char *name = lab_string(requester, sym->st_name);
    LabObject *owner = NULL;
    const Elf64_Sym *definition = find_symbol(ctx, name, &owner);
    if (!definition) {
        if (binding == STB_WEAK) { *value = 0; return true; }
        return lab_fail(err, "%s: undefined strong symbol %s", lab_basename(requester->path), name);
    }
    return symbol_address(owner, definition, value, err);
}

static bool apply_table(LabContext *ctx, LabObject *obj, const Elf64_Rela *table,
                        size_t count, LabError *err) {
    for (size_t i = 0; i < count; ++i) {
        const Elf64_Rela *r = &table[i];
        unsigned type = ELF64_R_TYPE(r->r_info);
        size_t index = ELF64_R_SYM(r->r_info), width;
        uint64_t symbol = 0, result;
        if (type == R_X86_64_NONE) continue;
        if (type == R_X86_64_RELATIVE && index) return lab_fail(err, "RELATIVE relocations must use symbol index zero");
        if (type != R_X86_64_RELATIVE && !lab_resolve(ctx, obj, index, &symbol, err)) return false;
        if (!lab_relocation_value(type, symbol, r->r_addend, obj->bias,
                                  obj->bias + r->r_offset, &result, &width, err)) return false;
        void *target = lab_address(obj, r->r_offset, width);
        if (!target || !lab_virtual_range(obj, r->r_offset, width, PF_W))
            return lab_fail(err, "Relocation target 0x%" PRIx64 " is outside writable PT_LOAD data", r->r_offset);
        memcpy(target, &result, width);
        ctx->relocation_count++;
        lab_trace(ctx, "%s +0x%" PRIx64 " %-22s -> 0x%" PRIx64,
                  lab_basename(obj->path), r->r_offset, lab_reloc_name(type), result);
    }
    return true;
}

static bool validate_hooks(LabContext *ctx, LabObject *obj, LabError *err) {
    const int64_t function_tags[] = { DT_INIT, DT_FINI };
    for (size_t i = 0; i < 2; ++i) {
        uint64_t value;
        if (lab_dynamic_value(obj, function_tags[i], &value) && value &&
            (!lab_address(obj, value, 1) || !lab_executable(ctx, obj->bias + value)))
            return lab_fail(err, "Initializer/finalizer is not in executable memory");
    }
    const int64_t pointer_tags[] = { DT_INIT_ARRAY, DT_FINI_ARRAY };
    const int64_t size_tags[] = { DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ };
    for (size_t i = 0; i < 2; ++i) {
        uint64_t address = 0, bytes = 0;
        lab_dynamic_value(obj, pointer_tags[i], &address);
        lab_dynamic_value(obj, size_tags[i], &bytes);
        if (!bytes) continue;
        if (!address || bytes % 8 || bytes / 8 > 4096 || !lab_virtual_range(obj, address, bytes, PF_R))
            return lab_fail(err, "Invalid initializer/finalizer array");
        const unsigned char *array = lab_address(obj, address, bytes);
        if (!array) return lab_fail(err, "Initializer array is outside the image");
        for (size_t j = 0; j < bytes / 8; ++j) {
            uint64_t function;
            memcpy(&function, array + j * 8, 8);
            if (function && function != UINT64_MAX && !lab_executable(ctx, function))
                return lab_fail(err, "Initializer array contains a non-executable address");
        }
    }
    return true;
}

bool lab_link(LabContext *ctx, LabError *err) {
    for (size_t i = 0; i < ctx->count; ++i) {
        LabObject *obj = ctx->objects[i];
        if (!lab_loadable(obj, err) || !lab_map(obj, err)) return false;
        ctx->mapped_total += obj->mapping_size;
        if (ctx->mapped_total > LAB_MAX_TOTAL) return lab_fail(err, "Total mapped images exceed 256 MiB");
        lab_trace(ctx, "mapped %s: bias=0x%" PRIx64 ", bytes=%zu", lab_basename(obj->path), obj->bias, obj->mapping_size);
    }
    for (size_t i = 0; i < ctx->count; ++i) {
        LabObject *obj = ctx->objects[i];
        if (!apply_table(ctx, obj, obj->rela, obj->rela_count, err) ||
            !apply_table(ctx, obj, obj->plt, obj->plt_count, err)) return false;
    }
    for (size_t i = 0; i < ctx->count; ++i)
        if (!validate_hooks(ctx, ctx->objects[i], err)) return false;
    for (size_t i = 0; i < ctx->count; ++i)
        if (!lab_protect(ctx->objects[i], err)) return false;
    ctx->linked = true;
    return true;
}

static void call_array(LabObject *obj, int64_t tag, int64_t size_tag, bool reverse) {
    uint64_t address = 0, bytes = 0;
    lab_dynamic_value(obj, tag, &address); lab_dynamic_value(obj, size_tag, &bytes);
    const unsigned char *array = lab_address(obj, address, bytes);
    for (size_t n = 0; n < bytes / 8; ++n) {
        size_t i = reverse ? bytes / 8 - n - 1 : n;
        uint64_t function;
        memcpy(&function, array + i * 8, 8);
        if (function && function != UINT64_MAX) lab_call0(function);
    }
}

static void initialize_object(LabContext *ctx, size_t index) {
    LabObject *obj = ctx->objects[index];
    if (obj->init_state) return;  // Visiting nodes break dependency cycles deterministically.
    obj->init_state = 1;
    for (size_t i = 0; i < obj->needed_count; ++i) initialize_object(ctx, obj->deps[i]);
    uint64_t function;
    lab_trace(ctx, "init %s", lab_basename(obj->path));
    if (lab_dynamic_value(obj, DT_INIT, &function) && function) lab_call0(obj->bias + function);
    call_array(obj, DT_INIT_ARRAY, DT_INIT_ARRAYSZ, false);
    obj->init_state = 2;
    ctx->init_order[ctx->init_count++] = index;
}

bool lab_initialize(LabContext *ctx, LabError *err) {
    if (!ctx->linked) return lab_fail(err, "Link the graph before initialization");
    initialize_object(ctx, 0);
    return true;
}

void lab_finalize(LabContext *ctx) {
    while (ctx->init_count) {
        LabObject *obj = ctx->objects[ctx->init_order[--ctx->init_count]];
        lab_trace(ctx, "fini %s", lab_basename(obj->path));
        call_array(obj, DT_FINI_ARRAY, DT_FINI_ARRAYSZ, true);
        uint64_t function;
        if (lab_dynamic_value(obj, DT_FINI, &function) && function) lab_call0(obj->bias + function);
        obj->init_state = 0;
    }
}

void lab_context_destroy(LabContext *ctx) {
    for (size_t i = 0; i < ctx->count; ++i) lab_object_destroy(ctx->objects[i]);
    ctx->count = 0; ctx->linked = false;
}
