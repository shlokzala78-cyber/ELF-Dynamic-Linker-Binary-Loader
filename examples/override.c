extern long math_calculate(long, long);
long base_bias = 19;
long optional_hook(void) { return 100; }

long demo_entry(long first, long second) {
    return math_calculate(first, second);
}
