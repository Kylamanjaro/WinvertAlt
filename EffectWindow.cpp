#include "pch.h"
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
    OutputDebugStringA("EffectWindow::Show called\n");

    auto outputManager = winrt::Winvert4::implementation::App::Current()->GetOutputManager();
    auto thread = outputManager->GetThreadForRect(m_desktopRect);
    if (!thread)
    {
        OutputDebugStringA("EffectWindow::Show failed to get a DuplicationThread\n");
        return;
    }

    m_d3d = thread->GetDevice();
    if (!m_d3d)
    {
        OutputDebugStringA("EffectWindow::Show failed to get a device from the thread\n");
        return;
    }
    m_d3d->GetImmediateContext(&m_ctx);

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
    if (!m_hwnd) { OutputDebugStringA("EffectWindow::Show failed to create HWND\n"); return; }

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE);

    // Create swap chain and pipeline
    {
        ::Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        m_d3d.As(&dxgiDevice);
        ::Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
        dxgiDevice->GetAdapter(&dxgiAdapter);
        dxgiAdapter->GetParent(IID_PPV_ARGS(&m_factory));

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = m_desktopRect.right - m_desktopRect.left;
        sd.Height = m_desktopRect.bottom - m_desktopRect.top;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

        m_factory->CreateSwapChainForHwnd(m_d3d.Get(), m_hwnd, &sd, nullptr, nullptr, &m_swapChain);

        static const char* kVS = R"(struct VSIn { float2 pos : POSITION; }; struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; }; PSIn main(VSIn i){ PSIn o; o.pos=float4(i.pos,0,1); o.uv = (i.pos*float2(0.5,-0.5))+float2(0.5,0.5); return o; })";
        static const char* kPS = R"(Texture2D srcTex : register(t0); SamplerState samp0 : register(s0); cbuffer Cb : register(b0){ float2 scale; float2 offset; }; float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target { float2 suv = uv*scale + offset; float4 c = srcTex.Sample(samp0, suv); c.rgb = 1.0 - c.rgb; return c; })";
        ::Microsoft::WRL::ComPtr<ID3DBlob> vsb, psb, err;
        D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsb, &err);
        D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psb, &err);
        m_d3d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs);
        m_d3d->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps);

        D3D11_INPUT_ELEMENT_DESC il[] = { {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0} };
        m_d3d->CreateInputLayout(il, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &m_il);

        struct V2 { float x, y; };
        V2 verts[6] = { {-1,-1},{-1,1},{1,-1},{1,-1},{-1,1},{1,1} };
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(verts); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = verts;
        m_d3d->CreateBuffer(&bd, &srd, &m_vb);

        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(CBData); cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        m_d3d->CreateBuffer(&cbd, nullptr, &m_cb);

        D3D11_SAMPLER_DESC sampd{}; sampd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampd.AddressU = sampd.AddressV = sampd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        m_d3d->CreateSamplerState(&sampd, &m_samp);
    }

    Subscription sub;
    sub.Subscriber = this;
    sub.Region = m_desktopRect;
    thread->AddSubscription(sub);

    m_run = true;
    m_renderThread = std::thread(&EffectWindow::RenderThreadProc, this);
}

void EffectWindow::Hide()
{
    m_run = false;
    m_textureCv.notify_one(); // Wake up render thread to exit
    if (m_renderThread.joinable())
    {
        m_renderThread.join();
    }

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
    while (m_run)
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        {
            std::unique_lock<std::mutex> lock(m_textureMutex);
            m_textureCv.wait(lock, [this] { return !m_run || m_sharedTexture != nullptr; });
            if (!m_run) break;
            texture = m_sharedTexture;
            m_sharedTexture.Reset();
        }

        if (!texture) continue;

        ::Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        m_d3d->CreateShaderResourceView(texture.Get(), nullptr, &srv);

        CBData cb{ {1.0f, 1.0f}, {0.0f, 0.0f} };
        m_ctx->UpdateSubresource(m_cb.Get(), 0, nullptr, &cb, 0, 0);

        ::Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
        m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        ::Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        m_d3d->CreateRenderTargetView(back.Get(), nullptr, &rtv);

        UINT stride = sizeof(float) * 2, offset = 0;
        ID3D11Buffer* vb = m_vb.Get();
        m_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        m_ctx->IASetInputLayout(m_il.Get());
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        ID3D11Buffer* cbp = m_cb.Get();
        m_ctx->VSSetConstantBuffers(0, 1, &cbp);
        m_ctx->PSSetConstantBuffers(0, 1, &cbp);
        m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvp = srv.Get();
        m_ctx->PSSetShaderResources(0, 1, &srvp);
        ID3D11SamplerState* smp = m_samp.Get();
        m_ctx->PSSetSamplers(0, 1, &smp);
        ID3D11RenderTargetView* r = rtv.Get();
        m_ctx->OMSetRenderTargets(1, &r, nullptr);

        D3D11_VIEWPORT vp{ 0,0,(float)(m_desktopRect.right - m_desktopRect.left),(float)(m_desktopRect.bottom - m_desktopRect.top),0,1 };
        m_ctx->RSSetViewports(1, &vp);

        float clear[4] = { 0,0,0,0 };
        m_ctx->ClearRenderTargetView(rtv.Get(), clear);
        m_ctx->Draw(6, 0);
        m_swapChain->Present(1, 0);
    }
}
