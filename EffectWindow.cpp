#include "pch.h"
#include "Log.h"
#include "EffectWindow.h"
#include "App.xaml.h"

EffectWindow::EffectWindow(RECT desktopRect)
    : m_desktopRect(desktopRect)
{
}

EffectWindow::~EffectWindow()
{
    Hide();
}

void EffectWindow::Show()
{
    winvert4::Log("EffectWindow::Show called");
    {
        char dbg[256];
        sprintf_s(dbg, "EW.Show rect=(%ld,%ld,%ld,%ld)", m_desktopRect.left, m_desktopRect.top, m_desktopRect.right, m_desktopRect.bottom);
        winvert4::Log(dbg);
    }

    auto app = winrt::Winvert4::implementation::App::Current();
    winvert4::Log(app ? "EW.Show got App::Current" : "EW.Show App::Current is null");
    if (!app)
    {
        winvert4::Log("EffectWindow::Show no App::Current() instance");
        return;
    }
    auto outputManager = app->GetOutputManager();
    winvert4::Log(outputManager ? "EW.Show got OutputManager" : "EW.Show GetOutputManager returned null");
    if (!outputManager)
    {
        winvert4::Log("EffectWindow::Show no OutputManager available");
        return;
    }
    winvert4::Log("EW.Show calling GetThreadForRect");
    m_thread = outputManager->GetThreadForRect(m_desktopRect);
    {
        char dbg[64]; sprintf_s(dbg, "EW.Show thread=%p", (void*)m_thread); winvert4::Log(dbg);
    }
    if (!m_thread)
    {
        winvert4::Log("EffectWindow::Show failed to get a DuplicationThread\n");
        return;
    }

    m_d3d = m_thread->GetDevice();
    if (!m_d3d)
    {
        winvert4::Log("EffectWindow::Show failed to get a device from the thread\n");
        return;
    }
    m_d3d->GetImmediateContext(&m_ctx);
    if (!m_ctx)
    {
        winvert4::Log("EffectWindow::Show GetImmediateContext returned null context");
        return;
    }

    // Register a basic window class
    constexpr wchar_t kEffectWndClass[] = L"Winvert4_EffectWindow";
    static ATOM s_atom = 0;
    if (!s_atom)
    {
        WNDCLASSEXW wcex{ sizeof(WNDCLASSEXW) };
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = DefWindowProcW;
        wcex.hInstance = GetModuleHandleW(nullptr);
        wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wcex.lpszClassName = kEffectWndClass;
        s_atom = RegisterClassExW(&wcex);
    }

    // Create the window
    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        kEffectWndClass, L"", WS_POPUP,
        m_desktopRect.left, m_desktopRect.top, m_desktopRect.right - m_desktopRect.left, m_desktopRect.bottom - m_desktopRect.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd) { winvert4::Log("EffectWindow::Show failed to create HWND"); return; }

    winvert4::Logf("EW: created HWND=%p size=(%ldx%ld)", (void*)m_hwnd,
        m_desktopRect.right - m_desktopRect.left,
        m_desktopRect.bottom - m_desktopRect.top);

    // Exclude this window from capture to avoid self-capture artifacts (white window)
    BOOL okAffinity = SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE);
    if (!okAffinity)
    {
        DWORD gle = GetLastError();
        winvert4::Logf("EW: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed err=%lu", gle);
    }
    else
    {
        winvert4::Log("EW: SetWindowDisplayAffinity applied");
    }

    BOOL exclude = TRUE;
    HRESULT hrDwm = DwmSetWindowAttribute(m_hwnd, DWMWA_EXCLUDED_FROM_CAPTURE, &exclude, sizeof(exclude));
    if (FAILED(hrDwm))
    {
        winvert4::Logf("EW: DwmSetWindowAttribute(DWMWA_EXCLUDED_FROM_CAPTURE) failed hr=0x%08X", hrDwm);
    }
    else
    {
        winvert4::Log("EW: Dwm EXCLUDEFROMCAPTURE applied");
    }

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);

    // Create swap chain and pipeline
    {
        ::Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        m_d3d.As(&dxgiDevice);
        ::Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
        dxgiDevice->GetAdapter(&dxgiAdapter);
        if (dxgiAdapter)
        {
            DXGI_ADAPTER_DESC adesc{};
            dxgiAdapter->GetDesc(&adesc);
            winvert4::Logf("EW: device adapter LUID=%08X:%08X", adesc.AdapterLuid.HighPart, adesc.AdapterLuid.LowPart);
        }
        dxgiAdapter->GetParent(IID_PPV_ARGS(&m_factory));

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = m_desktopRect.right - m_desktopRect.left;
        sd.Height = m_desktopRect.bottom - m_desktopRect.top;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

        HRESULT hrSC = m_factory->CreateSwapChainForHwnd(m_d3d.Get(), m_hwnd, &sd, nullptr, nullptr, &m_swapChain);
        if (FAILED(hrSC) || !m_swapChain)
        {
            winvert4::Logf("EW: CreateSwapChainForHwnd failed hr=0x%08X", hrSC);
            return;
        }

        static const char* kVS = R"(struct VSIn { float2 pos : POSITION; }; struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; }; PSIn main(VSIn i){ PSIn o; o.pos=float4(i.pos,0,1); o.uv = (i.pos*float2(0.5,-0.5))+float2(0.5,0.5); return o; })";
        static const char* kPS = R"(Texture2D srcTex : register(t0); SamplerState samp0 : register(s0); cbuffer Cb : register(b0){ float2 scale; float2 offset; }; float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target { float2 suv = uv*scale + offset; float4 c = srcTex.Sample(samp0, suv); c.rgb = 1.0 - c.rgb; return c; })";
        ::Microsoft::WRL::ComPtr<ID3DBlob> vsb, psb, err;
        HRESULT hrVS = D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsb, &err);
        if (FAILED(hrVS) || !vsb)
        {
            const char* emsg = err ? (const char*)err->GetBufferPointer() : "";
            winvert4::Logf("EW: VS compile failed hr=0x%08X msg=%s", hrVS, emsg);
            return;
        }
        HRESULT hrPS = D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psb, &err);
        if (FAILED(hrPS) || !psb)
        {
            const char* emsg = err ? (const char*)err->GetBufferPointer() : "";
            winvert4::Logf("EW: PS compile failed hr=0x%08X msg=%s", hrPS, emsg);
            return;
        }
        HRESULT hrCVS = m_d3d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs);
        if (FAILED(hrCVS) || !m_vs) { winvert4::Logf("EW: CreateVertexShader failed hr=0x%08X", hrCVS); return; }
        HRESULT hrCPS = m_d3d->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps);
        if (FAILED(hrCPS) || !m_ps) { winvert4::Logf("EW: CreatePixelShader failed hr=0x%08X", hrCPS); return; }

        D3D11_INPUT_ELEMENT_DESC il[] = { {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0} };
        HRESULT hrIL = m_d3d->CreateInputLayout(il, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &m_il);
        if (FAILED(hrIL) || !m_il) { winvert4::Logf("EW: CreateInputLayout failed hr=0x%08X", hrIL); return; }

        struct V2 { float x, y; };
        V2 verts[6] = { {-1,-1},{-1,1},{1,-1},{1,-1},{-1,1},{1,1} };
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(verts); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = verts;
        HRESULT hrVB = m_d3d->CreateBuffer(&bd, &srd, &m_vb);
        if (FAILED(hrVB) || !m_vb) { winvert4::Logf("EW: CreateBuffer VB failed hr=0x%08X", hrVB); return; }

        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(CBData); cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        HRESULT hrCB = m_d3d->CreateBuffer(&cbd, nullptr, &m_cb);
        if (FAILED(hrCB) || !m_cb) { winvert4::Logf("EW: CreateBuffer CB failed hr=0x%08X", hrCB); return; }

        D3D11_SAMPLER_DESC sampd{}; sampd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampd.AddressU = sampd.AddressV = sampd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        HRESULT hrSamp = m_d3d->CreateSamplerState(&sampd, &m_samp);
        if (FAILED(hrSamp) || !m_samp) { winvert4::Logf("EW: CreateSamplerState failed hr=0x%08X", hrSamp); return; }
    }

    Subscription sub;
    sub.Subscriber = this;
    sub.Region = m_desktopRect;
    m_thread->AddSubscription(sub);

    m_run = true;
    m_renderThread = std::thread(&EffectWindow::RenderThreadProc, this);
    winvert4::Log("EW: render thread created");
}

void EffectWindow::Hide()
{
    winvert4::Log("EW: Hide begin");
    m_run = false;
    m_textureCv.notify_one(); // Wake up render thread to exit
    if (m_renderThread.joinable())
    {
        m_renderThread.join();
    }
    winvert4::Log("EW: render thread joined");

    m_swapChain.Reset();
    m_ps.Reset();
    m_vs.Reset();
    m_il.Reset();
    m_vb.Reset();
    m_cb.Reset();
    m_samp.Reset();
    m_ctx.Reset();
    m_d3d.Reset();

    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    winvert4::Log("EW: Hide end");
}

void EffectWindow::OnFrameReady(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture)
{
    {
        std::lock_guard<std::mutex> lock(m_textureMutex);
        m_sharedTexture = texture;
    }
    m_textureCv.notify_one();
}

void EffectWindow::RenderThreadProc()
{
    winvert4::Log("EW: RenderThread start");
    using namespace std::chrono_literals;
    while (m_run)
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        {
            std::unique_lock<std::mutex> lock(m_textureMutex);
            m_textureCv.wait_for(lock, 16ms, [this] { return !m_run || m_sharedTexture != nullptr; });
            if (!m_run) break;
            if (m_sharedTexture)
            {
                m_lastTexture = m_sharedTexture;
                m_sharedTexture.Reset();
            }
            texture = m_lastTexture;
        }

        if (!texture) continue;

        ::Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        HRESULT hrSrv = m_d3d->CreateShaderResourceView(texture.Get(), nullptr, &srv);
        if (FAILED(hrSrv) || !srv)
        {
            winvert4::Log("EffectWindow: CreateShaderResourceView failed or null SRV\n");
            continue;
        }

        // Compute scale/offset from output rect to this window''s rect
        RECT out = m_thread ? m_thread->GetOutputRect() : RECT{0,0,1,1};
        float monW = float(out.right - out.left);
        float monH = float(out.bottom - out.top);
        float scaleX = monW > 0 ? float(m_desktopRect.right - m_desktopRect.left) / monW : 1.0f;
        float scaleY = monH > 0 ? float(m_desktopRect.bottom - m_desktopRect.top) / monH : 1.0f;
        float offsetX = monW > 0 ? float(m_desktopRect.left - out.left) / monW : 0.0f;
        float offsetY = monH > 0 ? float(m_desktopRect.top - out.top) / monH : 0.0f;
        CBData cb{ {scaleX, scaleY}, {offsetX, offsetY} };
        if (m_thread) { std::lock_guard<std::mutex> lock(m_thread->GetContextMutex()); m_ctx->UpdateSubresource(m_cb.Get(), 0, nullptr, &cb, 0, 0); }

        ::Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
        HRESULT hrGB = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        if (FAILED(hrGB) || !back)
        {
            winvert4::Logf("EW: GetBuffer(0) failed hr=0x%08X", hrGB);
            continue;
        }
        ::Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        HRESULT hrRTV = m_d3d->CreateRenderTargetView(back.Get(), nullptr, &rtv);
        if (FAILED(hrRTV) || !rtv)
        {
            winvert4::Logf("EW: CreateRenderTargetView failed hr=0x%08X", hrRTV);
            continue;
        }

        UINT stride = sizeof(float) * 2, offset = 0;
        ID3D11Buffer* vb = m_vb.Get();
        if (m_thread) {
            std::lock_guard<std::mutex> lock(m_thread->GetContextMutex());
            m_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            m_ctx->IASetInputLayout(m_il.Get());
            m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        }
        ID3D11Buffer* cbp = m_cb.Get();
        if (m_thread) {
            std::lock_guard<std::mutex> lock(m_thread->GetContextMutex());
            m_ctx->VSSetConstantBuffers(0, 1, &cbp);
            m_ctx->PSSetConstantBuffers(0, 1, &cbp);
            m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srvp = srv.Get();
            m_ctx->PSSetShaderResources(0, 1, &srvp);
            ID3D11SamplerState* smp = m_samp.Get();
            m_ctx->PSSetSamplers(0, 1, &smp);
            ID3D11RenderTargetView* r = rtv.Get();
            m_ctx->OMSetRenderTargets(1, &r, nullptr);
        }

        D3D11_VIEWPORT vp{ 0,0,(float)(m_desktopRect.right - m_desktopRect.left),(float)(m_desktopRect.bottom - m_desktopRect.top),0,1 };
        if (m_thread) { std::lock_guard<std::mutex> lock(m_thread->GetContextMutex()); m_ctx->RSSetViewports(1, &vp); }

        float clear[4] = { 0,0,0,0 };
        if (m_thread) {
            std::lock_guard<std::mutex> lock(m_thread->GetContextMutex());
            m_ctx->ClearRenderTargetView(rtv.Get(), clear);
            m_ctx->Draw(6, 0);
        }
        HRESULT hrPr = m_swapChain->Present(0, 0);
        if (FAILED(hrPr)) { winvert4::Logf("EW: Present failed hr=0x%08X", hrPr); }
    }
    winvert4::Log("EW: RenderThread exit");
}

