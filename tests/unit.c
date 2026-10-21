#include "elflab.h"
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static void relocation_tests(void) {
    LabError err = {{0}};
    uint64_t result;
    size_t width;
    CHECK(lab_relocation_value(R_X86_64_RELATIVE, 0, 24, 0x1000, 0, &result, &width, &err));
    CHECK(result == 0x1018 && width == 8);
    CHECK(lab_relocation_value(R_X86_64_64, 100, -7, 0, 0, &result, &width, &err));
    CHECK(result == 93);
    CHECK(lab_relocation_value(R_X86_64_GLOB_DAT, 100, 0, 0, 0, &result, &width, &err));
    CHECK(result == 100);
    CHECK(lab_relocation_value(R_X86_64_JUMP_SLOT, 777, 0, 0, 0, &result, &width, &err));
    CHECK(result == 777);
    CHECK(lab_relocation_value(R_X86_64_PC32, 0x1000, -4, 0, 0x1100, &result, &width, &err));
    CHECK(result == (uint32_t)-260 && width == 4);
    CHECK(!lab_relocation_value(R_X86_64_PC32, UINT64_MAX, 0, 0, 0, &result, &width, &err));
    CHECK(lab_relocation_value(R_X86_64_32, UINT32_MAX, 0, 0, 0, &result, &width, &err));
    CHECK(result == UINT32_MAX && width == 4);
    CHECK(!lab_relocation_value(R_X86_64_32, UINT32_MAX, 1, 0, 0, &result, &width, &err));
    CHECK(!lab_relocation_value(R_X86_64_32, 0, -1, 0, 0, &result, &width, &err));
    CHECK(lab_relocation_value(R_X86_64_32S, 0, INT32_MIN, 0, 0, &result, &width, &err));
    CHECK(result == (uint32_t)INT32_MIN);
    CHECK(!lab_relocation_value(R_X86_64_32S, INT32_MAX, 1, 0, 0, &result, &width, &err));
    CHECK(lab_relocation_value(R_X86_64_NONE, 0, 0, 0, 0, &result, &width, &err));
    CHECK(width == 0);
    CHECK(!lab_relocation_value(R_X86_64_COPY, 0, 0, 0, 0, &result, &width, &err));
    CHECK(!lab_relocation_value(R_X86_64_IRELATIVE, 0, 0, 0, 0, &result, &width, &err));
    CHECK(lab_range(UINT64_MAX, 0, UINT64_MAX));
    CHECK(!lab_range(UINT64_MAX, 1, UINT64_MAX));
    CHECK(!lab_range(0, UINT64_MAX, 64));
}

static void kernel_permissions(const LabObject *obj) {
    for (size_t page = 0; page < obj->page_count; ++page) {
        FILE *maps = fopen("/proc/self/maps", "r");
        CHECK(maps != NULL);
        unsigned long address = (unsigned long)(obj->mapping + page * obj->page_size);
        char line[1024], actual[5], expected[4];
        bool found = false;
        lab_permissions(obj->permissions[page], expected);
        while (fgets(line, sizeof(line), maps)) {
            unsigned long start, end;
            if (sscanf(line, "%lx-%lx %4s", &start, &end, actual) == 3 && address >= start && address < end) {
                CHECK(strncmp(actual, expected, 3) == 0);
                CHECK(!(actual[1] == 'w' && actual[2] == 'x'));
                found = true;
                break;
            }
        }
        fclose(maps); CHECK(found);
    }
}

static long read_global(LabContext *ctx, const char *name) {
    uint64_t address;
    unsigned type;
    LabError err = {{0}};
    CHECK(lab_symbol(ctx, name, &address, &type, &err));
    CHECK(type == STT_OBJECT);
    long value;
    memcpy(&value, (void *)(uintptr_t)address, sizeof(value));
    return value;
}

static void graph_tests(const char *path) {
    LabContext ctx = {0}; LabError err = {{0}};
    CHECK(lab_graph(&ctx, path, &err));
    CHECK(ctx.count == 3);
    CHECK(ctx.objects[0]->deps[0] == 1 && ctx.objects[1]->deps[0] == 2);
    if (!lab_link(&ctx, &err)) { fprintf(stderr, "%s\n", err.text); exit(1); }
    CHECK(ctx.relocation_count >= 10);
    CHECK(read_global(&ctx, "base_ready") == 0);
    CHECK(read_global(&ctx, "math_ready") == 0);
    uint64_t bss;
    unsigned type;
    CHECK(lab_symbol(&ctx, "base_bss", &bss, &type, &err));
    const long *zeros = (const long *)(uintptr_t)bss;
    for (size_t i = 0; i < 1024; ++i) CHECK(zeros[i] == 0);
    for (size_t i = 0; i < ctx.count; ++i) kernel_permissions(ctx.objects[i]);
    CHECK(lab_initialize(&ctx, &err));
    CHECK(read_global(&ctx, "base_ready") == 2);
    CHECK(read_global(&ctx, "math_ready") == 5);
    uint64_t entry;
    CHECK(lab_symbol(&ctx, "demo_entry", &entry, &type, &err));
    CHECK(type == STT_FUNC && lab_executable(&ctx, entry));
    CHECK(lab_call2(entry, 5, 7) == 49);
    CHECK(lab_call2(entry, 10, 2) == 59);
    CHECK(lab_call2(entry, -3, 2) == 20);
    CHECK(!lab_executable(&ctx, bss));
    lab_finalize(&ctx);
    CHECK(read_global(&ctx, "math_ready") == 12);
    CHECK(read_global(&ctx, "base_ready") == -1);
    CHECK(ctx.init_count == 0);
    lab_context_destroy(&ctx);
}

static void page_tests(void) {
    LabError err = {{0}};
    LabObject *obj = calloc(1, sizeof(*obj));
    CHECK(obj != NULL);
    obj->eh.e_phnum = 2;
    obj->ph[0] = (Elf64_Phdr){ .p_type = PT_LOAD, .p_flags = PF_R, .p_memsz = 16 };
    obj->ph[1] = (Elf64_Phdr){ .p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_vaddr = 8192, .p_memsz = 16 };
    CHECK(lab_map(obj, &err));
    CHECK(obj->page_size == 4096);
    CHECK(obj->page_count == 3);
    CHECK(lab_address(obj, 4096, 1) == NULL);
    CHECK(!lab_virtual_range(obj, 0, 32, PF_R));
    CHECK(lab_protect(obj, &err));
    kernel_permissions(obj);
    lab_object_destroy(obj);

    obj = calloc(1, sizeof(*obj)); CHECK(obj != NULL);
    obj->eh.e_phnum = 2;
    obj->ph[0] = (Elf64_Phdr){ .p_type = PT_LOAD, .p_flags = PF_R | PF_W, .p_memsz = 16 };
    obj->ph[1] = (Elf64_Phdr){ .p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_vaddr = 128, .p_memsz = 16 };
    CHECK(!lab_map(obj, &err));
    CHECK(strstr(err.text, "W^X") != NULL);
    lab_object_destroy(obj);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    relocation_tests();
    graph_tests(argv[1]);
    page_tests();
    printf("PASS: %u C assertions (relocations, BSS, linking, init/fini, kernel page permissions)\n", checks);
    return 0;
}
