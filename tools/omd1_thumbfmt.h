// omd1_thumbfmt.h - A THUMBNAIL FORMAT WITH NOTHING IN IT TO ATTACK.
//
// ---------------------------------------------------------------------------
// WHY NOT JPEG, OR PNG, OR ANYTHING NORMAL
// ---------------------------------------------------------------------------
//
// Image decoders are where image bugs live. Every one of them - libjpeg, libpng,
// WIC, the lot - is a large program that reads a length out of a file, allocates
// from it, and then walks a variable-length bit stream. That is a fine thing to
// do with a picture you chose to open. It is the worst possible thing to point
// at a file a stranger handed you, and "we use the OS decoder so it is Microsoft's
// problem" is not a security argument, it is a hope.
//
// So the format has no decoder, because it has nothing to decode:
//
//     8 bytes   the letters OMD2THM1
//    18432 bytes  BC1 (DXT1) blocks for a 256 x 144 image
//    ---------
//    18440 bytes  EXACTLY, ALWAYS. Any other length is refused.
//
// THERE IS NO LENGTH FIELD, NO WIDTH, NO HEIGHT, NO PALETTE, NO STRIDE, NO
// OFFSET TABLE AND NO COMPRESSION. Nothing in the file can influence a size, an
// index, a loop bound or an allocation, because every one of those is a
// compile-time constant on our side. The worst a hostile thumbnail can do is be
// an ugly picture.
//
// The bytes are never parsed at all - they are handed to the GPU as a DXT1
// surface, which is the one image format a graphics card decodes in hardware.
// There is no software decode step anywhere in the path.
//
// WHY BC1: 18 KB fixed for a card-sized picture. Raw RGB at that size is 110 KB
// and half-resolution RGB565 is the same 18 KB but blurrier, so this is the
// smallest honest option that still looks like the map. It is also exactly what
// the GPU wants, so showing it costs one upload and no conversion.

#pragma once
#include <stddef.h>

enum {
    kThumbPixW  = 256,
    kThumbPixH  = 144,
    kThumbMagic = 8,
    kThumbBytes = (kThumbPixW / 4) * (kThumbPixH / 4) * 8,   // 18432
    kThumbFile  = kThumbMagic + kThumbBytes                  // 18440
};
static const char kThumbTag[kThumbMagic] = { 'O','M','D','2','T','H','M','1' };

// The only check there is, and it is the only one there can be.
static int ThumbFileLooksRight(const void* data, int n) {
    if (!data || n != kThumbFile) return 0;
    const char* b = (const char*)data;
    for (int i = 0; i < kThumbMagic; ++i)
        if (b[i] != kThumbTag[i]) return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// THE ENCODER. Ours, used on the picture we took ourselves - it never runs on
// anything that came from somewhere else.
// ---------------------------------------------------------------------------
static unsigned short ThumbTo565(int r, int g, int b) {
    return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// One 4x4 block: take the corners of the colour box as the two endpoints, build
// the four-colour ramp BC1 defines, and pick the nearest for each pixel. This is
// the simple encoder, not the good one; at 256x144 on a card nobody can tell.
static void ThumbBlock(const unsigned char* src, int pitch, unsigned char* out) {
    int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
    for (int y = 0; y < 4; ++y) {
        const unsigned char* row = src + (size_t)y * pitch;
        for (int x = 0; x < 4; ++x) {
            const unsigned char* p = row + x * 4;      // BGRA
            for (int c = 0; c < 3; ++c) {
                const int v = p[2 - c];                 // to RGB order
                if (v < lo[c]) lo[c] = v;
                if (v > hi[c]) hi[c] = v;
            }
        }
    }
    unsigned short c0 = ThumbTo565(hi[0], hi[1], hi[2]);
    unsigned short c1 = ThumbTo565(lo[0], lo[1], lo[2]);
    // BC1 reads c0 <= c1 as "three colours and transparent". Our thumbnails are
    // opaque, so keep c0 strictly greater and let a flat block be flat.
    if (c0 < c1) { const unsigned short t = c0; c0 = c1; c1 = t; }
    if (c0 == c1 && c0 > 0) c1 = (unsigned short)(c0 - 1);

    int pal[4][3];
    pal[0][0] = ((c0 >> 11) & 31) << 3; pal[0][1] = ((c0 >> 5) & 63) << 2; pal[0][2] = (c0 & 31) << 3;
    pal[1][0] = ((c1 >> 11) & 31) << 3; pal[1][1] = ((c1 >> 5) & 63) << 2; pal[1][2] = (c1 & 31) << 3;
    for (int c = 0; c < 3; ++c) {
        pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
        pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
    }

    unsigned bits = 0;
    for (int y = 0; y < 4; ++y) {
        const unsigned char* row = src + (size_t)y * pitch;
        for (int x = 0; x < 4; ++x) {
            const unsigned char* p = row + x * 4;
            const int r = p[2], g = p[1], b = p[0];
            int best = 0, bestD = 1 << 30;
            for (int k = 0; k < 4; ++k) {
                const int dr = r - pal[k][0], dg = g - pal[k][1], db = b - pal[k][2];
                const int d = dr * dr + dg * dg + db * db;
                if (d < bestD) { bestD = d; best = k; }
            }
            bits |= (unsigned)best << ((y * 4 + x) * 2);
        }
    }
    out[0] = (unsigned char)(c0 & 0xFF); out[1] = (unsigned char)(c0 >> 8);
    out[2] = (unsigned char)(c1 & 0xFF); out[3] = (unsigned char)(c1 >> 8);
    out[4] = (unsigned char)(bits & 0xFF);
    out[5] = (unsigned char)((bits >> 8) & 0xFF);
    out[6] = (unsigned char)((bits >> 16) & 0xFF);
    out[7] = (unsigned char)((bits >> 24) & 0xFF);
}

// bgra must be exactly kThumbPixW x kThumbPixH. out must hold kThumbBytes.
static void ThumbEncode(const unsigned char* bgra, int pitch, unsigned char* out) {
    int at = 0;
    for (int by = 0; by < kThumbPixH; by += 4)
        for (int bx = 0; bx < kThumbPixW; bx += 4) {
            ThumbBlock(bgra + (size_t)by * pitch + (size_t)bx * 4, pitch, out + at);
            at += 8;
        }
}
