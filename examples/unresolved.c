extern long missing_function(long);
long demo_entry(long first, long second) { return missing_function(first) + second; }
