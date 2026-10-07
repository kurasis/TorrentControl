// Test-only prerequisite: no filesystem, registry or networking effects.
#ifndef TC_PROBE_EXIT
#define TC_PROBE_EXIT 0
#endif
int main() { return TC_PROBE_EXIT; }
