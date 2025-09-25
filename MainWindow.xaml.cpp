#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;

namespace
{
    constexpr wchar_t kSelectionWndClass[] = L"Winvert4_SelectionOverlayWindow";
}

namespace winrt::Winvert4::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
    }

    int32_t MainWindow::MyProperty() { return 0; }
    void MainWindow::MyProperty(int32_t) {}

    void MainWindow::ToggleSnipping()
    {
        StartScreenSelection();
    }

    void MainWindow::StartScreenSelection()
    {
        if (m_isSelecting) return;
        m_isSelecting = true;

        // 1) Capture the entire virtual screen once (freeze effect)
        CaptureScreenBitmap();

        // 2) Register overlay class with crosshair cursor
        WNDCLASSEXW wcex{ sizeof(WNDCLASSEXW) };
        wcex.style         = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc   = &MainWindow::SelectionWndProc;
        wcex.hInstance     = GetModuleHandleW(nullptr);
        wcex.hCursor       = LoadCursorW(nullptr, IDC_CROSS);
        wcex.lpszClassName = kSelectionWndClass;
        wcex.hbrBackground = nullptr;

        static ATOM s_atom = 0;
        if (!s_atom) s_atom = RegisterClassExW(&wcex);

        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        // 3) Create topmost, layered overlay across all monitors
        m_selectionHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            kSelectionWndClass,
            L"Winvert Selection",
            WS_POPUP,
            vx, vy, vw, vh,
            nullptr, nullptr, GetModuleHandleW(nullptr),
            this);

        if (m_selectionHwnd)
        {
            SetLayeredWindowAttributes(m_selectionHwnd, 0, 160, LWA_ALPHA); // ~62% alpha
            ShowWindow(m_selectionHwnd, SW_SHOW);
            SetForegroundWindow(m_selectionHwnd);
            SetCapture(m_selectionHwnd);
        }
        else
        {
            ReleaseScreenBitmap();
            m_isSelecting = false;
        }
    }

    // -------- Win32 overlay helpers --------

    void MainWindow::CaptureScreenBitmap()
    {
        ReleaseScreenBitmap();

        m_virtualOrigin.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        m_virtualOrigin.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        m_screenSize.cx   = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        m_screenSize.cy   = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        HDC screenDC = GetDC(nullptr);
        m_screenMemDC = CreateCompatibleDC(screenDC);
        m_screenBmp   = CreateCompatibleBitmap(screenDC, m_screenSize.cx, m_screenSize.cy);
        HGDIOBJ old   = SelectObject(m_screenMemDC, m_screenBmp);

        BitBlt(m_screenMemDC, 0, 0, m_screenSize.cx, m_screenSize.cy,
               screenDC, m_virtualOrigin.x, m_virtualOrigin.y, SRCCOPY);

        SelectObject(m_screenMemDC, old);
        ReleaseDC(nullptr, screenDC);
    }

    void MainWindow::ReleaseScreenBitmap()
    {
        if (m_screenMemDC)
        {
            DeleteDC(m_screenMemDC);
            m_screenMemDC = nullptr;
        }
        if (m_screenBmp)
        {
            DeleteObject(m_screenBmp);
            m_screenBmp = nullptr;
        }
        m_screenSize = { 0,0 };
        m_virtualOrigin = { 0,0 };
    }

    RECT MainWindow::MakeRectFromPoints(POINT a, POINT b) const
    {
        RECT r{};
        r.left   = (std::min)(a.x, b.x);
        r.top    = (std::min)(a.y, b.y);
        r.right  = (std::max)(a.x, b.x);
        r.bottom = (std::max)(a.y, b.y);
        return r;
    }

    void MainWindow::OnSelectionCompleted(RECT sel)
    {
        m_lastSelection = sel;
        ShowEffectAt(sel);
    }

    // -------- Overlay WndProc --------

    LRESULT CALLBACK MainWindow::SelectionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_NCCREATE:
        {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<MainWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }

        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;

        case WM_LBUTTONDOWN:
        {
            if (!self) break;
            self->m_isDragging = true;

            POINTS pts = MAKEPOINTS(lParam);
            self->m_ptStart = { pts.x, pts.y };
            self->m_ptEnd   = self->m_ptStart;

            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            if (!self || !self->m_isDragging) break;
            POINTS pts = MAKEPOINTS(lParam);
            self->m_ptEnd = { pts.x, pts.y };
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            if (!self) break;
            ReleaseCapture();
            self->m_isDragging = false;

            POINTS pts = MAKEPOINTS(lParam);
            self->m_ptEnd = { pts.x, pts.y };

            RECT sel = self->MakeRectFromPoints(self->m_ptStart, self->m_ptEnd);

            // Convert to absolute virtual-screen coordinates
            sel.left   += self->m_virtualOrigin.x;
            sel.right  += self->m_virtualOrigin.x;
            sel.top    += self->m_virtualOrigin.y;
            sel.bottom += self->m_virtualOrigin.y;

            self->OnSelectionCompleted(sel);

            DestroyWindow(hwnd); // tear down overlay
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                if (self) { self->m_isDragging = false; self->m_isSelecting = false; }
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_PAINT:
        {
            if (!self) break;

            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Draw frozen desktop
            if (self->m_screenMemDC && self->m_screenBmp)
            {
                HGDIOBJ old = SelectObject(self->m_screenMemDC, self->m_screenBmp);
                BitBlt(hdc, 0, 0, self->m_screenSize.cx, self->m_screenSize.cy,
                       self->m_screenMemDC, 0, 0, SRCCOPY);
                SelectObject(self->m_screenMemDC, old);
            }

            // Draw red rectangle
            if (self->m_isDragging)
            {
                RECT r = self->MakeRectFromPoints(self->m_ptStart, self->m_ptEnd);

                HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
                HGDIOBJ oldPen = SelectObject(hdc, pen);
                HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

                Rectangle(hdc, r.left, r.top, r.right, r.bottom);

                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(pen);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            if (self)
            {
                self->m_isSelecting = false;
                self->ReleaseScreenBitmap();
                self->m_selectionHwnd = nullptr;
            }
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

namespace
{
    constexpr wchar_t kEffectWndClass[] = L"Winvert4_EffectWindow";
}

namespace winrt::Winvert4::implementation
{
    static ::Microsoft::WRL::ComPtr<IDXGIOutput1> PickOutputForRect(RECT sel, RECT& outputRect)
    {
        ::Microsoft::WRL::ComPtr<IDXGIFactory1> factory1;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1)))) return {};

        ::Microsoft::WRL::ComPtr<IDXGIOutput1> bestOut1;
        RECT bestRect{ 0,0,0,0 };
        LONG bestArea = 0;

        for (UINT ai = 0;; ++ai)
        {
            ::Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory1->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;

            for (UINT oi = 0;; ++oi)
            {
                ::Microsoft::WRL::ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC desc{};
                output->GetDesc(&desc);
                RECT r = desc.DesktopCoordinates;

                RECT inter{};
                inter.left = (std::max)(sel.left, r.left);
                inter.top = (std::max)(sel.top, r.top);
                inter.right = (std::min)(sel.right, r.right);
                inter.bottom = (std::min)(sel.bottom, r.bottom);
                LONG w = inter.right - inter.left;
                LONG h = inter.bottom - inter.top;
                LONG area = (w > 0 && h > 0) ? (w * h) : 0;
                if (area > bestArea)
                {
                    bestArea = area;
                    output.As(&bestOut1);
                    bestRect = r;
                }
            }
        }

        outputRect = bestRect;
        return bestOut1;
    }

    static void RegisterEffectWndClass()
    {
        static ATOM s_atom = 0;
        if (s_atom) return;
        WNDCLASSEXW wcex{ sizeof(WNDCLASSEXW) };
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = DefWindowProcW;
        wcex.hInstance = GetModuleHandleW(nullptr);
        wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wcex.lpszClassName = kEffectWndClass;
        s_atom = RegisterClassExW(&wcex);
    }

    void MainWindow::HideEffect()
    {
        m_run = false;
        if (m_renderThread.joinable()) m_renderThread.join();

        m_swapChain.Reset();
        m_dup.Reset();
        m_ps.Reset();
        m_vs.Reset();
        m_il.Reset();
        m_vb.Reset();
        m_cb.Reset();
        m_samp.Reset();
        m_ctx.Reset();
        m_d3d.Reset();

        if (m_effectHwnd)
        {
            DestroyWindow(m_effectHwnd);
            m_effectHwnd = nullptr;
        }
    }

    void MainWindow::ShowEffectAt(RECT sel)
    {
        HideEffect();
        RegisterEffectWndClass();

        m_effectRect = sel;

        m_effectHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
            kEffectWndClass, L"", WS_POPUP,
            sel.left, sel.top, sel.right - sel.left, sel.bottom - sel.top,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!m_effectHwnd) return;

        ShowWindow(m_effectHwnd, SW_SHOWNOACTIVATE);
        // Exclude our overlay from desktop duplication/screen capture using display affinity
        {
            if (!SetWindowDisplayAffinity(m_effectHwnd, WDA_EXCLUDEFROMCAPTURE))
            {
                // Fallback for older OS: may at least black out via WDA_MONITOR
                SetWindowDisplayAffinity(m_effectHwnd, WDA_MONITOR);
            }
        }

        // Pick output for duplication first; create device on that adapter
        ::Microsoft::WRL::ComPtr<IDXGIOutput1> pickedOut;
        RECT outRect{};
        pickedOut = PickOutputForRect(sel, outRect);
        if (!pickedOut) return;
        m_outputRect = outRect;

        ::Microsoft::WRL::ComPtr<IDXGIAdapter> pickedAdapter;
        pickedOut->GetParent(IID_PPV_ARGS(&pickedAdapter));

        // Create D3D11 device on the picked adapter
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        // flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL flOut;
        if (FAILED(D3D11CreateDevice(pickedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0,
                                     D3D11_SDK_VERSION, &m_d3d, &flOut, &m_ctx)))
        {
            return;
        }

        // Factory & swap chain
        {
            ::Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            m_d3d.As(&dxgiDevice);
            ::Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
            dxgiDevice->GetAdapter(&dxgiAdapter);
            ::Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
            dxgiAdapter->GetParent(IID_PPV_ARGS(&factory2));
            m_factory = factory2;

            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = sel.right - sel.left;
            sd.Height = sel.bottom - sel.top;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

            factory2->CreateSwapChainForHwnd(m_d3d.Get(), m_effectHwnd, &sd, nullptr, nullptr, &m_swapChain);
        }

        // Create duplication on the picked output and our device
        pickedOut->DuplicateOutput(m_d3d.Get(), &m_dup);

        // Create pipeline (fullscreen quad + invert PS)
        {
            static const char* kVS = R"(
struct VSIn { float2 pos : POSITION; };
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
PSIn main(VSIn i){ PSIn o; o.pos=float4(i.pos,0,1); o.uv = (i.pos*float2(0.5,-0.5))+float2(0.5,0.5); return o; }
)";
            static const char* kPS = R"(
Texture2D srcTex : register(t0);
SamplerState samp0 : register(s0);
cbuffer Cb : register(b0){ float2 scale; float2 offset; };
float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {
  float2 suv = uv*scale + offset; float4 c = srcTex.Sample(samp0, suv); c.rgb = 1.0 - c.rgb; return c;
}
)";
            ::Microsoft::WRL::ComPtr<ID3DBlob> vsb, psb, err;
            if (FAILED(D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsb, &err))) return;
            if (FAILED(D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psb, &err))) return;
            m_d3d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs);
            m_d3d->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps);

            D3D11_INPUT_ELEMENT_DESC il[] = { {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0} };
            m_d3d->CreateInputLayout(il, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &m_il);

            struct V2 { float x, y; };
            V2 verts[6] = { {-1,-1},{-1,1},{1,-1},{1,-1},{-1,1},{1,1} };
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(verts);
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = 0;
            bd.MiscFlags = 0;
            bd.StructureByteStride = 0;
            D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = verts;
            m_d3d->CreateBuffer(&bd, &srd, &m_vb);

            D3D11_BUFFER_DESC cbd{};
            cbd.ByteWidth = sizeof(CBData);
            cbd.Usage = D3D11_USAGE_DEFAULT;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = 0;
            cbd.MiscFlags = 0;
            cbd.StructureByteStride = 0;
            m_d3d->CreateBuffer(&cbd, nullptr, &m_cb);

            D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            m_d3d->CreateSamplerState(&sd, &m_samp);
        }

        // Launch render thread
        m_run = true;
        m_renderThread = std::thread([this]()
        {
            while (m_run)
            {
                DXGI_OUTDUPL_FRAME_INFO fi{};
                ::Microsoft::WRL::ComPtr<IDXGIResource> res;
                HRESULT hr = m_dup ? m_dup->AcquireNextFrame(16, &fi, &res) : E_FAIL;
                if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
                if (FAILED(hr)) { std::this_thread::sleep_for(std::chrono::milliseconds(16)); continue; }

                ::Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                res.As(&tex);

                ::Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                m_d3d->CreateShaderResourceView(tex.Get(), nullptr, &srv);

                // Compute scale/offset mapping from window to monitor texcoords
                float monW = float(m_outputRect.right - m_outputRect.left);
                float monH = float(m_outputRect.bottom - m_outputRect.top);
                float scaleX = float(m_effectRect.right - m_effectRect.left) / monW;
                float scaleY = float(m_effectRect.bottom - m_effectRect.top) / monH;
                float offsetX = float(m_effectRect.left - m_outputRect.left) / monW;
                float offsetY = float(m_effectRect.top - m_outputRect.top) / monH;
                CBData cb{ {scaleX, scaleY}, {offsetX, offsetY} };
                m_ctx->UpdateSubresource(m_cb.Get(), 0, nullptr, &cb, 0, 0);

                // Get backbuffer and RTV
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

                D3D11_VIEWPORT vp{ 0,0,(float)(m_effectRect.right - m_effectRect.left),(float)(m_effectRect.bottom - m_effectRect.top),0,1 };
                m_ctx->RSSetViewports(1, &vp);

                float clear[4] = { 0,0,0,0 };
                m_ctx->ClearRenderTargetView(rtv.Get(), clear);
                m_ctx->Draw(6, 0);
                m_swapChain->Present(1, 0); // sync interval 1 = vblank

                // Unbind SRV before releasing the duplication frame to avoid hazards
                ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
                m_ctx->PSSetShaderResources(0, 1, nullSrv);
                m_dup->ReleaseFrame();
            }
        });
    }
}
