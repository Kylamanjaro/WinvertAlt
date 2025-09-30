#include "pch.h"
#include "Log.h"
#include "DuplicationThread.h"
#include "Subscription.h"
#include "EffectWindow.h"
#include <wrl.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>

using Microsoft::WRL::ComPtr;

namespace {
// ---- Diagnostics: probe a small patch in the texture for non-zero pixels ----
static RECT CenterRect(UINT texW, UINT texH, UINT sampleW, UINT sampleH)
{
    RECT r{};
    const LONG w = static_cast<LONG>(std::min(sampleW, texW));
    const LONG h = static_cast<LONG>(std::min(sampleH, texH));
    r.left   = static_cast<LONG>((texW - w) / 2);
    r.top    = static_cast<LONG>((texH - h) / 2);
    r.right  = r.left + w;
    r.bottom = r.top  + h;
    return r;
}

static bool ProbeTexturePatch(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* src,
    const RECT& sampleRect,
    uint64_t& outSum,
    uint8_t& outMin,
    uint8_t& outMax)
{
    if (!device || !ctx || !src) return false;

    D3D11_TEXTURE2D_DESC td{};
    src->GetDesc(&td);

    RECT r = sampleRect;
    LONG texW = static_cast<LONG>(td.Width);
    LONG texH = static_cast<LONG>(td.Height);
    r.left   = std::clamp<LONG>(r.left,   0, texW);
    r.top    = std::clamp<LONG>(r.top,    0, texH);
    r.right  = std::clamp<LONG>(r.right,  0, texW);
    r.bottom = std::clamp<LONG>(r.bottom, 0, texH);
    if (r.right <= r.left || r.bottom <= r.top) return false;

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width              = r.right - r.left;
    sd.Height             = r.bottom - r.top;
    sd.MipLevels          = 1;
    sd.ArraySize          = 1;
    sd.Format             = td.Format;
    sd.SampleDesc.Count   = 1;
    sd.Usage              = D3D11_USAGE_STAGING;
    sd.BindFlags          = 0;
    sd.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags          = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr) || !staging) return false;

    D3D11_BOX srcBox{};
    srcBox.left   = r.left;
    srcBox.top    = r.top;
    srcBox.right  = r.right;
    srcBox.bottom = r.bottom;
    srcBox.front  = 0;
    srcBox.back   = 1;

    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, src, 0, &srcBox);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData);
    const UINT pitch = mapped.RowPitch;
    const UINT w = sd.Width;
    const UINT h = sd.Height;

    uint64_t sum = 0;
    uint8_t vmin = 255, vmax = 0;

    for (UINT y = 0; y < h; ++y) {
        const uint8_t* px = row + y * pitch;
        for (UINT x = 0; x < w; ++x) {
            const uint8_t b = px[x * 4 + 0];
            const uint8_t g = px[x * 4 + 1];
            const uint8_t rch = px[x * 4 + 2];
            const uint8_t a = px[x * 4 + 3];
            sum += b + g + rch + a;
            vmin = std::min<uint8_t>(vmin, std::min(std::min(b,g), std::min(rch,a)));
            vmax = std::max<uint8_t>(vmax, std::max(std::max(b,g), std::max(rch,a)));
        }
    }

    ctx->Unmap(staging.Get(), 0);
    outSum = sum; outMin = vmin; outMax = vmax;
    return true;
}
} // namespace

// ===== DuplicationThread (implementation aligned with DuplicationThread.h) =====

DuplicationThread::DuplicationThread(IDXGIAdapter1* adapter, IDXGIOutput1* output)
    : m_output(output)
{
    // Cache output rect
    DXGI_OUTPUT_DESC outDesc{};
    if (m_output) {
        if (SUCCEEDED(m_output->GetDesc(&outDesc))) {
            m_outputRect = outDesc.DesktopCoordinates;
            winvert4::Logf("DT: ctor output rect=(%ld,%ld,%ld,%ld)",
                m_outputRect.left, m_outputRect.top, m_outputRect.right, m_outputRect.bottom);
        }
    }

    // Create device/context on this adapter
    D3D_FEATURE_LEVEL flOut{};
    const D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   flags, fls, _countof(fls),
                                   D3D11_SDK_VERSION, &m_device, &flOut, &m_context);
    if (FAILED(hr)) {
        winvert4::Logf("DT: device create FAILED hr=0x%08X", hr);
        return;
    }
    winvert4::Logf("DT: device/context created featureLevel=0x%X", flOut);
}

DuplicationThread::~DuplicationThread()
{
    Stop();
}

void DuplicationThread::Run()
{
    if (m_isRunning.exchange(true)) return;
    m_thread = std::thread(&DuplicationThread::ThreadProc, this);
}

void DuplicationThread::Stop()
{
    if (!m_isRunning.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(m_subMutex);
        m_subCv.notify_all();
    }
    if (m_thread.joinable()) m_thread.join();
    m_duplication.Reset();
    m_fullTexture.Reset();
    m_context.Reset();
    m_device.Reset();
}

void DuplicationThread::AddSubscription(const Subscription& sub)
{
    std::lock_guard<std::mutex> lk(m_subMutex);
    m_subscriptions.push_back(sub);
    winvert4::Logf("DT: AddSubscription sub=%p rect=(%ld,%ld,%ld,%ld) count=%zu",
        sub.Subscriber, sub.Region.left, sub.Region.top, sub.Region.right, sub.Region.bottom, m_subscriptions.size());
    m_subCv.notify_all();
}

void DuplicationThread::RemoveSubscriber(ISubscriber* sub)
{
    std::lock_guard<std::mutex> lk(m_subMutex);
    m_subscriptions.erase(
        std::remove_if(m_subscriptions.begin(), m_subscriptions.end(),
            [sub](const Subscription& s) { return s.Subscriber == sub; }),
        m_subscriptions.end());
}

void DuplicationThread::RequestRedraw()
{
    // Try to present effects immediately for ~32 cycles (~32ms at 1ms wait)
    m_redrawCountdown.store(32, std::memory_order_relaxed);
}

void DuplicationThread::ThreadProc()
{
    winvert4::Log("DT: waiting for subscribers");
    {
        std::unique_lock<std::mutex> lk(m_subMutex);
        m_subCv.wait(lk, [this] { return !m_isRunning || !m_subscriptions.empty(); });
        if (!m_isRunning) return;
        winvert4::Logf("DT: subscribers available: %zu", m_subscriptions.size());
    }

    if (!m_output || !m_device) return;

    // Create duplication
    ComPtr<IDXGIOutputDuplication> dupl;
    HRESULT hr = m_output->DuplicateOutput(m_device.Get(), &dupl);
    if (FAILED(hr)) {
        winvert4::Logf("DT: DuplicateOutput failed hr=0x%08X", hr);
        return;
    }
    m_duplication = dupl;

    // Describe duplication
    DXGI_OUTDUPL_DESC dd{};
    m_duplication->GetDesc(&dd);
    winvert4::Logf("DT: desc: Mode=%ux%u fmt=%u Rot=%u",
        dd.ModeDesc.Width, dd.ModeDesc.Height, dd.ModeDesc.Format, dd.Rotation);

    // Create full-frame texture to copy into
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = dd.ModeDesc.Width;
    texDesc.Height = dd.ModeDesc.Height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = dd.ModeDesc.Format; // usually BGRA8 (87)
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;
    m_device->CreateTexture2D(&texDesc, nullptr, &m_fullTexture);
    if (m_fullTexture) {
        winvert4::Logf("DT: full-frame texture created %ux%u", texDesc.Width, texDesc.Height);
    } else {
        winvert4::Log("DT: full-frame texture creation FAILED");
    }

    // Frame loop
    while (m_isRunning) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        const UINT waitMs = (m_redrawCountdown.load(std::memory_order_relaxed) > 0) ? 1u : 16u;
        HRESULT hrAcq = m_duplication->AcquireNextFrame(waitMs, &fi, &res);
        if (hrAcq == DXGI_ERROR_WAIT_TIMEOUT) {
            // No new desktop frame. If a redraw was requested (settings changed),
            // render using the last captured texture so changes appear immediately.
            int cnt = m_redrawCountdown.load(std::memory_order_relaxed);
            if (cnt > 0 && m_fullTexture) {
                winvert4::Logf("DT: timeout; redraw cnt=%d using last texture", cnt);
                std::vector<Subscription> subsCopy;
                {
                    std::lock_guard<std::mutex> lk(m_subMutex);
                    subsCopy = m_subscriptions;
                }
                winvert4::Logf("DT: timeout; rendering to %zu subscribers", subsCopy.size());
                for (auto& s : subsCopy) {
                    if (s.Subscriber) {
                        winvert4::Logf("DT: timeout; Render -> %p", s.Subscriber);
                        static_cast<EffectWindow*>(s.Subscriber)->Render(m_fullTexture.Get(), 0ULL);
                    }
                }
                m_redrawCountdown.store(cnt - 1, std::memory_order_relaxed);
            } else if (cnt > 0 && !m_fullTexture) {
                winvert4::Log("DT: timeout; requested redraw but no fullTexture yet");
            }
            continue;
        }
        if (FAILED(hrAcq)) {
            winvert4::Logf("DT: AcquireNextFrame failed hr=0x%08X", hrAcq);
            if (hrAcq == DXGI_ERROR_ACCESS_LOST) break;
            continue;
        }

        ComPtr<ID3D11Texture2D> frameTex;
        res.As(&frameTex);

        if (frameTex && m_fullTexture) {
            // Copy frame into our shared texture
            winvert4::Log("DT: frame acquired; copying to full texture");
            m_context->CopyResource(m_fullTexture.Get(), frameTex.Get());
            // Always notify subscribers so effect changes present even if captured pixels are unchanged.
        }

        // Render to all subscribers on this thread
        if (m_fullTexture) {
            std::vector<Subscription> subsCopy;
            {
                std::lock_guard<std::mutex> lk(m_subMutex);
                subsCopy = m_subscriptions;
            }
            winvert4::Logf("DT: rendering to %zu subscribers; LastPresentQpc=%llu", subsCopy.size(), fi.LastPresentTime.QuadPart);
            for (auto& s : subsCopy) {
                if (s.Subscriber) {
                    // Cast and call the new Render method
                    winvert4::Logf("DT: Render -> %p", s.Subscriber);
                    static_cast<EffectWindow*>(s.Subscriber)->Render(m_fullTexture.Get(), fi.LastPresentTime.QuadPart);
                }
            }
            m_redrawCountdown.store(0, std::memory_order_relaxed);
        }

        m_duplication->ReleaseFrame();
    }
}
