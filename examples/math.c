extern long base_transform(long);
extern long asm_add(long, long);
extern long base_bias, base_ready;
extern long optional_hook(void) __attribute__((weak));

long math_offset = 11;
long math_ready;
static volatile long local_seed = 4;
static volatile long *volatile seed_pointer = &local_seed;
long (*volatile dispatch)(long) = base_transform;

__attribute__((constructor)) static void math_init(void) { math_ready = base_ready + 3; }
__attribute__((destructor)) static void math_fini(void) { math_ready = base_ready + 10; }

long math_calculate(long first, long second) {
    long optional = optional_hook ? optional_hook() : 0;
    return asm_add(dispatch(first), second) + base_bias + math_offset +
           *seed_pointer + math_ready + optional;
}
