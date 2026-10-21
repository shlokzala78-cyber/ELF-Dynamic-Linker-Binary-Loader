#include "elflab.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int protection(unsigned flags) {
    return ((flags & PF_R) ? PROT_READ : 0) |
           ((flags & PF_W) ? PROT_WRITE : 0) |
           ((flags & PF_X) ? PROT_EXEC : 0);
}

bool lab_map(LabObject *obj, LabError *err) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || ((unsigned long)page & ((unsigned long)page - 1)))
        return lab_fail(err, "Unsupported system page size");
    obj->page_size = (size_t)page;
    uint64_t low = UINT64_MAX, high = 0, mask = obj->page_size - 1;
    for (size_t i = 0; i < obj->eh.e_phnum; ++i) {
        const Elf64_Phdr *ph = &obj->ph[i];
        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        if (ph->p_align > obj->page_size) return lab_fail(err, "PT_LOAD alignment larger than the system page size is unsupported");
        uint64_t end = ph->p_vaddr + ph->p_memsz;
        if (end > UINT64_MAX - mask) return lab_fail(err, "PT_LOAD page rounding overflow");
        uint64_t begin = ph->p_vaddr & ~mask;
        end = (end + mask) & ~mask;
        if (begin < low) low = begin;
        if (end > high) high = end;
    }
    if (low == UINT64_MAX || high <= low || high - low > LAB_MAX_IMAGE)
        return lab_fail(err, "Mapped image must occupy 1 byte..64 MiB");
    obj->low = low; obj->mapping_size = high - low;
    obj->page_count = obj->mapping_size / obj->page_size;
    obj->permissions = calloc(obj->page_count, 1);
    if (!obj->permissions) return lab_fail(err, "Out of memory tracking page permissions");
    for (size_t i = 0; i < obj->eh.e_phnum; ++i) {
        const Elf64_Phdr *ph = &obj->ph[i];
        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        size_t first = (ph->p_vaddr - low) / obj->page_size;
        size_t last = (ph->p_vaddr + ph->p_memsz - 1 - low) / obj->page_size;
        for (size_t p = first; p <= last; ++p) {
            obj->permissions[p] |= (unsigned char)((ph->p_flags & 7) | 0x80);
            if ((obj->permissions[p] & (PF_W | PF_X)) == (PF_W | PF_X))
                return lab_fail(err, "W^X policy rejects a writable and executable page (including shared segment pages)");
        }
    }
    void *mapping = mmap(NULL, obj->mapping_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return lab_fail(err, "mmap failed: %s", strerror(errno));
    obj->mapping = mapping; obj->bias = (uint64_t)(uintptr_t)mapping - low;
    // Reserve holes as PROT_NONE; only occupied pages become writable for copying.
    for (size_t p = 0; p < obj->page_count; ++p)
        if (obj->permissions[p] && mprotect(obj->mapping + p * obj->page_size, obj->page_size, PROT_READ | PROT_WRITE))
            return lab_fail(err, "Preparing segment pages failed: %s", strerror(errno));
    for (size_t i = 0; i < obj->eh.e_phnum; ++i) {
        const Elf64_Phdr *ph = &obj->ph[i];
        if (ph->p_type == PT_LOAD && ph->p_filesz)
            memcpy(obj->mapping + (ph->p_vaddr - low), obj->bytes + ph->p_offset, ph->p_filesz);
    }
    // Anonymous mappings already zero the p_memsz - p_filesz BSS tails.
    return true;
}

void *lab_address(const LabObject *obj, uint64_t address, size_t size) {
    if (!obj->mapping || address < obj->low ||
        !lab_range(address - obj->low, size, obj->mapping_size) ||
        !lab_virtual_range(obj, address, size, 0)) return NULL;
    return obj->mapping + (address - obj->low);
}

bool lab_protect(LabObject *obj, LabError *err) {
    uint64_t mask = obj->page_size - 1;
    for (size_t i = 0; i < obj->eh.e_phnum; ++i) {
        const Elf64_Phdr *ph = &obj->ph[i];
        if (ph->p_type != PT_GNU_RELRO || !ph->p_memsz) continue;
        if (!lab_virtual_range(obj, ph->p_vaddr, ph->p_memsz, PF_R))
            return lab_fail(err, "PT_GNU_RELRO is outside readable loadable memory");
        uint64_t first = ph->p_vaddr & ~mask, end = (ph->p_vaddr + ph->p_memsz) & ~mask;
        for (uint64_t address = first; address < end; address += obj->page_size)
            obj->permissions[(address - obj->low) / obj->page_size] &= (unsigned char)~PF_W;
    }
    for (size_t first = 0; first < obj->page_count;) {
        size_t end = first + 1;
        while (end < obj->page_count && obj->permissions[end] == obj->permissions[first]) ++end;
        if (mprotect(obj->mapping + first * obj->page_size, (end - first) * obj->page_size,
                     protection(obj->permissions[first])))
            return lab_fail(err, "mprotect failed: %s", strerror(errno));
        first = end;
    }
    return true;
}

bool lab_executable(const LabContext *ctx, uint64_t address) {
    for (size_t i = 0; i < ctx->count; ++i) {
        const LabObject *obj = ctx->objects[i];
        uint64_t start = (uintptr_t)obj->mapping;
        if (!obj->mapping || address < start || address - start >= obj->mapping_size) continue;
        uint64_t offset = address - start;
        if ((obj->permissions[offset / obj->page_size] & PF_X) &&
            lab_virtual_range(obj, obj->low + offset, 1, PF_X)) return true;
    }
    return false;
}
