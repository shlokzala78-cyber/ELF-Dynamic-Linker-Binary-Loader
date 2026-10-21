#include "elflab.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static bool headers(LabObject *obj, LabError *err) {
    if (obj->size < sizeof(Elf64_Ehdr)) return lab_fail(err, "Truncated ELF header");
    memcpy(&obj->eh, obj->bytes, sizeof(obj->eh));
    const Elf64_Ehdr *eh = &obj->eh;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG)) return lab_fail(err, "Not an ELF file");
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB)
        return lab_fail(err, "Only ELF64 little-endian files are supported");
    if (eh->e_ident[EI_VERSION] != EV_CURRENT || eh->e_version != EV_CURRENT)
        return lab_fail(err, "Unsupported ELF version");
    if (eh->e_machine != EM_X86_64) return lab_fail(err, "Only x86-64 ELF files are supported");
    if (eh->e_type != ET_DYN && eh->e_type != ET_EXEC)
        return lab_fail(err, "Expected ET_DYN or ET_EXEC, not a relocatable .o file");
    if (eh->e_ehsize != sizeof(*eh) || eh->e_phentsize != sizeof(Elf64_Phdr))
        return lab_fail(err, "Invalid ELF or program-header entry size");
    if (!eh->e_phnum || eh->e_phnum > LAB_MAX_PHDRS)
        return lab_fail(err, "Program-header count must be 1..%d (extended numbering unsupported)", LAB_MAX_PHDRS);
    if (!lab_range(eh->e_phoff, (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr), obj->size))
        return lab_fail(err, "Program-header table exceeds the file");
    memcpy(obj->ph, obj->bytes + eh->e_phoff, eh->e_phnum * sizeof(Elf64_Phdr));
    unsigned loads = 0, dynamics = 0;
    for (size_t i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr *ph = &obj->ph[i];
        if (!lab_range(ph->p_offset, ph->p_filesz, obj->size))
            return lab_fail(err, "Segment %zu exceeds the file", i);
        if (ph->p_type == PT_DYNAMIC && ++dynamics > 1)
            return lab_fail(err, "Multiple PT_DYNAMIC segments are unsupported");
        if (ph->p_type != PT_LOAD) continue;
        ++loads;
        if (ph->p_filesz > ph->p_memsz || ph->p_vaddr > UINT64_MAX - ph->p_memsz)
            return lab_fail(err, "Invalid PT_LOAD file/memory sizes");
        uint64_t alignment = ph->p_align;
        if (alignment > 1 && ((alignment & (alignment - 1)) ||
            ph->p_vaddr % alignment != ph->p_offset % alignment))
            return lab_fail(err, "Invalid PT_LOAD alignment");
        for (size_t j = 0; j < i; ++j) {
            const Elf64_Phdr *other = &obj->ph[j];
            if (other->p_type == PT_LOAD && ph->p_memsz && other->p_memsz &&
                ph->p_vaddr < other->p_vaddr + other->p_memsz &&
                other->p_vaddr < ph->p_vaddr + ph->p_memsz)
                return lab_fail(err, "Overlapping PT_LOAD byte ranges are unsupported");
        }
    }
    if (!loads) return lab_fail(err, "ELF file has no PT_LOAD segments");
    return true;
}

bool lab_object_read(LabObject *obj, const char *path, LabError *err) {
    if (!realpath(path, obj->path)) return lab_fail(err, "Cannot resolve %s: %s", path, strerror(errno));
    int fd = open(obj->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return lab_fail(err, "Cannot open %s: %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > LAB_MAX_FILE) {
        close(fd);
        return lab_fail(err, "Input must be a nonempty regular file of at most 64 MiB");
    }
    obj->size = (size_t)st.st_size;
    obj->bytes = malloc(obj->size);
    if (!obj->bytes) { close(fd); return lab_fail(err, "Out of memory reading ELF"); }
    size_t offset = 0;
    while (offset < obj->size) {
        ssize_t count = read(fd, obj->bytes + offset, obj->size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); return lab_fail(err, "ELF read failed or file was truncated"); }
        offset += (size_t)count;
    }
    close(fd);
    // A private byte snapshot avoids SIGBUS if a mapped input file is truncated later.
    return headers(obj, err) && lab_parse_dynamic(obj, err);
}

const void *lab_file_address(const LabObject *obj, uint64_t address, size_t size) {
    for (size_t i = 0; i < obj->eh.e_phnum; ++i) {
        const Elf64_Phdr *ph = &obj->ph[i];
        if (ph->p_type == PT_LOAD && address >= ph->p_vaddr &&
            lab_range(address - ph->p_vaddr, size, ph->p_filesz))
            return obj->bytes + ph->p_offset + (address - ph->p_vaddr);
    }
    return NULL;
}

bool lab_virtual_range(const LabObject *obj, uint64_t address, size_t size, unsigned required) {
    for (size_t i = 0; i < obj->eh.e_phnum; ++i) {
        const Elf64_Phdr *ph = &obj->ph[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & required) == required &&
            address >= ph->p_vaddr && lab_range(address - ph->p_vaddr, size, ph->p_memsz)) return true;
    }
    return false;
}

void lab_object_destroy(LabObject *obj) {
    if (!obj) return;
    if (obj->mapping) munmap(obj->mapping, obj->mapping_size);
    free(obj->permissions);
    free(obj->bytes);
    free(obj->dynamic);
    free(obj->symbols);
    free(obj->rela);
    free(obj->plt);
    free(obj);
}

bool lab_loadable(const LabObject *obj, LabError *err) {
    if (obj->eh.e_type != ET_DYN) return lab_fail(err, "%s: execution requires ET_DYN position-independent modules", lab_basename(obj->path));
    if (obj->eh.e_ident[EI_OSABI] != ELFOSABI_NONE && obj->eh.e_ident[EI_OSABI] != ELFOSABI_LINUX)
        return lab_fail(err, "Unsupported ELF OS ABI");
    if (!obj->dynamic_count) return lab_fail(err, "Runnable modules must contain PT_DYNAMIC");
    if (obj->unsupported[0]) return lab_fail(err, "%s: unsupported feature: %s", lab_basename(obj->path), obj->unsupported);
    for (size_t i = 0; i < obj->eh.e_phnum; ++i) {
        const Elf64_Phdr *ph = &obj->ph[i];
        if (ph->p_type == PT_INTERP) return lab_fail(err, "PT_INTERP is unsupported; use the provided freestanding module build flags");
        if (ph->p_type == PT_TLS && ph->p_memsz) return lab_fail(err, "Thread-local storage (PT_TLS) is unsupported");
        if (ph->p_type == PT_GNU_STACK && (ph->p_flags & PF_X)) return lab_fail(err, "Executable stacks are unsupported");
    }
    for (size_t i = 1; i < obj->symbol_count; ++i) {
        const Elf64_Sym *sym = &obj->symbols[i];
        unsigned type = ELF64_ST_TYPE(sym->st_info), bind = ELF64_ST_BIND(sym->st_info);
        if (type == STT_TLS || type == STT_GNU_IFUNC || bind == STB_GNU_UNIQUE)
            return lab_fail(err, "TLS, GNU IFUNC, and GNU UNIQUE symbols are unsupported");
        if (sym->st_shndx == SHN_COMMON || sym->st_shndx == SHN_XINDEX)
            return lab_fail(err, "COMMON and extended symbol section indices are unsupported");
        if (sym->st_shndx != SHN_UNDEF && sym->st_shndx != SHN_ABS &&
            !lab_virtual_range(obj, sym->st_value, sym->st_size ? sym->st_size : 1, 0))
            return lab_fail(err, "Defined symbol %s is outside loadable memory", lab_string(obj, sym->st_name));
    }
    return true;
}
