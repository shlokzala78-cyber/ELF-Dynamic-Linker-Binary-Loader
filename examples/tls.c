__thread long unsupported_tls = 1;
long demo_entry(long first, long second) { return first + second + unsupported_tls; }
