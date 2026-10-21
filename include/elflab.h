#ifndef ELFLAB_H
#define ELFLAB_H

#include <elf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

#define LAB_MAX_OBJECTS 32
#define LAB_MAX_NEEDED 32
#define LAB_MAX_PHDRS 128
#define LAB_MAX_SYMBOLS 65536
#define LAB_MAX_RELOCS 131072
#define LAB_MAX_FILE (64u * 1024u * 1024u)
#define LAB_MAX_IMAGE (64u * 1024u * 1024u)
#define LAB_MAX_TOTAL (256u * 1024u * 1024u)

typedef struct { char text[768]; } LabError;
typedef struct {
    char path[PATH_MAX];
    unsigned char *bytes;
    size_t size;
    Elf64_Ehdr eh;
    Elf64_Phdr ph[LAB_MAX_PHDRS];
    Elf64_Dyn *dynamic;
    size_t dynamic_count;
    const char *strings;
    size_t strings_size;
    Elf64_Sym *symbols;
    size_t symbol_count;
    Elf64_Rela *rela, *plt;
    size_t rela_count, plt_count;
    const char *needed[LAB_MAX_NEEDED];
    size_t needed_count;
    const char *soname;
    char unsupported[192];
    size_t deps[LAB_MAX_NEEDED];
    unsigned char *mapping, *permissions;
    size_t mapping_size, page_size, page_count;
    uint64_t low, bias;
    unsigned init_state;
} LabObject;

typedef struct {
    LabObject *objects[LAB_MAX_OBJECTS];
    size_t count, mapped_total;
    const char *directories[16];
    size_t directory_count;
    char root_directory[PATH_MAX];
    size_t init_order[LAB_MAX_OBJECTS], init_count;
    size_t relocation_count;
    bool trace, linked;
} LabContext;

bool lab_fail(LabError *err, const char *format, ...);
bool lab_range(uint64_t offset, uint64_t length, uint64_t total);
const char *lab_basename(const char *path);
const char *lab_reloc_name(unsigned type);
void lab_permissions(unsigned flags, char out[4]);
void lab_trace(const LabContext *ctx, const char *format, ...);
void lab_quoted(FILE *stream, const char *text);

bool lab_object_read(LabObject *obj, const char *path, LabError *err);
bool lab_parse_dynamic(LabObject *obj, LabError *err);
void lab_object_destroy(LabObject *obj);
const void *lab_file_address(const LabObject *obj, uint64_t address, size_t size);
bool lab_virtual_range(const LabObject *obj, uint64_t address, size_t size, unsigned required_flags);
bool lab_dynamic_value(const LabObject *obj, int64_t tag, uint64_t *value);
const char *lab_string(const LabObject *obj, uint64_t offset);
bool lab_loadable(const LabObject *obj, LabError *err);

bool lab_map(LabObject *obj, LabError *err);
void *lab_address(const LabObject *obj, uint64_t address, size_t size);
bool lab_protect(LabObject *obj, LabError *err);
bool lab_executable(const LabContext *ctx, uint64_t address);

bool lab_graph(LabContext *ctx, const char *path, LabError *err);
bool lab_resolve(LabContext *ctx, LabObject *requester, size_t index, uint64_t *value, LabError *err);
bool lab_symbol(LabContext *ctx, const char *name, uint64_t *value, unsigned *type, LabError *err);
bool lab_link(LabContext *ctx, LabError *err);
bool lab_initialize(LabContext *ctx, LabError *err);
void lab_finalize(LabContext *ctx);
void lab_context_destroy(LabContext *ctx);

bool lab_relocation_value(unsigned type, uint64_t symbol, int64_t addend,
                          uint64_t base, uint64_t place, uint64_t *result,
                          size_t *width, LabError *err);
void lab_inspect(const LabObject *obj);
void lab_print_symbols(const LabObject *obj);
void lab_print_relocations(const LabObject *obj);
void lab_print_graph(const LabContext *ctx, bool dot);
void lab_print_maps(const LabContext *ctx);

long lab_call2(uint64_t address, long first, long second);
void lab_call0(uint64_t address);

#endif
