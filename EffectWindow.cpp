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

    // Pixel shader: sample and invert RGB; force alpha 1 for opaque overlay
    static const char* kPS = R"(
        Texture2D srcTex : register(t0);
        SamplerState samp0 : register(s0);
        struct PSIn { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
        float4 main(PSIn i) : SV_Target {
          float4 c = srcTex.Sample(samp0, i.uv);
          float3 inv = 1.0 - c.rgb;
          return float4(inv, 1.0);
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

EffectWindow::EffectWindow(RECT desktopRect)
    : m_desktopRect(desktopRect)
{
}

EffectWindow::~EffectWindow()
{
    Hide();
}

// Accept either callback name
void EffectWindow::OnFrameReady(ComPtr<ID3D11Texture2D> fullFrameTexture)
{
    {
        std::lock_guard<std::mutex> lk(m_textureMutex);
        m_sharedTexture = fullFrameTexture;
    }
    m_textureCv.notify_all();
}
void EffectWindow::OnFrame(ComPtr<ID3D11Texture2D> fullFrameTexture)
{
    OnFrameReady(fullFrameTexture);
}

void EffectWindow::Show()
{
    winvert4::Log("EffectWindow::Show called");
    winvert4::Logf("EW.Show rect=(%ld,%ld,%ld,%ld)", m_desktopRect.left, m_desktopRect.top, m_desktopRect.right, m_desktopRect.bottom);

    // 1) Resolve duplication thread and D3D device/context
    auto app = winrt::Winvert4::implementation::App::Current();
    winvert4::Log(app ? "EW.Show got App::Current" : "EW.Show App::Current is null");
    if (!app) return;
    auto outputManager = app->GetOutputManager();
    winvert4::Log(outputManager ? "EW.Show got OutputManager" : "EW.Show GetOutputManager returned null");
    if (!outputManager) return;

    winvert4::Log("EW.Show calling GetThreadForRect");
    m_thread = outputManager->GetThreadForRect(m_desktopRect);
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
    }

    // 3) Wait for first frame (up to 750ms)
    {
        using namespace std::chrono_literals;
        std::unique_lock<std::mutex> lk(m_textureMutex);
        bool ok = m_textureCv.wait_for(lk, 750ms, [this] { return m_sharedTexture != nullptr; });
        if (ok) {
            m_lastTexture = m_sharedTexture;
            m_sharedTexture.Reset();
            winvert4::Log("EW: first frame received BEFORE window creation");
        } else {
            winvert4::Log("EW: first frame NOT ready within 750ms; proceeding");
        }
    }

    // 4) Start render thread. It will create the window and pipeline.
    m_run = true;
    m_renderThread = std::thread(&EffectWindow::RenderThreadProc, this);
    winvert4::Log("EW: RenderThread start");
}

void EffectWindow::Hide()
{
    if (m_run.exchange(false)) {
        if (m_renderThread.joinable()) m_renderThread.join();
    }
    m_srv.Reset();
    m_samp.Reset();
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

    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
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

void EffectWindow::UpdateCBForRegion_()
{
    RECT outRect = m_thread->GetOutputRect();
    const float outW = float(outRect.right - outRect.left);
    const float outH = float(outRect.bottom - outRect.top);

    const float selL = float(m_desktopRect.left   - outRect.left);
    const float selT = float(m_desktopRect.top    - outRect.top);
    const float selW = float(m_desktopRect.right  - m_desktopRect.left);
    const float selH = float(m_desktopRect.bottom - m_desktopRect.top);

    CBData cb{};
    cb.scale[0]  = (outW > 0) ? (selW / outW) : 0.0f;
    cb.scale[1]  = (outH > 0) ? (selH / outH) : 0.0f;
    cb.offset[0] = (outW > 0) ? (selL / outW) : 0.0f;
    cb.offset[1] = (outH > 0) ? (selT / outH) : 0.0f;

    m_deferredCtx->UpdateSubresource(m_cb.Get(), 0, nullptr, &cb, 0, 0);
    winvert4::Logf("EW: CB scale=(%.3f,%.3f) offset=(%.3f,%.3f)", cb.scale[0], cb.scale[1], cb.offset[0], cb.offset[1]);
}

void EffectWindow::RenderThreadProc()
{
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
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        kEffectWndClass, L"", WS_POPUP,
        m_desktopRect.left, m_desktopRect.top, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd) { winvert4::Log("EffectWindow::RenderThreadProc failed to create HWND"); return; }
    winvert4::Logf("EW: created HWND=%p size=(%dx%d)", (void*)m_hwnd, w, h);

    // This is the key to making the window truly click-through.
    // By setting LWA_COLORKEY, we tell Windows that mouse events should pass through
    // the window as if it weren't there. The color key itself doesn't matter since
    // we are rendering with DirectX, not GDI.
    // This is more reliable than relying on WS_EX_TRANSPARENT alone for DX-rendered windows.
    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_COLORKEY);

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

    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(CBData); cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_d3d->CreateBuffer(&cbd, nullptr, &m_cb))) return;

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

    // Also run a message loop on this thread to keep the window responsive.
    MSG msg{};

    while (m_run) {
        // Process window messages
        if (PeekMessageW(&msg, m_hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Wait for a frame (or reuse last)
        ComPtr<ID3D11Texture2D> tex;
        {
            using namespace std::chrono_literals;
            std::unique_lock<std::mutex> lk(m_textureMutex);
            m_textureCv.wait_for(lk, 33ms, [this] { return !m_run || m_sharedTexture != nullptr; });
            if (!m_run) break;

            if (m_sharedTexture) {
                tex = m_sharedTexture;
                m_lastTexture = m_sharedTexture;
                m_sharedTexture.Reset();
            } else {
                tex = m_lastTexture;
            }
        }

        if (!tex || !m_swapChain) {
            continue;
        }

        // Ensure SRV reflects current texture
        EnsureSRVLocked_(tex.Get());
        if (!m_srv) { continue; }

        // Recreate RTV each frame (resize-robust)
        backBuf.Reset();
        if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf)))) {
            m_swapChain->Present(1, 0);
            continue;
        }
        ComPtr<ID3D11RenderTargetView> localRTV;
        m_d3d->CreateRenderTargetView(backBuf.Get(), nullptr, &localRTV);

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
        ID3D11Buffer* cb = m_cb.Get();
        m_deferredCtx->VSSetConstantBuffers(0, 1, &cb);

        m_deferredCtx->PSSetShader(m_ps.Get(), nullptr, 0);
        ID3D11SamplerState* ss = m_samp.Get();
        m_deferredCtx->PSSetSamplers(0, 1, &ss);
        ID3D11ShaderResourceView* srv = m_srv.Get();
        m_deferredCtx->PSSetShaderResources(0, 1, &srv);

        ID3D11RenderTargetView* rtv = localRTV.Get();
        m_deferredCtx->OMSetRenderTargets(1, &rtv, nullptr);

        // Update constants & draw (invert handled in pixel shader)
        UpdateCBForRegion_();

        const float clear[4] = { 0,0,0,0 };
        m_deferredCtx->ClearRenderTargetView(localRTV.Get(), clear);
        m_deferredCtx->Draw(3, 0);

        // Unbind SRV to avoid hazards if source updates immediately
        ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
        m_deferredCtx->PSSetShaderResources(0, 1, nullSRV);

        // Execute commands on the immediate context
        ComPtr<ID3D11CommandList> commandList;
        if (SUCCEEDED(m_deferredCtx->FinishCommandList(FALSE, &commandList))) {
            std::lock_guard<std::mutex> lock(m_thread->GetContextMutex());
            m_immediateCtx->ExecuteCommandList(commandList.Get(), FALSE);
        }

        m_swapChain->Present(1, 0);
    }
}
