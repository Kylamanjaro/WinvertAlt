#include "pch.h"
#include "Log.h"
#include "DuplicationThread.h"

DuplicationThread::DuplicationThread(IDXGIAdapter1* adapter, IDXGIOutput1* output)
    : m_output(output)
{
    DXGI_OUTPUT_DESC desc;
    m_output->GetDesc(&desc);
    m_outputRect = desc.DesktopCoordinates;
    winvert4::Logf("DT: ctor output rect=(%ld,%ld,%ld,%ld)", m_outputRect.left, m_outputRect.top, m_outputRect.right, m_outputRect.bottom);

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &m_device, &featureLevel, &m_context);
    if (FAILED(hr))
    {
        winvert4::Logf("DT: D3D11CreateDevice failed hr=0x%08X", hr);
    }
    else
    {
        ::Microsoft::WRL::ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(m_device.As(&mt)) && mt) { mt->SetMultithreadProtected(TRUE); }
        winvert4::Logf("DT: device/context created featureLevel=0x%X", (unsigned)featureLevel);
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
    // Wake any waits on subscription changes
    m_subCv.notify_all();
    if (m_thread.joinable()) { m_thread.join(); }
}

void DuplicationThread::AddSubscription(const Subscription& sub)
{
    {
        std::lock_guard<std::mutex> lock(m_subMutex);
        m_subscriptions.push_back(sub);
    }
    winvert4::Logf("DT: AddSubscription sub=%p rect=(%ld,%ld,%ld,%ld) count=%zu",
        (void*)sub.Subscriber,
        sub.Region.left, sub.Region.top, sub.Region.right, sub.Region.bottom,
        m_subscriptions.size());
    m_subCv.notify_all();
}

void DuplicationThread::ThreadProc()
{
    if (!m_device) return;

    while (m_isRunning)
    {
        // Wait for at least one subscriber
        {
            std::unique_lock<std::mutex> lk(m_subMutex);
            if (m_subscriptions.empty())
            {
                if (m_duplication)
                {
                    winvert4::Log("DT: no subscribers, releasing duplication");
                    m_duplication.Reset();
                }
                winvert4::Log("DT: waiting for subscribers");
                m_subCv.wait(lk, [this]() { return !m_isRunning || !m_subscriptions.empty(); });
                if (!m_isRunning) break;
                winvert4::Log("DT: subscriber arrived, resuming");
            }
        }

        // Ensure duplication is created lazily
        if (!m_duplication)
        {
            HRESULT hrDup = m_output->DuplicateOutput(m_device.Get(), &m_duplication);
            if (FAILED(hrDup))
            {
                winvert4::Logf("DT: DuplicateOutput failed hr=0x%08X", hrDup);
                // Backoff a bit before retrying
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            winvert4::Log("DT: DuplicateOutput created");
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ::Microsoft::WRL::ComPtr<IDXGIResource> resource;
        HRESULT hr = m_duplication->AcquireNextFrame(500, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { continue; }
        if (FAILED(hr))
        {
            winvert4::Logf("DT: AcquireNextFrame failed hr=0x%08X, resetting duplication", hr);
            m_duplication.Reset();
            continue;
        }

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
                "DT: outputRect=(%ld,%ld,%ld,%ld) cap=(%u x %u fmt=%u) subs=%zu",
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
            if (FAILED(hrT)) { winvert4::Logf("DT: CreateTexture2D fullTexture failed hr=0x%08X", hrT); }
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
                if (FAILED(hrT2)) { winvert4::Logf("DT: Recreate fullTexture failed hr=0x%08X", hrT2); }
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_immediateCtxMutex);
            if (m_fullTexture)
            {
                m_context->CopyResource(m_fullTexture.Get(), capturedTexture.Get());
                winvert4::Log("DT: Copied full-frame into shared texture");
            }
            else
            {
                winvert4::Log("DT: fullTexture is null, skip copy");
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
            winvert4::Logf("DT: notified %zu subscribers", idx);
        }
        else
        {
            winvert4::Log("DT: fullTexture null at notify");
        }
    }

    m_duplication.Reset();
}

void DuplicationThread::RemoveSubscriber(ISubscriber* sub)
{
    if (!sub) return;
    {
        std::lock_guard<std::mutex> lock(m_subMutex);
        m_subscriptions.erase(std::remove_if(m_subscriptions.begin(), m_subscriptions.end(),
                           [sub](const Subscription& s){ return s.Subscriber == sub; }), m_subscriptions.end());
    }
    winvert4::Logf("DT: RemoveSubscriber sub=%p remaining=%zu", (void*)sub, m_subscriptions.size());
    m_subCv.notify_all();
}
