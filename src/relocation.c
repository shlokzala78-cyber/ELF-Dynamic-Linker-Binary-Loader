#include "elflab.h"
#include <inttypes.h>

bool lab_relocation_value(unsigned type, uint64_t symbol, int64_t addend,
                          uint64_t base, uint64_t place, uint64_t *result,
                          size_t *width, LabError *err) {
    __int128 value;
    *width = 8;
    switch (type) {
        case R_X86_64_NONE: *width = 0; *result = 0; return true;
        case R_X86_64_RELATIVE: *result = base + (uint64_t)addend; return true;
        case R_X86_64_64: *result = symbol + (uint64_t)addend; return true;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: *result = symbol; return true;
        case R_X86_64_PC32:
            value = (__int128)symbol + addend - place;
            if (value < INT32_MIN || value > INT32_MAX) return lab_fail(err, "R_X86_64_PC32 overflow");
            *result = (uint32_t)(int32_t)value; *width = 4; return true;
        case R_X86_64_32:
            value = (__int128)symbol + addend;
            if (value < 0 || value > UINT32_MAX) return lab_fail(err, "R_X86_64_32 overflow");
            *result = (uint32_t)value; *width = 4; return true;
        case R_X86_64_32S:
            value = (__int128)symbol + addend;
            if (value < INT32_MIN || value > INT32_MAX) return lab_fail(err, "R_X86_64_32S overflow");
            *result = (uint32_t)(int32_t)value; *width = 4; return true;
        default: return lab_fail(err, "Unsupported relocation %s (%u)", lab_reloc_name(type), type);
    }
}
