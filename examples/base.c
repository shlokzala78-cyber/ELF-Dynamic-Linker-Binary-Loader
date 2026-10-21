long base_bias = 7;
long base_ready;
long base_bss[1024];

__attribute__((constructor)) static void base_init(void) { base_ready = 2; }
__attribute__((destructor)) static void base_fini(void) { base_ready = -1; }

long base_transform(long value) { return value * 3 + base_bss[1000]; }
