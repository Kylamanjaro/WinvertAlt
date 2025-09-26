#include "pch.h"
#include "OutputManager.h"

OutputManager::OutputManager()
{
}

OutputManager::~OutputManager()
{
}

HRESULT OutputManager::Initialize()
{
    m_duplicationThreads.clear();

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        return hr;
    }

    for (UINT i = 0; ; ++i)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
        {
            break; // No more adapters
        }

        for (UINT j = 0; ; ++j)
        {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(j, &output) == DXGI_ERROR_NOT_FOUND)
            {
                break; // No more outputs for this adapter
            }

            Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
            hr = output.As(&output1);
            if (FAILED(hr))
            {
                continue;
            }

            DXGI_OUTPUT_DESC outputDesc;
            hr = output1->GetDesc(&outputDesc);
            if (FAILED(hr))
            {
                continue;
            }

            std::wstring deviceName = outputDesc.DeviceName;
            auto thread = std::make_unique<DuplicationThread>(adapter.Get(), output1.Get());
            thread->Run();
            m_duplicationThreads[deviceName] = std::move(thread);
        }
    }

    return S_OK;
}

DuplicationThread* OutputManager::GetThreadForRect(const RECT& rc)
{
    DuplicationThread* bestThread = nullptr;
    LONG bestArea = 0;

    for (auto const& [key, val] : m_duplicationThreads)
    {
        const RECT& outputRect = val->GetOutputRect();
        RECT intersection;
        if (IntersectRect(&intersection, &rc, &outputRect))
        {
            LONG area = (intersection.right - intersection.left) * (intersection.bottom - intersection.top);
            if (area > bestArea)
            {
                bestArea = area;
                bestThread = val.get();
            }
        }
    }
    return bestThread;
}
