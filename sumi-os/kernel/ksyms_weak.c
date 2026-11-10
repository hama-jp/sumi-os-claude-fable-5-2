/* Empty symbol table used for the first link pass; the generated table overrides it. */
typedef struct KSym { unsigned addr; const char *name; } KSym;
__attribute__((weak)) const KSym ksyms[1] = { { 0, "" } };
__attribute__((weak)) const int ksym_count = 0;
