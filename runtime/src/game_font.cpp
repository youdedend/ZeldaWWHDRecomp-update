// The message font's characters (game_font.h): SARC pack -> Yaz0-compressed SARC -> BFFNT CMAP
// blocks. Right-to-left text (rtl_text_hooks.cpp) reads which scripts the font covers.
#include "game_font.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace game_font {
namespace {

namespace fs = std::filesystem;

struct Reader {  // bounds-checked reads in the file's byte order
    const uint8_t* d;
    size_t n;
    bool big = true;
    bool ok(size_t at, size_t len) const { return at <= n && len <= n - at; }
    uint16_t u16(size_t at) const { return !ok(at, 2) ? 0 : big ? d[at] << 8 | d[at + 1] : d[at + 1] << 8 | d[at]; }
    uint32_t u32(size_t at) const {
        if (!ok(at, 4)) return 0;
        return big ? (uint32_t)d[at] << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3]
                   : (uint32_t)d[at + 3] << 24 | d[at + 2] << 16 | d[at + 1] << 8 | d[at];
    }
};

std::vector<uint8_t> yaz0(const std::vector<uint8_t>& in) {
    if (in.size() < 16 || memcmp(in.data(), "Yaz0", 4)) return in;
    const size_t size = (size_t)in[4] << 24 | in[5] << 16 | in[6] << 8 | in[7];
    std::vector<uint8_t> out;
    out.reserve(size);
    size_t i = 16;
    while (out.size() < size && i < in.size()) {
        const uint8_t code = in[i++];
        for (int b = 0; b < 8 && out.size() < size; b++) {
            if (code & (0x80 >> b)) {
                if (i >= in.size()) return {};
                out.push_back(in[i++]);
                continue;
            }
            if (i + 1 >= in.size()) return {};
            const size_t dist = ((in[i] & 15) << 8 | in[i + 1]) + 1;
            size_t len = in[i] >> 4;
            i += 2;
            if (!len) {
                if (i >= in.size()) return {};
                len = in[i++] + 0x12;
            } else {
                len += 2;
            }
            if (dist > out.size()) return {};
            for (size_t k = 0; k < len && out.size() < size; k++) out.push_back(out[out.size() - dist]);
        }
    }
    return out;
}

// a SARC archive's file `name`: offset and size in the archive (header bytes in `h`, which must reach
// the end of the name table)
bool sarc_find(const uint8_t* h, size_t n, const char* name, size_t* off, size_t* len) {
    if (n < 0x20 || memcmp(h, "SARC", 4)) return false;
    Reader r{h, n, h[6] == 0xFE};
    const uint32_t data = r.u32(0x0C);
    const uint16_t count = r.u16(0x1A);
    const size_t names = 0x20 + (size_t)count * 16 + 8;  // after SFAT entries and the SFNT header
    for (uint16_t k = 0; k < count; k++) {
        const size_t e = 0x20 + (size_t)k * 16;
        const uint32_t attr = r.u32(e + 4), begin = r.u32(e + 8), end = r.u32(e + 12);
        const size_t at = names + (attr & 0xFFFF) * 4;
        if (!r.ok(at, strlen(name) + 1) || memcmp(h + at, name, strlen(name) + 1)) continue;
        if (end < begin) return false;
        *off = (size_t)data + begin;
        *len = end - begin;
        return true;
    }
    return false;
}

}  // namespace

bool parse_bffnt(const uint8_t* d, size_t n, Glyphs& out) {
    if (n < 0x14 || (memcmp(d, "FFNT", 4) && memcmp(d, "CFNT", 4))) return false;
    Reader r{d, n, d[4] == 0xFE};
    const size_t finf = r.u16(6);  // the header's size: FINF follows
    if (!r.ok(finf, 32) || memcmp(d + finf, "FINF", 4)) return false;
    // FINF: ... +0x14 TGLP, +0x18 CWDH, +0x1C CMAP (pointers to block data, 8 bytes after each block's magic)
    size_t cmap = r.u32(finf + 0x1C);
    for (int guard = 0; cmap >= 8 && guard < 4096; guard++) {
        const size_t b = cmap - 8;
        if (!r.ok(b, 20) || memcmp(d + b, "CMAP", 4)) return false;
        const uint16_t first = r.u16(b + 8), last = r.u16(b + 10), method = r.u16(b + 12);
        const size_t p = b + 20;
        if (method == 0) {  // direct: a run of codes
            for (uint32_t c = first; c <= last; c++) out.insert(c);
        } else if (method == 1) {  // table: one glyph index per code, 0xFFFF for none
            for (uint32_t c = first; c <= last; c++)
                if (r.ok(p + (c - first) * 2, 2) && r.u16(p + (c - first) * 2) != 0xFFFF) out.insert(c);
        } else if (method == 2) {  // scan: count, then (code, index) pairs
            const uint16_t count = r.u16(p);
            for (uint16_t k = 0; k < count && r.ok(p + 2 + k * 4, 4); k++) out.insert(r.u16(p + 2 + k * 4));
        }
        cmap = r.u32(b + 16);
    }
    return true;
}

std::shared_ptr<const Glyphs> pack_font(const std::string& pack_path, std::string* why) {
    const fs::path pack(pack_path);
    FILE* f = fopen(pack.string().c_str(), "rb");
    if (!f) {
        *why = "can't open " + pack.string();
        return nullptr;
    }
    std::vector<uint8_t> head(0x20);
    std::vector<uint8_t> inner;
    size_t off = 0, len = 0;
    bool found = false;
    if (fread(head.data(), 1, head.size(), f) == head.size() && !memcmp(head.data(), "SARC", 4)) {
        Reader r{head.data(), head.size(), head[6] == 0xFE};
        const uint32_t data = r.u32(0x0C);  // the header and name table end where the file data starts
        if (data > 0x20 && data < (64u << 20)) {
            head.resize(data);
            if (fread(head.data() + 0x20, 1, data - 0x20, f) == data - 0x20 &&
                sarc_find(head.data(), head.size(), "CKingMsg_bffnt.szs", &off, &len) && len < (64u << 20)) {
                inner.resize(len);
                found = fseek(f, (long)off, SEEK_SET) == 0 && fread(inner.data(), 1, len, f) == len;
            }
        }
    }
    fclose(f);
    if (!found) {
        *why = "no CKingMsg_bffnt.szs in " + pack.string();
        return nullptr;
    }
    inner = yaz0(inner);
    size_t foff = 0, flen = 0;
    if (!sarc_find(inner.data(), inner.size(), "CKingMsg.bffnt", &foff, &flen) || foff > inner.size() || flen > inner.size() - foff) {
        *why = "no CKingMsg.bffnt in " + pack.filename().string();
        return nullptr;
    }
    auto glyphs = std::make_shared<Glyphs>();
    if (!parse_bffnt(inner.data() + foff, flen, *glyphs) || glyphs->empty()) {
        *why = "CKingMsg.bffnt in " + pack.filename().string() + " is no font";
        return nullptr;
    }
    return glyphs;
}

}  // namespace game_font
