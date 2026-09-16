#include "../src/ui/spectra_variation.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace lat;

static bool same(const SpectraChangeList& a, const SpectraChangeList& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].value != b[i].value) return false;
    return true;
}

int main() {
    SpectraParamValues base{};
    for (int i = 0; i < kSpectraVariationParamCount; ++i) base[(std::size_t)i] = 0.37f;
    base[0] = 8.f; base[8] = 8.f; base[40] = 1.5f;
    const auto a = makeSpectraVariation(base, 0x1234u, 0.75f);
    assert(same(a, makeSpectraVariation(base, 0x1234u, 0.75f)));
    assert(!same(a, makeSpectraVariation(base, 0x1235u, 0.75f)));
    for (const auto& c : a) {
        assert(c.id >= 0 && c.id < kSpectraVariationParamCount);
        assert(std::isfinite(c.value));
        for(int protectedId : {0,2,3,8,10,11,38,39,40,68,69,70,94,95,96,97,98,99,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,131})
            assert(c.id != protectedId);
        if (c.id == 1 || c.id == 4 || c.id == 7 || c.id == 9 || c.id == 12 || c.id == 15)
            assert(c.value >= 0.08f && c.value <= 0.92f);
        if (c.id == 19) assert(c.value >= 0.08f && c.value <= 0.72f);
        if (c.id == 21) assert(c.value >= 0.f && c.value <= 12.f);
    }
    assert(makeSpectraVariation(base, 77u, 0.f).empty());
    assert(makeSpectraVariation(base, 77u, NAN).empty());
    base[5] = 7.f; base[12] = 0.f; base[18] = 20000.f;
    const auto tiny = makeSpectraVariation(base, 77u, 0.001f);
    float tinyUnison = base[5];
    for (const auto& c : tiny) {
        if (c.id == 5) tinyUnison = c.value;
        if (c.id == 12) assert(c.value >= 0.f && c.value < 0.001f);
        if (c.id == 18) assert(c.value > 19800.f && c.value <= 20000.f);
    }
    assert(tinyUnison == 7.f);
    const auto macros = makeSpectraMacroVariation(base, 91u, 1.f);
    assert(!macros.empty());
    for (const auto& c : macros) assert(c.id >= 94 && c.id <= 97 && c.value >= 0.f && c.value <= 1.f);
    assert(makeSpectraMacroVariation(base, 91u, 0.f).empty());
    std::printf("Spectra variation: %zu sound, %zu macro changes\n", a.size(), macros.size());
}
