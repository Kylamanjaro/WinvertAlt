#include "pch.h"
#include "Log.h"
#include "DuplicationThread.h"

DuplicationThread::DuplicationThread(IDXGIAdapter1* adapter, IDXGIOutput1* output)
    : m_output(output)
{
    DXGI_OUTPUT_DESC desc;
    m_output->GetDesc(&desc);
    m_outputRect = desc.DesktopCoordinates;

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &m_device, &featureLevel, &m_context);
    if (!FAILED(hr))
    {
        ::Microsoft::WRL::ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(m_device.As(&mt)) && mt) { mt->SetMultithreadProtected(TRUE); }
    }
}

DuplicationThread::~DuplicationThread()
{
    Stop();
}

void DuplicationThread::Run()
{
    if (!m_isRunning) { m_isRunning = true; m_thread = std::thread(&DuplicationThread::ThreadProc, this); }
}

void DuplicationThread::Stop()
{
    m_isRunning = false;
    if (m_thread.joinable()) { m_thread.join(); }
}

void DuplicationThread::AddSubscription(const Subscription& sub)
{
    std::lock_guard<std::mutex> lock(m_subMutex);
    m_subscriptions.push_back(sub);
}

void DuplicationThread::ThreadProc()
{
    if (!m_device) return;

    HRESULT hr = m_output->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr)) return;

    while (m_isRunning)
    {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ::Microsoft::WRL::ComPtr<IDXGIResource> resource;
        hr = m_duplication->AcquireNextFrame(500, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { continue; }
        if (FAILED(hr)) { m_duplication.Reset(); m_output->DuplicateOutput(m_device.Get(), &m_duplication); continue; }

        std::vector<Subscription> subsCopy;
        { std::lock_guard<std::mutex> lock(m_subMutex); subsCopy = m_subscriptions; }
        if (subsCopy.empty()) { m_duplication->ReleaseFrame(); continue; }

        ::Microsoft::WRL::ComPtr<ID3D11Texture2D> capturedTexture;
        resource.As(&capturedTexture);

        // Ensure shared full-frame texture exists and matches size
        D3D11_TEXTURE2D_DESC outDesc{};
        capturedTexture->GetDesc(&outDesc);
        {
            char dbg[256];
            sprintf_s(dbg,
                "DT: outputRect=(%ld,%ld,%ld,%ld) cap=(%u x %u fmt=%u) subs=%zu\n",
                m_outputRect.left, m_outputRect.top, m_outputRect.right, m_outputRect.bottom,
                outDesc.Width, outDesc.Height, static_cast<unsigned>(outDesc.Format), subsCopy.size());
            winvert4::Log(dbg);
        }
        if (!m_fullTexture)
        {
            D3D11_TEXTURE2D_DESC fd = outDesc;
            fd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            fd.MipLevels = 1; fd.ArraySize = 1; fd.CPUAccessFlags = 0; fd.Usage = D3D11_USAGE_DEFAULT;
            HRESULT hrT = m_device->CreateTexture2D(&fd, nullptr, &m_fullTexture);
            if (FAILED(hrT)) { winvert4::Log("DT: CreateTexture2D fullTexture failed\n"); }
        }
        else
        {
            D3D11_TEXTURE2D_DESC fd2{}; m_fullTexture->GetDesc(&fd2);
            if (fd2.Width != outDesc.Width || fd2.Height != outDesc.Height || fd2.Format != outDesc.Format)
            {
                m_fullTexture.Reset();
                D3D11_TEXTURE2D_DESC fd = outDesc;
                fd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                fd.MipLevels = 1; fd.ArraySize = 1; fd.CPUAccessFlags = 0; fd.Usage = D3D11_USAGE_DEFAULT;
                HRESULT hrT2 = m_device->CreateTexture2D(&fd, nullptr, &m_fullTexture);
                if (FAILED(hrT2)) { winvert4::Log("DT: Recreate fullTexture failed\n"); }
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_immediateCtxMutex);
            if (m_fullTexture)
            {
                m_context->CopyResource(m_fullTexture.Get(), capturedTexture.Get());
                winvert4::Log("DT: Copied full-frame into shared texture\n");
            }
            else
            {
                winvert4::Log("DT: fullTexture is null, skip copy\n");
            }
        }

        // Release duplication frame before notifying subscribers
        m_duplication->ReleaseFrame();

        // Notify subscribers with the full-frame texture
        if (m_fullTexture)
        {
            size_t idx = 0;
            for (auto const& sub : subsCopy)
            {
                if (sub.Subscriber)
                {
                    sub.Subscriber->OnFrameReady(m_fullTexture);
                    ++idx;
                }
            }
            char dbg2[128];
            sprintf_s(dbg2, "DT: notified %zu subscribers\n", idx);
            winvert4::Log(dbg2);
        }
        else
        {
            winvert4::Log("DT: fullTexture null at notify\n");
        }
    }

    m_duplication.Reset();
}

void DuplicationThread::RemoveSubscriber(ISubscriber* sub)
{
    if (!sub) return;
    std::lock_guard<std::mutex> lock(m_subMutex);
    m_subscriptions.erase(std::remove_if(m_subscriptions.begin(), m_subscriptions.end(),
                       [sub](const Subscription& s){ return s.Subscriber == sub; }), m_subscriptions.end());
}
