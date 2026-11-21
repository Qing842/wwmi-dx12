// character_recognition.cpp -- see character_recognition.h.
#include "../addon_common.h"
#include "character_recognition.h"
#include "buffer_relocation.h"
#include "component_table.h"
#include "game_cb_reader.h"
#include "layout_registry.h"
#include "../core/fnv.h"
#include "../draw/draw_toggles.h"
#include "../instrumentation/diag_gate.h"
#include "../hooks/command_list_registry.h"
#include "../hooks/pipeline_registry.h"
#include "../instrumentation/profiler.h"
#include "../textures/tex_rules.h"
#include "../textures/tex_select.h"
#include "../textures/tex_lod.h"
#include "mod_set.h"
#include <cwctype>

CharacterRecognition g_recognition;

bool CharacterRecognition::CharMeshOrigin(uint64_t vb0, CharVbOrigin* out)
{
    std::lock_guard l(char_mesh_mutex_);
    auto cv = char_mesh_vb0_.find(vb0);
    if (cv == char_mesh_vb0_.end()) return false;
    *out = cv->second;
    return true;
}

bool CharacterRecognition::IsCharMeshVb0(uint64_t vb0)
{
    std::lock_guard l(char_mesh_mutex_);
    return char_mesh_vb0_.count(vb0) != 0;
}

void CharacterRecognition::NoteCharMeshVb0(uint64_t vb0, const CharVbOrigin& origin)
{
    std::lock_guard l(char_mesh_mutex_);
    char_mesh_vb0_[vb0] = origin;
}

void CharacterRecognition::ForgetCharMeshVb0(uint64_t va, uint64_t bytes)
{
    if (!va) return;
    if (!bytes) bytes = 1;
    unsigned n = 0;
    {
        std::lock_guard l(char_mesh_mutex_);
        for (auto it = char_mesh_vb0_.begin(); it != char_mesh_vb0_.end();)
            if (it->first >= va && it->first - va < bytes) {
                it = char_mesh_vb0_.erase(it);
                ++n;
            } else
                ++it;
    }
    if (n && logger::enabled(2))
        logger::write("CHARVB: forgot %u learned VB0(s) in destroyed buffer va=%llx bytes=%llu", n,
            (unsigned long long)va, (unsigned long long)bytes);
}

bool CharacterRecognition::AnyCharMesh()
{
    std::lock_guard l(char_mesh_mutex_);
    return !char_mesh_vb0_.empty();
}

size_t CharacterRecognition::CharMeshCount()
{
    std::lock_guard l(char_mesh_mutex_);
    return char_mesh_vb0_.size();
}

bool CharacterRecognition::InFrameSkel(uint32_t hash)
{
    std::lock_guard l(char_skel_mutex_);
    return frame_skel_.count(hash) != 0;
}

void CharacterRecognition::NoteFrameSkel(uint32_t hash)
{
    std::lock_guard l(char_skel_mutex_);
    frame_skel_.insert(hash);
}

bool CharacterRecognition::IsCharLodVb0(uint64_t vb0)
{
    std::lock_guard l(char_skel_mutex_);
    return char_lod_vb0_.count(vb0) != 0;
}

bool CharacterRecognition::RecogniseLod(uint32_t skel_hash, uint64_t vb0, bool* linked_now)
{
    std::lock_guard l(char_skel_mutex_);
    if (skel_hash && vb0 && frame_skel_.count(skel_hash)) {
        if (char_lod_vb0_.insert(vb0).second) *linked_now = true;
    }
    return vb0 && char_lod_vb0_.count(vb0) != 0;
}

void CharacterRecognition::CompSet::set(int i)
{
    const size_t k = (size_t)i / 64;
    if (w.size() <= k) w.resize(k + 1, 0);
    w[k] |= 1ull << (i % 64);
}

bool CharacterRecognition::CompSet::empty() const
{
    for (uint64_t x : w)
        if (x) return false;
    return true;
}

bool CharacterRecognition::CompSet::covers(const CompSet& d) const
{
    for (size_t k = 0; k < d.w.size(); ++k)
        if ((d.w[k] & (k < w.size() ? w[k] : 0)) != d.w[k]) return false;
    return true;
}

unsigned CharacterRecognition::CompSet::count() const
{
    unsigned n = 0;
    for (uint64_t x : w)
        while (x) {
            x &= x - 1;
            ++n;
        }
    return n;
}

struct CharacterRecognition::PerSet : SetState {
    std::unordered_map<int, CompSet> declared;
    std::vector<uint8_t> shape_ambiguous;
    std::vector<std::vector<unsigned>> sk_cbs;
    void Build(ModSet& set) override
    {
        auto is_numbered_comp = [](const WwmiMod::Component* wc) {
            if (!wc || wc->tail.size() <= 9 || _wcsnicmp(wc->tail.c_str(), L"Component", 9) != 0) return false;
            for (size_t i = 9; i < wc->tail.size(); ++i)
                if (!iswdigit(wc->tail[i])) return false;
            return true;
        };
        const size_t n_mods = set.mods.Count();
        std::vector<uint8_t> has_numbered_comp(n_mods, 0);
        for (const MergedComp& c : set.comps.All()) {
            if (c.comp_idx < 0 || c.mod_id < 0 || (size_t)c.mod_id >= n_mods) continue;
            if (is_numbered_comp(set.mods.Comp(c.mod_id, c.comp_idx))) has_numbered_comp[(size_t)c.mod_id] = 1;
        }
        for (const MergedComp& c : set.comps.All()) {
            if (c.comp_idx < 0 || c.mod_id < 0 || (size_t)c.mod_id >= n_mods) continue;
            if (has_numbered_comp[(size_t)c.mod_id] && !is_numbered_comp(set.mods.Comp(c.mod_id, c.comp_idx))) continue;
            declared[c.mod_id].set(c.comp_idx);
        }
        shape_ambiguous.assign(n_mods, 0);
        for (size_t m = 0; m < n_mods; ++m) {
            auto dit = declared.find((int)m);
            if (dit == declared.end() || dit->second.count() < 2) {
                shape_ambiguous[m] = 1;
                continue;
            }
            bool any_unique_range = false;
            for (const MergedComp& c : set.comps.All()) {
                if (c.mod_id != (int)m || c.comp_idx < 0) continue;
                CompSet single;
                single.set(c.comp_idx);
                if (!dit->second.covers(single)) continue;
                const WwmiMod::Component* wc = set.mods.Comp(c.mod_id, c.comp_idx);
                const uint32_t own_ib = wc ? wc->ib_hash : 0u;
                const std::vector<int>* sh = set.comps.SharedSlots(c.first_index, c.match_count);
                bool diff_ib = false;
                if (sh) {
                    for (int s : *sh) {
                        const MergedComp& oc = set.comps.At((size_t)s);
                        const WwmiMod::Component* owc = set.mods.Comp(oc.mod_id, oc.comp_idx);
                        const uint32_t other_ib = owc ? owc->ib_hash : 0u;
                        if (other_ib != own_ib || !own_ib) {
                            diff_ib = true;
                            break;
                        }
                    }
                }
                if (!diff_ib) {
                    any_unique_range = true;
                    break;
                }
            }
            if (!any_unique_range) shape_ambiguous[m] = 1;
        }
        sk_cbs.resize(n_mods);
        for (size_t m = 0; m < n_mods; ++m) {
            const auto sets = PortApi::ModDeclaredShapeKeySets(set.mods.At(m).wcl_ns.c_str());
            for (const PortApi::ShapeKeySet& s : sets)
                if (s.cb) sk_cbs[m].push_back(s.cb);
        }
    }
};

CharacterRecognition::CompSet CharacterRecognition::ModDeclaredComps(int mod_id)
{
    const std::unordered_map<int, CompSet>& declared = CurrentSet().State<PerSet>().declared;
    auto it = declared.find(mod_id);
    return it == declared.end() ? CompSet{} : it->second;
}

bool CharacterRecognition::ModNeedsProof(int mod_id)
{
    const auto& amb = CurrentSet().State<PerSet>().shape_ambiguous;
    return mod_id < 0 || (size_t)mod_id >= amb.size() || amb[(size_t)mod_id] != 0;
}

bool CharacterRecognition::ModDeclaresShapekeyHash(int mod_id, uint32_t u0_hash)
{
    if (!u0_hash || mod_id < 0) return false;
    const auto& cbs = CurrentSet().State<PerSet>().sk_cbs;
    if ((size_t)mod_id >= cbs.size()) return false;
    for (unsigned h : cbs[(size_t)mod_id])
        if (h == u0_hash) return true;
    return false;
}

uint32_t CharacterRecognition::BoundShapekeyU0Hash(command_list* cmd_list)
{
    const auto& st = g_command_lists.Gfx(cmd_list);
    if (!st.gfx_pso || !g_pipelines.ReadsStream6(st.gfx_pso)) return 0;
    if (!st.vb[6].res || !st.vb[6].res_width || st.vb[6].res_width > 0xFFFFFFFFull) return 0;
    const unsigned orig = g_buffer_reloc.IdentityBytes(st.vb[6].res, (unsigned)st.vb[6].res_width);
    return orig ? PortApi::GameComputeUavBufferHash(orig) : 0u;
}

bool CharacterRecognition::DrawMatchesModShapekey(command_list* cmd_list, int mod_id)
{
    const uint32_t u0 = BoundShapekeyU0Hash(cmd_list);
    return u0 && ModDeclaresShapekeyHash(mod_id, u0);
}

void CharacterRecognition::NotePassiveSharedEvidence(uint64_t ib_va, const std::vector<int>& cands)
{
    if (!ib_va || cands.empty()) return;
    std::lock_guard l(ib_anchor_mutex_);
    IbEvidence& e = ib_ev_[ib_va];
    if (e.frame != g_frame_id) {
        e.frame = g_frame_id;
        e.entries.clear();
    }
    for (int s : cands) {
        const MergedComp& mc = Comps().At((size_t)s);
        if (mc.mod_id < 0 || mc.comp_idx < 0 || (size_t)mc.mod_id >= Mods().Count()) continue;
        const uint64_t ns_hash = Mods().At((size_t)mc.mod_id).ns_hash;
        if (!ns_hash) continue;
        IbEvidenceEntry* ent = nullptr;
        for (IbEvidenceEntry& it : e.entries)
            if (it.ns_hash == ns_hash) {
                ent = &it;
                break;
            }
        if (!ent) {
            if (e.entries.size() >= 8) continue;
            e.entries.push_back(IbEvidenceEntry{ ns_hash, {}, false });
            ent = &e.entries.back();
        }
        ent->seen.set(mc.comp_idx);
    }
}

int CharacterRecognition::PickCandidateByIbEvidence(uint64_t ib_va, const std::vector<int>& cands)
{
    if (!ib_va || cands.empty()) return -1;
    std::lock_guard l(ib_anchor_mutex_);
    auto it = ib_ev_.find(ib_va);
    if (it == ib_ev_.end() || it->second.frame != g_frame_id || it->second.entries.empty()) return -1;
    const auto& entries = it->second.entries;
    int best_s = -1;
    uint32_t best_ib = 0;
    bool best_proven = false;
    unsigned best_cnt = 0;
    bool tied_diff_ib = false;
    for (int s : cands) {
        const MergedComp& mc = Comps().At((size_t)s);
        if (mc.mod_id < 0 || (size_t)mc.mod_id >= Mods().Count()) continue;
        const uint64_t ns_hash = Mods().At((size_t)mc.mod_id).ns_hash;
        const WwmiMod::Component* wc = Mods().Comp(mc.mod_id, mc.comp_idx);
        const uint32_t cand_ib = wc ? wc->ib_hash : 0u;
        const IbEvidenceEntry* ent = nullptr;
        for (const IbEvidenceEntry& e : entries)
            if (e.ns_hash == ns_hash) {
                ent = &e;
                break;
            }
        if (!ent) continue;
        const unsigned cnt = ent->seen.count();
        if (!cnt && !ent->proven) continue;
        if (best_s < 0 || (ent->proven && !best_proven) || (ent->proven == best_proven && cnt > best_cnt)) {
            best_s = s;
            best_ib = cand_ib;
            best_proven = ent->proven;
            best_cnt = cnt;
            tied_diff_ib = false;
        } else if (ent->proven == best_proven && cnt == best_cnt && (cand_ib != best_ib || !cand_ib)) {
            tied_diff_ib = true;
        }
    }
    return tied_diff_ib ? -1 : best_s;
}

bool CharacterRecognition::IbAnchorNoteAndAsk(
    uint64_t ib_va, uint64_t ib_bytes, int mod_id, int comp_idx, bool content_proven)
{
    if (!ib_va || mod_id < 0 || comp_idx < 0 || mod_id >= (int)Mods().Count()) return false;
    const CompSet declared = ModDeclaredComps(mod_id);
    if (declared.empty()) return false;
    const bool need_proof = ModNeedsProof(mod_id);
    const uint64_t ns_hash = Mods().At(mod_id).ns_hash;
    if (!ns_hash) return false;
    std::lock_guard l(ib_anchor_mutex_);
    IbEvidence& e = ib_ev_[ib_va];
    if (e.frame != g_frame_id) {
        e.frame = g_frame_id;
        e.entries.clear();
    }
    IbEvidenceEntry* ent = nullptr;
    for (IbEvidenceEntry& it : e.entries)
        if (it.ns_hash == ns_hash) {
            ent = &it;
            break;
        }
    if (!ent) {
        if (e.entries.size() >= 8) e.entries.erase(e.entries.begin());
        e.entries.push_back(IbEvidenceEntry{ ns_hash, {}, false });
        ent = &e.entries.back();
    }
    ent->seen.set(comp_idx);
    if (content_proven) ent->proven = true;
    const bool shape_only = need_proof && !ent->proven;
#ifdef WWMI_DEBUG
    if (shape_only && ent->seen.covers(declared) &&
        DiagOnce("IBANCHOR-SHAPE", (int)(uint32_t)(ns_hash ^ (ns_hash >> 32))))
        logger::write("IBANCHOR-SHAPE mod %d: index buffer %llx holds draws matching its component shape, but "
                      "nothing in it proves it is this mod's mesh -- not anchored",
            mod_id, (unsigned long long)ib_va);
#endif
    if (!shape_only && ent->seen.covers(declared)) {
        auto cur = ib_anchor_.find(ib_va);
        const bool blocked_by_proven = cur != ib_anchor_.end() && cur->second.ns_hash != ns_hash &&
            cur->second.proven && !ent->proven && g_frame_id - cur->second.confirmed <= 4u;
        if (!blocked_by_proven) {
            IbAnchorRec& a = ib_anchor_[ib_va];
            a.bytes = ib_bytes;
            if (a.ns_hash != ns_hash) {
                a.ns_hash = ns_hash;
                a.learned = g_frame_id;
                a.proven = ent->proven;
                g_draw_stats.ib_anchor_learned.fetch_add(1, std::memory_order_relaxed);
                logger::write("IBANCHOR learned: index buffer %llx carries mod %d's COMPLETE declared component "
                              "set (%u components, proven=%d) in frame %u -- its draws are claimed from the next frame on",
                    (unsigned long long)ib_va, mod_id, declared.count(), (int)ent->proven, g_frame_id);
            } else if (ent->proven) {
                a.proven = true;
            }
            a.confirmed = g_frame_id;
        }
    }
    if (ib_anchor_.size() > kIbAnchorCap)
        for (auto it = ib_anchor_.begin(); it != ib_anchor_.end();)
            if (g_frame_id - it->second.confirmed > kIbAnchorStale)
                it = ib_anchor_.erase(it);
            else
                ++it;
    auto it = ib_anchor_.find(ib_va);
    if (it == ib_anchor_.end() || it->second.ns_hash != ns_hash) return false;
    return it->second.learned < g_frame_id;
}

bool CharacterRecognition::IbAnchorResolve(uint64_t index_va, uint32_t index_bytes, int* mod_id, uint64_t* ib_base_va)
{
    if (!index_va || !index_bytes) return false;
    std::lock_guard l(ib_anchor_mutex_);
    auto it = ib_anchor_.upper_bound(index_va);
    if (it == ib_anchor_.begin()) return false;
    --it; // the greatest anchored base at or below the address
    const uint64_t off = index_va - it->first;
    if (!it->second.bytes || off >= it->second.bytes || (off % index_bytes) != 0 || !it->second.ns_hash) return false;
    *mod_id = Mods().IdOfNsHash(it->second.ns_hash);
    if (*mod_id < 0) return false;
    *ib_base_va = it->first;
    return true;
}

bool CharacterRecognition::IbAnchorJustLearned(uint64_t index_va)
{
    if (!index_va) return false;
    std::lock_guard l(ib_anchor_mutex_);
    auto it = ib_anchor_.upper_bound(index_va);
    if (it == ib_anchor_.begin()) return false;
    --it;
    return index_va - it->first < it->second.bytes && it->second.learned + 1 == g_frame_id;
}

void CharacterRecognition::IbAnchorForget(uint64_t va, uint64_t bytes)
{
    if (!va) return;
    if (!bytes) bytes = 1;
    std::lock_guard l(ib_anchor_mutex_);
    ib_ev_.erase(ib_ev_.lower_bound(va), ib_ev_.lower_bound(va + bytes));
    ib_anchor_.erase(ib_anchor_.lower_bound(va), ib_anchor_.lower_bound(va + bytes));
}

uint32_t HashBoneCB(ID3D12Resource* res, uint64_t va)
{
    PROF(PROF_DI_BoneHash);
    uint8_t buf[1536];
    const uint32_t n = g_game_cbs.Snapshot(res, va, buf, sizeof(buf));
    if (!n) return 0;
    const uint32_t h = Fnv1a32(buf, n);
    return h ? h : 1;
}

static void CollectBoundModTexHashes(command_list* cmd_list, uint32_t* out, int cap, int& n)
{
    n = 0;
    const TexRules& tr = g_tex_rules.Now();
    if (tr.targets.empty()) return;
    PROF(PROF_DI_TexScan); // the per-draw table walk behind char_tex
    const auto& root = g_command_lists.Gfx(cmd_list).root;
    uint32_t scan_len[64];
    g_layouts.TexScanLens(g_command_lists.GfxLayout(cmd_list), scan_len);
    for (int p = 0; p < 64; ++p) {
        const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
        if (!ts) continue;
        uint32_t num = ts.Heap()->GetDesc().NumDescriptors;
        ID3D12Resource* res[kTexScanMax];
        uint32_t hs[kTexScanMax];
        const uint32_t maxS = g_tex_select.ResolveBound(ts.heap, ts.base, num, res, hs, scan_len[p]);
        for (uint32_t s = 0; s < maxS; ++s) {
            const uint32_t h = !hs[s] || tr.targets.count(hs[s]) ? hs[s] : g_tex_select.LodTargetOf(hs[s]);
            if (h && tr.targets.count(h)) {
                if (n < cap) out[n++] = h;
                if (n >= cap) return;
            }
        }
    }
}

bool CharacterRecognition::DrawBindsModTexture(command_list* cmd_list)
{
    uint32_t hs[8];
    int n = 0;
    CollectBoundModTexHashes(cmd_list, hs, 8, n);
    return n > 0;
}

bool CharacterRecognition::DrawBindsOwnModTexture(command_list* cmd_list, int mod_id)
{
    if (mod_id < 0 || (size_t)mod_id >= Mods().Count()) return false;
    const wchar_t* ns = Mods().At((size_t)mod_id).wcl_ns.c_str();
    if (!ns || !*ns) return false;
    uint32_t hs[16];
    int n = 0;
    CollectBoundModTexHashes(cmd_list, hs, 16, n);
    for (int i = 0; i < n; ++i)
        if (PortApi::TextureOverrideInNs(hs[i], ns)) return true;
    return false;
}

int CharacterRecognition::PickCandidateByBoundTextures(command_list* cmd_list, const std::vector<int>& cands)
{
    if (cands.empty()) return -1;
    uint32_t hs[16];
    int n = 0;
    CollectBoundModTexHashes(cmd_list, hs, 16, n);
    if (!n) return -1;
    int chosen_s = -1;
    uint32_t chosen_ib = 0;
    for (int i = 0; i < n; ++i) {
        const uint32_t h = hs[i];
        int match_s = -1;
        uint32_t match_ib = 0;
        bool multi_ib = false;
        for (int s : cands) {
            const MergedComp& mc = Comps().At((size_t)s);
            if (mc.mod_id < 0 || (size_t)mc.mod_id >= Mods().Count()) continue;
            const wchar_t* ns = Mods().At((size_t)mc.mod_id).wcl_ns.c_str();
            if (!ns || !*ns || !PortApi::TextureOverrideInNs(h, ns)) continue;
            const WwmiMod::Component* wc = Mods().Comp(mc.mod_id, mc.comp_idx);
            const uint32_t cand_ib = wc ? wc->ib_hash : 0u;
            if (match_s < 0) {
                match_s = s;
                match_ib = cand_ib;
            } else if (cand_ib != match_ib || !cand_ib) {
                multi_ib = true;
                break;
            }
        }
        if (match_s < 0 || multi_ib) continue; // shared texture across candidates, or matched none
        if (chosen_s < 0) {
            chosen_s = match_s;
            chosen_ib = match_ib;
        } else if (match_ib != chosen_ib || !chosen_ib) {
            return -1; // conflicting exclusive textures on the same draw
        }
    }
    return chosen_s;
}

bool CharacterRecognition::IbHashEverClaimed(uint32_t ib_hash)
{
    std::lock_guard l(claimed_ibhash_mutex_);
    return claimed_ibhash_.count(ib_hash) != 0;
}

void CharacterRecognition::NoteClaimedIbHash(uint32_t ib_hash)
{
    std::lock_guard l(claimed_ibhash_mutex_);
    claimed_ibhash_.insert(ib_hash);
}

std::shared_ptr<const std::unordered_set<uint32_t>> CharacterRecognition::SkipVbHashes()
{
    std::lock_guard l(skip_vbhash_mutex_);
    return skip_vbhash_;
}

void CharacterRecognition::SetSkipVbHashes(std::shared_ptr<const std::unordered_set<uint32_t>> hashes)
{
    std::lock_guard l(skip_vbhash_mutex_);
    skip_vbhash_ = std::move(hashes);
    skip_vbhash_any_ = skip_vbhash_ != nullptr;
}

std::shared_ptr<const std::unordered_set<uint32_t>> CharacterRecognition::SmallMeshHashes(uint32_t* gen)
{
    std::lock_guard l(small_mesh_mutex_);
    *gen = small_mesh_gen_.load(std::memory_order_relaxed);
    return small_mesh_;
}

void CharacterRecognition::SetSmallMeshHashes(std::shared_ptr<const std::unordered_set<uint32_t>> hashes)
{
    std::lock_guard l(small_mesh_mutex_);
    small_mesh_ = std::move(hashes);
    small_mesh_any_ = small_mesh_ != nullptr;
    small_mesh_gen_.fetch_add(1);
}

void CharacterRecognition::SetModelHashes(std::shared_ptr<const std::unordered_set<uint32_t>> hashes)
{
    std::lock_guard l(model_mutex_);
    model_hashes_ = std::move(hashes);
    model_drawn_.clear();
    model_any_ = model_hashes_ != nullptr;
    model_gen_.fetch_add(1);
}

void CharacterRecognition::NoteModelDrawn(uint32_t hash)
{
    thread_local uint32_t t_gen = ~0u;
    thread_local std::shared_ptr<const std::unordered_set<uint32_t>> t_hashes;
    if (model_gen_.load(std::memory_order_acquire) != t_gen) {
        std::lock_guard l(model_mutex_);
        t_hashes = model_hashes_;
        t_gen = model_gen_.load(std::memory_order_relaxed);
    }
    if (!t_hashes || !t_hashes->count(hash)) return;
    std::lock_guard l(model_mutex_);
    model_drawn_[hash] = g_frame_id;
}

void CharacterRecognition::ModelsDrawnWithin(uint32_t frames, std::unordered_set<uint32_t>& out)
{
    std::lock_guard l(model_mutex_);
    for (const auto& [hash, frame] : model_drawn_)
        if (g_frame_id - frame <= frames) out.insert(hash);
}

void CharacterRecognition::UpdateSkinBand()
{
    uint64_t mn = ~0ull;
    uint64_t mx = 0;
    {
        std::lock_guard l(char_mesh_mutex_);
        for (const auto& kv : char_mesh_vb0_) {
            const uint64_t v = kv.first;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    }
    if (mn == ~0ull) return; // nothing learned yet -> keep fallback
    skin_band_lo_.store(mn & ~0xFFFFFFFFull); // round min down to 4GB boundary
    skin_band_hi_.store((mx & ~0xFFFFFFFFull) + 0x200000000ull); // cover max + ~8GB pool headroom
}

bool CharacterRecognition::SkinCsAccepts(uint64_t cs_hash)
{
    std::lock_guard l(skincache_mutex_);
    return char_skin_cs_.empty() || char_skin_cs_.count(cs_hash);
}

bool CharacterRecognition::IsSkinCs(uint64_t cs_hash)
{
    std::lock_guard l(skincache_mutex_);
    return char_skin_cs_.count(cs_hash) != 0;
}

bool CharacterRecognition::LearnSkinCs(uint64_t cs_hash, const std::vector<SkinOut>& outs, int* added, size_t* total)
{
    std::lock_guard l(skincache_mutex_);
    *added = 0;
    for (const SkinOut& b : outs) {
        bool dup = false;
        for (auto& o : skincache_out_)
            if (o.va == b.va) {
                dup = true;
                break;
            }
        if (!dup) {
            skincache_out_.push_back({ b.va, b.size });
            ++*added;
        }
    }
    *total = skincache_out_.size();
    return char_skin_cs_.insert(cs_hash).second;
}

bool CharacterRecognition::InSkinOut(uint64_t vb0)
{
    std::lock_guard l(skincache_mutex_);
    for (auto& o : skincache_out_)
        if (vb0 >= o.va && vb0 < o.va + o.size) return true;
    return false;
}

size_t CharacterRecognition::SkinOutCount()
{
    std::lock_guard l(skincache_mutex_);
    return skincache_out_.size();
}

void CharacterRecognition::ForgetSkinOutRange(uint64_t va, uint64_t bytes)
{
    std::lock_guard l(skincache_mutex_);
    std::erase_if(skincache_out_, [&](const SkinOut& o) { return o.va >= va && o.va < va + bytes; });
}

void CharacterRecognition::NoteCharBoneVa(uint64_t cur_va, uint64_t prev_va)
{
    std::lock_guard l(skincache_mutex_);
    if (cur_va && char_bone_va_.size() < 64) char_bone_va_.insert(cur_va);
    if (prev_va && char_bone_va_.size() < 64) char_bone_va_.insert(prev_va);
}

uint64_t CharacterRecognition::CharBoneDistance(uint64_t va)
{
    uint64_t bdist = ~0ull;
    std::lock_guard l(skincache_mutex_);
    for (uint64_t v : char_bone_va_) {
        uint64_t d = va > v ? va - v : v - va;
        if (d < bdist) bdist = d;
    }
    return bdist;
}

void CharacterRecognition::BeginFrame()
{
    {
        std::lock_guard l(char_skel_mutex_);
        frame_skel_.clear();
    }
    {
        std::lock_guard l(skincache_mutex_);
        char_bone_va_.clear();
    }
}

void CharacterRecognition::NoteHeartbeatDraw(uint64_t vb0)
{
    std::lock_guard l(hb_mutex_);
    ++hb_large_;
    if (vb0 < hb_vbmin_) hb_vbmin_ = vb0;
    if (vb0 > hb_vbmax_) hb_vbmax_ = vb0;
    if (InSkinBand(vb0)) ++hb_pool_;
}

void CharacterRecognition::TakeHeartbeat(uint64_t* vbmin, uint64_t* vbmax, uint32_t* large, uint32_t* pool)
{
    std::lock_guard l(hb_mutex_);
    *vbmin = hb_vbmin_;
    *vbmax = hb_vbmax_;
    *large = hb_large_;
    *pool = hb_pool_;
    hb_vbmin_ = ~0ull;
    hb_vbmax_ = 0;
    hb_large_ = 0;
    hb_pool_ = 0;
}
