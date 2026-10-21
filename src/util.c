#include "elflab.h"
#include <stdarg.h>
#include <string.h>

bool lab_fail(LabError *err, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(err->text, sizeof(err->text), format, args);
    va_end(args);
    return false;
}

bool lab_range(uint64_t offset, uint64_t length, uint64_t total) {
    return offset <= total && length <= total - offset;
}

const char *lab_basename(const char *path) {
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

const char *lab_reloc_name(unsigned type) {
    switch (type) {
        case R_X86_64_NONE: return "R_X86_64_NONE";
        case R_X86_64_64: return "R_X86_64_64";
        case R_X86_64_PC32: return "R_X86_64_PC32";
        case R_X86_64_GLOB_DAT: return "R_X86_64_GLOB_DAT";
        case R_X86_64_JUMP_SLOT: return "R_X86_64_JUMP_SLOT";
        case R_X86_64_RELATIVE: return "R_X86_64_RELATIVE";
        case R_X86_64_32: return "R_X86_64_32";
        case R_X86_64_32S: return "R_X86_64_32S";
        case R_X86_64_COPY: return "R_X86_64_COPY";
        case R_X86_64_IRELATIVE: return "R_X86_64_IRELATIVE";
        default: return "UNSUPPORTED";
    }
}

void lab_permissions(unsigned flags, char out[4]) {
    out[0] = flags & PF_R ? 'r' : '-';
    out[1] = flags & PF_W ? 'w' : '-';
    out[2] = flags & PF_X ? 'x' : '-';
    out[3] = '\0';
}

void lab_trace(const LabContext *ctx, const char *format, ...) {
    if (!ctx->trace) return;
    va_list args;
    fputs("[elflab] ", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

void lab_quoted(FILE *stream, const char *text) {
    fputc('"', stream);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') fprintf(stream, "\\%c", *p);
        else if (*p < 32 || *p == 127) fprintf(stream, "\\x%02x", *p);
        else fputc(*p, stream);
    }
    fputc('"', stream);
}
