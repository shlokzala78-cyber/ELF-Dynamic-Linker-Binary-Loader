extern long protected_read(void);
long protected_value = 99;
long demo_entry(long first, long second) { return protected_read() + first + second; }
