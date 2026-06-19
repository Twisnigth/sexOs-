// Petit binaire ELF statique de test (aucune libc) : ecrit puis quitte.
static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");
    return r;
}
static unsigned slen(const char *s){ unsigned n=0; while(s[n]) n++; return n; }
void _start(void) {
    const char *m = "Bonjour depuis le ring 3 : binaire ELF Linux execute par MonOS\n";
    sys3(1 /*write*/, 1 /*stdout*/, (long)m, slen(m));
    sys3(60 /*exit*/, 42, 0, 0);
    for(;;){}
}
