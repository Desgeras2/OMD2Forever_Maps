// validate_cli.cpp - the SAME validator the game runs, as a command-line tool.
//
// WHY THIS EXISTS AND WHY IT IS NOT A SECOND IMPLEMENTATION.
//
// Uploads are checked automatically, which means the check runs somewhere that
// is not a Windows game process. The temptation is to write a quick script that
// "does roughly the same thing". That script and the real validator would then
// drift, and the day they disagree is the day something gets published that the
// game refuses - or worse, the other way round.
//
// So this is a thin shell around omd1_share.h itself. The header includes no
// Windows and no engine on purpose, which is exactly what makes this possible.
// One implementation, two callers.
//
// Build (Linux CI):   g++ -O2 -o validate validate_cli.cpp -I..
// Build (Windows):    cl /EHsc /I.. validate_cli.cpp
//
// Usage:
//   validate <map.delve> <map.placements> <kit_paths.txt>
// Exit code 0 = the map is acceptable. Non-zero and a line on stdout = why.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omd1_share.h"

// ---- the allowlist, loaded from the generated list -------------------------
static char** g_kit = 0;
static int    g_kitN = 0;
static int    g_mobs = 0;      // from the "#mobs=N" line the generator writes

static int EqNoCase(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        // Windows writes these paths with backslashes; a list generated on
        // another machine may not. The separator is not what is being checked.
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int KnownPath(const char* p) {
    for (int i = 0; i < g_kitN; ++i)
        if (EqNoCase(g_kit[i], p)) return 1;
    // A MARKER MAY CARRY A "#N" SUFFIX. The editor adds it so several rifts or
    // doors in one level stay apart; the name before the '#' is what has to be
    // real. Only digits are allowed after it - "@Rift#../../x" is not a marker
    // with a suffix, it is a path wearing one.
    if (p[0] == '@') {
        const char* hash = strchr(p, '#');
        if (hash && hash[1]) {
            for (const char* d = hash + 1; *d; ++d)
                if (*d < '0' || *d > '9') return 0;
            char base[kShareMaxPathLen + 1];
            const size_t n = (size_t)(hash - p);
            if (n == 0 || n > kShareMaxPathLen) return 0;
            memcpy(base, p, n);
            base[n] = 0;
            for (int i = 0; i < g_kitN; ++i)
                if (EqNoCase(g_kit[i], base)) return 1;
        }
    }
    return 0;
}

static char* SlurpFile(const char* path, size_t* outN) {
    *outN = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > kShareMaxBytes) { fclose(f); return 0; }
    char* b = (char*)malloc((size_t)n + 1);
    if (!b) { fclose(f); return 0; }
    const size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    *outN = got;
    return b;
}

static int LoadKit(const char* path) {
    size_t n = 0;
    char* b = SlurpFile(path, &n);
    if (!b) return 0;
    int cap = 64;
    g_kit = (char**)malloc(sizeof(char*) * cap);
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && b[e] != '\n' && b[e] != '\r') ++e;
        if (e > i) {
            size_t len = e - i;
            while (len && (b[i + len - 1] == ' ' || b[i + len - 1] == '\t')) --len;
            if (len > 6 && memcmp(b + i, "#mobs=", 6) == 0) {
                g_mobs = atoi(b + i + 6);
            } else if (len && b[i] != '#') {
                if (g_kitN == cap) { cap *= 2; g_kit = (char**)realloc(g_kit, sizeof(char*) * cap); }
                char* s = (char*)malloc(len + 1);
                memcpy(s, b + i, len);
                s[len] = 0;
                g_kit[g_kitN++] = s;
            }
        }
        while (e < n && (b[e] == '\n' || b[e] == '\r')) ++e;
        i = (e == i) ? i + 1 : e;
    }
    free(b);
    return g_kitN > 0;
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        printf("usage: validate <map.delve> <map.placements> <kit_paths.txt> [map.waves]\n");
        return 2;
    }
    if (!LoadKit(argv[3])) { printf("could not read the kit list: %s\n", argv[3]); return 2; }

    size_t dn = 0, pn = 0;
    char* d = SlurpFile(argv[1], &dn);
    char* p = SlurpFile(argv[2], &pn);
    if (!d) { printf("could not read %s\n", argv[1]); return 2; }
    if (!p) { printf("could not read %s\n", argv[2]); return 2; }

    ShareResult r;
    ShareCheckDelve(d, dn, &r);
    if (!r.ok) { printf("delve refused: %s\n", r.why); return 1; }

    ShareCheckPlacements(p, pn, KnownPath, &r);
    if (!r.ok) { printf("placements refused: line %d: %s\n", r.line, r.why); return 1; }

    if (argc == 5) {
        size_t wn = 0;
        char* w = SlurpFile(argv[4], &wn);
        if (!w) { printf("could not read %s\n", argv[4]); return 2; }
        if (g_mobs <= 0) { printf("the kit list has no monster count - regenerate it\n"); return 2; }
        ShareCheckWaves(w, wn, g_mobs, &r);
        if (!r.ok) { printf("waves refused: line %d: %s\n", r.line, r.why); return 1; }
    }

    printf("ok: the map passes, against %d known pieces and %d monsters\n", g_kitN, g_mobs);
    return 0;
}
