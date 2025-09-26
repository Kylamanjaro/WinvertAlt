#include "pch.h"
#include "Log.h"
#include "OutputManager.h"

OutputManager::OutputManager()
{
}

OutputManager::~OutputManager()
{
}

HRESULT OutputManager::Initialize()
{    winvert4::Log("OM.Initialize start");
    m_duplicationThreads.clear();

    ::Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { winvert4::Logf("OM.CreateDXGIFactory1 FAILED hr=0x%08X", hr); return hr; }
    winvert4::Log("OM: factory created");

    for (UINT i = 0; ; ++i)
    {
        ::Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        HRESULT hrEnumA = factory->EnumAdapters1(i, &adapter);
        if (hrEnumA == DXGI_ERROR_NOT_FOUND)
        {
            break; // No more adapters
        }
        if (FAILED(hrEnumA))
        {
            winvert4::Logf("OM: EnumAdapters1(%u) failed hr=0x%08X", i, hrEnumA);
            continue;
        }

        DXGI_ADAPTER_DESC1 adesc{};
        adapter->GetDesc1(&adesc);
        winvert4::Logf("OM: Adapter[%u] LUID=%08X:%08X VendorId=%u DeviceId=%u Flags=0x%08X",
            i, adesc.AdapterLuid.HighPart, adesc.AdapterLuid.LowPart, adesc.VendorId, adesc.DeviceId, adesc.Flags);

        for (UINT j = 0; ; ++j)
        {
            ::Microsoft::WRL::ComPtr<IDXGIOutput> output;
            HRESULT hrEnumO = adapter->EnumOutputs(j, &output);
            if (hrEnumO == DXGI_ERROR_NOT_FOUND)
            {
                break; // No more outputs for this adapter
            }
            if (FAILED(hrEnumO))
            {
                winvert4::Logf("OM: EnumOutputs(%u) failed hr=0x%08X", j, hrEnumO);
                continue;
            }

            ::Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
            hr = output.As(&output1);
            if (FAILED(hr))
            {
                winvert4::Logf("OM: output.As(Output1) failed hr=0x%08X", hr);
                continue;
            }

            DXGI_OUTPUT_DESC outputDesc;
            hr = output1->GetDesc(&outputDesc);
            if (FAILED(hr))
            {
                winvert4::Logf("OM: GetDesc failed hr=0x%08X", hr);
                continue;
            }

            std::wstring deviceName = outputDesc.DeviceName;
            winvert4::Logf("OM: Output[%u] name=%ls rect=(%ld,%ld,%ld,%ld)", j, deviceName.c_str(),
                outputDesc.DesktopCoordinates.left, outputDesc.DesktopCoordinates.top,
                outputDesc.DesktopCoordinates.right, outputDesc.DesktopCoordinates.bottom);
            auto thread = std::make_unique<DuplicationThread>(adapter.Get(), output1.Get());
            winvert4::Logf("OM: created DuplicationThread %p", (void*)thread.get());
            thread->Run();
            winvert4::Log("OM: started thread");
            m_duplicationThreads[deviceName] = std::move(thread);
        }
    }

    winvert4::Log("OM.Initialize done");
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
    if (bestThread)
    {
        winvert4::Logf("OM: GetThreadForRect rc=(%ld,%ld,%ld,%ld) -> thread=%p area=%ld",
            rc.left, rc.top, rc.right, rc.bottom, (void*)bestThread, bestArea);
    }
    else
    {
        winvert4::Logf("OM: GetThreadForRect rc=(%ld,%ld,%ld,%ld) -> no match",
            rc.left, rc.top, rc.right, rc.bottom);
    }
    return bestThread;
}

