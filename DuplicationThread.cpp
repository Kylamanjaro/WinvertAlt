#include "pch.h"
#include "DuplicationThread.h"

DuplicationThread::DuplicationThread(IDXGIAdapter1* adapter, IDXGIOutput1* output)
    : m_output(output)
{
    DXGI_OUTPUT_DESC desc;
    m_output->GetDesc(&desc);
    m_outputRect = desc.DesktopCoordinates;

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &m_device, &featureLevel, &m_context);
    if (FAILED(hr))
    {
        // Handle error - for now, we'll just have a null device
    }
}

DuplicationThread::~DuplicationThread()
{
    Stop();
}

void DuplicationThread::Run()
{
    if (!m_isRunning)
    {
        m_isRunning = true;
        m_thread = std::thread(&DuplicationThread::ThreadProc, this);
    }
}

void DuplicationThread::Stop()
{
    m_isRunning = false;
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void DuplicationThread::AddSubscription(const Subscription& sub)
{
    // TODO: Add thread-safety (mutex)
    m_subscriptions.push_back(sub);
}

void DuplicationThread::ThreadProc()
{
    if (!m_device)
    {
        return;
    }

    HRESULT hr = m_output->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr))
    {
        return;
    }

    while (m_isRunning)
    {
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        Microsoft::WRL::ComPtr<IDXGIResource> resource;
        hr = m_duplication->AcquireNextFrame(500, &frameInfo, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            continue;
        }
        else if (FAILED(hr))
        {
            m_duplication.Reset();
            m_output->DuplicateOutput(m_device.Get(), &m_duplication);
            continue;
        }

        if (m_subscriptions.empty())
        {
            m_duplication->ReleaseFrame();
            continue;
        }

        OutputDebugStringA("DuplicationThread: Acquired frame, processing subscriptions\n");

        Microsoft::WRL::ComPtr<ID3D11Texture2D> capturedTexture;
        resource.As(&capturedTexture);

        for (const auto& sub : m_subscriptions)
        {
            RECT subRect = sub.Region;
            RECT outputRect = GetOutputRect();
            RECT intersection;
            if (!IntersectRect(&intersection, &subRect, &outputRect)) continue;

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = intersection.right - intersection.left;
            desc.Height = intersection.bottom - intersection.top;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTexture;
            hr = m_device->CreateTexture2D(&desc, nullptr, &targetTexture);
            if (FAILED(hr)) 
            {
                OutputDebugStringA("DuplicationThread: Failed to create target texture\n");
                continue; 
            }

            D3D11_BOX sourceRegion;
            sourceRegion.left = intersection.left - outputRect.left;
            sourceRegion.right = intersection.right - outputRect.left;
            sourceRegion.top = intersection.top - outputRect.top;
            sourceRegion.bottom = intersection.bottom - outputRect.top;
            sourceRegion.front = 0;
            sourceRegion.back = 1;

            m_context->CopySubresourceRegion(targetTexture.Get(), 0, 0, 0, 0, capturedTexture.Get(), 0, &sourceRegion);

            OutputDebugStringA("DuplicationThread: Notifying subscriber\n");
            sub.Subscriber->OnFrameReady(targetTexture);
        }

        m_duplication->ReleaseFrame();
    }

    m_duplication.Reset();
}