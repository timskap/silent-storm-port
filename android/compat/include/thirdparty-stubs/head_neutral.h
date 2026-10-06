// Neutral geometry reader for the two LifeStudio animator stream formats in
// Heads/. Facial muscle deformation and speech sequences are not implemented.
#ifndef A5_HEAD_NEUTRAL_H
#define A5_HEAD_NEUTRAL_H
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
namespace A5Head {
struct Point { float x, y, z; };
class Reader {
    const unsigned char *data;
    size_t size;
public:
    size_t pos = 0;
    Reader(const void *p, size_t n): data(static_cast<const unsigned char *>(p)), size(n) {}
    bool has(size_t n) const { return pos <= size && n <= size - pos; }
    bool skip(size_t n) { if (!has(n)) return false; pos += n; return true; }
    uint32_t word(size_t offset) const {
        const unsigned char *p = data + pos + offset;
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    float real(size_t offset) const { uint32_t bits = word(offset); float f; std::memcpy(&f, &bits, 4); return f; }
    bool name() { return has(1) && skip(size_t(data[pos]) + 1); }
};
inline bool Load(const void *data, size_t size, std::vector<Point> &out) {
    out.clear();
    if (!data) return false;
    Reader r(data, size);
    if (!r.has(28)) return false;
    const bool old = r.word(0) == 0xad5a018dU;
    if (!old && r.word(0) != 0x37d30dc0U) return false;
    const uint32_t muscles = r.word(4), count = r.word(8), moving = r.word(12), bones = r.word(16);
    // Bound allocation and loop counts by the actual stream, before allocating.
    if (!count || count > size / 16 || moving > count || muscles > size / 28 || bones > size / 108) return false;
    if (!r.has(old ? 28 : 36)) return false;
    const size_t muscleSize = old ? 172 : (r.word(32) & 1 ? 112 : 28);
    r.skip(old ? 28 : 36);
    for (uint32_t i = 0; i < muscles; ++i)
        if ((!old && !r.name()) || !r.skip(muscleSize)) return false;
    std::vector<Point> points(count);
    std::vector<bool> seen(count, false);
    auto vertex = [&](size_t recordSize, size_t position) {
        if (!r.has(recordSize)) return false;
        uint32_t index = r.word(0);
        if (index >= count || seen[index]) return false;
        Point p = {r.real(position), r.real(position + 4), r.real(position + 8)};
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
        points[index] = p; seen[index] = true;
        return true;
    };
    for (uint32_t i = 0; i < moving; ++i) {
        const size_t base = old ? 28 : 20;
        if (!vertex(base, old ? 12 : 4)) return false;
        const uint32_t weights = r.word(base - 4);
        if (!r.skip(base) || weights > size / (old ? 12 : 8) || !r.skip(size_t(weights) * (old ? 12 : 8))) return false;
    }
    for (uint32_t i = 0; i < bones; ++i) {
        if (!old && !r.name()) return false;
        const size_t base = old ? 164 : 108;
        if (!r.has(base)) return false;
        const uint32_t links = r.word(old ? 64 : 0);
        if (!r.skip(base) || links > size / 4 || !r.skip(size_t(links) * 4)) return false;
    }
    for (uint32_t i = moving; i < count; ++i) {
        if (!vertex(old ? 24 : 16, old ? 12 : 4)) return false;
        r.skip(old ? 24 : 16);
    }
    // New-format streams may end with expression targets; neutral geometry is
    // complete here. Unique in-range indices across count records cover all vertices.
    out.swap(points);
    return true;
}
} // namespace A5Head
#endif
