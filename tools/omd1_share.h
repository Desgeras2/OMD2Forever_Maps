// omd1_share.h - THE GATE EVERY DOWNLOADED MAP HAS TO GET THROUGH.
//
// ---------------------------------------------------------------------------
// WHY THIS FILE EXISTS, AND WHY IT IS NOT PART OF THE LOADER
// ---------------------------------------------------------------------------
//
// A shared map is a file written by a stranger. Everything in here is written
// on that assumption: no allocation sized by the file, no number from the file
// used as an index or a length before it is checked, no string used before it
// is proved terminated, and NO PATH FROM THE FILE EVER REACHING THE ENGINE.
//
// THE HAZARD IS REAL AND IT IS SPECIFIC. A .placements line is
//
//     <prefab path>|x|y|z|yaw|keep|pitch|roll|mirror
//
// and that first field is a PATH that the mod hands to the engine's mesh
// loader. A downloaded map could put anything there - "..\..\..\evil.dll", a
// UNC share, a path into the user's profile - and the engine would open it,
// because opening what it is told to open is the engine's whole job. That is
// the remote-code-execution route, and it is not hypothetical; it is the
// format we already ship.
//
// The fix is an ALLOWLIST, not an escape or a sanitiser. A path is acceptable
// only if it is byte-for-byte one of the 1670 entries in this install's own
// generated kit table, or one of the marker names. Anything else and the WHOLE
// MAP is refused - not the line, the map. A file that contains one path we do
// not recognise is a file we do not understand, and a parser that guesses at
// what a stranger meant is the bug.
//
// That gives exactly the safety of storing integer indices, which was the
// other option, without changing the format or breaking the maps that already
// exist. The table IS the integer set; we just look it up by name.
//
// ---------------------------------------------------------------------------
// NO WINDOWS, NO ENGINE, ON PURPOSE
// ---------------------------------------------------------------------------
//
// Nothing in here includes windows.h or calls into the game, so the same code
// compiles into a standalone harness and can be fuzzed with a few million
// mutated files and no game running. That is the only real proof that a parser
// is safe; everything else is the author asserting it. See scratchpad/fuzz.cpp.
//
// The caller supplies the allowlist as a callback, so the harness can stub it.

#pragma once
#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// LIMITS. Every one of these exists so that a hostile file cannot make us
// allocate, loop, or index without a bound. They are generous for a real map
// and tiny compared to what an attacker would want.
// ---------------------------------------------------------------------------
enum {
    kShareMaxBytes   = 4 * 1024 * 1024,  // a map is kilobytes; this is mercy
    kShareMaxLines   = 60000,
    kShareMaxLineLen = 512,
    kShareMaxPlace   = 20000,            // placements in one level
    kShareMaxPathLen = 200
};
// World coordinates. The level grid is 200 units to a cell; a map a thousand
// cells across is already absurd, and anything past this is either a mistake
// or an attempt to find something that overflows downstream.
static const float kShareMaxCoord = 2000000.0f;
static const float kShareMaxAngle = 100000.0f;

// Is this prefab path one this install actually has? Supplied by the caller so
// this file stays free of the game. Must compare EXACTLY (case-insensitively is
// fine - the kit table is the authority either way) and must never fall back to
// "looks about right".
typedef int (*ShareKnownPathFn)(const char* path);

struct ShareResult {
    int  ok;
    int  line;              // 1-based line the refusal is about, 0 if not a line
    char why[160];
};

// ---------------------------------------------------------------------------
// Small helpers. Deliberately dull: no strtok, no sscanf, no atoi, nothing that
// reads past a length or writes an unbounded result.
// ---------------------------------------------------------------------------
static void ShareSay(ShareResult* r, int line, const char* why) {
    r->ok = 0;
    r->line = line;
    size_t i = 0;
    for (; why[i] && i + 1 < sizeof(r->why); ++i) r->why[i] = why[i];
    r->why[i] = 0;
}

// A float, parsed by hand with a hard digit budget. strtod on a stranger's
// buffer is fine in principle but it reads until it stops wanting to, and the
// budget here is what makes "1e999999999" cheap to refuse.
static int ShareFloat(const char* s, size_t n, float* out) {
    size_t i = 0;
    int neg = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); ++i; }
    double v = 0.0;
    int digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        if (++digits > 12) return 0;            // no 400-digit integers
        v = v * 10.0 + (double)(s[i] - '0');
        ++i;
    }
    int frac = 0;
    if (i < n && s[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (++frac > 8) return 0;
            v += (double)(s[i] - '0') * scale;
            scale *= 0.1;
            ++i;
        }
    }
    if (digits == 0 && frac == 0) return 0;     // "", "-", "." are not numbers
    if (i != n) return 0;                       // trailing junk, including 'e'
    *out = (float)(neg ? -v : v);
    return 1;
}

static int ShareInt(const char* s, size_t n, int lo, int hi, int* out) {
    float f;
    if (!ShareFloat(s, n, &f)) return 0;
    const int v = (int)f;
    if ((float)v != f) return 0;                // an int field is an int
    if (v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

static int ShareFinite(float v, float limit) {
    // NaN fails every comparison, which is exactly the test we want: a NaN
    // coordinate reaching the navmesh sampler or PhysX is a crash at best.
    if (!(v >= -limit && v <= limit)) return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// THE PLACEMENTS FILE
// ---------------------------------------------------------------------------
//
//   # comment
//   <prefab path>|x|y|z|yaw|keep|pitch|roll|mirror
//
// Fields after the fifth are optional - the format was designed to be appended
// to - so a short line is fine and a long one is refused rather than guessed
// at. The path is checked against the install's own kit; everything else is
// checked for range.
static void ShareCheckPlacements(const char* buf, size_t len,
                                 ShareKnownPathFn known, ShareResult* r) {
    r->ok = 1; r->line = 0; r->why[0] = 0;

    if (!buf) { ShareSay(r, 0, "the placements file is missing"); return; }
    if (len > kShareMaxBytes) { ShareSay(r, 0, "the placements file is too big"); return; }

    size_t i = 0;
    int lineNo = 0, placements = 0;
    while (i < len) {
        if (++lineNo > kShareMaxLines) { ShareSay(r, lineNo, "too many lines"); return; }

        size_t e = i;
        while (e < len && buf[e] != '\n' && buf[e] != '\r') ++e;
        const size_t lineLen = e - i;
        if (lineLen > kShareMaxLineLen) { ShareSay(r, lineNo, "a line is too long"); return; }
        const char* L = buf + i;

        // step past the terminator(s) before anything can `continue`
        size_t next = e;
        if (next < len && buf[next] == '\r') ++next;
        if (next < len && buf[next] == '\n') ++next;
        if (next == i) ++next;                  // never fail to advance
        i = next;

        if (lineLen == 0 || L[0] == '#') continue;

        // Split on '|' into at most nine fields, by length - no copies, no
        // terminators assumed, nothing written.
        const char* f[9]; size_t fn[9];
        int nf = 0;
        size_t s = 0;
        for (size_t k = 0; k <= lineLen; ++k) {
            if (k == lineLen || L[k] == '|') {
                if (nf >= 9) { ShareSay(r, lineNo, "too many fields on the line"); return; }
                f[nf] = L + s; fn[nf] = k - s; ++nf;
                s = k + 1;
            }
        }
        if (nf < 5) { ShareSay(r, lineNo, "a placement needs at least five fields"); return; }

        // ---- THE PATH. The whole reason this file exists. -----------------
        if (fn[0] == 0 || fn[0] > kShareMaxPathLen) {
            ShareSay(r, lineNo, "the prefab path is empty or too long"); return;
        }
        char path[kShareMaxPathLen + 1];
        for (size_t k = 0; k < fn[0]; ++k) {
            const unsigned char c = (unsigned char)f[0][k];
            // Printable ASCII only. This is not the security check - the
            // allowlist below is - but a control byte or a UTF-8 lookalike in
            // a path is never anything but an attempt at one.
            if (c < 0x20 || c > 0x7E) { ShareSay(r, lineNo, "the prefab path has a byte that is not printable ASCII"); return; }
            path[k] = (char)c;
        }
        path[fn[0]] = 0;
        if (!known || !known(path)) {
            // Deliberately the same message for a typo and for an attack. We
            // do not know which it is, and it does not change what we do.
            ShareSay(r, lineNo, "this map uses a piece this install does not have");
            return;
        }

        // ---- the numbers --------------------------------------------------
        static const char* const kName[4] = { "x", "y", "z", "yaw" };
        for (int k = 0; k < 4; ++k) {
            float v;
            if (!ShareFloat(f[k + 1], fn[k + 1], &v) ||
                !ShareFinite(v, k == 3 ? kShareMaxAngle : kShareMaxCoord)) {
                char why[80];
                size_t w = 0;
                const char* a = "the ";
                while (*a) why[w++] = *a++;
                const char* b = kName[k];
                while (*b) why[w++] = *b++;
                const char* c = " value is not a number in range";
                while (*c) why[w++] = *c++;
                why[w] = 0;
                ShareSay(r, lineNo, why);
                return;
            }
        }
        if (nf >= 6) { int v; if (!ShareInt(f[5], fn[5], 0, 1, &v)) { ShareSay(r, lineNo, "keep must be 0 or 1"); return; } }
        if (nf >= 7) { float v; if (!ShareFloat(f[6], fn[6], &v) || !ShareFinite(v, kShareMaxAngle)) { ShareSay(r, lineNo, "pitch is not a number in range"); return; } }
        if (nf >= 8) { float v; if (!ShareFloat(f[7], fn[7], &v) || !ShareFinite(v, kShareMaxAngle)) { ShareSay(r, lineNo, "roll is not a number in range"); return; } }
        if (nf >= 9) { int v; if (!ShareInt(f[8], fn[8], 0, 1, &v)) { ShareSay(r, lineNo, "mirror must be 0 or 1"); return; } }

        if (++placements > kShareMaxPlace) { ShareSay(r, lineNo, "too many placements"); return; }
    }

    if (placements == 0) { ShareSay(r, 0, "the map has no placements in it"); return; }
}

// ---------------------------------------------------------------------------
// THE DELVE
// ---------------------------------------------------------------------------
//
// The delve is XML and it is parsed by the GAME, not by us, so we cannot make
// its parser safe from here. What we CAN do is refuse the things that turn a
// data file into a loader: a DOCTYPE or ENTITY declaration (XXE - the classic
// way to make a parser read a file off disk or a URL off the network), a
// processing instruction, and any absolute path, UNC path or parent-directory
// step anywhere in the text.
//
// This is a blocklist, and a blocklist is normally the weaker tool. It is used
// here because the alternative is writing our own XML parser for a format the
// engine defines, which trades a known risk for an unknown one. It is paired
// with the fact that a delve names pieces the placements file has already had
// allowlisted, and with the size and depth caps below.
static int ShareHasI(const char* h, size_t n, const char* needle) {
    const size_t m = strlen(needle);
    if (m == 0 || n < m) return 0;
    for (size_t i = 0; i + m <= n; ++i) {
        size_t k = 0;
        for (; k < m; ++k) {
            char a = h[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (k == m) return 1;
    }
    return 0;
}

static void ShareCheckDelve(const char* buf, size_t len, ShareResult* r) {
    r->ok = 1; r->line = 0; r->why[0] = 0;

    if (!buf) { ShareSay(r, 0, "the delve file is missing"); return; }
    if (len > kShareMaxBytes) { ShareSay(r, 0, "the delve file is too big"); return; }
    if (len < 16) { ShareSay(r, 0, "the delve file is too short to be one"); return; }

    // No byte outside printable ASCII, tab, CR or LF. A delve is text.
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)buf[i];
        if (c == 9 || c == 10 || c == 13) continue;
        if (c < 0x20 || c > 0x7E) { ShareSay(r, 0, "the delve file has bytes that are not text"); return; }
    }

    // The parser-turns-into-a-loader constructs.
    if (ShareHasI(buf, len, "<!DOCTYPE")) { ShareSay(r, 0, "the delve declares a DOCTYPE"); return; }
    if (ShareHasI(buf, len, "<!ENTITY"))  { ShareSay(r, 0, "the delve declares an ENTITY"); return; }
    if (ShareHasI(buf, len, "<?"))        { ShareSay(r, 0, "the delve has a processing instruction"); return; }
    if (ShareHasI(buf, len, "SYSTEM"))    { ShareSay(r, 0, "the delve refers to an external SYSTEM id"); return; }
    if (ShareHasI(buf, len, "PUBLIC"))    { ShareSay(r, 0, "the delve refers to an external PUBLIC id"); return; }

    // A DELVE LEGITIMATELY NAMES GAME ASSETS - <LoadingScreen> is a .dds path,
    // and that is normal. The hazard is not "a path" but "a path that leaves
    // the game's own data", so what is refused below is every way of doing
    // that: a parent step, a drive letter, a UNC host, a URL, an environment
    // variable. What survives can only resolve to a file inside the install the
    // player already has, which is a file they already trusted.
    if (ShareHasI(buf, len, ".."))     { ShareSay(r, 0, "the delve contains a parent-directory step"); return; }
    if (ShareHasI(buf, len, "\\\\"))   { ShareSay(r, 0, "the delve contains a UNC path"); return; }
    if (ShareHasI(buf, len, "//"))     { ShareSay(r, 0, "the delve contains a network path or URL"); return; }
    if (ShareHasI(buf, len, ":\\"))    { ShareSay(r, 0, "the delve contains an absolute path"); return; }
    if (ShareHasI(buf, len, ":/"))     { ShareSay(r, 0, "the delve contains an absolute path"); return; }
    if (ShareHasI(buf, len, "%"))      { ShareSay(r, 0, "the delve contains an environment reference"); return; }
    if (ShareHasI(buf, len, "&#"))     { ShareSay(r, 0, "the delve contains a numeric character reference"); return; }

    // Tag depth and count, so a file of 200000 nested tags cannot be handed to
    // a recursive-descent parser we do not control.
    //
    // COMMENTS ARE NOT ELEMENTS. Counting "<!-- x -->" as an open tag is what
    // made this refuse every delve the editor has ever written - it writes two
    // comments into each one. Skipped whole here, and any OTHER "<!" is
    // refused outright: DOCTYPE and ENTITY are named above, and this catches
    // CDATA and whatever else nobody has thought of.
    int depth = 0, maxDepth = 0, tags = 0;
    for (size_t i = 0; i + 1 < len; ++i) {
        if (buf[i] != '<') continue;
        if (buf[i + 1] == '!') {
            if (i + 4 <= len && buf[i + 2] == '-' && buf[i + 3] == '-') {
                size_t e = i + 4;
                while (e + 2 < len && !(buf[e] == '-' && buf[e + 1] == '-' && buf[e + 2] == '>')) ++e;
                if (e + 2 >= len) { ShareSay(r, 0, "the delve has an unterminated comment"); return; }
                i = e + 2;
                continue;
            }
            ShareSay(r, 0, "the delve has a declaration in it");
            return;
        }
        if (++tags > kShareMaxLines) { ShareSay(r, 0, "the delve has too many tags"); return; }
        if (buf[i + 1] == '/') {
            if (--depth < 0) { ShareSay(r, 0, "the delve's tags do not balance"); return; }
        } else {
            // self-closing tags do not change the depth
            size_t e = i + 1;
            while (e < len && buf[e] != '>') ++e;
            if (e >= len) { ShareSay(r, 0, "the delve has an unterminated tag"); return; }
            if (buf[e - 1] != '/') { ++depth; if (depth > maxDepth) maxDepth = depth; }
        }
        if (maxDepth > 64) { ShareSay(r, 0, "the delve is nested too deeply"); return; }
    }
    if (depth != 0) { ShareSay(r, 0, "the delve's tags do not balance"); return; }
    if (tags == 0)  { ShareSay(r, 0, "the delve has no tags in it"); return; }
}

// ---------------------------------------------------------------------------
// A NAME WE ARE WILLING TO MAKE A FOLDER OUT OF.
//
// We never use a name from the download as a path - the folder is named by the
// map's content hash - but the TITLE is still shown on screen and stored, so it
// gets the same treatment: printable ASCII, bounded, and no path separators in
// case a later version of this code is less careful than this one.
// ---------------------------------------------------------------------------
static int ShareCleanTitle(const char* in, size_t n, char* out, size_t outN) {
    if (!in || !out || outN < 2) return 0;
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < outN; ++i) {
        const unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c > 0x7E) continue;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') continue;
        out[w++] = (char)c;
    }
    while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '.')) --w;
    out[w] = 0;
    return w > 0;
}

// ---------------------------------------------------------------------------
// THE INDEX
// ---------------------------------------------------------------------------
//
// One file lists what is downloadable:
//
//   { "format": 1, "maps": [
//       { "id": "<64 hex>", "title": "...", "author": "...",
//         "players": 4, "bytes": 12345,
//         "delve": "<64 hex>", "place": "<64 hex>", "thumb": "<64 hex>" } ] }
//
// THIS IS NOT A JSON PARSER and it must never become one. It reads exactly the
// shape above and refuses everything else. A general parser would have to cope
// with nesting, unicode escapes, big numbers and duplicate keys - every one of
// which is a place to be wrong, for a file whose whole job is to be a list. The
// strictness IS the feature: unknown keys are skipped, unknown shapes are a
// refusal, and the whole index is rejected rather than partly believed.
//
// The three "<64 hex>" fields are content hashes. Nothing is parsed, written or
// opened until the bytes we downloaded hash to what the index said they would,
// so a swapped file on a CDN cannot become a map.

enum { kShareIdLen = 64 };          // sha-256, lowercase hex

struct ShareEntry {
    char id[kShareIdLen + 1];
    char title[64];
    char author[48];
    char delveSha[kShareIdLen + 1];
    char placeSha[kShareIdLen + 1];
    char thumbSha[kShareIdLen + 1];
    int  players;
    int  bytes;
};

enum { kShareMaxEntries = 512 };

static int ShareIsHex64(const char* s) {
    for (int i = 0; i < kShareIdLen; ++i) {
        const char c = s[i];
        const int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return 0;
    }
    return s[kShareIdLen] == 0;
}

// Skip spaces, tabs, CR and LF. Nothing else counts as whitespace - a vertical
// tab between two keys means the file is not what it says it is.
static void ShareSkipWs(const char* b, size_t n, size_t* i) {
    while (*i < n && (b[*i] == ' ' || b[*i] == '\t' || b[*i] == '\r' || b[*i] == '\n')) ++*i;
}

// A string, with a deliberately tiny escape set. No \u: a unicode escape is a
// decoder, a decoder is code, and nothing in this format needs one.
static int ShareStr(const char* b, size_t n, size_t* i, char* out, size_t outN) {
    ShareSkipWs(b, n, i);
    if (*i >= n || b[*i] != '"') return 0;
    ++*i;
    size_t w = 0;
    while (*i < n) {
        const unsigned char c = (unsigned char)b[*i];
        if (c == '"') { ++*i; if (out && w < outN) out[w] = 0; return out ? (w < outN) : 1; }
        if (c == '\\') {
            ++*i;
            if (*i >= n) return 0;
            const char e = b[*i];
            char r;
            if      (e == '"')  r = '"';
            else if (e == '\\') r = '\\';
            else if (e == '/')  r = '/';
            else if (e == 'n')  r = '\n';
            else if (e == 't')  r = '\t';
            else return 0;                 // including \u, on purpose
            if (out && w + 1 < outN) out[w++] = r;
            else if (out) return 0;        // too long for the field it belongs in
            ++*i;
            continue;
        }
        if (c < 0x20 || c > 0x7E) return 0;
        if (out && w + 1 < outN) out[w++] = (char)c;
        else if (out) return 0;
        ++*i;
    }
    return 0;                              // ran off the end: unterminated
}

static int ShareNum(const char* b, size_t n, size_t* i, int lo, int hi, int* out) {
    ShareSkipWs(b, n, i);
    const size_t s = *i;
    if (*i < n && b[*i] == '-') ++*i;
    size_t digits = 0;
    while (*i < n && b[*i] >= '0' && b[*i] <= '9') { ++*i; if (++digits > 10) return 0; }
    if (digits == 0) return 0;
    if (*i < n && (b[*i] == '.' || b[*i] == 'e' || b[*i] == 'E')) return 0;  // integers only
    long long v = 0; int neg = 0; size_t k = s;
    if (b[k] == '-') { neg = 1; ++k; }
    for (; k < *i; ++k) v = v * 10 + (b[k] - '0');
    if (neg) v = -v;
    if (v < lo || v > hi) return 0;
    *out = (int)v;
    return 1;
}

// Skip one value of any shape we allow, so an unknown key costs nothing.
// Depth-limited: a file of ten thousand open brackets must not recurse.
static int ShareSkipValue(const char* b, size_t n, size_t* i, int depth) {
    if (depth > 8) return 0;
    ShareSkipWs(b, n, i);
    if (*i >= n) return 0;
    const char c = b[*i];
    if (c == '"') return ShareStr(b, n, i, 0, 0);
    if (c == '-' || (c >= '0' && c <= '9')) { int d; return ShareNum(b, n, i, -2000000000, 2000000000, &d); }
    if (c == 't') { if (*i + 4 > n || memcmp(b + *i, "true", 4)) return 0;  *i += 4; return 1; }
    if (c == 'f') { if (*i + 5 > n || memcmp(b + *i, "false", 5)) return 0; *i += 5; return 1; }
    if (c == 'n') { if (*i + 4 > n || memcmp(b + *i, "null", 4)) return 0;  *i += 4; return 1; }
    if (c == '{' || c == '[') {
        const char close = (c == '{') ? '}' : ']';
        ++*i;
        for (;;) {
            ShareSkipWs(b, n, i);
            if (*i >= n) return 0;
            if (b[*i] == close) { ++*i; return 1; }
            if (b[*i] == ',') { ++*i; continue; }
            if (c == '{') {
                if (!ShareStr(b, n, i, 0, 0)) return 0;
                ShareSkipWs(b, n, i);
                if (*i >= n || b[*i] != ':') return 0;
                ++*i;
            }
            if (!ShareSkipValue(b, n, i, depth + 1)) return 0;
        }
    }
    return 0;
}

static int ShareKeyIs(const char* k, const char* want) {
    size_t i = 0;
    for (; k[i] && want[i]; ++i) if (k[i] != want[i]) return 0;
    return k[i] == 0 && want[i] == 0;
}

// Returns the number of entries filled, or -1 with `r` saying why.
static int ShareParseIndex(const char* b, size_t n, ShareEntry* out, int maxOut,
                           ShareResult* r) {
    r->ok = 1; r->line = 0; r->why[0] = 0;
    if (!b || !out || maxOut <= 0) { ShareSay(r, 0, "nothing to parse"); return -1; }
    if (n > kShareMaxBytes) { ShareSay(r, 0, "the index is too big"); return -1; }
    if (n < 2) { ShareSay(r, 0, "the index is empty"); return -1; }

    size_t i = 0;
    ShareSkipWs(b, n, &i);
    if (i >= n || b[i] != '{') { ShareSay(r, 0, "the index does not start with an object"); return -1; }
    ++i;

    int count = 0, sawMaps = 0, format = 0;
    for (;;) {
        ShareSkipWs(b, n, &i);
        if (i >= n) { ShareSay(r, 0, "the index ends in the middle"); return -1; }
        if (b[i] == '}') { ++i; break; }
        if (b[i] == ',') { ++i; continue; }

        char key[32];
        if (!ShareStr(b, n, &i, key, sizeof(key))) { ShareSay(r, 0, "a key is not a short string"); return -1; }
        ShareSkipWs(b, n, &i);
        if (i >= n || b[i] != ':') { ShareSay(r, 0, "a key has no value"); return -1; }
        ++i;

        if (ShareKeyIs(key, "format")) {
            if (!ShareNum(b, n, &i, 0, 1000, &format)) { ShareSay(r, 0, "format is not a small number"); return -1; }
            continue;
        }
        if (!ShareKeyIs(key, "maps")) {
            if (!ShareSkipValue(b, n, &i, 0)) { ShareSay(r, 0, "a value is not one this format allows"); return -1; }
            continue;
        }

        // ---- the list ----------------------------------------------------
        sawMaps = 1;
        ShareSkipWs(b, n, &i);
        if (i >= n || b[i] != '[') { ShareSay(r, 0, "maps is not a list"); return -1; }
        ++i;
        for (;;) {
            ShareSkipWs(b, n, &i);
            if (i >= n) { ShareSay(r, 0, "the map list ends in the middle"); return -1; }
            if (b[i] == ']') { ++i; break; }
            if (b[i] == ',') { ++i; continue; }
            if (b[i] != '{') { ShareSay(r, 0, "a map entry is not an object"); return -1; }
            ++i;

            ShareEntry e;
            memset(&e, 0, sizeof(e));
            for (;;) {
                ShareSkipWs(b, n, &i);
                if (i >= n) { ShareSay(r, 0, "a map entry ends in the middle"); return -1; }
                if (b[i] == '}') { ++i; break; }
                if (b[i] == ',') { ++i; continue; }
                char k[32];
                if (!ShareStr(b, n, &i, k, sizeof(k))) { ShareSay(r, 0, "a map key is not a short string"); return -1; }
                ShareSkipWs(b, n, &i);
                if (i >= n || b[i] != ':') { ShareSay(r, 0, "a map key has no value"); return -1; }
                ++i;
                int okv = 1;
                if      (ShareKeyIs(k, "id"))      okv = ShareStr(b, n, &i, e.id, sizeof(e.id));
                else if (ShareKeyIs(k, "title"))   okv = ShareStr(b, n, &i, e.title, sizeof(e.title));
                else if (ShareKeyIs(k, "author"))  okv = ShareStr(b, n, &i, e.author, sizeof(e.author));
                else if (ShareKeyIs(k, "delve"))   okv = ShareStr(b, n, &i, e.delveSha, sizeof(e.delveSha));
                else if (ShareKeyIs(k, "place"))   okv = ShareStr(b, n, &i, e.placeSha, sizeof(e.placeSha));
                else if (ShareKeyIs(k, "thumb"))   okv = ShareStr(b, n, &i, e.thumbSha, sizeof(e.thumbSha));
                else if (ShareKeyIs(k, "players")) okv = ShareNum(b, n, &i, 1, 16, &e.players);
                else if (ShareKeyIs(k, "bytes"))   okv = ShareNum(b, n, &i, 0, kShareMaxBytes, &e.bytes);
                else                               okv = ShareSkipValue(b, n, &i, 0);
                if (!okv) { ShareSay(r, 0, "a map field is not the shape this format allows"); return -1; }
            }

            // EVERY ENTRY IS CHECKED WHOLE. A half-filled row is a refusal for
            // the index, not a row we quietly drop - if the file is not what it
            // claims, we do not know which other rows are either.
            if (!ShareIsHex64(e.id))       { ShareSay(r, 0, "a map id is not a 64-character hash"); return -1; }
            if (!ShareIsHex64(e.delveSha)) { ShareSay(r, 0, "a delve hash is not a 64-character hash"); return -1; }
            if (!ShareIsHex64(e.placeSha)) { ShareSay(r, 0, "a placements hash is not a 64-character hash"); return -1; }
            if (e.thumbSha[0] && !ShareIsHex64(e.thumbSha)) { ShareSay(r, 0, "a thumbnail hash is not a 64-character hash"); return -1; }
            char clean[64];
            if (!ShareCleanTitle(e.title, strlen(e.title), clean, sizeof(clean))) { ShareSay(r, 0, "a map has no usable title"); return -1; }
            memcpy(e.title, clean, sizeof(e.title) < sizeof(clean) ? sizeof(e.title) : sizeof(clean));
            e.title[sizeof(e.title) - 1] = 0;
            if (e.players < 1 || e.players > 16) e.players = 1;

            if (count >= maxOut || count >= kShareMaxEntries) { ShareSay(r, 0, "the index lists more maps than we can hold"); return -1; }
            out[count++] = e;
        }
    }

    if (!sawMaps) { ShareSay(r, 0, "the index has no map list"); return -1; }
    if (format != 1) { ShareSay(r, 0, "the index is a format this build does not know"); return -1; }
    return count;
}
