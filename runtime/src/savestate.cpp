// Save states: snapshot the whole running game into one of 5 slots and restore it, also in a new
// process.
//
// The recompiled game runs on host threads with native call stacks, so a snapshot can only be taken
// (and restored) when every guest thread is at a point whose complete state is guest memory, its Cpu
// registers and the HLE objects:
//   - the game's main thread at the top of the per-frame function (0203593C; interp.cpp calls
//     service() there), which requests the save/load;
//   - every other guest thread parked in an HLE wait (OSReceiveMessage, OSWaitEvent, OSLockMutex,
//     OSSleepThread, OSSleepTicks, GX2WaitForVsync, ...: park_wait in threads.cpp). A parked thread
//     re-checks its condition after waking and only then consumes anything, so the HLE objects
//     decide where it continues;
//   - service threads (alarms, AX frame) idle between their callbacks.
// threads::quiesce() freezes the game in that state (other threads keep running until they park).
//
// Restoring into a running process requires each thread to be parked at the same place it was
// saved at (same stack pointer, return address and guest back chain => same host call stack); the
// main thread always is. Worker threads normally idle at the same wait. If not, the load is retried
// on the following frames and finally refused with a message; nothing is changed then.
//
// Slot file: header (uncompressed) + LZ4 blocks of the payload: tagged sections from each module
// (threads + HLE sync objects + alarms + guest clock, heaps, open files, AX voices, GX2 registers,
// runtime allocations) and guest memory (MEM2, runtime objects, fixed slots, foreground bucket,
// MEM1; all-zero 64 KiB chunks are left out).
#include "savestate.h"

#ifdef __APPLE__
#include <compression.h>
#include <mach-o/ldsyms.h>
#include <mach-o/loader.h>
#else
#include <elf.h>
#include <link.h>
#include <zlib.h>  // blocks are deflated instead of LZ4 (states don't move between platforms)
#endif
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "runtime.h"
#include "input.h"
#include "mem_writes.h"
#include "release.h"
#include "portable_state.h"

// module sections
bool threads_ss_save(ss::Writer& w, std::string& why);
bool threads_ss_check(ss::Reader r, std::string& why);
void threads_ss_load(ss::Reader& r);
void threads_ss_targets(ss::Reader r);
void mem_ss_save(ss::Writer& w);
bool mem_ss_check(ss::Reader r, std::string& why);
void mem_ss_load(ss::Reader& r);
void fs_ss_save(ss::Writer& w);
void fs_ss_load(ss::Reader& r);
void ax_ss_save(ss::Writer& w);
bool ax_ss_check(ss::Reader r, std::string& why);
void ax_ss_load(ss::Reader& r);
void gx2_ss_drain();
void gx2_ss_save(ss::Writer& w);
bool gx2_ss_check(ss::Reader r, std::string& why);
void gx2_ss_load(ss::Reader& r);
namespace interp { void ss_reset(); }
namespace gfx {
uint64_t frame_count();
void request_tv_dump(const std::string& path, int frames_ahead);
}
namespace dispatch { std::vector<std::pair<uint32_t, std::string>> host_functions(); }

namespace ss {
namespace {

constexpr char kMagic[8] = {'W', 'W', 'H', 'D', 'S', 'T', 'A', 'T'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kChunk = 0x10000;
constexpr uint32_t kBlock = 8 << 20;  // compression block (raw bytes)

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t created;       // unix time
    char area[32];          // stage name
    uint8_t build[16];      // LC_UUID of the executable that wrote it (informational)
    uint64_t game_id;       // hash of cking.rpx
    uint32_t cpu_size;      // sizeof(Cpu)
    uint32_t blocks;        // compressed blocks that follow
    uint64_t raw_size;      // payload bytes
    // added later (files written before end at header_size == kHeaderV1Size and read these as 0)
    uint32_t controller;    // the controls at the time: 0 unknown, 1 GamePad, 2 Pro Controller
    uint32_t reserved;
};
constexpr uint32_t kHeaderV1Size = offsetof(Header, controller);

enum : uint32_t {
    kSecThreads = 'THRD',
    kSecHeaps = 'HEAP',
    kSecFiles = 'FILE',
    kSecAudio = 'AX  ',
    kSecGx2 = 'GX2 ',
    kSecAllocs = 'RALC',
    kSecDispatch = 'DSPT',
    kSecMemory = 'MEM ',
};

struct Region { uint32_t base, size; };

std::vector<Region> regions() {
    uint32_t top = (mem::runtime_top() + kChunk - 1) & ~(kChunk - 1);
    return {{mem::kMem2Start, mem::kMem2End - mem::kMem2Start},
            {mem::kRuntimeStart, top - mem::kRuntimeStart},
            {mem::kFixedStart, mem::kFixedSize},
            {mem::kFgBucket, mem::kFgBucketSize},
            {mem::kMem1, mem::kMem1Size}};
}

// A snapshot in memory: header + payload (sections); memory chunks point into the payload.
struct Snapshot {
    Header h{};
    std::vector<uint8_t> payload;
    std::map<uint32_t, std::pair<size_t, size_t>> sections;  // tag -> (offset, size)
    std::unordered_map<uint32_t, const uint8_t*> chunks;      // chunk address -> data (absent = zero)
    std::vector<Region> regs;
    int slot = 0;

    Reader section(uint32_t tag) const {
        auto it = sections.find(tag);
        if (it == sections.end()) return Reader(nullptr, 0);
        return Reader(payload.data() + it->second.first, it->second.second);
    }
    bool parse() {
        Reader r(payload.data(), payload.size());
        while (r.ok && !r.at_end()) {
            uint32_t tag = r.u32();
            uint64_t n = r.u64();
            size_t off = r.p - payload.data();
            if (!r.bytes(nullptr, n)) return false;
            sections[tag] = {off, (size_t)n};
        }
        if (!r.ok || !sections.count(kSecMemory)) return false;
        Reader m = section(kSecMemory);
        uint32_t nr = m.u32();
        for (uint32_t i = 0; i < nr && m.ok; i++) {
            Region g{m.u32(), m.u32()};
            regs.push_back(g);
            uint32_t present = m.u32();
            for (uint32_t k = 0; k < present && m.ok; k++) {
                uint32_t idx = m.u32();
                chunks[g.base + idx * kChunk] = m.p;
                if (!m.bytes(nullptr, kChunk)) return false;
            }
        }
        return m.ok;
    }
};

std::mutex g_mu;  // messages, pending requests
std::mutex g_io;  // one slot file operation at a time
std::string g_message;
std::chrono::steady_clock::time_point g_message_time;
std::atomic<int> g_save_req{0};
std::shared_ptr<Snapshot> g_load_ready;   // decompressed, waiting for the frame boundary
std::atomic<bool> g_loading{false};       // a slot file is being read
int g_attempts = 0;
const Snapshot* g_check = nullptr;        // snapshot whose memory snap_ld32 reads

void message(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void message(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG("[savestate] %s", buf);
    std::lock_guard<std::mutex> lk(g_mu);
    g_message = buf;
    g_message_time = std::chrono::steady_clock::now();
}

std::string state_dir() {
    static const std::string dir = [] {
        std::string d;
        if (const char* e = getenv("WWHD_STATE_DIR")) d = e;
#ifdef __APPLE__
        else d = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/Library/Application Support/wwhd/states";
#else
        else d = config::save_dir.substr(0, config::save_dir.find_last_of('/')) + "/states";  // next to save/
#endif
        for (size_t i = 1; i <= d.size(); i++)
            if (i == d.size() || d[i] == '/') mkdir(d.substr(0, i).c_str(), 0755);
        return d;
    }();
    return dir;
}
std::string slot_path(int slot, const char* ext = "bin") { return state_dir() + "/slot" + std::to_string(slot) + "." + ext; }

void build_uuid(uint8_t out[16]) {
    memset(out, 0, 16);
#ifdef __APPLE__
    const auto* h = (const mach_header_64*)&_mh_execute_header;
    const uint8_t* p = (const uint8_t*)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const auto* lc = (const load_command*)p;
        if (lc->cmd == LC_UUID) { memcpy(out, ((const uuid_command*)lc)->uuid, 16); return; }
        p += lc->cmdsize;
    }
#else
    // the GNU build id of the image containing this function
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            uintptr_t self = (uintptr_t)&build_uuid;
            bool mine = false;
            for (int i = 0; i < info->dlpi_phnum; i++) {
                const auto& ph = info->dlpi_phdr[i];
                uintptr_t a = info->dlpi_addr + ph.p_vaddr;
                if (ph.p_type == PT_LOAD && self >= a && self < a + ph.p_memsz) mine = true;
            }
            if (!mine) return 0;
            for (int i = 0; i < info->dlpi_phnum; i++) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE) continue;
                const uint8_t* p = (const uint8_t*)(info->dlpi_addr + ph.p_vaddr);
                const uint8_t* end = p + ph.p_memsz;
                while (p + sizeof(ElfW(Nhdr)) <= end) {
                    const auto* n = (const ElfW(Nhdr)*)p;
                    const uint8_t* desc = p + sizeof *n + ((n->n_namesz + 3) & ~3u);
                    if (n->n_type == NT_GNU_BUILD_ID) {
                        memcpy(data, desc, std::min<size_t>(16, n->n_descsz));
                        return 1;
                    }
                    p = desc + ((n->n_descsz + 3) & ~3u);
                }
            }
            return 1;
        },
        out);
#endif
}

// mincore() takes char* on macOS, unsigned char* on Linux
int page_residency(void* p, size_t n, char* vec) {
#ifdef __APPLE__
    return mincore(p, n, vec);
#else
    return mincore(p, n, (unsigned char*)vec);
#endif
}

// one block of the slot file; 0 if it doesn't get smaller
size_t compress_block(uint8_t* dst, size_t dstSize, const uint8_t* src, size_t n, void* scratch) {
#ifdef __APPLE__
    return compression_encode_buffer(dst, dstSize, src, n, (uint8_t*)scratch, COMPRESSION_LZ4);
#else
    uLongf out = dstSize;
    return compress2(dst, &out, src, n, 1) == Z_OK ? out : 0;
#endif
}
size_t decompress_block(uint8_t* dst, size_t n, const uint8_t* src, size_t srcSize, void* scratch) {
#ifdef __APPLE__
    return compression_decode_buffer(dst, n, src, srcSize, (uint8_t*)scratch, COMPRESSION_LZ4);
#else
    uLongf out = n;
    return uncompress(dst, &out, src, srcSize) == Z_OK ? out : 0;
#endif
}
size_t scratch_size(bool encode) {
#ifdef __APPLE__
    return encode ? compression_encode_scratch_buffer_size(COMPRESSION_LZ4) : compression_decode_scratch_buffer_size(COMPRESSION_LZ4);
#else
    (void)encode;
    return 1;
#endif
}

uint64_t game_id() {
    static const uint64_t id = [] {
        uint64_t h = 0xcbf29ce484222325ull;
        FILE* f = fopen((config::game_dir + "/code/cking.rpx").c_str(), "rb");
        if (!f) return h;
        std::vector<uint8_t> buf(1 << 20);
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
            for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 0x100000001b3ull;
        fclose(f);
        return h;
    }();
    return id;
}

// current stage (dComIfG_gameInfo.play: the start stage, 8 chars)
const release::Data kStageInfo{0x1046F0B0};
constexpr uint32_t kStageNameOff = 0x5134;  // dStage_startStage_c (next stage at +0x5140)
std::string stage_name() {
    if (const char* e = getenv("WWHD_STATE_STAGE_ADDR")) {
        uint32_t a = (uint32_t)strtoul(e, nullptr, 16);
        return std::string((const char*)mem::ptr(a), strnlen((const char*)mem::ptr(a), 8));
    }
    const char* p = (const char*)mem::ptr(kStageInfo + kStageNameOff);
    size_t n = strnlen(p, 8);
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] > 0x7E) return "";
    return std::string(p, n);
}

// ---------------------------------------------------------------- memory
void capture_memory(Writer& w) {
    auto rs = regions();
    w.u32((uint32_t)rs.size());
    std::vector<char> vec(kChunk / getpagesize() + 1);
    for (auto& g : rs) {
        w.u32(g.base);
        w.u32(g.size);
        size_t count_at = w.b.size();
        w.u32(0);
        uint32_t present = 0;
        for (uint32_t off = 0; off < g.size; off += kChunk) {
            uint8_t* p = mem::ptr(g.base + off);
            // untouched pages are zero: skip them without reading (reading would commit them)
            if (page_residency(p, kChunk, vec.data()) == 0) {
                bool touched = false;
                for (size_t i = 0; i < kChunk / (size_t)getpagesize(); i++) touched |= vec[i] != 0;
                if (!touched) continue;
            }
            const uint64_t* q = (const uint64_t*)p;
            bool zero = true;
            for (size_t i = 0; i < kChunk / 8 && zero; i++) zero = q[i] == 0;
            if (zero) continue;
            w.u32(off / kChunk);
            w.bytes(p, kChunk);
            present++;
        }
        memcpy(&w.b[count_at], &present, 4);
    }
}

void restore_memory(const Snapshot& s) {
    std::vector<char> vec(kChunk / getpagesize() + 1);
    for (auto& g : s.regs) {
        for (uint32_t off = 0; off < g.size; off += kChunk) {
            uint32_t a = g.base + off;
            uint8_t* p = mem::ptr(a);
            auto it = s.chunks.find(a);
            if (it != s.chunks.end()) {
                memcpy(p, it->second, kChunk);
                continue;
            }
            if (page_residency(p, kChunk, vec.data()) == 0) {
                bool touched = false;
                for (size_t i = 0; i < kChunk / (size_t)getpagesize(); i++) touched |= vec[i] != 0;
                if (!touched) continue;
            }
            const uint64_t* q = (const uint64_t*)p;
            for (size_t i = 0; i < kChunk / 8; i++)
                if (q[i]) { memset(p, 0, kChunk); break; }
        }
    }
}

// ---------------------------------------------------------------- slot files
bool write_slot(int slot, const Header& h0, const std::vector<uint8_t>& payload) {
    Header h = h0;
    size_t nblocks = (payload.size() + kBlock - 1) / kBlock;
    h.blocks = (uint32_t)nblocks;
    h.raw_size = payload.size();
    std::vector<std::vector<uint8_t>> out(nblocks);
    std::vector<std::thread> pool;
    std::atomic<size_t> next{0};
    unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    for (unsigned t = 0; t < nt; t++)
        pool.emplace_back([&] {
            std::vector<uint8_t> scratch(scratch_size(true));
            for (size_t i; (i = next++) < nblocks;) {
                size_t raw = std::min<size_t>(kBlock, payload.size() - i * kBlock);
                std::vector<uint8_t>& o = out[i];
                o.resize(8 + raw + raw / 8 + 1024);
                size_t n = compress_block(o.data() + 8, o.size() - 8, payload.data() + i * kBlock, raw, scratch.data());
                uint32_t hdr[2] = {(uint32_t)raw, (uint32_t)n};
                if (!n || n >= raw) {  // incompressible: stored
                    memcpy(o.data() + 8, payload.data() + i * kBlock, raw);
                    hdr[1] = 0;
                    n = raw;
                }
                memcpy(o.data(), hdr, 8);
                o.resize(8 + n);
            }
        });
    for (auto& t : pool) t.join();
    std::string tmp = slot_path(slot, "tmp");
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(&h, sizeof h, 1, f) == 1;
    for (auto& o : out) ok = ok && fwrite(o.data(), 1, o.size(), f) == o.size();
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp.c_str(), slot_path(slot).c_str()) == 0;
    else unlink(tmp.c_str());
    return ok;
}

bool read_header(FILE* f, Header& h, std::string& why) {
    memset(&h, 0, sizeof h);
    if (fread(&h, kHeaderV1Size, 1, f) != 1 || memcmp(h.magic, kMagic, 8) != 0) { why = "not a save state"; return false; }
    if (h.version != kVersion || h.header_size < kHeaderV1Size || h.header_size > 4096) { why = "saved by another version"; return false; }
    // the fields added since (a newer header's unknown rest is skipped)
    size_t known = std::min<size_t>(h.header_size, sizeof h) - kHeaderV1Size;
    uint32_t headerSize = h.header_size;
    if (known && fread((uint8_t*)&h + kHeaderV1Size, known, 1, f) != 1) { why = "file is truncated"; return false; }
    if (headerSize > sizeof h && fseek(f, headerSize - sizeof h, SEEK_CUR) != 0) { why = "file is truncated"; return false; }
    if (h.cpu_size != sizeof(Cpu)) { why = "saved by an incompatible build"; return false; }
    if (h.game_id != game_id()) { why = "saved with a different game executable"; return false; }
    return true;
}

std::shared_ptr<Snapshot> read_slot(int slot, std::string& why) {
    FILE* f = fopen(slot_path(slot).c_str(), "rb");
    if (!f) { why = "empty"; return nullptr; }
    auto s = std::make_shared<Snapshot>();
    s->slot = slot;
    if (!read_header(f, s->h, why)) { fclose(f); return nullptr; }
    std::vector<std::vector<uint8_t>> blocks(s->h.blocks);
    std::vector<uint32_t> raws(s->h.blocks), comps(s->h.blocks);
    bool ok = true;
    for (uint32_t i = 0; i < s->h.blocks && ok; i++) {
        uint32_t hdr[2];
        ok = fread(hdr, 8, 1, f) == 1 && hdr[0] <= kBlock;
        if (!ok) break;
        raws[i] = hdr[0];
        comps[i] = hdr[1];
        blocks[i].resize(hdr[1] ? hdr[1] : hdr[0]);
        ok = fread(blocks[i].data(), 1, blocks[i].size(), f) == blocks[i].size();
    }
    fclose(f);
    if (!ok) { why = "file is truncated"; return nullptr; }
    s->payload.resize(s->h.raw_size);
    std::atomic<size_t> next{0};
    std::atomic<bool> bad{false};
    std::vector<std::thread> pool;
    unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    for (unsigned t = 0; t < nt; t++)
        pool.emplace_back([&] {
            std::vector<uint8_t> scratch(scratch_size(false));
            for (size_t i; (i = next++) < blocks.size();) {
                uint8_t* dst = s->payload.data() + i * (size_t)kBlock;
                if ((size_t)i * kBlock + raws[i] > s->payload.size()) { bad = true; continue; }
                if (!comps[i]) { memcpy(dst, blocks[i].data(), raws[i]); continue; }
                size_t n = decompress_block(dst, raws[i], blocks[i].data(), comps[i], scratch.data());
                if (n != raws[i]) bad = true;
            }
        });
    for (auto& t : pool) t.join();
    if (bad || !s->parse()) { why = "file is corrupt"; return nullptr; }
    return s;
}

// ---------------------------------------------------------------- save / load at the frame boundary
void put_section(Writer& w, uint32_t tag, const Writer& sec) {
    w.u32(tag);
    w.u64(sec.b.size());
    w.bytes(sec.b.data(), sec.b.size());
}

void save_allocs(Writer& w) {
    auto log = mem::runtime_alloc_log();
    w.u32(mem::runtime_top());
    w.u32((uint32_t)log.size());
    for (auto& a : log) w.pod(a);
}

bool check_allocs(Reader r, std::string& why) {
    uint32_t top = r.u32();
    (void)top;
    uint32_t n = r.u32();
    auto cur = mem::runtime_alloc_log();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        mem::AllocRec a = r.pod<mem::AllocRec>();
        if (i >= cur.size()) break;  // allocated later in the saved session: beyond our top
        const mem::AllocRec& b = cur[i];
        // (the allocating code address, a.tag, changes with every rebuild: only logged)
        if (a.tag != b.tag && i < 4) LOG("[savestate] runtime object #%u allocated by different code (rebuilt?)", i);
        if (a.addr != b.addr || a.size != b.size) {
            char buf[160];
            snprintf(buf, sizeof buf, "runtime objects are laid out differently (#%u: %08X+%X vs %08X+%X)", i, a.addr, a.size, b.addr,
                     b.size);
            why = buf;
            return false;
        }
    }
    return r.ok;
}

void save_dispatch(Writer& w) {
    auto v = dispatch::host_functions();
    w.u32((uint32_t)v.size());
    for (auto& [a, n] : v) {
        w.u32(a);
        w.str(n);
    }
}

bool check_dispatch(Reader r, std::string& why) {
    std::unordered_map<uint32_t, std::string> cur;
    for (auto& [a, n] : dispatch::host_functions()) cur[a] = n;
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        uint32_t a = r.u32();
        std::string name = r.str();
        auto it = cur.find(a);
        if (it == cur.end() || it->second != name) {
            why = "host function " + name + " is not registered at the same address";
            return false;
        }
    }
    return r.ok;
}

std::string area_label(const char* stage) {
    static const std::map<std::string, std::string> names = {
        {"sea", "Great Sea"}, {"LinkRM", "Link's House"}, {"MajyuE", "Forsaken Fortress"}, {"M_NewD2", "Dragon Roost Cavern"},
        {"kindan", "Forbidden Woods"}, {"Siren", "Tower of the Gods"}, {"Asoko", "Tetra's Ship"},
    };
    auto it = names.find(stage);
    return it == names.end() ? std::string(stage) : it->second;
}

// true when done (saved or failed for good); false to retry on the next frame
bool do_save(int slot) {
    std::string busy, why;
    auto t0 = std::chrono::steady_clock::now();
    if (!threads::quiesce(250, busy, 1, 30)) {
        threads::thaw();
        if (++g_attempts < 30) return false;
        message("Slot %d: not saved (game busy:%s)", slot, busy.c_str());
        return true;
    }
    gx2_ss_drain();
    Writer threads_w;
    if (!threads_ss_save(threads_w, why)) {
        threads::thaw();
        if (++g_attempts < 30) return false;
        message("Slot %d: not saved (%s)", slot, why.c_str());
        return true;
    }
    auto payload = std::make_shared<Writer>();
    payload->b.reserve(512u << 20);
    put_section(*payload, kSecThreads, threads_w);
    Writer w;
    mem_ss_save(w);
    put_section(*payload, kSecHeaps, w);
    w = Writer();
    fs_ss_save(w);
    put_section(*payload, kSecFiles, w);
    w = Writer();
    ax_ss_save(w);
    put_section(*payload, kSecAudio, w);
    w = Writer();
    gx2_ss_save(w);
    put_section(*payload, kSecGx2, w);
    w = Writer();
    save_allocs(w);
    put_section(*payload, kSecAllocs, w);
    w = Writer();
    save_dispatch(w);
    put_section(*payload, kSecDispatch, w);
    // memory last, written straight into the payload
    payload->u32(kSecMemory);
    size_t len_at = payload->b.size();
    payload->u64(0);
    capture_memory(*payload);
    uint64_t mlen = payload->b.size() - len_at - 8;
    memcpy(&payload->b[len_at], &mlen, 8);
    Header h{};
    memcpy(h.magic, kMagic, 8);
    h.version = kVersion;
    h.header_size = sizeof(Header);
    h.created = (uint64_t)time(nullptr);
    std::string stage = stage_name();
    snprintf(h.area, sizeof h.area, "%s", stage.c_str());
    build_uuid(h.build);
    h.game_id = game_id();
    h.cpu_size = sizeof(Cpu);
    h.controller = input::pro_controller() ? 2 : 1;
    threads::thaw();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOG("[savestate] slot %d: captured %.1f MB in %.1f ms (stage %s)", slot, payload->b.size() / 1048576.0, ms, stage.c_str());
    gfx::request_tv_dump(slot_path(slot, "png"), 0);
    // compress and write in the background; the game continues
    std::thread([slot, h, payload, stage] {
        auto t1 = std::chrono::steady_clock::now();
        bool ok;
        {
            std::lock_guard<std::mutex> io(g_io);
            ok = write_slot(slot, h, payload->b);
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        struct stat st{};
        stat(slot_path(slot).c_str(), &st);
        LOG("[savestate] slot %d: %s (%.1f MB on disk, %.0f ms)", slot, ok ? "written" : "WRITE FAILED", st.st_size / 1048576.0, ms);
        std::lock_guard<std::mutex> lk(g_mu);
        g_message = ok ? "Saved to slot " + std::to_string(slot) + (stage.empty() ? "" : " (" + area_label(stage.c_str()) + ")")
                       : "Slot " + std::to_string(slot) + ": write failed";
        g_message_time = std::chrono::steady_clock::now();
    }).detach();
    return true;
}

bool do_load(const std::shared_ptr<Snapshot>& s) {
    std::string busy, why;
    auto t0 = std::chrono::steady_clock::now();
    threads_ss_targets(s->section(kSecThreads));
    if (!threads::quiesce(250, busy, 2, 0)) {
        threads::thaw();
        if (++g_attempts < 30) return false;
        message("Slot %d: not loaded (game busy:%s)", s->slot, busy.c_str());
        return true;
    }
    gx2_ss_drain();
    g_check = s.get();
    bool ok = check_allocs(s->section(kSecAllocs), why) && check_dispatch(s->section(kSecDispatch), why) &&
              mem_ss_check(s->section(kSecHeaps), why) && ax_ss_check(s->section(kSecAudio), why) &&
              gx2_ss_check(s->section(kSecGx2), why);
    bool layout_ok = ok;
    ok = ok && threads_ss_check(s->section(kSecThreads), why);
    g_check = nullptr;
    if (!ok) {
        threads::thaw();
        // threads at other waits right now may be where they were saved on a later frame
        if (layout_ok && ++g_attempts < 30) {
            if (g_attempts == 1) LOG("[savestate] slot %d: waiting for the game threads (%s)", s->slot, why.c_str());
            return false;
        }
        message("Slot %d: cannot load here (%s)", s->slot, why.c_str());
        return true;
    }
    restore_memory(*s);
    Reader r = s->section(kSecAllocs);
    mem::raise_runtime_top(r.u32());
    r = s->section(kSecHeaps);
    mem_ss_load(r);
    r = s->section(kSecFiles);
    fs_ss_load(r);
    r = s->section(kSecAudio);
    ax_ss_load(r);
    r = s->section(kSecGx2);
    gx2_ss_load(r);
    interp::ss_reset();
    r = s->section(kSecThreads);
    threads_ss_load(r);  // last: wakes the parked threads (they continue after the thaw)
    threads::thaw();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string area = s->h.area[0] ? " (" + area_label(s->h.area) + ")" : "";
    // the controls the state was saved with: the game expects input from that controller
    bool pro = s->h.controller == 2;
    if (s->h.controller && pro != input::pro_controller()) {
        input::set_pro_controller(pro);
        message("Loaded slot %d%s; controls act as %s again", s->slot, area.c_str(), pro ? "Wii U Pro Controller" : "Wii U GamePad");
    } else {
        message("Loaded slot %d%s", s->slot, area.c_str());
    }
    memw::mark_all();  // guest memory replaced: the renderer copies everything again
    LOG("[savestate] slot %d: restored in %.1f ms", s->slot, ms);
    return true;
}

// test aid: WWHD_STATE_SAVE_AT=frame:slot,...  WWHD_STATE_LOAD_AT=frame:slot,... (TV frames)
struct Timed { uint64_t frame; int slot; };
std::vector<Timed> parse_timed(const char* var) {
    std::vector<Timed> v;
    const char* e = getenv(var);
    unsigned long long f;
    int s, n;
    while (e && sscanf(e, "%llu:%d%n", &f, &s, &n) == 2) {
        v.push_back({f, s});
        e += n;
        if (*e != ',') break;
        e++;
    }
    return v;
}

// ---------------------------------------------------------------- portable states (portable_state.h)
// Small text states (a few KB: the Quest Log's save data and Link's place) for bug reports, made
// and applied on the game thread at the frame boundary with the game's own save functions
// (docs/portable-save-states.md). Unlike upstream, full snapshots stay this app's default: a
// portable state is written on request (the bug-report flow, WWHD_PORTABLE_SAVE, imports), and a
// slot load takes the slot's newer file of either kind.
//
// Game addresses below are the canonical USA ones, mapped to the running release (release.h); the
// structures the game itself lays out are the same in every release.
bool in_mem2(uint32_t a) { return a >= mem::kMem2Start && a < mem::kMem2End; }

constexpr uint32_t kSaveInfoUsa = 0x101F84DC;  // dComIfGs save area (dSv_info_c at +0x20)
constexpr uint32_t kPlayUsa = 0x1046F0B0;      // g_dComIfG_gameInfo.play
constexpr uint32_t kMenuFlagUsa = 0x101EA069;  // dMenu_flag
inline uint32_t ps_data(uint32_t usa) { return release::Data{usa}; }
inline uint32_t ps_code(uint32_t usa) { return release::Code{usa}; }
inline uint32_t kSaveInfoPtr() { return ps_data(kSaveInfoUsa); }
inline uint32_t kPlay() { return ps_data(kPlayUsa); }
inline uint32_t kStartStage() { return kPlay() + 0x5134; }  // dStage_startStage_c
inline uint32_t kNextStage() { return kPlay() + 0x5140; }   // dStage_nextStage_c
inline uint32_t kStageData() { return kPlay() + 0x5150; }   // dStage_stageDt_c
inline uint32_t kLinkPtr() { return kPlay() + 0x5B34; }     // mpPlayerPtr[0]: daPy_lk_c
inline uint32_t kShipPtr() { return kPlay() + 0x5B3C; }     // daShip_c (0 without a boat)
constexpr uint32_t kActorPos = 0x314, kActorRoom = 0x326, kShapeAngleY = 0x32A;  // fopAc_ac_c
constexpr uint32_t kLinkProc = 0x65F0;           // daPy_lk_c::mCurProc
constexpr uint32_t kInfo = 0x20;                 // dSv_info_c in the save area
constexpr uint32_t kDataNum = kInfo + 0x1290;    // dSv_info_c::mDataNum (Quest Log 0..2)
constexpr uint32_t kReturnPlace = 0x50;          // dSv_player_return_place_c (0xC bytes)
constexpr uint32_t kMemoryTable = kInfo + 0x380;  // dSv_save_c::mSave[16] (dSv_memory_c, 0x24 each)
constexpr uint32_t kTurnRestart = kInfo + 0x1258;  // dSv_turnRestart_c
constexpr uint32_t kRestart = kInfo + 0x1128;      // dSv_restart_c
constexpr uint32_t kHdArea = 0x12C0;               // HD per-file data (SaveMgr getters 027200A0..)
constexpr uint32_t kCardStatusB = 0x18;  // status B in the card block: time 0xC (f32), date 0x10 (u16)
// game functions (USA; mapped at the call)
constexpr uint32_t fn_memory_to_card = 0x025BA9FC, fn_card_to_memory = 0x025BA7B0, fn_putSave = 0x025B9D24,
                   fn_getSave = 0x025B9C9C, fn_setGameStartStage = 0x025217F8, fn_danInit = 0x025B9174,
                   fn_eventInit = 0x025B8B10, fn_refreshGame = 0x02721880, fn_turnRestartSet = 0x025B998C;
// HD sections: (slot getter, live getter, live <- slot copy) and size in the file
struct HdSection { uint32_t slot_get, live_get, copy; size_t size; std::vector<uint8_t> pstate::State::*field; };
const HdSection kHdSections[] = {
    {0x027200A0, 0x027200D0, 0x0271FCB4, pstate::kHdPlayerSize, &pstate::State::hd_player},
    {0x027200D8, 0x027200F4, 0x0271FAC0, pstate::kHdStatusSize, &pstate::State::hd_status},
    {0x027200FC, 0x02720118, 0x0271F914, pstate::kHdEventSize, &pstate::State::hd_event},
    {0x02720180, 0x0272019C, 0x027208F4, pstate::kHdMapSize, &pstate::State::hd_map},
};
constexpr uint32_t fn_name_obj = 0x02720154;  // per-file player name (SafeString: +0 UTF-16 text)
constexpr uint32_t fn_ovlpDoingReq = 0x025DBE38;  // fopOvlpM_IsDoingReq (a wipe or scene overlap runs)

// every address above maps to the running release (0 = inside a function that differs there)
bool ps_addrs_ok() {
    static const int ok = [] {
        if (!ps_data(kSaveInfoUsa) || !ps_data(kPlayUsa) || !ps_data(kMenuFlagUsa)) return 0;
        for (uint32_t f : {fn_memory_to_card, fn_card_to_memory, fn_putSave, fn_getSave, fn_setGameStartStage, fn_danInit,
                           fn_eventInit, fn_refreshGame, fn_turnRestartSet, fn_name_obj, fn_ovlpDoingReq})
            if (!ps_code(f)) return 0;
        for (auto& h : kHdSections)
            if (!ps_code(h.slot_get) || !ps_code(h.live_get) || !ps_code(h.copy)) return 0;
        return 1;
    }();
    return ok;
}

// guest calls from the frame boundary: every register is put back afterwards
struct CallScope {
    Cpu* c;
    Cpu saved;
    explicit CallScope(Cpu* cpu) : c(cpu), saved(*cpu) {}
    ~CallScope() {
        uint32_t core = c->core;
        void* th = c->thread;
        *c = saved;
        c->core = core;
        c->thread = th;
    }
};

uint32_t ps_scratch() {  // host-only guest buffer for the game's card functions (not part of states)
    static const uint32_t s = mem::host_alloc(0x2000, 64);
    return s;
}

// the stage's save-table index (dStage_stagInfo_GetSaveTbl), as dStage_Delete computes it
int current_stage_no(Cpu* c) {
    uint32_t vt = ld32(kStageData());
    if (!in_mem2(vt)) return -1;
    uint32_t stag = guest_call(c, ld32(vt + 0x15C), {kStageData()});
    if (!in_mem2(stag)) return -1;
    return (ld8(stag + 9) >> 1) & 0x7F;
}

// a Quest Log is being played (not the title screen or file select) and nothing is changing stage
bool gameplay_ready(std::string& why, bool& retry) {
    retry = false;
    if (!ps_addrs_ok()) { why = "portable states are not mapped for this game release"; return false; }
    uint32_t sv = ld32(kSaveInfoPtr());
    if (!in_mem2(sv)) { why = "no game in progress"; return false; }
    std::string st = stage_name();
    if (st.empty() || st == "sea_T" || st == "Name" || st == "ENDumi") {
        why = "no game in progress (start or continue a Quest Log first)";
        retry = true;
        return false;
    }
    uint32_t link = ld32(kLinkPtr());
    if (!in_mem2(link)) { why = "Link is not in the scene"; retry = true; return false; }
    if (ld8(kNextStage() + 12)) { why = "a stage change is in progress"; retry = true; return false; }
    if (ld8(sv + kDataNum) > 2) { why = "no Quest Log loaded"; retry = true; return false; }
    return true;
}

// Link is under the player's control: the conditions under which the game opens its pause menu
// (no event for 5 frames, no message box, no menu, no stage change or wipe in progress, Link is
// the controlled actor, Link is not on a rope). Updated every frame for the event delay.
inline uint32_t kEventRun() { return kPlay() + 0x5292; }  // dComIfGp_event_runCheck
inline uint32_t kMesgStatus() { return kPlay() + 0x5BB2; }
inline uint32_t kScopeMesgStatus() { return kPlay() + 0x5BB3; }
inline uint32_t kMenuFlag() { return ps_data(kMenuFlagUsa); }
inline uint32_t kPlayerPtr() { return kPlay() + 0x5B2C; }     // mpPlayer[0]: the controlled actor
inline uint32_t kPlayerStatus0() { return kPlay() + 0x5CD8; }  // mPlayerStatus[0][0]
constexpr uint32_t kSttsRope = 0x00800000;  // daPyStts0_UNK800000_e: set by Link's rope procedures
int g_event_wait = 0;
void track_events() {
    if (ld8(kEventRun())) g_event_wait = 5;
    else if (g_event_wait > 0) g_event_wait--;
}
bool player_has_control(Cpu* c, std::string& why) {
    if (ld8(kEventRun()) || g_event_wait > 0) { why = "a cutscene or event is running"; return false; }
    if (ld8(kMesgStatus()) || ld8(kScopeMesgStatus())) { why = "a message or dialogue is open"; return false; }
    if (ld8(kMenuFlag())) { why = "a game menu is open"; return false; }
    if (ld8(kNextStage() + 12)) { why = "a stage change is in progress"; return false; }
    if (ld32(kPlayerPtr()) != ld32(kLinkPtr())) { why = "Link is not the controlled character"; return false; }
    // on a rope or swinging from the Grappling Hook: he would restart in mid-air
    if (ld32(kPlayerStatus0()) & kSttsRope) { why = "Link is on a rope"; return false; }
    CallScope scope(c);
    if (guest_call(c, ps_code(fn_ovlpDoingReq)) & 0xFF) { why = "a scene transition is in progress"; return false; }
    return true;
}

std::string meta_value(const char* key) {
    static const std::string meta = [] {
        std::string s;
        if (FILE* f = fopen((config::game_dir + "/meta/meta.xml").c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && s.size() < (1u << 20)) s.append(buf, n);
            fclose(f);
        }
        return s;
    }();
    std::string open = std::string("<") + key;
    size_t p = meta.find(open);
    if (p == std::string::npos) return "";
    p = meta.find('>', p);
    size_t e = meta.find('<', p);
    if (p == std::string::npos || e == std::string::npos) return "";
    return meta.substr(p + 1, e - p - 1);
}
std::string title_id() { std::string t = meta_value("title_id"); return t.empty() ? "0005000010143500" : t; }

std::string hex64(uint64_t v) { char b[20]; snprintf(b, sizeof b, "%016llx", (unsigned long long)v); return b; }

std::string utf8_from_utf16be(uint32_t p, int max) {
    std::string s;
    for (int i = 0; i < max && in_mem2(p); i++, p += 2) {
        uint16_t ch = ld16(p);
        if (!ch) break;
        if (ch < 0x80) s += (char)ch;
        else if (ch < 0x800) { s += (char)(0xC0 | ch >> 6); s += (char)(0x80 | (ch & 0x3F)); }
        else { s += (char)(0xE0 | ch >> 12); s += (char)(0x80 | (ch >> 6 & 0x3F)); s += (char)(0x80 | (ch & 0x3F)); }
    }
    return s;
}

std::string now_text() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

float be_f32(const uint8_t* p) { return u32_as_f32((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]); }

#ifdef WWHD_RUNTIME_VERSION
constexpr const char* kRuntimeVersion = WWHD_RUNTIME_VERSION;
#else
constexpr const char* kRuntimeVersion = "android-0.6 (unknown)";
#endif

// the current game as a portable state (game thread, frame boundary)
bool capture_portable(Cpu* c, pstate::State& s, std::string& why) {
    bool retry;
    if (!gameplay_ready(why, retry)) return false;
    CallScope scope(c);
    const uint32_t sv = ld32(kSaveInfoPtr()), info = sv + kInfo, buf = ps_scratch();
    std::string stage = stage_name();
    int stage_no = current_stage_no(c);
    // what the in-game save does first, on a copy: the live game stays unchanged
    uint8_t rp[0xC], entry[0x24];
    memcpy(rp, mem::ptr(sv + kReturnPlace), sizeof rp);
    if (stage_no >= 0 && stage_no < 16) {
        memcpy(entry, mem::ptr(sv + kMemoryTable + stage_no * 0x24), sizeof entry);
        guest_call(c, ps_code(fn_putSave), {info, (uint32_t)stage_no});
    }
    if (stage != "PShip") guest_call(c, ps_code(fn_setGameStartStage));  // (PShip would also set the next time of day)
    memset(mem::ptr(buf), 0, pstate::kSaveDataSize);
    int32_t r = (int32_t)guest_call(c, ps_code(fn_memory_to_card), {info, buf, 0});
    memcpy(mem::ptr(sv + kReturnPlace), rp, sizeof rp);
    if (stage_no >= 0 && stage_no < 16) memcpy(mem::ptr(sv + kMemoryTable + stage_no * 0x24), entry, sizeof entry);
    if (r == -1) { why = "the game's save function failed"; return false; }
    s.savedata.assign(mem::ptr(buf), mem::ptr(buf) + pstate::kSaveDataSize);
    pstate::seal_savedata(s.savedata);
    // HD per-file sections: this file's stored copy with the live data copied over
    const uint32_t hd = sv + kHdArea;
    const int slot = ld8(sv + kDataNum);
    for (auto& h : kHdSections) {
        uint32_t stored = guest_call(c, ps_code(h.slot_get), {hd, (uint32_t)slot}),
                 live = guest_call(c, ps_code(h.live_get), {hd});
        memcpy(mem::ptr(buf), mem::ptr(stored), h.size);
        guest_call(c, ps_code(h.copy), {buf, live});
        (s.*h.field).assign(mem::ptr(buf), mem::ptr(buf) + h.size);
    }
    uint32_t name_obj = guest_call(c, ps_code(fn_name_obj), {hd, (uint32_t)slot});
    s.player_name = utf8_from_utf16be(ld32(name_obj), 8);
    // header and place
    s.title_id = title_id();
    s.title_version = (uint32_t)strtoul(meta_value("title_version").c_str(), nullptr, 10);
    s.game_hash = hex64(game_id());
    s.runtime = kRuntimeVersion;
    s.created = now_text();
    s.file_slot = slot;
    s.stage = stage;
    s.start_point = (int16_t)ld16(kStartStage() + 8);
    s.start_room = (int8_t)ld8(kStartStage() + 10);
    s.layer = (int8_t)ld8(kStartStage() + 11);
    const uint32_t link = ld32(kLinkPtr());
    for (int i = 0; i < 3; i++) s.pos[i] = (float)ldf32(link + kActorPos + 4 * i);
    s.room = (int8_t)ld8(link + kActorRoom);
    s.angle_y = (int16_t)ld16(link + kShapeAngleY);
    s.link_proc = (int)ld32(link + kLinkProc);
    const uint32_t ship = ld32(kShipPtr());
    s.has_ship = in_mem2(ship);
    if (s.has_ship) {
        for (int i = 0; i < 3; i++) s.ship_pos[i] = (float)ldf32(ship + kActorPos + 4 * i);
        s.ship_angle_y = (int16_t)ld16(ship + kShapeAngleY);
    }
    // daPyProc_SHIP_READY..SHIP_RESTART (not SHIP_GET_OFF): riding the boat
    s.on_ship = s.has_ship && s.link_proc >= 0x86 && s.link_proc <= 0x91 && s.link_proc != 0x90;
    s.time_of_day = be_f32(&s.savedata[kCardStatusB + 0xC]);
    s.date = s.savedata[kCardStatusB + 0x10] << 8 | s.savedata[kCardStatusB + 0x11];
    return true;
}

// the HD per-file sections into the live game (SaveMgr direction: live <- stored)
void apply_hd_sections(Cpu* c, const pstate::State& s) {
    const uint32_t hd = ld32(kSaveInfoPtr()) + kHdArea, buf = ps_scratch();
    for (auto& h : kHdSections) {
        memcpy(mem::ptr(buf), (s.*h.field).data(), h.size);
        guest_call(c, ps_code(h.copy), {guest_call(c, ps_code(h.live_get), {hd}), buf});
    }
}

// puts a portable state into the running game; false + retry when the game is not ready yet
bool apply_portable(Cpu* c, const pstate::State& s, std::string& why, bool& retry) {
    if (!gameplay_ready(why, retry)) return false;
    CallScope scope(c);
    const uint32_t sv = ld32(kSaveInfoPtr()), info = sv + kInfo, buf = ps_scratch();
    memcpy(mem::ptr(buf), s.savedata.data(), pstate::kSaveDataSize);
    if ((int32_t)guest_call(c, ps_code(fn_card_to_memory), {info, buf, 0}) == -1) {
        why = "the game refused the save data";
        return false;
    }
    int stage_no = current_stage_no(c);
    if (stage_no >= 0 && stage_no < 16) guest_call(c, ps_code(fn_getSave), {info, (uint32_t)stage_no});
    guest_call(c, ps_code(fn_danInit), {info + 0x79C, 0xFFFFFFFFu});  // dSv_danBit_c::init(-1)
    guest_call(c, ps_code(fn_eventInit), {info + 0x1158});            // dSv_info_c::mTmp (temporary flags)
    apply_hd_sections(c, s);
    guest_call(c, ps_code(fn_refreshGame), {0});  // SaveMgr: item buttons and equipment from the loaded data
    // Link's place: the void-out restart (point -1), or with the boat the Song of Passing restart
    // (point -3, the boat back at its position, Link on it with start mode 2)
    const uint32_t rs = sv + kRestart;
    st8(rs + 0, (uint8_t)s.room);
    st16(rs + 0x16, (uint16_t)s.angle_y);
    for (int i = 0; i < 3; i++) stf32(rs + 0x18 + 4 * i, s.pos[i]);
    // player parameters: room, start mode 0 (standing), no start event
    st32(rs + 0x24, (uint32_t)(s.room & 0x3F) | 0xFF000000u);
    stf32(rs + 0x28, 0.0);
    st32(rs + 0x2C, 0);
    char name[8] = {};
    memcpy(name, s.stage.data(), std::min<size_t>(s.stage.size(), 7));
    for (int i = 0; i < 8; i++) st8(kNextStage() + i, (uint8_t)name[i]);
    uint16_t point = 0xFFFF;  // point -1: the restart position
    if (s.has_ship) {
        uint32_t param = (uint32_t)(s.room & 0x3F) | (uint32_t)(s.on_ship ? 2 : 0) << 12 | 0xFF000000u | 0x100u;
        for (int i = 0; i < 3; i++) {
            stf32(buf + 4 * i, s.pos[i]);
            stf32(buf + 0x10 + 4 * i, s.ship_pos[i]);
        }
        guest_call(c, ps_code(fn_turnRestartSet), {sv + kTurnRestart, buf, (uint32_t)(uint16_t)s.angle_y, (uint32_t)(uint8_t)s.room,
                                                  param, buf + 0x10, (uint32_t)(uint16_t)s.ship_angle_y, 1});
        point = 0xFFFD;  // -3: the turn restart
    }
    st16(kNextStage() + 8, point);
    st8(kNextStage() + 10, (uint8_t)s.room);
    st8(kNextStage() + 11, (uint8_t)s.layer);
    st8(kNextStage() + 13, 0);  // wipe: fade
    st8(kNextStage() + 12, 1);  // enabled
    return true;
}

std::string portable_label(int slot) { return slot > 0 ? "Slot " + std::to_string(slot) : std::string("Bug report state"); }
std::string portable_path(int slot) {
    return slot > 0 ? slot_path(slot, pstate::kExtension) : state_dir() + "/bugreport." + pstate::kExtension;
}

bool read_text(const std::string& path, std::string& out, std::string& why) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { why = "empty"; return false; }
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() > pstate::kMaxFileSize) { fclose(f); why = "file is too large to be a portable state"; return false; }
    }
    fclose(f);
    return true;
}

bool read_portable(const std::string& path, pstate::State& s, std::string& why) {
    std::string text;
    if (!read_text(path, text, why)) return false;
    if (!pstate::read(text, s, why)) return false;
    if (s.title_id != title_id()) { why = "made with another game (title " + s.title_id + ")"; return false; }
    return true;
}

bool write_portable(const std::string& path, const pstate::State& s, std::string& why) {
    std::string text = pstate::write(s, why);  // refuses anything that is not small (the guard)
    if (text.empty()) return false;
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { why = "cannot write " + tmp; return false; }
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
    else remove(tmp.c_str());
    if (!ok) why = "write failed";
    return ok;
}

struct PendingPortable {
    std::shared_ptr<pstate::State> s;
    int slot = 0;
    std::string path;
    std::string label;  // file imports show their name instead of a slot label
};
std::string pload_label(const PendingPortable& p) { return p.label.empty() ? portable_label(p.slot) : p.label; }
std::atomic<int> g_psave_req{0};
PendingPortable g_pload;          // under g_mu
bool g_pload_waiting = false;     // the "waiting for gameplay" message was shown
std::string g_last_portable;      // newest portable state written or loaded (under g_mu)
// after a portable load: the old Link's process id (a new Link has a new one), the state for the HD sections
struct Arrival { std::string stage; float pos[3]; uint64_t frame = 0; uint32_t link_id = 0; int frames = 0; std::shared_ptr<pstate::State> s; } g_arrival;

void do_portable_save(Cpu* c, int slot) {
    pstate::State s;
    std::string why;
    bool retry;
    // only while the player controls Link (no portable states mid-cutscene/dialogue; full states stay allowed)
    if (gameplay_ready(why, retry) && !player_has_control(c, why)) {
        LOG("[savestate] %s: portable state refused: %s", portable_label(slot).c_str(), why.c_str());
        message("%s: can't save during a cutscene or dialogue (%s) - try again when you have control of Link",
                portable_label(slot).c_str(), why.c_str());
        return;
    }
    if (!capture_portable(c, s, why)) {
        message("%s: not saved (%s)", portable_label(slot).c_str(), why.c_str());
        return;
    }
    std::string path = portable_path(slot);
    if (!write_portable(path, s, why)) {
        message("%s: not saved (%s)", portable_label(slot).c_str(), why.c_str());
        return;
    }
    struct stat st{};
    stat(path.c_str(), &st);
    LOG("[savestate] %s: portable state written (%lld bytes; %s room %d at %.1f %.1f %.1f, angle %d, Quest Log %d%s)",
        portable_label(slot).c_str(), (long long)st.st_size, s.stage.c_str(), s.room, s.pos[0], s.pos[1], s.pos[2], s.angle_y,
        s.file_slot + 1, s.on_ship ? ", on the boat" : s.has_ship ? ", boat nearby" : "");
    if (slot > 0) {
        gfx::request_tv_dump(slot_path(slot, "png"), 0);  // the slot's picture stays with the slot
        struct stat full{};
        if (stat(slot_path(slot).c_str(), &full) == 0)
            LOG("[savestate] slot %d also holds an older full state (%.0f MB); it is kept", slot, full.st_size / 1048576.0);
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_last_portable = path;
    }
    message("%s saved (%s)", portable_label(slot).c_str(), area_label(s.stage.c_str()).c_str());
}

// a pending portable load at the frame boundary
void service_portable_load(Cpu* c) {
    PendingPortable p;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        p = g_pload;
    }
    if (!p.s) return;
    std::string why;
    bool retry = false;
    const uint32_t sv0 = ld32(kSaveInfoPtr());
    const int current_log = in_mem2(sv0) ? ld8(sv0 + kDataNum) : -1;
    if (!apply_portable(c, *p.s, why, retry)) {
        if (retry) {
            if (!g_pload_waiting) message("%s: waiting to load (%s)", pload_label(p).c_str(), why.c_str());
            g_pload_waiting = true;
            return;
        }
        message("%s: cannot load (%s)", pload_label(p).c_str(), why.c_str());
    } else {
        LOG("[savestate] %s: portable state applied: entering %s room %d layer %d at %.1f %.1f %.1f", pload_label(p).c_str(),
            p.s->stage.c_str(), p.s->room, p.s->layer, p.s->pos[0], p.s->pos[1], p.s->pos[2]);
        if (current_log >= 0 && current_log <= 2 && current_log != p.s->file_slot) {
            LOG("[savestate] %s: this state is from Quest Log %d; it is loaded into Quest Log %d", pload_label(p).c_str(),
                p.s->file_slot + 1, current_log + 1);
            message("This state is from Quest Log %d; it is loaded into Quest Log %d (saving in game will write it there).",
                    p.s->file_slot + 1, current_log + 1);
        } else {
            message("Loaded %s (%s; progress and position)", pload_label(p).c_str(), area_label(p.s->stage.c_str()).c_str());
        }
        g_arrival = Arrival{p.s->stage, {p.s->pos[0], p.s->pos[1], p.s->pos[2]}, gfx::frame_count(), ld32(ld32(kLinkPtr()) + 4), 0, p.s};
        std::lock_guard<std::mutex> lk(g_mu);
        g_last_portable = p.path;
    }
    g_pload_waiting = false;
    std::lock_guard<std::mutex> lk(g_mu);
    g_pload = PendingPortable{};
}

// after a portable load: once the new Link exists, the HD sections once more (leaving the old stage
// updates the live sea-chart position), then log where Link arrived
void watch_arrival(Cpu* c) {
    if (g_arrival.stage.empty()) return;
    uint32_t link = ld32(kLinkPtr());
    if (stage_name() != g_arrival.stage || !in_mem2(link) || ld32(link + 4) == g_arrival.link_id) {  // fpcBs mBsPcId
        if (gfx::frame_count() - g_arrival.frame > 30 * 60) {
            LOG("[savestate] portable load: did not arrive in %s", g_arrival.stage.c_str());
            g_arrival = Arrival{};
        }
        return;
    }
    if (g_arrival.frames++ == 0) {
        CallScope scope(c);
        apply_hd_sections(c, *g_arrival.s);
    }
    if (g_arrival.frames < 30) return;  // let Link settle (start animation, ground check)
    float p[3];
    for (int i = 0; i < 3; i++) p[i] = (float)ldf32(link + kActorPos + 4 * i);
    float dx = p[0] - g_arrival.pos[0], dy = p[1] - g_arrival.pos[1], dz = p[2] - g_arrival.pos[2];
    LOG("[savestate] portable load: arrived in %s room %d at %.1f %.1f %.1f (distance %.1f from the saved position)",
        stage_name().c_str(), (int8_t)ld8(link + kActorRoom), p[0], p[1], p[2], sqrtf(dx * dx + dy * dy + dz * dz));
    g_arrival = Arrival{};
}

// which file of a slot to load: the newer one
bool newer_is_portable(int slot) {
    struct stat a{}, b{};
    bool full = stat(slot_path(slot).c_str(), &a) == 0, port = stat(slot_path(slot, pstate::kExtension).c_str(), &b) == 0;
    if (full && port) {
#ifdef __APPLE__
        return b.st_mtimespec.tv_sec > a.st_mtimespec.tv_sec ||
               (b.st_mtimespec.tv_sec == a.st_mtimespec.tv_sec && b.st_mtimespec.tv_nsec >= a.st_mtimespec.tv_nsec);
#else
        return b.st_mtime >= a.st_mtime;
#endif
    }
    return port;
}

SlotInfo full_slot_info(int slot) {
    SlotInfo info;
    info.path = slot_path(slot);
    FILE* f = fopen(info.path.c_str(), "rb");
    if (!f) return info;
    Header h;
    std::string why;
    info.used = true;
    info.compatible = read_header(f, h, why);
    fclose(f);
    if (info.compatible || memcmp(h.magic, kMagic, 8) == 0) {
        time_t t = (time_t)h.created;
        struct tm tmv;
        localtime_r(&t, &tmv);
        char buf[64];
        strftime(buf, sizeof buf, "%b %d %H:%M:%S", &tmv);
        info.when = buf;
        h.area[sizeof h.area - 1] = 0;
        if (h.area[0]) info.area = area_label(h.area);
        info.controller = (int)h.controller;
    }
    return info;
}

SlotInfo portable_slot_info(int slot) {
    SlotInfo info;
    info.portable = true;
    info.path = slot_path(slot, pstate::kExtension);
    std::string text, why;
    if (!read_text(info.path, text, why)) {
        info.used = why != "empty";
        info.compatible = false;
        return info;
    }
    info.used = true;
    pstate::State s;
    info.compatible = pstate::read(text, s, why) && s.title_id == title_id();
    if (info.compatible) {
        // "2026-10-08 14:03:11" -> "Oct 08 14:03:11" like the full states
        static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        int y, mo, d;
        char rest[16];
        if (sscanf(s.created.c_str(), "%d-%d-%d %15s", &y, &mo, &d, rest) == 4 && mo >= 1 && mo <= 12) {
            char buf[48];
            snprintf(buf, sizeof buf, "%s %02d %s", mon[mo - 1], d, rest);
            info.when = buf;
        } else info.when = s.created;
        info.area = area_label(s.stage.c_str());
    }
    return info;
}

}  // namespace

uint32_t snap_ld32(uint32_t ea) {
    if (!g_check) return ld32(ea);
    uint32_t base = ea & ~(kChunk - 1);
    auto it = g_check->chunks.find(base);
    if (it == g_check->chunks.end()) {
        for (auto& g : g_check->regs)
            if (ea - g.base < g.size) return 0;
        return ld32(ea);  // not part of the snapshot (code, host-only memory)
    }
    uint32_t v;
    memcpy(&v, it->second + (ea - base), 4);
    return __builtin_bswap32(v);
}

SlotInfo slot_info(int slot) {
    SlotInfo info = newer_is_portable(slot) ? portable_slot_info(slot) : full_slot_info(slot);
    // the slot's other, older file (kept; shown so it is not forgotten)
    struct stat st{};
    if (stat(slot_path(slot, info.portable ? "bin" : pstate::kExtension).c_str(), &st) == 0) {
        info.older_other = true;
        info.older_bytes = (uint64_t)st.st_size;
    }
    return info;
}

void request_save(int slot) {
    if (slot < 1 || slot > kSlots) return;
    g_save_req = slot;
}

void request_load(int slot) {
    if (slot < 1 || slot > kSlots) return;
    if (g_loading.exchange(true)) return;
    std::thread([slot] {
        auto t0 = std::chrono::steady_clock::now();
        std::string why;
        std::shared_ptr<Snapshot> s;
        {
            std::lock_guard<std::mutex> io(g_io);
            s = read_slot(slot, why);
        }
        if (!s) {
            message("Slot %d: %s", slot, why.c_str());
            g_loading = false;
            return;
        }
        LOG("[savestate] slot %d: read and decompressed %.1f MB in %.0f ms", slot, s->payload.size() / 1048576.0,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        std::lock_guard<std::mutex> lk(g_mu);
        g_load_ready = s;
    }).detach();
}

void request_save_portable(int slot) {
    if (slot < 0 || slot > kSlots) return;
    g_psave_req = slot + 1;  // 0 is no request; the game thread consumes it in service()
}

void request_load_portable_file(const std::string& path) {
    // small enough (<= 64 KB) to read right here; the game thread applies it in service()
    pstate::State s;
    std::string why;
    {
        std::lock_guard<std::mutex> io(g_io);
        if (!read_portable(path, s, why)) {
            std::string name = path.substr(path.find_last_of("/\\") + 1);
            message("%s: cannot load (%s)", name.c_str(), why.c_str());
            return;
        }
    }
    PendingPortable p{std::make_shared<pstate::State>(std::move(s)), 0, path};
    p.label = path.substr(path.find_last_of("/\\") + 1);
    std::lock_guard<std::mutex> lk(g_mu);
    g_pload = std::move(p);
    g_pload_waiting = false;
}

std::string bug_report_paths() {
    // the newest portable state in the states folder (any run, not just this one)
    const std::string ext = std::string(".") + pstate::kExtension;
    std::string best;
    time_t best_t = 0;
    if (DIR* d = opendir(state_dir().c_str())) {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name.size() <= ext.size() || name.compare(name.size() - ext.size(), ext.size(), ext) != 0) continue;
            std::string full = state_dir() + "/" + name;
            struct stat st{};
            if (stat(full.c_str(), &st) == 0 && st.st_mtime >= best_t) {
                best_t = st.st_mtime;
                best = full;
            }
        }
        closedir(d);
    }
    std::string sav;
    for (const char* sub : {"user", "common"}) {
        std::string p = config::save_dir + "/" + sub + "/cking.sav";
        struct stat st{};
        if (stat(p.c_str(), &st) == 0) {
            sav = p;
            break;
        }
    }
    return best + "|" + sav;
}

std::string last_message() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_message.empty() || std::chrono::steady_clock::now() - g_message_time > std::chrono::seconds(4)) return "";
    return g_message;
}

void service(Cpu* c) {
    track_events();  // the event delay for player_has_control
    // personal builds (wwhd.env): WWHD_PORTABLE_SAVE=slot (0 = bugreport.wwstate) once gameplay is
    // ready, WWHD_PORTABLE_LOAD=path (waits for gameplay on its own)
    static bool portable_save_env_done = false, portable_load_env_done = false;
    if (!portable_load_env_done) {
        portable_load_env_done = true;
        if (const char* e = getenv("WWHD_PORTABLE_LOAD")) request_load_portable_file(e);
    }
    if (!portable_save_env_done) {
        if (const char* e = getenv("WWHD_PORTABLE_SAVE")) {
            std::string why;
            bool retry;
            if (gameplay_ready(why, retry)) {
                request_save_portable(atoi(e));
                portable_save_env_done = true;
            }
        } else portable_save_env_done = true;
    }
    static const std::vector<Timed> save_at = parse_timed("WWHD_STATE_SAVE_AT"), load_at = parse_timed("WWHD_STATE_LOAD_AT");
    if (!save_at.empty() || !load_at.empty()) {
        static uint64_t last = 0;
        uint64_t f = gfx::frame_count();
        for (auto& t : save_at)
            if (t.frame > last && t.frame <= f) request_save(t.slot);
        for (auto& t : load_at)
            if (t.frame > last && t.frame <= f) request_load(t.slot);
        last = f;
    }
    // test aid: WWHD_STATE_DUMP=n dumps the n TV frames after each save/load (state_<save|load><slot>_<k>.png)
    static const int dump = getenv("WWHD_STATE_DUMP") ? atoi(getenv("WWHD_STATE_DUMP")) : 0;
    if (int slot = g_save_req.load()) {
        if (do_save(slot)) {
            g_save_req = 0;
            g_attempts = 0;
            for (int k = 1; k <= dump; k++)
                gfx::request_tv_dump("state_save" + std::to_string(slot) + "_" + std::to_string(k) + ".png", k);
        }
        return;
    }
    std::shared_ptr<Snapshot> s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_load_ready;
    }
    if (s && do_load(s)) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_load_ready.reset();
        }
        g_loading = false;
        g_attempts = 0;
        for (int k = 1; k <= dump; k++)
            gfx::request_tv_dump("state_load" + std::to_string(s->slot) + "_" + std::to_string(k) + ".png", k);
    }
    // portable states (the bug-report flow, WWHD_PORTABLE_SAVE/LOAD): same frame boundary
    if (int q = g_psave_req.exchange(0)) do_portable_save(c, q - 1);
    service_portable_load(c);
    watch_arrival(c);
}

}  // namespace ss
