#include "pch.h"
#include "Log.h"
#include "EffectWindow.h"
#include "App.xaml.h"
#include "OutputManager.h"
#include <dwmapi.h>
#include <d3dcompiler.h>

using ::Microsoft::WRL::ComPtr;

namespace {
    constexpr wchar_t kEffectWndClass[] = L"Winvert4_EffectWindow";

    static const float kFSVerts[6] = { -1.f,-1.f,  -1.f,3.f,  3.f,-1.f };

    // Vertex shader: pass UV with scale/offset
    static const char* kVS = R"(
        struct VSIn  { float2 pos : POSITION; };
        struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
        cbuffer CB0 : register(b0) { float2 scale; float2 offset; };
        VSOut main(VSIn i) {
          VSOut o; o.pos = float4(i.pos,0,1);
          float2 suv=(i.pos*float2(0.5,-0.5))+float2(0.5,0.5);
          o.uv = suv*scale + offset; return o;
        })";

    // Pixel shader: sample and apply effects; force alpha 1 for opaque overlay
    static const char* kPS = R"(
        Texture2D srcTex : register(t0);
        SamplerState samp0 : register(s0);
        cbuffer PixelCB : register(b1) {
            uint enableInvert;
            uint enableGrayscale;
            uint enableMatrix;
            uint enableColorMap;
            row_major float4x4 colorMat;
            float4   colorOffset;
            uint colorMapCount;
            float3 _pad1;
            float4 colorMapSrc[64];
            float4 colorMapDst[64];
        };

        struct PSIn { float4 pos:SV_Position; float2 uv:TEXCOORD0; };

        // Utility: luminance helper
        float Luma(float3 x) { return dot(x, float3(0.299, 0.587, 0.114)); }

        float4 main(PSIn i) : SV_Target {
          float4 c = srcTex.Sample(samp0, i.uv);
          float3 result = c.rgb;
          if (enableInvert) { result = 1.0 - result; }
          if (enableGrayscale) {
              result = dot(result, float3(0.299, 0.587, 0.114));
          }
          if (enableMatrix != 0) { float4 cr = mul(float4(result,1.0), colorMat); result = cr.rgb + colorOffset.rgb; }
          if (enableColorMap != 0 && colorMapCount > 0) {
              float3 rgb = result;
              float maxW = 0.0;
              float3 best = rgb;
              [loop]
              for (uint i = 0; i < colorMapCount; ++i) {
                  float3 src = colorMapSrc[i].xyz;
                  float t2 = colorMapSrc[i].w;              // tolerance^2 in RGB space
                  float3 d = rgb - src;
                  float dist2 = dot(d,d);
                  if (dist2 <= t2 && t2 > 1e-12) {
                      // Softly steer nearby colors toward the destination color, preserving original luminance
                      float3 dst = colorMapDst[i].xyz;
                      float lOrig = Luma(rgb);
                      float lDst  = max(Luma(dst), 1e-5);
                      float3 mapped = dst * (lOrig / lDst);
                      // Blend weight with quadratic falloff without sqrt for performance
                      float w = saturate(1.0 - dist2 / t2);
                      // Slightly sharpen the falloff
                      w = w * w;
                      if (w > maxW) { maxW = w; best = lerp(rgb, saturate(mapped), w); }
                  }
              }
              if (maxW > 0.0) result = best;
          }
          return float4(result, 1.0);
        })";
}

LRESULT CALLBACK EffectWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Make the window completely transparent to mouse clicks
    if (msg == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

EffectWindow::EffectWindow(RECT desktopRect, OutputManager* outputManager)
    : m_desktopRect(desktopRect)
    , m_outputManager(outputManager)
{
}

EffectWindow::~EffectWindow()
{
    Hide();
}

// ISubscriber implementation. This is now a no-op because the DuplicationThread
// calls Render() directly, but it's required to satisfy the interface.
void EffectWindow::OnFrameReady(ComPtr<ID3D11Texture2D>)
{
}

void EffectWindow::UpdateSettings(const EffectSettings& settings)
{
    m_settings = settings;
    // Request an immediate redraw so changes are visible without desktop activity
    if (m_thread) m_thread->RequestRedraw();
}

void EffectWindow::Show()
{
    winvert4::Log("EffectWindow::Show called");

    // 1) Resolve duplication thread and D3D device/context
    if (!m_outputManager) return;

    winvert4::Log("EW.Show calling GetThreadForRect");
    m_thread = m_outputManager->GetThreadForRect(m_desktopRect);
    winvert4::Logf("EW.Show thread=%p", (void*)m_thread);
    if (!m_thread) return;

    m_d3d = m_thread->GetDevice();
    if (!m_d3d) { winvert4::Log("EW.Show: thread->GetDevice null"); return; }
    m_d3d->GetImmediateContext(&m_immediateCtx);
    if (!m_immediateCtx) { winvert4::Log("EW.Show: GetImmediateContext null"); return; }
    m_d3d->CreateDeferredContext(0, &m_deferredCtx);
    if (!m_deferredCtx) { winvert4::Log("EW.Show: CreateDeferredContext null"); return; }

    // Ensure multithread protection (same immediate context as duplication thread)
    {
        Microsoft::WRL::ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(m_immediateCtx.As(&mt)) && mt) {
            mt->SetMultithreadProtected(TRUE);
            winvert4::Log("EW.Show: ID3D11Multithread protection ENABLED on immediate context");
        }
    }

    // 2) Subscribe BEFORE any window exists
    {
        Subscription sub;
        sub.Subscriber = this;
        sub.Region     = m_desktopRect;
        m_thread->AddSubscription(sub);
        winvert4::Log("EW: subscribed to duplication thread");
        // Ask the duplication thread to render immediately so the window shows content without
        // waiting for desktop activity.
        m_thread->RequestRedraw();
        winvert4::Log("EW: requested redraw (pre-window)");
    }

    // 3) Create the window and pipeline now.
    CreateAndShow();
    winvert4::Log("EW: CreateAndShow returned");
    m_isHidden = false;

    // We requested a redraw before creating the window, but the duplication
    // thread may have attempted to render while our swapchain wasn't ready yet
    // (m_run=false / m_swapChain=null) and thus returned early. Request one
    // more redraw now that the window is fully initialized so the first frame
    // presents without waiting for desktop activity.
    if (m_thread) { winvert4::Log("EW: requested redraw (post-window)"); m_thread->RequestRedraw(); }
}

void EffectWindow::Hide()
{
    m_run = false;
    m_cb.Reset();
    m_vb.Reset();
    m_il.Reset();
    m_ps.Reset();
    m_vs.Reset();
    m_rtv.Reset();
    m_swapChain.Reset();
    m_factory.Reset();
    m_deferredCtx.Reset();
    m_immediateCtx.Reset();
    m_d3d.Reset();

    // Unsubscribe
    if (m_thread) m_thread->RemoveSubscriber(this);

    // Destroy window
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
}

void EffectWindow::SetHidden(bool hidden)
{
    m_isHidden = hidden;
    if (m_hwnd)
    {
        ::ShowWindow(m_hwnd, hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
    }
}

void EffectWindow::EnsureSRVLocked_(ID3D11Texture2D* currentTex)
{
    if (!currentTex) return;

    if (!m_srv || m_srvSourceRaw != currentTex) {
        m_srv.Reset();
        HRESULT hr = m_d3d->CreateShaderResourceView(currentTex, nullptr, &m_srv);
        if (SUCCEEDED(hr)) {
            m_srvSourceRaw = currentTex;
            winvert4::Log("EW: SRV (re)created for new frame texture");
        } else {
            winvert4::Logf("EW: CreateShaderResourceView failed hr=0x%08X", hr);
        }
    }
}

void EffectWindow::UpdateCBs_()
{
    RECT outRect = m_thread->GetOutputRect();
    const float outW = float(outRect.right - outRect.left);
    const float outH = float(outRect.bottom - outRect.top);

    const float selL = float(m_desktopRect.left   - outRect.left);
    const float selT = float(m_desktopRect.top    - outRect.top);
    const float selW = float(m_desktopRect.right  - m_desktopRect.left);
    const float selH = float(m_desktopRect.bottom - m_desktopRect.top);

    // Update Vertex Shader CB
    VertexCB vcb{};
    vcb.scale[0]  = (outW > 0) ? (selW / outW) : 0.0f;
    vcb.scale[1]  = (outH > 0) ? (selH / outH) : 0.0f;
    vcb.offset[0] = (outW > 0) ? (selL / outW) : 0.0f;
    vcb.offset[1] = (outH > 0) ? (selT / outH) : 0.0f;
    m_deferredCtx->UpdateSubresource(m_cb.Get(), 0, nullptr, &vcb, 0, 0);

    // Update Pixel Shader CB
    PixelCB pcb{};
    bool inv = m_settings.isBrightnessProtectionEnabled ? m_effectiveInvert : m_settings.isInvertEffectEnabled;
    pcb.enableInvert     = inv ? 1u : 0u;
    pcb.enableGrayscale  = m_settings.isGrayscaleEffectEnabled ? 1u : 0u;
    pcb.enableMatrix     = m_settings.isCustomEffectActive ? 1u : 0u;
    pcb.enableColorMap   = (m_settings.isColorMappingEnabled && !m_settings.colorMaps.empty()) ? 1u : 0u;
    if (m_settings.isCustomEffectActive)
    {
        memcpy(pcb.colorMat,    m_settings.colorMat,    sizeof(pcb.colorMat));
        memcpy(pcb.colorOffset, m_settings.colorOffset, sizeof(pcb.colorOffset));
    }
    else
    {
        const float ident[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 };
        memset(pcb.colorOffset, 0, sizeof(pcb.colorOffset));
        memcpy(pcb.colorMat, ident, sizeof(ident));
    }
    // Fill color map list
    uint32_t count = 0;
    if (pcb.enableColorMap)
    {
        // Copy only enabled mappings to the GPU list to honor per-row toggles
        for (size_t i = 0; i < m_settings.colorMaps.size() && count < EffectWindow::kMaxColorMaps; ++i)
        {
            const auto& e = m_settings.colorMaps[i];
            if (!e.enabled) continue;
            float sr = e.srcR / 255.0f, sg = e.srcG / 255.0f, sb = e.srcB / 255.0f;
            float dr = e.dstR / 255.0f, dg = e.dstG / 255.0f, db = e.dstB / 255.0f;
            float t  = std::max(0, std::min(255, e.tolerance)) / 255.0f;
            float t2 = t * t; // store squared radius to avoid sqrt in shader
            pcb.colorMapSrc[count][0] = sr; pcb.colorMapSrc[count][1] = sg; pcb.colorMapSrc[count][2] = sb; pcb.colorMapSrc[count][3] = t2;
            pcb.colorMapDst[count][0] = dr; pcb.colorMapDst[count][1] = dg; pcb.colorMapDst[count][2] = db; pcb.colorMapDst[count][3] = 0.0f;
            ++count;
        }
    }
    pcb.colorMapCount = count;
    m_deferredCtx->UpdateSubresource(m_pixelCb.Get(), 0, nullptr, &pcb, 0, 0);

    winvert4::Logf("EW: CB scale=(%.3f,%.3f) offset=(%.3f,%.3f)", vcb.scale[0], vcb.scale[1], vcb.offset[0], vcb.offset[1]);
}

void EffectWindow::CreateAndShow()
{
    winvert4::Log("EW.CreateAndShow: begin");
    // 1) Create window and pipeline objects on this thread
    static ATOM s_atom = 0;
    if (!s_atom) {
        WNDCLASSEXW wcex{ sizeof(WNDCLASSEXW) };
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = &EffectWindow::WndProc;
        wcex.hInstance = GetModuleHandleW(nullptr);
        wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wcex.lpszClassName = kEffectWndClass;
        s_atom = RegisterClassExW(&wcex);
    }

    const int w = m_desktopRect.right - m_desktopRect.left;
    const int h = m_desktopRect.bottom - m_desktopRect.top;
    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, // pass-through input
        kEffectWndClass, L"", WS_POPUP,
        m_desktopRect.left, m_desktopRect.top, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd) { winvert4::Log("EffectWindow::RenderThreadProc failed to create HWND"); return; }
    winvert4::Logf("EW: created HWND=%p size=(%dx%d)", (void*)m_hwnd, w, h);

    // Make the window click-through via HTTRANSPARENT in WndProc.
    // Use LWA_ALPHA (opaque) instead of COLORKEY to avoid the entire window
    // becoming fully transparent when the first clear is black.
    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);

    // Factory
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_d3d.As(&dxgiDevice))) return;
    ComPtr<IDXGIAdapter> dxgiAdapter;
    if (FAILED(dxgiDevice->GetAdapter(&dxgiAdapter))) return;
    if (FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&m_factory)))) return;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    HRESULT hrSC = m_factory->CreateSwapChainForHwnd(m_d3d.Get(), m_hwnd, &sd, nullptr, nullptr, &m_swapChain);
    if (FAILED(hrSC) || !m_swapChain) {
        winvert4::Logf("EW: CreateSwapChainForHwnd failed hr=0x%08X", hrSC);
        return;
    }
    winvert4::Log("EW: swapchain created");

    // Initialize D2D/DirectWrite for FPS overlay
    if (!m_d2dFactory)
    {
        D2D1_FACTORY_OPTIONS opts{};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts, reinterpret_cast<void**>(m_d2dFactory.ReleaseAndGetAddressOf()));
    }
    if (!m_dwriteFactory)
    {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.ReleaseAndGetAddressOf()));
    }
    if (!m_d2dDevice)
    {
        m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
    }
    if (!m_d2dCtx)
    {
        m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dCtx);
        if (m_d2dCtx)
        {
            m_d2dCtx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            if (!m_textBrush)
            {
                m_d2dCtx->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.85f), &m_textBrush);
            }
        }
    }
    if (!m_textFormat && m_dwriteFactory)
    {
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_textFormat);
        if (m_textFormat)
        {
            m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    }

    // Init FPS timer (frequency for converting duplication QPC to seconds)
    QueryPerformanceFrequency(&m_qpcFreq);

    // Create GPU timer queries (ring to avoid stalls)
    D3D11_QUERY_DESC qd{}; qd.MiscFlags = 0;
    for (int i = 0; i < 8; ++i)
    {
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; m_d3d->CreateQuery(&qd, &m_gpuTimer[i].disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;          m_d3d->CreateQuery(&qd, &m_gpuTimer[i].start);
        qd.Query = D3D11_QUERY_TIMESTAMP;          m_d3d->CreateQuery(&qd, &m_gpuTimer[i].end);
        m_gpuTimer[i].inFlight = false;
    }

    // Shaders
    ComPtr<ID3DBlob> vsb, psb, err;
    HRESULT hrVS = D3DCompile(kVS, (UINT)strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hrVS) || !vsb) {
        const char* emsg = err ? (const char*)err->GetBufferPointer() : "";
        winvert4::Logf("EW: VS compile failed 0x%08X %s", hrVS, emsg);
        return;
    }
    HRESULT hrPS = D3DCompile(kPS, (UINT)strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hrPS) || !psb) {
        const char* emsg = err ? (const char*)err->GetBufferPointer() : "";
        winvert4::Logf("EW: PS compile failed 0x%08X %s", hrPS, emsg);
        return;
    }

    if (FAILED(m_d3d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs))) return;
    if (FAILED(m_d3d->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps))) return;

    D3D11_INPUT_ELEMENT_DESC ied{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (FAILED(m_d3d->CreateInputLayout(&ied, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &m_il))) return;

    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(kFSVerts); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = kFSVerts;
    if (FAILED(m_d3d->CreateBuffer(&bd, &srd, &m_vb))) return;

    D3D11_BUFFER_DESC vscbd{}; vscbd.ByteWidth = sizeof(VertexCB); vscbd.Usage = D3D11_USAGE_DEFAULT; vscbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_d3d->CreateBuffer(&vscbd, nullptr, &m_cb))) return;

    D3D11_BUFFER_DESC pscbd{}; pscbd.ByteWidth = sizeof(PixelCB); pscbd.Usage = D3D11_USAGE_DEFAULT; pscbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_d3d->CreateBuffer(&pscbd, nullptr, &m_pixelCb))) return;

    D3D11_SAMPLER_DESC sampd{}; sampd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampd.AddressU = sampd.AddressV = sampd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(m_d3d->CreateSamplerState(&sampd, &m_samp))) return;

    // Precreate RTV (render loop also recreates per-frame for robustness)
    ComPtr<ID3D11Texture2D> backBuf;
    if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf)))) {
        m_d3d->CreateRenderTargetView(backBuf.Get(), nullptr, &m_rtv);
    }

    // Exclude from capture (you mentioned you need this)
    // This is the call that was failing. Now it's on the same thread that created the HWND.
    SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE);
    {
        BOOL exclude = TRUE;
        HRESULT hr = DwmSetWindowAttribute(m_hwnd, DWMWA_EXCLUDED_FROM_CAPTURE, &exclude, sizeof(exclude));
        if (FAILED(hr)) {
            winvert4::Logf("EW: DwmSetWindowAttribute failed hr=0x%08X", hr);
        }
    }

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    winvert4::Log("EW: ShowWindow(SW_SHOWNOACTIVATE)");

    m_run = true;

    // Immediately present a cleared frame so the window becomes visible right away,
    // even before the first duplication frame arrives. Use opaque alpha to avoid
    // the layered window being fully transparent on the first Present.
    {
        ComPtr<ID3D11Texture2D> bb;
        if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb)
        {
            ComPtr<ID3D11RenderTargetView> rtv;
            if (SUCCEEDED(m_d3d->CreateRenderTargetView(bb.Get(), nullptr, &rtv)))
            {
                const float clear[4] = { 0,0,0,1 };
                m_immediateCtx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
                m_immediateCtx->ClearRenderTargetView(rtv.Get(), clear);
                // Present immediately without waiting for vblank on the very first frame
                winvert4::Log("EW: first clear + Present(0,0)");
                m_swapChain->Present(0, 0);
            }
        }
    }

    // Prepare luminance mip resources for brightness protection
    {
        const int w = m_desktopRect.right - m_desktopRect.left;
        const int h = m_desktopRect.bottom - m_desktopRect.top;
        UINT maxDim = (UINT)(w > h ? w : h);
        UINT mips = 1; while ((1u << mips) < maxDim) ++mips; // ceil(log2(maxDim))
        m_mipLastLevel = mips - 1;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = mips; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        m_d3d->CreateTexture2D(&td, nullptr, &m_mipTex);
        if (m_mipTex)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = td.Format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = td.MipLevels;
            m_d3d->CreateShaderResourceView(m_mipTex.Get(), &sd, &m_mipSrv);
        }

        // 1x1 staging readback
        D3D11_TEXTURE2D_DESC rd{};
        rd.Width = 1; rd.Height = 1; rd.MipLevels = 1; rd.ArraySize = 1;
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Usage = D3D11_USAGE_STAGING;
        rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        m_d3d->CreateTexture2D(&rd, nullptr, &m_mipReadback1x1);
    }
    winvert4::Log("EW.CreateAndShow: end");
}

void EffectWindow::Render(ID3D11Texture2D* frame, unsigned long long lastPresentQpc)
{
    if (!m_run) { winvert4::Log("EW.Render early exit: m_run=false"); return; }
    if (m_isHidden) { winvert4::Log("EW.Render early exit: hidden"); return; }
    if (!frame) { winvert4::Log("EW.Render early exit: frame=null"); return; }
    if (!m_swapChain) { winvert4::Log("EW.Render early exit: swapchain=null"); return; }

    // Ensure SRV reflects current texture
    EnsureSRVLocked_(frame);
    if (!m_srv) { winvert4::Log("EW.Render early exit: SRV null"); return; }

    // Brightness protection: compute average luminance of the region via mipmap reduction
    if (m_settings.isBrightnessProtectionEnabled && m_mipTex && m_mipSrv && m_mipReadback1x1)
    {
        RECT outRect = m_thread->GetOutputRect();
        D3D11_BOX box{};
        box.left = (UINT)(m_desktopRect.left - outRect.left);
        box.top  = (UINT)(m_desktopRect.top  - outRect.top);
        box.right  = (UINT)(box.left + (m_desktopRect.right - m_desktopRect.left));
        box.bottom = (UINT)(box.top  + (m_desktopRect.bottom - m_desktopRect.top));
        box.front = 0; box.back = 1;
        m_immediateCtx->CopySubresourceRegion(m_mipTex.Get(), 0, 0, 0, 0, frame, 0, &box);
        m_immediateCtx->GenerateMips(m_mipSrv.Get());

        // Copy last mip to 1x1 staging and read CPU-side
        m_immediateCtx->CopySubresourceRegion(m_mipReadback1x1.Get(), 0, 0, 0, 0, m_mipTex.Get(), m_mipLastLevel, nullptr);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(m_immediateCtx->Map(m_mipReadback1x1.Get(), 0, D3D11_MAP_READ, 0, &map)))
        {
            const uint8_t* px = static_cast<const uint8_t*>(map.pData);
            float b = px[0] / 255.0f; float g = px[1] / 255.0f; float r = px[2] / 255.0f;
            // Match luma weights used elsewhere
            m_avgLuma = r * 0.299f + g * 0.587f + b * 0.114f;
            m_immediateCtx->Unmap(m_mipReadback1x1.Get(), 0);

            // Choose effective invert to minimize luminance w.r.t threshold
            float thr = (m_settings.brightnessThreshold / 255.0f);
            // If above threshold, prefer inverted; else prefer original
            bool invertPref = (m_avgLuma > thr);
            m_effectiveInvert = invertPref;
        }
    }
    else
    {
        m_effectiveInvert = m_settings.isInvertEffectEnabled;
    }

    // Recreate RTV each frame (resize-robust)
    ComPtr<ID3D11Texture2D> backBuf;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf)))) {
        winvert4::Log("EW.Render: GetBuffer failed; Present passthrough");
        m_swapChain->Present(1, 0);
        return;
    }
    ComPtr<ID3D11RenderTargetView> localRTV;
    m_d3d->CreateRenderTargetView(backBuf.Get(), nullptr, &localRTV);

    // Use the deferred context to build commands
    m_deferredCtx->ClearState();

    // Bind pipeline
    UINT stride = sizeof(float) * 2, offset = 0;
    ID3D11Buffer* vb = m_vb.Get();
    m_deferredCtx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    m_deferredCtx->IASetInputLayout(m_il.Get());
    m_deferredCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set viewport to window size to be explicit
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = static_cast<FLOAT>(m_desktopRect.right - m_desktopRect.left);
    vp.Height = static_cast<FLOAT>(m_desktopRect.bottom - m_desktopRect.top);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    m_deferredCtx->RSSetViewports(1, &vp);

    m_deferredCtx->VSSetShader(m_vs.Get(), nullptr, 0);
    ID3D11Buffer* vscb = m_cb.Get();
    m_deferredCtx->VSSetConstantBuffers(0, 1, &vscb);

    m_deferredCtx->PSSetShader(m_ps.Get(), nullptr, 0);
    ID3D11Buffer* pscb = m_pixelCb.Get();
    m_deferredCtx->PSSetConstantBuffers(1, 1, &pscb);

    ID3D11SamplerState* ss = m_samp.Get();
    m_deferredCtx->PSSetSamplers(0, 1, &ss);
    ID3D11ShaderResourceView* srv = m_srv.Get();
    m_deferredCtx->PSSetShaderResources(0, 1, &srv);

    ID3D11RenderTargetView* rtv = localRTV.Get();
    m_deferredCtx->OMSetRenderTargets(1, &rtv, nullptr);

    // Update constants & draw (invert handled in pixel shader)
    UpdateCBs_();

    const float clear[4] = { 0,0,0,0 };
    m_deferredCtx->ClearRenderTargetView(localRTV.Get(), clear);
    m_deferredCtx->Draw(3, 0);

    // Unbind SRV to avoid hazards if source updates immediately
    ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    m_deferredCtx->PSSetShaderResources(0, 1, nullSRV);

    // Execute commands on the immediate context
    ComPtr<ID3D11CommandList> commandList;
    if (SUCCEEDED(m_deferredCtx->FinishCommandList(FALSE, &commandList))) {
        // GPU timer begin (immediate context)
        GpuTimerSlot& slot = m_gpuTimer[m_gpuTimerIndex];
        if (slot.inFlight)
        {
            // Avoid reusing queries whose results haven't been collected yet
            // We'll try to resolve them after Present below.
        }
        if (!slot.inFlight && slot.disjoint && slot.start && slot.end)
        {
            m_immediateCtx->Begin(slot.disjoint.Get());
            m_immediateCtx->End(slot.start.Get());
        }

        // Execute draw commands
        m_immediateCtx->ExecuteCommandList(commandList.Get(), FALSE);

        // GPU timer end
        if (!slot.inFlight && slot.disjoint && slot.start && slot.end)
        {
            m_immediateCtx->End(slot.end.Get());
            m_immediateCtx->End(slot.disjoint.Get());
            slot.inFlight = true;
            m_gpuTimerIndex = (m_gpuTimerIndex + 1) % 8;
        }
    }

    // Optional FPS overlay (top-right)
    if (m_settings.showFpsOverlay && m_d2dCtx && m_textFormat && m_textBrush)
    {
        // Obtain current back buffer as DXGI surface and set as D2D target
        ComPtr<ID3D11Texture2D> backBuf;
        if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf))) && backBuf)
        {
            ComPtr<IDXGISurface> surf;
            if (SUCCEEDED(backBuf.As(&surf)))
            {
                D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                ::Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBmp;
                if (SUCCEEDED(m_d2dCtx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &targetBmp)))
                {
                    m_d2dCtx->SetTarget(targetBmp.Get());
                    m_d2dCtx->BeginDraw();

                    const float w = static_cast<float>(m_desktopRect.right - m_desktopRect.left);
                    // Two-line compact overlay: source Hz and our processing time
                    D2D1_RECT_F rect1 = D2D1::RectF(w - 140.f, 4.f,  w - 6.f, 24.f);
                    D2D1_RECT_F rect2 = D2D1::RectF(w - 140.f, 24.f, w - 6.f, 44.f);

                    wchar_t line1[64]; swprintf_s(line1, L"Src %.0f Hz", m_fps);
                    wchar_t line2[64]; swprintf_s(line2, L"Draw %.1f ms (%.0f)", m_gpuMsLast, m_procFps);
                    m_d2dCtx->DrawTextW(line1, (UINT32)wcslen(line1), m_textFormat.Get(), rect1, m_textBrush.Get());
                    m_d2dCtx->DrawTextW(line2, (UINT32)wcslen(line2), m_textFormat.Get(), rect2, m_textBrush.Get());

                    // Optional: subtle background for readability
                    // Could draw with another brush under text if desired

                    HRESULT hr = m_d2dCtx->EndDraw();
                    (void)hr;
                }
            }
        }
    }

    // Present blocks to vblank when sync interval = 1 (vsynced to monitor)
    winvert4::Log("EW.Render: Present(1,0)");
    m_swapChain->Present(1, 0);

    // FPS calculation based on DXGI Desktop Duplication's LastPresentTime (QPC)
    // This reflects the monitor's present cadence, independent of our swapchain/window mode.
    if (lastPresentQpc != 0 && m_qpcFreq.QuadPart != 0)
    {
        if (m_prevDupPresentQpc != 0 && lastPresentQpc > m_prevDupPresentQpc)
        {
            const double dt = double(lastPresentQpc - m_prevDupPresentQpc) / double(m_qpcFreq.QuadPart);
            if (dt > 0)
            {
                m_fpsAccum += dt; m_fpsFrames += 1;
                if (m_fpsAccum >= 0.5)
                {
                    m_fps = static_cast<float>(m_fpsFrames / m_fpsAccum);
                    m_fpsAccum = 0.0; m_fpsFrames = 0;
                }
            }
        }
        m_prevDupPresentQpc = lastPresentQpc;
    }

    // Resolve GPU timers for any slots in flight (non-blocking)
    for (int i = 0; i < 8; ++i)
    {
        if (!m_gpuTimer[i].inFlight) continue;
        auto& t = m_gpuTimer[i];
        if (!t.disjoint || !t.start || !t.end) { t.inFlight = false; continue; }

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
        if (S_OK == m_immediateCtx->GetData(t.disjoint.Get(), &disjointData, sizeof(disjointData), 0))
        {
            if (!disjointData.Disjoint)
            {
                UINT64 tsStart = 0, tsEnd = 0;
                if (S_OK == m_immediateCtx->GetData(t.start.Get(), &tsStart, sizeof(tsStart), 0) &&
                    S_OK == m_immediateCtx->GetData(t.end.Get(),   &tsEnd,   sizeof(tsEnd),   0) &&
                    tsEnd > tsStart)
                {
                    const double gpuSec = double(tsEnd - tsStart) / double(disjointData.Frequency);
                    m_gpuMsLast = gpuSec * 1000.0;
                    if (m_gpuMsLast > 0.0) m_procFps = static_cast<float>(1000.0 / m_gpuMsLast);
                }
            }
            t.inFlight = false;
        }
    }
}



