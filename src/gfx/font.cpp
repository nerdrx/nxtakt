#include "font.h"
#include "gl.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cstring>
#include <unistd.h>
#if defined(_WIN32)
// GetWindowsDirectoryA + MAX_PATH for findSystemFont's Windows branch. gl.h has
// already pulled windows.h in on this platform; saying so here keeps that an
// implementation detail of gl.h rather than a dependency of this file.
#include <windows.h>
#endif

namespace lat {

static FT_Library g_ft = nullptr;
static bool ensureFT() {
    if (g_ft) return true;
    if (FT_Init_FreeType(&g_ft)) { LOGE("FreeType init failed"); g_ft = nullptr; return false; }
    return true;
}

std::string findSystemFont(bool bold) {
#if defined(_WIN32)
    // Windows has no fontconfig and no /usr/share/fonts. It has two places to
    // look, and this tries both in the order that costs least.
    //
    // 1. %WINDIR%\Fonts\<file>. On any real Windows install this hits on the
    //    first entry and the function is three system calls long.
    //
    // 2. The font registry, HKLM\...\Windows NT\CurrentVersion\Fonts, keyed by
    //    FACE NAME rather than filename. This is not a fallback for exotic
    //    setups -- it is the path that makes the program run under WINE AT
    //    ALL. A wine prefix's C:\windows\Fonts is EMPTY: wine finds the host's
    //    fonts through fontconfig and publishes them in exactly this registry
    //    key, as Z:\usr\share\fonts\... paths. Step 1 finds nothing there, and
    //    a font search that only knew about %WINDIR%\Fonts would take the whole
    //    GUI down with "no usable system font found" (which is precisely how
    //    this file's first Windows version failed).
    //
    // The face-name order is the design order: Segoe UI is the Windows system
    // UI face and the closest relative of what this interface was drawn
    // against; Tahoma and Verdana are its predecessors; Arial is on every
    // Windows ever shipped; Liberation Sans and DejaVu Sans are what a Linux
    // host offers through wine. Whatever wins is logged, because a run that
    // came out looking wrong should say which file it drew with.
    static const char* kFamilies[] = {
        "Segoe UI", "Tahoma", "Verdana", "Arial",
        "Liberation Sans", "DejaVu Sans", "Noto Sans", nullptr};
    // Same order, as the filenames Windows actually uses. (Bold has no
    // pattern -- segoeuib, tahomabd, arialbd -- so it is a second table
    // rather than a suffix rule.)
    static const char* kFilesRegular[] = {
        "segoeui.ttf", "tahoma.ttf", "verdana.ttf", "arial.ttf",
        "LiberationSans-Regular.ttf", "DejaVuSans.ttf", "NotoSans-Regular.ttf", nullptr};
    static const char* kFilesBold[] = {
        "segoeuib.ttf", "tahomabd.ttf", "verdanab.ttf", "arialbd.ttf",
        "LiberationSans-Bold.ttf", "DejaVuSans-Bold.ttf", "NotoSans-Bold.ttf", nullptr};

    char winDir[MAX_PATH];
    const UINT wlen = GetWindowsDirectoryA(winDir, (UINT)sizeof winDir);
    if (wlen == 0 || wlen >= sizeof winDir) { LOGE("GetWindowsDirectoryA failed"); return {}; }
    const std::string fontDir = std::string(winDir) + "\\Fonts\\";

    for (const char** p = bold ? kFilesBold : kFilesRegular; *p; ++p) {
        const std::string path = fontDir + *p;
        if (access(path.c_str(), R_OK) == 0) { LOGI("font: %s", path.c_str()); return path; }
    }

    // The registry pass. Values are named "<face> (TrueType)" -- e.g.
    // "Segoe UI Bold (TrueType)" -- and hold either a bare filename, relative
    // to %WINDIR%\Fonts, or a full path (which is the form wine writes).
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        auto lookup = [&](const char* family, bool wantBold) -> std::string {
            char valueName[256];
            std::snprintf(valueName, sizeof valueName, "%s%s (TrueType)",
                          family, wantBold ? " Bold" : "");
            char data[MAX_PATH * 2];
            DWORD size = sizeof data, type = 0;
            if (RegQueryValueExA(key, valueName, nullptr, &type, (LPBYTE)data, &size)
                    != ERROR_SUCCESS || type != REG_SZ || size == 0)
                return {};
            data[(size < sizeof data) ? size : sizeof data - 1] = '\0';
            // A backslash or a drive letter means it is already absolute.
            std::string path = (data[0] && (data[1] == ':' || data[0] == '\\'))
                             ? std::string(data) : fontDir + data;
            return (access(path.c_str(), R_OK) == 0) ? path : std::string();
        };
        std::string found;
        for (const char** f = kFamilies; *f && found.empty(); ++f) found = lookup(*f, bold);
        // A family with no bold cut is common (and every one of these has a
        // regular), so widen before giving up on the family list entirely.
        if (found.empty() && bold)
            for (const char** f = kFamilies; *f && found.empty(); ++f) found = lookup(*f, false);
        RegCloseKey(key);
        if (!found.empty()) { LOGI("font: %s (from the font registry)", found.c_str()); return found; }
    }

    LOGE("font: nothing usable in %s or the font registry", fontDir.c_str());
    return {};
#else
    static const char* regular[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        nullptr};
    static const char* boldFonts[] = {
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        nullptr};
    for (const char** p = bold ? boldFonts : regular; *p; ++p)
        if (access(*p, R_OK) == 0) return *p;
    // Last resort: whatever the regular list can offer.
    if (bold) for (const char** p = regular; *p; ++p)
        if (access(*p, R_OK) == 0) return *p;
    return {};
#endif
}

// How every glyph is rasterised. FT_LOAD_NO_BITMAP is the load-bearing part.
//
// A TrueType file may carry hand-tuned MONOCHROME bitmap strikes for small
// sizes alongside its outlines, and FreeType prefers a strike when one matches
// the requested ppem exactly. Wine's bundled Tahoma -- the face a default wine
// prefix hands us -- has strikes at UI sizes, and the result was two bugs at
// once: the glyphs came back 1 bit per pixel (blitGlyph below now handles that
// correctly regardless), and even expanded they would be ALIASED, in an
// interface whose entire type system is antialiased coverage. Half the labels
// would have been jagged and the other half smooth, at sizes a pixel apart.
//
// So: always rasterise the outline. Typography is then identical on every
// platform and every font, which is the property this UI actually wants.
// Fonts with no outlines at all (bitmap-only .fon/.pcf) draw nothing, and that
// is the right trade -- they are not UI faces.
static constexpr FT_Int32 kGlyphLoad =
    FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_BITMAP;

// Copy one rasterised glyph into the atlas, converting whatever FreeType chose
// to give us into 8-bit coverage.
//
// The previous version memcpy'd `width` bytes per row unconditionally, which
// is only correct for FT_PIXEL_MODE_GRAY. Handed a 1-bit bitmap it read the
// packed bits AND overran each row -- for a 6px glyph, six bytes out of a
// one-byte row -- and drew the neighbouring rows' bits as coverage. It looked
// like every letter had been replaced by a 45-degree hatch. Cheap to get
// right, and impossible to notice on a distro font, because DejaVu and
// Liberation ship no strikes and FreeType therefore always returned GRAY.
//
// `pitch` is signed and may be negative (FreeType's flow-up bitmaps), so the
// row stride arithmetic is done in ptrdiff_t rather than size_t. The old cast
// through size_t turned a negative pitch into an address in the exabytes.
static void blitGlyph(u8* dst, int dstStride, const FT_Bitmap& b) {
    const int w = (int)b.width, h = (int)b.rows;
    const std::ptrdiff_t pitch = b.pitch;
    if (!b.buffer || w <= 0 || h <= 0) return;

    switch (b.pixel_mode) {
    case FT_PIXEL_MODE_GRAY:
        // num_grays is 256 for every antialiased render FreeType produces; a
        // face with fewer would need scaling, and none of ours has one.
        for (int y = 0; y < h; ++y)
            std::memcpy(dst + (std::ptrdiff_t)y * dstStride, b.buffer + (std::ptrdiff_t)y * pitch, (size_t)w);
        break;
    case FT_PIXEL_MODE_MONO:
        // 1 bit per pixel, most significant bit leftmost, rows padded to bytes.
        for (int y = 0; y < h; ++y) {
            const u8* src = b.buffer + (std::ptrdiff_t)y * pitch;
            u8* out = dst + (std::ptrdiff_t)y * dstStride;
            for (int x = 0; x < w; ++x)
                out[x] = (src[x >> 3] >> (7 - (x & 7))) & 1 ? 255 : 0;
        }
        break;
    default:
        // BGRA (colour emoji), LCD triples, 2/4-bit grays. Leaving the cell
        // blank draws a space; drawing whatever the bytes happen to be draws
        // the hatch this function exists to prevent.
        LOGW("font: unsupported glyph pixel mode %d, glyph left blank", (int)b.pixel_mode);
        break;
    }
}

Font::~Font() { destroy(); }

void Font::destroy() {
    if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
}

bool Font::load(const char* path, int pixelSize) {
    if (!ensureFT()) return false;
    FT_Face face = nullptr;
    if (FT_New_Face(g_ft, path, 0, &face)) { LOGE("cannot open font %s", path); return false; }
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixelSize);
    pixelSize_ = pixelSize;
    ascent_  =  face->size->metrics.ascender  / 64.f;
    descent_ = -face->size->metrics.descender / 64.f;
    height_  =  face->size->metrics.height    / 64.f;

    // Lay glyphs out in rows; ASCII at UI sizes fits comfortably in 512px.
    const int atlasW = 512;
    int penX = 1, penY = 1, rowH = 0, needH = 1;
    for (u32 cp = kFirst; cp <= kLast; ++cp) {
        if (FT_Load_Char(face, cp, kGlyphLoad)) continue;
        const int gw = (int)face->glyph->bitmap.width, gh = (int)face->glyph->bitmap.rows;
        if (penX + gw + 1 > atlasW) { penX = 1; penY += rowH + 1; rowH = 0; }
        penX += gw + 1;
        if (gh > rowH) rowH = gh;
        needH = penY + rowH + 1;
    }
    int atlasH = 1; while (atlasH < needH) atlasH <<= 1;

    std::vector<u8> pix((size_t)atlasW * atlasH, 0);
    penX = 1; penY = 1; rowH = 0;
    for (u32 cp = kFirst; cp <= kLast; ++cp) {
        if (FT_Load_Char(face, cp, kGlyphLoad)) continue;
        FT_GlyphSlot g = face->glyph;
        const int gw = (int)g->bitmap.width, gh = (int)g->bitmap.rows;
        if (penX + gw + 1 > atlasW) { penX = 1; penY += rowH + 1; rowH = 0; }
        blitGlyph(&pix[(size_t)penY * atlasW + penX], atlasW, g->bitmap);

        Glyph& gl = glyphs_[cp - kFirst];
        gl.u0 = (f32)penX / atlasW;
        gl.v0 = (f32)penY / atlasH;
        gl.u1 = (f32)(penX + gw) / atlasW;
        gl.v1 = (f32)(penY + gh) / atlasH;
        gl.w = (f32)gw; gl.h = (f32)gh;
        gl.bearingX = (f32)g->bitmap_left;
        gl.bearingY = (f32)g->bitmap_top;
        gl.advance  = g->advance.x / 64.f;
        gl.valid = true;

        penX += gw + 1;
        if (gh > rowH) rowH = gh;
    }
    FT_Done_Face(face);

    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlasW, atlasH, 0, GL_RED, GL_UNSIGNED_BYTE, pix.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

const Glyph& Font::glyph(u32 cp) const {
    if (cp < kFirst || cp > kLast) return invalid_;
    return glyphs_[cp - kFirst];
}

f32 Font::measure(const char* s, int len) const {
    if (!s) return 0.f;
    f32 w = 0.f;
    for (int i = 0; (len < 0 ? s[i] != 0 : i < len); ++i) w += glyph((u8)s[i]).advance;
    return w;
}

int Font::fitLength(const char* s, f32 maxW, bool* ell) const {
    *ell = false;
    if (!s) return 0;
    const f32 dotW = glyph('.').advance * 3.f;
    f32 w = 0.f;
    int i = 0;
    for (; s[i]; ++i) {
        const f32 aw = glyph((u8)s[i]).advance;
        if (w + aw > maxW) break;
        w += aw;
    }
    if (!s[i]) return i;                       // fits whole
    *ell = true;
    while (i > 0 && w + dotW > maxW) { w -= glyph((u8)s[i - 1]).advance; --i; }
    return i;
}

} // namespace lat
