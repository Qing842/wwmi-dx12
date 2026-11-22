// ib_hash.cpp -- see ib_hash.h. GPU crc32c of the char VB prefix (in-place SRV read, 1024 slice CRCs read
// back and combined here) + CPU desc chain = 3dmigoto's ini `hash=`. Bounded: one dispatch per distinct
// (res,offset,byteLen), cached.
#include "ib_hash.h"
#include "wwmi_runtime.h" // WwmiReadFile / WwmiMakeBuffer
#include "../migoto/port_api.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/addon.h"
#include "mods_toggles.h"
#include "../instrumentation/gpu_timer.h"
#include "../hooks/resource_tracker.h" // ResourceTracker::StateOf: the state the game's barriers left a buffer in
#include "../migoto/dx12_bridge.h" // kResStateUnknown
#include "../core/log.h"
#include "../core/d3d12_barrier.h"
#include <algorithm>
#include <map>
#include <wrl.h>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <atomic>

ModsToggles g_mods_toggles;

using Microsoft::WRL::ComPtr;

namespace {
std::shared_mutex g_mtx; // shared for the per-draw cache read, exclusive for every change
bool g_ready = false;
bool g_failed = false;
std::atomic<bool> g_ready_fast{ false }; // g_ready, readable without g_mtx
ComPtr<ID3D12RootSignature> g_rs;
ComPtr<ID3D12PipelineState> g_pso;
ComPtr<ID3D12Resource> g_out; // DEFAULT UAV
ComPtr<ID3D12Resource> g_rb; // READBACK
ID3D12Fence* g_fence = nullptr; // the present pass's fence and the value that completes the recorded list
UINT64 g_fv = 0;

struct Job {
    ID3D12Resource* ib = nullptr;
    ID3D12Resource* vb = nullptr;
    ID3D12Resource* args = nullptr;
    uint32_t ib_off = 0;
    uint32_t ib_bytes = 0;
    uint32_t idx_size = 0;
    uint32_t vb_off = 0;
    uint32_t vb_stride = 0;
    uint64_t ib_base_va = 0;
    uint64_t ib_va = 0;
    uint64_t ib_end_va = 0;
    uint64_t vb_va = 0;
    uint64_t args_off = 0;
    uint64_t args_va = 0;
    bool unskinned = false;
};
bool JobHitsRange(const Job& j, uint64_t va, uint64_t end)
{
    const bool vb_hit = j.vb_va >= va && j.vb_va < end;
    const bool ib_hit = j.ib_va < end && va < j.ib_end_va;
    const bool args_hit = j.args != nullptr && j.args_va < end && va < j.args_va + 20ull;
    return vb_hit || ib_hit || args_hit;
}
std::vector<Job> g_pending;
std::vector<Job> g_small_pending; // < 256 indices & indirect VFX jobs: ahead of g_medium_pending
std::vector<Job> g_medium_pending; // >= 256 indices unskinned jobs
std::unordered_map<uint64_t, uint32_t> g_small_queued; // vb_va -> queued ib_reach (ib_off + ib_bytes)
struct Cached {
    uint32_t hash = 0;
    uint32_t ib_bytes = 0; // the index extent the hash was taken over (Lookup grow)
    uint32_t ib_reach = 0; // highest (ib_off + ib_bytes) probed for this vb_va
    uint32_t vb_bytes = 0;
    uint64_t ib_base_va = 0;
    bool matched = false;
};
std::map<uint64_t, Cached> g_cache;
std::atomic<uint32_t> g_cache_gen{ 1 };
struct LookupMemo {
    uint64_t vb_va = 0;
    uint64_t ib_base_va = 0;
    uint32_t hash = 0;
    uint32_t ib_bytes = 0;
    uint32_t ib_reach = 0;
    uint32_t gen = 0;
    bool matched = false;
};
thread_local LookupMemo t_memo[1024] = {};

void StoreLocked(uint64_t vb_va, const Cached& c)
{
    if (!g_cache.insert_or_assign(vb_va, c).second) g_cache_gen.fetch_add(1, std::memory_order_acq_rel);
}
bool ValidAlignment(uint64_t ib_va, uint64_t vb_va, uint32_t idx_size)
{
    return ib_va && vb_va && (vb_va & 3u) == 0 && (ib_va & 1u) == 0 && !(idx_size == 4u && (ib_va & 3u) != 0);
}
bool IsUploadOrReadbackHeap(ID3D12Resource* res)
{
    D3D12_HEAP_PROPERTIES hp = {};
    D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
    if (!res || FAILED(res->GetHeapProperties(&hp, &hf))) return true;
    return hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == D3D12_HEAP_TYPE_READBACK;
}
static constexpr uint32_t kMaxBatch = 8;
struct InflightSlot {
    Job job = {};
    uint64_t ext = 0;
    bool stale = false;
};
// up to kMaxBatch hashes in flight per present (NON-BLOCKING: dispatch, read the result a later present -- never stall).
bool g_inflight = false;
uint32_t g_inflight_count = 0;
UINT64 g_inflight_fv = 0;
InflightSlot g_inflight_slots[kMaxBatch] = {};
bool IsVbInflightLocked(uint64_t vb_va)
{
    if (!g_inflight) return false;
    for (uint32_t i = 0; i < g_inflight_count; ++i)
        if (g_inflight_slots[i].job.vb_va == vb_va) return true;
    return false;
}
template <class Pred> bool AnyJobMatchesLocked(Pred&& pred)
{
    if (g_inflight)
        for (uint32_t i = 0; i < g_inflight_count; ++i)
            if (pred(g_inflight_slots[i].job)) return true;
    for (const Job& j : g_pending)
        if (pred(j)) return true;
    for (const Job& j : g_small_pending)
        if (pred(j)) return true;
    for (const Job& j : g_medium_pending)
        if (pred(j)) return true;
    return false;
}
template <class Pred> void PurgeQueueLocked(std::vector<Job>& q, bool erase_queued, Pred&& pred, std::vector<Job>& dropped)
{
    for (size_t i = 0; i < q.size();) {
        if (pred(q[i])) {
            dropped.push_back(q[i]);
            if (erase_queued) g_small_queued.erase(q[i].vb_va);
            q.erase(q.begin() + i);
        } else {
            ++i;
        }
    }
}
template <class Pred> void PurgeJobsLocked(Pred&& pred, std::vector<Job>& dropped)
{
    if (g_inflight)
        for (uint32_t i = 0; i < g_inflight_count; ++i)
            if (pred(g_inflight_slots[i].job)) g_inflight_slots[i].stale = true;
    PurgeQueueLocked(g_pending, false, pred, dropped);
    PurgeQueueLocked(g_small_pending, true, pred, dropped);
    PurgeQueueLocked(g_medium_pending, true, pred, dropped);
}
void ReleaseJob(const Job& j)
{
    if (j.ib) j.ib->Release();
    if (j.vb) j.vb->Release();
    if (j.args) j.args->Release();
}
void ReleaseJobs(const std::vector<Job>& dropped)
{
    for (const Job& j : dropped) ReleaseJob(j);
}
bool g_recorded = false; // Record wrote a dispatch this frame; Submitted names its fence
LARGE_INTEGER g_inflight_t0 = {};
std::atomic<uint32_t> g_ibhash_slow_logged{ 0 };
std::atomic<uint32_t> g_ibhash_skip_logged{ 0 };
LARGE_INTEGER g_rate_t0 = {};
uint32_t g_rate_jobs = 0;
uint32_t g_rate_slow = 0;
uint64_t g_rate_bytes = 0;
double g_rate_max_ms = 0.0;

double QpcMs(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
{
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart;
}

// THE SHADER'S OUTPUT: a header and one (crc, bytes) per thread. See IbVbHash.hlsl.
static constexpr uint32_t kThreads = 1024;
static constexpr uint32_t kOutBytes = 8 + kThreads * 8;
static constexpr uint32_t kSlotStride = (kOutBytes + 15u) & ~15u;
static constexpr uint32_t kBufBytes = (kMaxBatch * kSlotStride + 255u) & ~255u;
static constexpr uint32_t kSkipIndirectEmpty = 0xFFFFFFFEu;
static constexpr uint32_t kSkipIndirectRange = 0xFFFFFFFDu;

uint32_t gf2_times(const uint32_t* mat, uint32_t vec)
{
    uint32_t sum = 0;
    for (; vec; vec >>= 1, ++mat)
        if (vec & 1u) sum ^= *mat;
    return sum;
}

void gf2_square(uint32_t* square, const uint32_t* mat)
{
    for (int n = 0; n < 32; ++n) square[n] = gf2_times(mat, mat[n]);
}

struct CombineOp {
    uint64_t len = 0;
    uint32_t m[32] = {};

    uint32_t operator()(uint32_t crc1, uint32_t crc2, uint64_t len2)
    {
        if (len2 == 0) return crc1;
        if (len2 != len) {
            len = len2;
            build(len2);
        }
        return gf2_times(m, crc1) ^ crc2;
    }

    void build(uint64_t bytes)
    {
        uint32_t base[32];
        uint32_t sq[32];
        uint32_t acc[32];
        base[0] = 0x82F63B78u; // one zero bit: the reflected polynomial
        for (int n = 1; n < 32; ++n) base[n] = 1u << (n - 1);
        for (int k = 0; k < 3; ++k) { // 8 bits
            gf2_square(sq, base);
            for (int n = 0; n < 32; ++n) base[n] = sq[n];
        }
        for (int n = 0; n < 32; ++n) m[n] = 1u << n; // identity
        for (;;) {
            if (bytes & 1u) {
                for (int n = 0; n < 32; ++n) acc[n] = gf2_times(base, m[n]);
                for (int n = 0; n < 32; ++n) m[n] = acc[n];
            }
            bytes >>= 1;
            if (!bytes) break;
            gf2_square(sq, base);
            for (int n = 0; n < 32; ++n) base[n] = sq[n];
        }
    }
};

uint32_t chain_desc(uint32_t data_crc, uint32_t byteWidth)
{
    uint32_t desc[6] = { byteWidth, 0u, 0x9u, 0u, 0u, 0u };
    return PortApi::Crc32c(data_crc, desc, sizeof(desc));
}

bool DeclaredOverrideHash(uint32_t h)
{
    return h != 0u && !PortApi::TextureHashHasSections(0u) && PortApi::TextureHashHasSections(h);
}

uint32_t ResolveVbHash(uint32_t data_crc, uint32_t byteWidth, bool* out_matched)
{
    const uint32_t primary = chain_desc(data_crc, byteWidth);
    if (DeclaredOverrideHash(primary)) {
        if (out_matched) *out_matched = true;
        return primary;
    }
    if (!PortApi::TextureHashHasSections(0u)) {
        static constexpr uint32_t kAltDescs[][5] = {
            { 0u, 0x1u, 0u, 0u, 0u },       // DEFAULT, VERTEX_BUFFER
            { 0u, 0x9u, 0u, 0x20u, 0u },    // DEFAULT, VERTEX | SRV, ALLOW_RAW_VIEWS
            { 0u, 0x89u, 0u, 0u, 0u },      // DEFAULT, VERTEX | SRV | UAV
            { 0u, 0x89u, 0u, 0x20u, 0u },   // DEFAULT, VERTEX | SRV | UAV, ALLOW_RAW_VIEWS
            { 1u, 0x1u, 0u, 0u, 0u },       // IMMUTABLE, VERTEX_BUFFER
            { 1u, 0x9u, 0u, 0u, 0u },       // IMMUTABLE, VERTEX | SRV
            { 2u, 0x1u, 0x10000u, 0u, 0u }, // DYNAMIC, VERTEX_BUFFER, CPU_ACCESS_WRITE
            { 2u, 0x9u, 0x10000u, 0u, 0u }, // DYNAMIC, VERTEX | SRV, CPU_ACCESS_WRITE
        };
        for (const auto& a : kAltDescs) {
            const uint32_t desc[6] = { byteWidth, a[0], a[1], a[2], a[3], a[4] };
            const uint32_t alt = PortApi::Crc32c(data_crc, desc, sizeof(desc));
            if (PortApi::TextureHashHasSections(alt)) {
                if (out_matched) *out_matched = true;
                return alt;
            }
        }
    }
    if (out_matched) *out_matched = false;
    return primary;
}

bool TryWidenQueuedJobLocked(
    std::vector<Job>& q, uint64_t vb_va, uint64_t ib_base_va, uint32_t ib_off, uint32_t ib_bytes, uint32_t ib_reach)
{
    for (Job& j : q) {
        if (j.vb_va != vb_va || j.args != nullptr || j.ib_base_va != ib_base_va) continue;
        if (ib_off >= j.ib_off && ib_reach - j.ib_off <= 131072u) {
            j.ib_bytes = ib_reach - j.ib_off;
            j.ib_end_va = j.ib_va + j.ib_bytes;
        } else {
            j.ib_off = ib_off;
            j.ib_bytes = ib_bytes;
            j.ib_va = ib_base_va + ib_off;
            j.ib_end_va = j.ib_va + ib_bytes;
        }
        return true;
    }
    return false;
}
}

namespace IbHash {

bool Init(ID3D12Device* dev)
{
    if (g_ready_fast.load(std::memory_order_acquire)) return true;
    std::unique_lock l(g_mtx);
    if (g_ready) return true;
    if (g_failed || !dev) return false;
    D3D12_ROOT_PARAMETER rp[5] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 9;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].Descriptor.ShaderRegister = 0; // t0 = IB
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[2].Descriptor.ShaderRegister = 1; // t1 = VB
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[3].Descriptor.ShaderRegister = 2; // t2 = Args (indirect)
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[4].Descriptor.ShaderRegister = 0; // u0 = out (crc,vbBytes)
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 5;
    rsd.pParameters = rp;
    ComPtr<ID3DBlob> bl;
    ComPtr<ID3DBlob> er;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &bl, &er))) {
        logger::write("IbHash: rootsig fail");
        g_failed = true;
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, bl->GetBufferPointer(), bl->GetBufferSize(), IID_PPV_ARGS(&g_rs)))) {
        g_failed = true;
        return false;
    }
    auto cso = WwmiReadFile((g_addon.cso_dir + L"\\IbVbHash.cso").c_str());
    if (cso.empty()) {
        logger::write("IbHash: IbVbHash.cso missing");
        g_failed = true;
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_rs.Get();
    pd.CS.pShaderBytecode = cso.data();
    pd.CS.BytecodeLength = cso.size();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)))) {
        logger::write("IbHash: PSO fail");
        g_failed = true;
        return false;
    }
    g_out = WwmiMakeBuffer(dev, kBufBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_rb = WwmiMakeBuffer(
        dev, kBufBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    static_assert(kMaxBatch * kSlotStride <= kBufBytes, "IbHash: the batch slice table must fit the readback");
    if (!g_out || !g_rb) {
        g_failed = true;
        return false;
    }
    g_ready = true;
    g_ready_fast.store(true, std::memory_order_release);
    logger::write("IbHash: initialized (GPU crc32c, up to %u dispatches/present)", kMaxBatch);
    return true;
}

uint32_t Lookup(ID3D12Resource* ib_res, uint32_t ib_off, uint32_t ib_bytes, uint32_t idx_size, ID3D12Resource* vb_res,
    uint32_t vb_off, uint32_t vb_stride, bool grow)
{
    if (!ib_res || !vb_res || !ib_bytes || (idx_size != 2u && idx_size != 4u) || !vb_stride) return 0;
    const uint64_t ib_base_va = ib_res->GetGPUVirtualAddress();
    const uint64_t ib_va = ib_base_va + ib_off;
    uint64_t vb_va = vb_res->GetGPUVirtualAddress() + vb_off; // UNIQUE per char (pooled VBs)
    if (!ValidAlignment(ib_va, vb_va, idx_size)) return 0;
    if (!Init(g_addon.d3d12_device)) return 0;
    LookupMemo& m = t_memo[((vb_va >> 8) ^ (vb_va >> 20)) & 1023];
    const uint32_t gen = g_cache_gen.load(std::memory_order_acquire);
    if (m.vb_va == vb_va && m.gen == gen && !(grow && m.hash && ib_bytes > m.ib_bytes)) return m.hash;
    uint32_t meanwhile = 0;
    {
        std::shared_lock l(g_mtx); // the hot path: a cached hash, read in parallel
        auto it = g_cache.find(vb_va);
        const bool further = grow && it != g_cache.end() && it->second.hash && ib_bytes > it->second.ib_bytes;
        if (it != g_cache.end() && !further) {
            m = LookupMemo{ vb_va, it->second.ib_base_va, it->second.hash, it->second.ib_bytes, it->second.ib_reach,
                gen, it->second.matched };
            return it->second.hash;
        }
        meanwhile = further ? it->second.hash : 0;
        if (IsVbInflightLocked(vb_va)) return meanwhile; // being computed
    }
    std::unique_lock l(g_mtx);
    if (IsVbInflightLocked(vb_va)) return meanwhile;
    for (auto& j : g_pending) {
        if (j.vb_va == vb_va) { // dedup pending; a queued job may still widen
            if (grow && ib_bytes > j.ib_bytes) {
                j.ib_bytes = ib_bytes;
                j.ib_end_va = j.ib_va + ib_bytes;
            }
            return meanwhile;
        }
    }
    if (g_pending.size() < 32) {
        ib_res->AddRef();
        vb_res->AddRef();
        Job j{};
        j.ib = ib_res;
        j.vb = vb_res;
        j.ib_off = ib_off;
        j.ib_bytes = ib_bytes;
        j.idx_size = idx_size;
        j.vb_off = vb_off;
        j.vb_stride = vb_stride;
        j.ib_base_va = ib_base_va;
        j.ib_va = ib_va;
        j.ib_end_va = ib_va + ib_bytes;
        j.vb_va = vb_va;
        j.unskinned = false;
        g_pending.push_back(j);
    }
    return meanwhile;
}

uint32_t LookupSmall(ID3D12Resource* ib_res, uint32_t ib_off, uint32_t ib_bytes, uint32_t idx_size,
    ID3D12Resource* vb_res, uint32_t vb_off, uint32_t vb_stride, bool medium)
{
    if (!ib_res || !vb_res || !ib_bytes || (idx_size != 2u && idx_size != 4u) || !vb_stride) return 0;
    const uint64_t ib_base_va = ib_res->GetGPUVirtualAddress();
    const uint64_t ib_va = ib_base_va + ib_off;
    const uint64_t vb_va = vb_res->GetGPUVirtualAddress() + vb_off;
    if (!ValidAlignment(ib_va, vb_va, idx_size)) return 0;
    if (!Init(g_addon.d3d12_device)) return 0;
    const uint32_t ib_reach = ib_off + ib_bytes;
    LookupMemo& m = t_memo[((vb_va >> 8) ^ (vb_va >> 20)) & 1023];
    const uint32_t gen = g_cache_gen.load(std::memory_order_acquire);
    const bool memo_widen = !m.matched && m.hash != 0 && m.ib_base_va == ib_base_va && ib_reach > m.ib_reach;
    if (m.vb_va == vb_va && m.gen == gen && !memo_widen) return m.hash;
    uint32_t meanwhile = 0;
    {
        std::shared_lock l(g_mtx);
        auto it = g_cache.find(vb_va);
        const bool further = it != g_cache.end() && !it->second.matched && it->second.hash != 0 &&
            it->second.ib_base_va == ib_base_va && ib_reach > it->second.ib_reach;
        if (it != g_cache.end() && !further) {
            m = LookupMemo{ vb_va, it->second.ib_base_va, it->second.hash, it->second.ib_bytes, it->second.ib_reach,
                gen, it->second.matched };
            return it->second.hash;
        }
        meanwhile = further ? it->second.hash : 0;
        auto qit = g_small_queued.find(vb_va);
        if (IsVbInflightLocked(vb_va) || (qit != g_small_queued.end() && ib_reach <= qit->second)) return meanwhile;
    }
    const bool cpu_written = IsUploadOrReadbackHeap(vb_res);
    std::unique_lock l(g_mtx);
    auto it = g_cache.find(vb_va);
    const bool further = it != g_cache.end() && !it->second.matched && it->second.hash != 0 &&
        it->second.ib_base_va == ib_base_va && ib_reach > it->second.ib_reach;
    if (it != g_cache.end() && !further) return it->second.hash;
    meanwhile = further ? it->second.hash : 0;
    if (IsVbInflightLocked(vb_va)) return meanwhile;
    if (auto qit = g_small_queued.find(vb_va); qit != g_small_queued.end()) {
        if (ib_reach > qit->second) {
            if (TryWidenQueuedJobLocked(g_small_pending, vb_va, ib_base_va, ib_off, ib_bytes, ib_reach) ||
                TryWidenQueuedJobLocked(g_medium_pending, vb_va, ib_base_va, ib_off, ib_bytes, ib_reach)) {
                qit->second = ib_reach;
            }
        }
        return meanwhile;
    }
    if (cpu_written) {
        StoreLocked(vb_va, Cached{ 0, ib_bytes, ib_reach, 0, ib_base_va, false });
        return 0;
    }
    for (const Job& j : g_pending)
        if (j.vb_va == vb_va) return meanwhile;
    std::vector<Job>& dst = medium ? g_medium_pending : g_small_pending;
    if (dst.size() >= 256) return meanwhile;
    ib_res->AddRef();
    vb_res->AddRef();
    Job j{};
    j.ib = ib_res;
    j.vb = vb_res;
    j.ib_off = ib_off;
    j.ib_bytes = ib_bytes;
    j.idx_size = idx_size;
    j.vb_off = vb_off;
    j.vb_stride = vb_stride;
    j.ib_base_va = ib_base_va;
    j.ib_va = ib_va;
    j.ib_end_va = ib_va + ib_bytes;
    j.vb_va = vb_va;
    j.unskinned = true;
    dst.push_back(j);
    g_small_queued[vb_va] = ib_reach;
    return meanwhile;
}

uint32_t LookupIndirect(ID3D12Resource* ib_res, uint32_t ib_off, uint32_t idx_size, ID3D12Resource* vb_res,
    uint32_t vb_off, uint32_t vb_stride, ID3D12Resource* args_res, uint64_t args_off)
{
    if (!ib_res || !vb_res || !args_res || (idx_size != 2u && idx_size != 4u) || !vb_stride || (args_off & 3u) != 0)
        return 0;
    const uint64_t ib_base_va = ib_res->GetGPUVirtualAddress();
    const uint64_t ib_va = ib_base_va + ib_off;
    const uint64_t vb_va = vb_res->GetGPUVirtualAddress() + vb_off;
    const uint64_t args_va = args_res->GetGPUVirtualAddress() + args_off;
    if (!ValidAlignment(ib_va, vb_va, idx_size) || !args_va || (args_va & 3u) != 0) return 0;
    if (!Init(g_addon.d3d12_device)) return 0;
    LookupMemo& m = t_memo[((vb_va >> 8) ^ (vb_va >> 20)) & 1023];
    const uint32_t gen = g_cache_gen.load(std::memory_order_acquire);
    if (m.vb_va == vb_va && m.gen == gen) return m.hash;
    {
        std::shared_lock l(g_mtx);
        auto it = g_cache.find(vb_va);
        if (it != g_cache.end()) {
            m = LookupMemo{ vb_va, it->second.ib_base_va, it->second.hash, it->second.ib_bytes, it->second.ib_reach,
                gen, it->second.matched };
            return it->second.hash;
        }
        if (IsVbInflightLocked(vb_va) || g_small_queued.count(vb_va)) return 0;
    }
    const bool cpu_written = IsUploadOrReadbackHeap(vb_res);
    const uint64_t ibW = ib_res->GetDesc().Width;
    std::unique_lock l(g_mtx);
    if (auto it = g_cache.find(vb_va); it != g_cache.end()) return it->second.hash;
    if (IsVbInflightLocked(vb_va) || g_small_queued.count(vb_va)) return 0;
    if (cpu_written) {
        StoreLocked(vb_va, Cached{ 0, 0, ~0u, 0, ib_base_va, false });
        return 0;
    }
    for (const Job& j : g_pending)
        if (j.vb_va == vb_va) return 0;
    if (g_small_pending.size() >= 256) return 0;
    ib_res->AddRef();
    vb_res->AddRef();
    args_res->AddRef();
    Job j{};
    j.ib = ib_res;
    j.vb = vb_res;
    j.args = args_res;
    j.ib_off = ib_off;
    j.ib_bytes = 0;
    j.idx_size = idx_size;
    j.vb_off = vb_off;
    j.vb_stride = vb_stride;
    j.ib_base_va = ib_base_va;
    j.ib_va = ib_va;
    j.ib_end_va = ib_base_va + ibW;
    j.vb_va = vb_va;
    j.args_off = args_off;
    j.args_va = args_va;
    j.unskinned = true;
    g_small_pending.push_back(j);
    g_small_queued[vb_va] = ~0u;
    return 0;
}

void ForgetRange(uint64_t va, uint64_t bytes)
{
    if (!va || !bytes) return;
    const uint64_t end = va + bytes;
    const auto hits = [va, end](const Job& j) { return JobHitsRange(j, va, end); };
    {
        std::shared_lock l(g_mtx);
        const bool hit = g_cache.lower_bound(va) != g_cache.lower_bound(end) || AnyJobMatchesLocked(hits);
        if (!hit) return;
    }
    std::vector<Job> dropped;
    {
        std::unique_lock l(g_mtx);
        g_cache.erase(g_cache.lower_bound(va), g_cache.lower_bound(end));
        g_cache_gen.fetch_add(1, std::memory_order_acq_rel);
        PurgeJobsLocked(hits, dropped);
    }
    ReleaseJobs(dropped);
}

void ForgetResource(ID3D12Resource* res)
{
    if (!res) return;
    const auto hits = [res](const Job& j) { return j.ib == res || j.vb == res || j.args == res; };
    {
        std::shared_lock l(g_mtx);
        if (!AnyJobMatchesLocked(hits)) return;
    }
    std::vector<Job> dropped;
    {
        std::unique_lock l(g_mtx);
        PurgeJobsLocked(hits, dropped);
    }
    ReleaseJobs(dropped);
}

void DrainQueue()
{
    if (!g_inflight) return;
    if (g_recorded || !g_fence || g_fence->GetCompletedValue() < g_inflight_fv)
        return; // still computing -- check next present

    struct SlotOut {
        uint32_t vb_bytes = 0;
        uint32_t code = 0;
        uint32_t full = 0;
        uint32_t data_crc = 0;
        bool matched = false;
    };
    SlotOut outs[kMaxBatch] = {};
    const uint32_t count = g_inflight_count;
    void* p = nullptr;
    D3D12_RANGE rr = { 0, count * kSlotStride };
    if (count > 0 && SUCCEEDED(g_rb->Map(0, &rr, &p)) && p) {
        const auto* base = reinterpret_cast<const uint8_t*>(p);
        CombineOp combine;
        for (uint32_t s = 0; s < count; ++s) {
            const auto* o = reinterpret_cast<const uint32_t*>(base + s * kSlotStride);
            outs[s].vb_bytes = o[0];
            outs[s].code = o[1];
            uint32_t crc = o[2];
            if (outs[s].vb_bytes) {
                for (uint32_t k = 1; k < kThreads; ++k) crc = combine(crc, o[2 + k * 2], o[3 + k * 2]);
            }
            outs[s].data_crc = crc;
        }
        D3D12_RANGE w = { 0, 0 };
        g_rb->Unmap(0, &w);
    }
    LARGE_INTEGER t1 = {};
    if (g_debug) QueryPerformanceCounter(&t1);
    const double ms = g_debug ? QpcMs(g_inflight_t0, t1) : 0.0;

    for (uint32_t s = 0; s < count; ++s) {
        const InflightSlot& sl = g_inflight_slots[s];
        const Job& ij = sl.job;
        const uint32_t vbBytes = outs[s].vb_bytes;
        const uint32_t code = outs[s].code;
        if (vbBytes) outs[s].full = ResolveVbHash(outs[s].data_crc, vbBytes, &outs[s].matched);
        if (!vbBytes && code != kSkipIndirectEmpty && code != kSkipIndirectRange)
            logger::write(
                "IBHASH-ZERO: a collected job holds vbBytes=0 -- its dispatch never ran | ib_off=%u ibBytes=%u idx=%u stride=%u vbExtent=%llu vb_va=%llx",
                ij.ib_off, ij.ib_bytes, ij.idx_size, ij.vb_stride, (unsigned long long)sl.ext,
                (unsigned long long)ij.vb_va);
        if (g_debug && vbBytes) {
            const bool slow = ms >= 100.0 || vbBytes >= (1u << 20);
            ++g_rate_jobs;
            g_rate_bytes += vbBytes;
            if (ms > g_rate_max_ms) g_rate_max_ms = ms;
            if (slow) ++g_rate_slow;
            if (slow && g_ibhash_slow_logged.fetch_add(1, std::memory_order_relaxed) < 2000)
                logger::write(
                    "IBHASH-SLOW %.1f ms dispatch-to-seen | vbBytes=%u (%u vertices x stride %u) vbExtent=%llu B"
                    " | ib_off=%u ibBytes=%u idx=%u (%u indices) vb_va=%llx",
                    ms, vbBytes, ij.vb_stride ? vbBytes / ij.vb_stride : 0u, ij.vb_stride, (unsigned long long)sl.ext,
                    ij.ib_off, ij.ib_bytes, ij.idx_size, ij.idx_size ? ij.ib_bytes / ij.idx_size : 0u,
                    (unsigned long long)ij.vb_va);
        }
        if (vbBytes)
            logger::trace("IBHASH vb_va=%llx ibBytes=%u idx=%u stride=%u vbBytes=%u dataCrc=%08x FULL(vbHash)=%08x",
                (unsigned long long)ij.vb_va, ij.ib_bytes, ij.idx_size, ij.vb_stride, vbBytes, outs[s].data_crc,
                outs[s].full);
    }
    if (g_debug && count > 0) {
        if (!g_rate_t0.QuadPart) g_rate_t0 = t1;
        if (QpcMs(g_rate_t0, t1) >= 10000.0) {
            logger::write(
                "IBHASH-RATE last %.0f s: %u hashes, %llu VB bytes, slowest %.1f ms, %u slow (>=100 ms or >=1 MB)",
                QpcMs(g_rate_t0, t1) / 1000.0, g_rate_jobs, (unsigned long long)g_rate_bytes, g_rate_max_ms,
                g_rate_slow);
            g_rate_t0 = t1;
            g_rate_jobs = 0;
            g_rate_slow = 0;
            g_rate_bytes = 0;
            g_rate_max_ms = 0.0;
        }
    }

    Job done[kMaxBatch] = {};
    {
        std::unique_lock l(g_mtx);
        for (uint32_t s = 0; s < count; ++s) {
            const InflightSlot& sl = g_inflight_slots[s];
            const Job& ij = sl.job;
            done[s] = ij;
            if (!sl.stale) {
                const uint32_t vbBytes = outs[s].vb_bytes;
                const uint32_t code = outs[s].code;
                const uint32_t reach = ij.args ? ~0u : (ij.ib_off + ij.ib_bytes);
                if (vbBytes) {
                    if (ij.unskinned) {
                        auto prev = g_cache.find(ij.vb_va);
                        if (prev != g_cache.end() && prev->second.ib_base_va == ij.ib_base_va && prev->second.hash != 0) {
                            const uint32_t merged_reach = (std::max)(prev->second.ib_reach, reach);
                            const uint32_t merged_ib = (std::max)(prev->second.ib_bytes, ij.ib_bytes);
                            if (outs[s].matched || (!prev->second.matched && vbBytes >= prev->second.vb_bytes))
                                StoreLocked(ij.vb_va,
                                    Cached{ outs[s].full, merged_ib, merged_reach, vbBytes, ij.ib_base_va,
                                        outs[s].matched });
                            else
                                StoreLocked(ij.vb_va,
                                    Cached{ prev->second.hash, merged_ib, merged_reach, prev->second.vb_bytes,
                                        ij.ib_base_va, prev->second.matched });
                            continue;
                        }
                    }
                    StoreLocked(
                        ij.vb_va, Cached{ outs[s].full, ij.ib_bytes, reach, vbBytes, ij.ib_base_va, outs[s].matched });
                } else if (code == kSkipIndirectEmpty || code == kSkipIndirectRange) {
                    StoreLocked(ij.vb_va, Cached{ 0, 0, ~0u, 0, ij.ib_base_va, false });
                }
            }
            g_inflight_slots[s] = {};
        }
        for (uint32_t s = 0; s < count; ++s) g_inflight_slots[s] = {};
        g_inflight_count = 0;
        g_inflight = false;
    }
    for (uint32_t s = 0; s < count; ++s) ReleaseJob(done[s]);
}

static D3D12_RESOURCE_STATES SrvStateOf(ID3D12Resource* res)
{
    const uint32_t s = g_resources.StateOf(res);
    if (s == MigotoPort::kResStateUnknown || s == (uint32_t)D3D12_RESOURCE_STATE_COMMON || s == kTrackedCommon)
        return D3D12_RESOURCE_STATE_COMMON;
    return (D3D12_RESOURCE_STATES)s;
}

static bool NeedsSrvTransition(ID3D12Resource* res, D3D12_RESOURCE_STATES* out_st)
{
    *out_st = D3D12_RESOURCE_STATE_COMMON;
    if (!res || IsUploadOrReadbackHeap(res)) return false;
    const D3D12_RESOURCE_STATES st = SrvStateOf(res);
    *out_st = st;
    return st != D3D12_RESOURCE_STATE_COMMON && !(st & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool Record(ID3D12GraphicsCommandList* cl)
{
    if (!cl || g_inflight) return false;
    Job popped[kMaxBatch] = {};
    uint32_t popped_count = 0;
    {
        std::unique_lock l(g_mtx);
        if (!g_ready) return false;
        while (popped_count < kMaxBatch) {
            if (!g_pending.empty()) {
                popped[popped_count++] = g_pending.front();
                g_pending.erase(g_pending.begin());
            } else if (!g_small_pending.empty()) {
                Job j = g_small_pending.front();
                g_small_pending.erase(g_small_pending.begin());
                g_small_queued.erase(j.vb_va);
                popped[popped_count++] = j;
            } else if (!g_medium_pending.empty()) {
                Job j = g_medium_pending.front();
                g_medium_pending.erase(g_medium_pending.begin());
                g_small_queued.erase(j.vb_va);
                popped[popped_count++] = j;
            } else {
                break;
            }
        }
    }
    if (!popped_count) return false;

    InflightSlot valid[kMaxBatch] = {};
    uint32_t valid_count = 0;
    for (uint32_t i = 0; i < popped_count; ++i) {
        const Job& j = popped[i];
        const uint64_t ibW = j.ib->GetDesc().Width;
        const uint64_t vbW = j.vb->GetDesc().Width;
        const uint64_t ib_va = j.ib->GetGPUVirtualAddress() + j.ib_off;
        const uint64_t vb_va = j.vb->GetGPUVirtualAddress() + j.vb_off;
        const uint32_t ib_shift = (uint32_t)(ib_va & 3u);
        const uint64_t ib_aligned_off = j.ib_off >= ib_shift ? (uint64_t)(j.ib_off - ib_shift) : ibW + 1;
        const uint64_t ib_read_end = j.args
            ? ib_aligned_off + 4ull
            : ib_aligned_off + (((uint64_t)ib_shift + j.ib_bytes + 3ull) & ~3ull);
        const bool args_ok = !j.args ||
            ((j.args_va & 3u) == 0 && j.args_off + 20ull <= j.args->GetDesc().Width);
        const uint32_t reach = j.args ? ~0u : (j.ib_off + j.ib_bytes);
        if (!ValidAlignment(ib_va, vb_va, j.idx_size) || ib_read_end > ibW || j.vb_off >= vbW || !args_ok) {
            {
                std::unique_lock l(g_mtx);
                StoreLocked(j.vb_va, Cached{ 0, j.ib_bytes, reach, 0, j.ib_base_va, false });
            }
            ReleaseJob(j);
            continue;
        }
        if (g_mods_toggles.ibhash_gpu_skip) {
            if (g_debug && g_ibhash_skip_logged.fetch_add(1, std::memory_order_relaxed) < 50)
                logger::write(
                    "IBHASH-SKIP vb_va=%llx ibBytes=%u idx=%u stride=%u vbExtent=%llu B -- X35 on, cached 0, not dispatched",
                    (unsigned long long)j.vb_va, j.ib_bytes, j.idx_size, j.vb_stride,
                    (unsigned long long)(vbW - j.vb_off));
            {
                std::unique_lock l(g_mtx);
                StoreLocked(j.vb_va, Cached{ 0, j.ib_bytes, reach, 0, j.ib_base_va, false });
            }
            ReleaseJob(j);
            continue;
        }
        valid[valid_count++] = InflightSlot{ j, vbW - j.vb_off, false };
    }
    if (!valid_count) return false;

    cl->SetPipelineState(g_pso.Get());
    cl->SetComputeRootSignature(g_rs.Get());
    cl->SetComputeRootUnorderedAccessView(4, g_out->GetGPUVirtualAddress());
    const int gt = g_gpu_timer.Begin(cl, GT_IbHash);
    void* const hp = HotPath::GpuBracketBegin(cl, GT_IbHash);
    for (uint32_t s = 0; s < valid_count; ++s) {
        const Job& j = valid[s].job;
        const uint64_t ibW = j.ib->GetDesc().Width;
        const uint64_t ib_va = j.ib->GetGPUVirtualAddress() + j.ib_off;
        const uint64_t vb_va = j.vb->GetGPUVirtualAddress() + j.vb_off;
        const uint32_t ib_shift = (uint32_t)(ib_va & 3u);
        const uint64_t ib_aligned_off = (uint64_t)(j.ib_off - ib_shift);
        const uint64_t ib_total = (ibW - ib_aligned_off) & ~3ull;
        D3D12_RESOURCE_STATES ib_st = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES vb_st = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES args_st = D3D12_RESOURCE_STATE_COMMON;
        const bool ib_tr = NeedsSrvTransition(j.ib, &ib_st);
        const bool vb_tr = j.vb != j.ib && NeedsSrvTransition(j.vb, &vb_st);
        const bool args_tr = j.args && j.args != j.ib && j.args != j.vb && NeedsSrvTransition(j.args, &args_st);
        if (ib_tr) TransitionBarrier(cl, j.ib, ib_st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (vb_tr) TransitionBarrier(cl, j.vb, vb_st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (args_tr) TransitionBarrier(cl, j.args, args_st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        const uint64_t vb_ext = valid[s].ext;
        const uint32_t consts[9] = {
            j.ib_bytes,
            j.idx_size,
            j.vb_stride,
            vb_ext > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)vb_ext,
            ib_shift,
            j.args ? 1u : 0u,
            0u,
            ib_total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)ib_total,
            s * kSlotStride,
        };
        cl->SetComputeRoot32BitConstants(0, 9, consts, 0);
        cl->SetComputeRootShaderResourceView(1, ib_va - ib_shift); // t0 IB (4-byte aligned)
        cl->SetComputeRootShaderResourceView(2, vb_va); // t1 VB (4-byte aligned)
        cl->SetComputeRootShaderResourceView(3, j.args ? j.args_va : (ib_va - ib_shift)); // t2 Args (4-byte aligned)
        cl->Dispatch(1, 1, 1);

        if (args_tr) TransitionBarrier(cl, j.args, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, args_st);
        if (vb_tr) TransitionBarrier(cl, j.vb, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, vb_st);
        if (ib_tr) TransitionBarrier(cl, j.ib, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ib_st);
    }
    HotPath::GpuBracketEnd(cl, hp);
    g_gpu_timer.End(cl, gt);
    CopyAsSource(cl, g_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        [&] { cl->CopyBufferRegion(g_rb.Get(), 0, g_out.Get(), 0, valid_count * kSlotStride); });
    QueryPerformanceCounter(&g_inflight_t0);
    {
        std::unique_lock l(g_mtx); // the fence comes with Submitted
        for (uint32_t s = 0; s < valid_count; ++s) g_inflight_slots[s] = valid[s];
        g_inflight_count = valid_count;
        g_inflight = true;
        g_recorded = true;
    }
    return true;
}

void Submitted(ID3D12Fence* fence, uint64_t value)
{
    std::unique_lock l(g_mtx);
    if (!g_recorded) return;
    g_recorded = false;
    if (!g_inflight) return;
    g_fence = fence; // read the result once this value passes
    g_inflight_fv = value;
}

void LogInflight(const char* why)
{
    if (!g_inflight || !g_inflight_count) {
        logger::write("IBHASH-INFLIGHT (%s): no hash dispatch was in flight", why);
        return;
    }
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    const InflightSlot& sl = g_inflight_slots[0];
    const Job& ij = sl.job;
    logger::write("IBHASH-INFLIGHT (%s): %u hash(es) dispatched %.1f ms earlier and never collected | first: ib_off=%u "
                  "ibBytes=%u idx=%u (%u indices) stride=%u vbExtent=%llu B vb_va=%llx",
        why, g_inflight_count, QpcMs(g_inflight_t0, t1), ij.ib_off, ij.ib_bytes, ij.idx_size,
        ij.idx_size ? ij.ib_bytes / ij.idx_size : 0u, ij.vb_stride, (unsigned long long)sl.ext,
        (unsigned long long)ij.vb_va);
}

} // namespace IbHash
