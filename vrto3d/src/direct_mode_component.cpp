/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VRto3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with VRto3D. If not, see <http://www.gnu.org/licenses/>.
 */

#include "direct_mode_component.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include <dxgi.h>

#include "dx11_renderer.h"
#include "vrto3dlib/debug_log.hpp"
#include "vrto3dlib/win32_helper.hpp"


DirectModeComponent::DirectModeComponent(Dx11Renderer* renderer)
    : renderer_(renderer)
{}


DirectModeComponent::~DirectModeComponent()
{
    std::lock_guard<std::mutex> lk(sets_mutex_);
    // Each TextureSet shows up three times in handle_map_; delete each unique
    // pointer once.
    std::set<TextureSet*> deleted;
    for (auto& kv : handle_map_) {
        if (deleted.insert(kv.second.first).second) {
            delete kv.second.first;
        }
    }
    handle_map_.clear();
}


void DirectModeComponent::CreateSwapTextureSet(uint32_t unPid,
                                               const SwapTextureSetDesc_t* pDesc,
                                               SwapTextureSet_t* pOut)
{
    if (!pDesc || !pOut || !renderer_ || !renderer_->Device()) return;

    LOG() << "DirectModeComponent::CreateSwapTextureSet pid=" << unPid
          << " " << pDesc->nWidth << "x" << pDesc->nHeight
          << " fmt=" << pDesc->nFormat
          << " samples=" << pDesc->nSampleCount;

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = pDesc->nWidth;
    td.Height           = pDesc->nHeight;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = static_cast<DXGI_FORMAT>(pDesc->nFormat);
    td.SampleDesc.Count = pDesc->nSampleCount == 0 ? 1 : pDesc->nSampleCount;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // Legacy shared handle (matches ALVR's working path; the NTHANDLE +
    // keyed-mutex path is documented but not exercised here).
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;

    auto* set = new TextureSet();
    set->pid = unPid;

    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        HRESULT hr = renderer_->Device()->CreateTexture2D(&td, nullptr, &set->textures[i]);
        if (FAILED(hr) || !set->textures[i]) {
            LOG() << "DirectModeComponent: CreateTexture2D[" << i << "] failed hr=0x"
                  << std::hex << hr;
            ok = false;
            break;
        }

        Microsoft::WRL::ComPtr<IDXGIResource> dxgi_res;
        hr = set->textures[i].As(&dxgi_res);
        if (FAILED(hr) || !dxgi_res) {
            LOG() << "DirectModeComponent: QueryInterface(IDXGIResource)[" << i
                  << "] failed hr=0x" << std::hex << hr;
            ok = false;
            break;
        }

        hr = dxgi_res->GetSharedHandle(&set->handles[i]);
        if (FAILED(hr) || !set->handles[i]) {
            LOG() << "DirectModeComponent: GetSharedHandle[" << i << "] failed hr=0x"
                  << std::hex << hr;
            ok = false;
            break;
        }

        pOut->rSharedTextureHandles[i] =
            reinterpret_cast<vr::SharedTextureHandle_t>(set->handles[i]);
    }

    if (!ok) {
        delete set;
        for (int i = 0; i < 3; ++i) pOut->rSharedTextureHandles[i] = 0;
        pOut->unTextureFlags = 0;
        return;
    }

    // unTextureFlags = 0 — we use legacy shared handles, not NT handles.
    pOut->unTextureFlags = 0;

    {
        std::lock_guard<std::mutex> lk(sets_mutex_);
        for (int i = 0; i < 3; ++i) {
            handle_map_.emplace(set->handles[i], std::make_pair(set, i));
        }
    }
}


void DirectModeComponent::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle)
{
    auto raw = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sharedTextureHandle));
    std::lock_guard<std::mutex> lk(sets_mutex_);
    auto it = handle_map_.find(raw);
    if (it == handle_map_.end()) return;
    TextureSet* set = it->second.first;
    for (HANDLE h : set->handles) handle_map_.erase(h);
    delete set;
}


void DirectModeComponent::DestroyAllSwapTextureSets(uint32_t unPid)
{
    std::lock_guard<std::mutex> lk(sets_mutex_);
    // Collect unique sets owned by this pid, delete them, then prune the map.
    std::set<TextureSet*> to_delete;
    for (auto& kv : handle_map_) {
        if (kv.second.first->pid == unPid) to_delete.insert(kv.second.first);
    }
    for (TextureSet* s : to_delete) {
        for (HANDLE h : s->handles) handle_map_.erase(h);
        delete s;
    }
}


void DirectModeComponent::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t /*handles*/[2],
                                                     uint32_t (*pIndices)[2])
{
    (*pIndices)[0] = ((*pIndices)[0] + 1) % 3;
    (*pIndices)[1] = ((*pIndices)[1] + 1) % 3;
}


void DirectModeComponent::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2])
{
    std::lock_guard<std::mutex> lk(submit_mutex_);
    if (submit_layer_count_ < kMaxLayers) {
        submit_layers_[submit_layer_count_][0] = perEye[0];
        submit_layers_[submit_layer_count_][1] = perEye[1];
    }
    // Always increment so the logged count reflects what the compositor
    // submits even when it exceeds kMaxLayers.
    ++submit_layer_count_;
}


void DirectModeComponent::Present(vr::SharedTextureHandle_t syncTexture)
{
    if (!renderer_) return;

    SubmitLayerPerEye_t local[kMaxLayers][2]{};
    int submitted_count = 0;
    int kept_count      = 0;
    {
        std::lock_guard<std::mutex> lk(submit_mutex_);
        submitted_count      = submit_layer_count_;
        submit_layer_count_  = 0;
        if (submitted_count == 0) {
            if (last_logged_submit_count_ != 0) {
                LOG() << "DirectModeComponent: Present with no SubmitLayer "
                         "(was " << last_logged_submit_count_ << ")";
                last_logged_submit_count_ = 0;
            }
            return;
        }
        kept_count = submitted_count > kMaxLayers ? kMaxLayers : submitted_count;
        for (int i = 0; i < kept_count; ++i) {
            local[i][0] = submit_layers_[i][0];
            local[i][1] = submit_layers_[i][1];
        }
    }

    if (submitted_count != last_logged_submit_count_) {
        LOG() << "DirectModeComponent: SubmitLayer calls per Present = "
              << submitted_count
              << " (was " << last_logged_submit_count_ << ")";
        last_logged_submit_count_ = submitted_count;
    }

    // Resolve each layer's eye handles back to the textures we allocated in
    // CreateSwapTextureSet. We don't filter by pid — UEVR-driven games can
    // submit the scene and HUD via two different pids (game-pid for one
    // layer, SteamVR-compositor-pid for the overlay), and dropping either
    // visibly loses content.
    Dx11Renderer::DirectModeLayerPair pairs[kMaxLayers]{};
    int resolved_count = 0;
    {
        std::lock_guard<std::mutex> lk(sets_mutex_);
        auto find_one = [&](vr::SharedTextureHandle_t h, uint32_t* out_pid)
            -> Microsoft::WRL::ComPtr<ID3D11Texture2D>
        {
            auto raw = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h));
            auto it = handle_map_.find(raw);
            if (it == handle_map_.end()) return {};
            if (out_pid) *out_pid = it->second.first->pid;
            return it->second.first->textures[it->second.second];
        };
        for (int i = 0; i < kept_count; ++i) {
            uint32_t pid_l = 0, pid_r = 0;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> left  = find_one(local[i][0].hTexture, &pid_l);
            Microsoft::WRL::ComPtr<ID3D11Texture2D> right = find_one(local[i][1].hTexture, &pid_r);
            if (!left || !right) {
                ++handle_miss_count_;
                continue;
            }
            pairs[resolved_count].left         = std::move(left);
            pairs[resolved_count].right        = std::move(right);
            pairs[resolved_count].bounds_left  = local[i][0].bounds;
            pairs[resolved_count].bounds_right = local[i][1].bounds;
            pairs[resolved_count].pid          = pid_l;
            ++resolved_count;
        }
    }

    // Reorder layers so OnDirectModeFrame's "layers[0] is the opaque base,
    // layers[1+] composite on top" assumption holds across mods.
    //
    // Two-tier sort:
    //   1. Layers owned by the game process come before layers owned by
    //      vrcompositor.exe. This catches Luke Ross R.E.A.L., which submits
    //      the actual 3D scene from the game's pid (a smaller texture) but
    //      routes its in-game UI / mod menu through a larger vrcompositor-
    //      owned overlay layer. Without this, the area heuristic alone
    //      picked the UI layer as base and the game scene (alpha=1 across
    //      the surface) drew over it, hiding the UI completely.
    //   2. Within the same group, larger effective area (texture dims
    //      scaled by submitted bounds rect) wins. This preserves UEVR's
    //      behavior where the game submits both scene and HUD from one
    //      pid: the full 5760x3240 scene beats a 2x4 HUD quad. It also
    //      handles bounds-shrunk allocations (e.g. a 2592x1458 texture
    //      with bounds=(0,0,0.741,0.741) rendered into the top-left
    //      1920x1080) — raw width*height would mis-prefer a smaller
    //      full-bounds layer like SteamVR Home.
    if (resolved_count > 1) {
        auto effective_area = [](const Dx11Renderer::DirectModeLayerPair& p) -> uint64_t {
            D3D11_TEXTURE2D_DESC d{};
            p.left->GetDesc(&d);
            const float du = std::abs(p.bounds_left.uMax - p.bounds_left.uMin);
            const float dv = std::abs(p.bounds_left.vMax - p.bounds_left.vMin);
            const double eff_w = static_cast<double>(d.Width)  * du;
            const double eff_h = static_cast<double>(d.Height) * dv;
            if (eff_w <= 0.0 || eff_h <= 0.0) return 0;
            return static_cast<uint64_t>(eff_w * eff_h);
        };

        struct LayerKey {
            int      original_index;
            bool     is_compositor;
            uint64_t eff_area;
        };
        LayerKey keys[kMaxLayers];
        for (int i = 0; i < resolved_count; ++i) {
            auto it = pid_name_cache_.find(pairs[i].pid);
            if (it == pid_name_cache_.end()) {
                it = pid_name_cache_.emplace(pairs[i].pid, GetProcessName(pairs[i].pid)).first;
            }
            keys[i].original_index = i;
            keys[i].is_compositor  = _stricmp(it->second.c_str(), "vrcompositor.exe") == 0;
            keys[i].eff_area       = effective_area(pairs[i]);
        }

        int order[kMaxLayers];
        for (int i = 0; i < resolved_count; ++i) order[i] = i;
        std::stable_sort(order, order + resolved_count,
            [&](int a, int b) {
                if (keys[a].is_compositor != keys[b].is_compositor) {
                    return !keys[a].is_compositor;
                }
                return keys[a].eff_area > keys[b].eff_area;
            });

        bool needs_reorder = false;
        for (int i = 0; i < resolved_count; ++i) {
            if (order[i] != i) { needs_reorder = true; break; }
        }
        if (needs_reorder) {
            Dx11Renderer::DirectModeLayerPair sorted_pairs[kMaxLayers]{};
            SubmitLayerPerEye_t               sorted_local[kMaxLayers][2]{};
            for (int i = 0; i < resolved_count; ++i) {
                sorted_pairs[i]    = std::move(pairs[order[i]]);
                sorted_local[i][0] = local[order[i]][0];
                sorted_local[i][1] = local[order[i]][1];
            }
            for (int i = 0; i < resolved_count; ++i) {
                pairs[i]    = std::move(sorted_pairs[i]);
                local[i][0] = sorted_local[i][0];
                local[i][1] = sorted_local[i][1];
            }
        }
    }

    // Per-layer log: emitted once per change in resolved layer count.
    // Useful when adding a new mod/game combo — pid + process name lets
    // us sanity-check the post-reorder order (game-pid layer should be
    // at index 0, vrcompositor-pid layers should follow).
    if (resolved_count != last_logged_layer_dim_count_) {
        last_logged_layer_dim_count_ = resolved_count;
        for (int i = 0; i < resolved_count; ++i) {
            D3D11_TEXTURE2D_DESC ld{}, rd{};
            pairs[i].left->GetDesc(&ld);
            pairs[i].right->GetDesc(&rd);

            const uint32_t pid = pairs[i].pid;
            auto it = pid_name_cache_.find(pid);
            if (it == pid_name_cache_.end()) {
                it = pid_name_cache_.emplace(pid, GetProcessName(pid)).first;
            }
            const std::string& pname = it->second;

            LOG() << "DirectModeComponent: layer " << i
                  << " pid=" << pid << "(" << pname.c_str() << ")"
                  << " dims L=" << ld.Width << "x" << ld.Height
                  << " R=" << rd.Width << "x" << rd.Height
                  << " fmt=" << ld.Format
                  << " bounds L=(" << local[i][0].bounds.uMin << "," << local[i][0].bounds.vMin
                  << "," << local[i][0].bounds.uMax << "," << local[i][0].bounds.vMax << ")";
        }
    }

    if (resolved_count == 0) {
        if (handle_miss_count_ == 1 || (handle_miss_count_ % 60) == 0) {
            LOG() << "DirectModeComponent::Present: no submitted layers resolved "
                     "to handle_map_ entries (misses=" << handle_miss_count_ << ")";
        }
        return;
    }
    if (handle_miss_count_ > 0) {
        LOG() << "DirectModeComponent::Present: handle lookups recovered after "
              << handle_miss_count_ << " misses";
        handle_miss_count_ = 0;
    }

    renderer_->OnDirectModeFrame(pairs, resolved_count, syncTexture);
}
