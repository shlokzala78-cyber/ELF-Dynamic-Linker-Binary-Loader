#include "elflab.h"
#include <stdlib.h>
#include <string.h>

bool lab_dynamic_value(const LabObject *obj, int64_t tag, uint64_t *value) {
    for (size_t i = 0; i < obj->dynamic_count; ++i) {
        if (obj->dynamic[i].d_tag == tag) {
            *value = obj->dynamic[i].d_un.d_val;
            return true;
        }
    }
    return false;
}

const char *lab_string(const LabObject *obj, uint64_t offset) {
    if (!obj->strings || offset >= obj->strings_size) return NULL;
    const char *text = obj->strings + offset;
    return memchr(text, '\0', obj->strings_size - offset) ? text : NULL;
}

static void unsupported(LabObject *obj, const char *feature) {
    if (!obj->unsupported[0]) snprintf(obj->unsupported, sizeof(obj->unsupported), "%s", feature);
}

static bool word(const LabObject *obj, uint64_t address, uint32_t *value) {
    const void *ptr = lab_file_address(obj, address, sizeof(*value));
    if (!ptr) return false;
    memcpy(value, ptr, sizeof(*value));
    return true;
}

static bool symbol_count(LabObject *obj, LabError *err) {
    uint64_t address;
    if (lab_dynamic_value(obj, DT_HASH, &address)) {
        uint32_t buckets, chains;
        if (address > UINT64_MAX - 8 || !word(obj, address, &buckets) || !word(obj, address + 4, &chains) ||
            !buckets || buckets > LAB_MAX_SYMBOLS || !chains || chains > LAB_MAX_SYMBOLS ||
            !lab_file_address(obj, address, (2ull + buckets + chains) * 4))
            return lab_fail(err, "Invalid SysV hash table");
        obj->symbol_count = chains;
        return true;
    }
    if (!lab_dynamic_value(obj, DT_GNU_HASH, &address)) return lab_fail(err, "Dynamic symbols require a SysV or GNU hash table");
    uint32_t h[4];
    const void *ptr = lab_file_address(obj, address, sizeof(h));
    if (!ptr) return lab_fail(err, "Truncated GNU hash header");
    memcpy(h, ptr, sizeof(h));
    uint32_t buckets = h[0], first = h[1], bloom = h[2];
    if (!buckets || buckets > LAB_MAX_SYMBOLS || !bloom || bloom > LAB_MAX_SYMBOLS ||
        (bloom & (bloom - 1)) || first > LAB_MAX_SYMBOLS || h[3] >= 64)
        return lab_fail(err, "Invalid GNU hash dimensions");
    uint64_t prefix = 16ull + bloom * 8ull + buckets * 4ull;
    if (address > UINT64_MAX - prefix || !lab_file_address(obj, address, prefix))
        return lab_fail(err, "Truncated GNU hash buckets");
    uint64_t bucket_address = address + 16 + bloom * 8ull;
    uint32_t max_bucket = 0;
    for (uint32_t i = 0; i < buckets; ++i) {
        uint32_t index;
        if (!word(obj, bucket_address + i * 4ull, &index)) return lab_fail(err, "Invalid GNU hash bucket");
        if (index && (index < first || index >= LAB_MAX_SYMBOLS)) return lab_fail(err, "GNU hash symbol index out of bounds");
        if (index > max_bucket) max_bucket = index;
    }
    obj->symbol_count = first;
    if (!max_bucket) return first ? true : lab_fail(err, "GNU hash has no null symbol");
    uint64_t chain_address = address + prefix;
    for (uint32_t index = max_bucket; index < LAB_MAX_SYMBOLS; ++index) {
        uint64_t delta = (uint64_t)(index - first) * 4;
        uint32_t hash;
        if (chain_address > UINT64_MAX - delta || !word(obj, chain_address + delta, &hash))
            return lab_fail(err, "Truncated GNU hash chain");
        if (hash & 1) { obj->symbol_count = index + 1; return true; }
    }
    return lab_fail(err, "Unterminated GNU hash chain");
}

static bool relocation_table(LabObject *obj, uint64_t address, uint64_t bytes,
                             Elf64_Rela **out, size_t *count, LabError *err) {
    if (bytes % sizeof(Elf64_Rela) || bytes / sizeof(Elf64_Rela) > LAB_MAX_RELOCS)
        return lab_fail(err, "Invalid RELA table size");
    if (!bytes) return true;
    const void *raw = lab_file_address(obj, address, bytes);
    if (!raw) return lab_fail(err, "RELA table exceeds file-backed PT_LOAD data");
    *out = malloc(bytes);
    if (!*out) return lab_fail(err, "Out of memory reading relocations");
    memcpy(*out, raw, bytes);
    *count = bytes / sizeof(Elf64_Rela);
    for (size_t i = 0; i < *count; ++i)
        if (ELF64_R_SYM((*out)[i].r_info) >= obj->symbol_count)
            return lab_fail(err, "Relocation symbol index out of bounds");
    return true;
}

bool lab_parse_dynamic(LabObject *obj, LabError *err) {
    const Elf64_Phdr *segment = NULL;
    for (size_t i = 0; i < obj->eh.e_phnum; ++i)
        if (obj->ph[i].p_type == PT_DYNAMIC) segment = &obj->ph[i];
    if (!segment) return true;
    if (!segment->p_filesz || segment->p_filesz % sizeof(Elf64_Dyn) ||
        segment->p_filesz / sizeof(Elf64_Dyn) > 512)
        return lab_fail(err, "Invalid dynamic table size");
    const void *raw = lab_file_address(obj, segment->p_vaddr, segment->p_filesz);
    if (!raw || raw != obj->bytes + segment->p_offset)
        return lab_fail(err, "PT_DYNAMIC must agree with file-backed PT_LOAD data");
    obj->dynamic = malloc(segment->p_filesz);
    if (!obj->dynamic) return lab_fail(err, "Out of memory reading dynamic entries");
    memcpy(obj->dynamic, raw, segment->p_filesz);
    bool terminated = false;
    for (size_t i = 0; i < segment->p_filesz / sizeof(Elf64_Dyn); ++i) {
        int64_t tag = obj->dynamic[i].d_tag;
        if (tag == DT_NULL) { terminated = true; break; }
        for (size_t j = 0; j < i; ++j)
            if (tag != DT_NEEDED && tag == obj->dynamic[j].d_tag)
                return lab_fail(err, "Duplicate singleton dynamic tag");
        obj->dynamic_count++;
        switch (tag) {
            case DT_REL: case DT_RELSZ: case DT_RELENT: unsupported(obj, "REL relocations"); break;
            case DT_RELR: case DT_RELRSZ: case DT_RELRENT: unsupported(obj, "packed RELR relocations"); break;
            case DT_VERSYM: case DT_VERNEED: case DT_VERDEF: unsupported(obj, "symbol versioning"); break;
            case DT_TEXTREL: unsupported(obj, "text relocations"); break;
            case DT_SYMBOLIC: unsupported(obj, "DT_SYMBOLIC binding"); break;
            case DT_FILTER: case DT_AUXILIARY: unsupported(obj, "filter/auxiliary objects"); break;
            case DT_PREINIT_ARRAY: case DT_PREINIT_ARRAYSZ: unsupported(obj, "preinit arrays"); break;
            case DT_FLAGS:
                if (obj->dynamic[i].d_un.d_val & ~((uint64_t)DF_BIND_NOW))
                    unsupported(obj, "dynamic flags other than BIND_NOW");
                break;
            case DT_FLAGS_1:
                if (obj->dynamic[i].d_un.d_val & ~((uint64_t)DF_1_NOW | DF_1_PIE))
                    unsupported(obj, "unsupported DT_FLAGS_1 semantics");
                break;
            default: break;
        }
    }
    if (!terminated) return lab_fail(err, "Dynamic table is missing DT_NULL");
    uint64_t strings = 0, size = 0, symbols = 0, entry = 0;
    if (!lab_dynamic_value(obj, DT_STRTAB, &strings) || !lab_dynamic_value(obj, DT_STRSZ, &size) ||
        !size || !(obj->strings = lab_file_address(obj, strings, size)))
        return lab_fail(err, "Invalid dynamic string table");
    obj->strings_size = size;
    if (obj->strings[0] != '\0') return lab_fail(err, "Dynamic string table must start with a null byte");
    if (!lab_dynamic_value(obj, DT_SYMTAB, &symbols) ||
        !lab_dynamic_value(obj, DT_SYMENT, &entry) || entry != sizeof(Elf64_Sym) ||
        !symbol_count(obj, err)) {
        if (!err->text[0]) lab_fail(err, "Invalid dynamic symbol table");
        return false;
    }
    raw = lab_file_address(obj, symbols, obj->symbol_count * sizeof(Elf64_Sym));
    if (!raw) return lab_fail(err, "Dynamic symbol table exceeds file-backed data");
    obj->symbols = malloc(obj->symbol_count * sizeof(Elf64_Sym));
    if (!obj->symbols) return lab_fail(err, "Out of memory reading symbols");
    memcpy(obj->symbols, raw, obj->symbol_count * sizeof(Elf64_Sym));
    for (size_t i = 0; i < obj->symbol_count; ++i)
        if (!lab_string(obj, obj->symbols[i].st_name)) return lab_fail(err, "Invalid symbol name offset or unterminated string");
    for (size_t i = 0; i < obj->dynamic_count; ++i) {
        int64_t tag = obj->dynamic[i].d_tag;
        if (tag == DT_NEEDED || tag == DT_SONAME || tag == DT_RPATH || tag == DT_RUNPATH) {
            const char *name = lab_string(obj, obj->dynamic[i].d_un.d_val);
            if (!name) return lab_fail(err, "Invalid dependency or path string");
            if (tag == DT_SONAME) obj->soname = name;
            if (tag == DT_NEEDED) {
                if (obj->needed_count == LAB_MAX_NEEDED) return lab_fail(err, "Too many DT_NEEDED entries");
                obj->needed[obj->needed_count++] = name;
            }
        }
    }
    uint64_t rela = 0, relasz = 0, relaent = 0, plt = 0, pltsz = 0, plttype = 0;
    lab_dynamic_value(obj, DT_RELA, &rela);
    lab_dynamic_value(obj, DT_RELASZ, &relasz);
    lab_dynamic_value(obj, DT_RELAENT, &relaent);
    lab_dynamic_value(obj, DT_JMPREL, &plt);
    lab_dynamic_value(obj, DT_PLTRELSZ, &pltsz);
    lab_dynamic_value(obj, DT_PLTREL, &plttype);
    if ((rela || relasz) && (!rela || relaent != sizeof(Elf64_Rela)))
        return lab_fail(err, "RELA entries require DT_RELA and the correct DT_RELAENT");
    if (pltsz && !plt) return lab_fail(err, "PLT relocation table address is missing");
    if (pltsz && plttype != DT_RELA) { unsupported(obj, "non-RELA PLT entries"); pltsz = 0; }
    if (relasz && pltsz && rela <= UINT64_MAX - relasz && plt <= UINT64_MAX - pltsz &&
        rela < plt + pltsz && plt < rela + relasz) return lab_fail(err, "Overlapping RELA and PLT tables");
    return relocation_table(obj, rela, relasz, &obj->rela, &obj->rela_count, err) &&
           relocation_table(obj, plt, pltsz, &obj->plt, &obj->plt_count, err);
}
