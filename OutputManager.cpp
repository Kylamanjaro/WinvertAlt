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
{
    // Lightweight init: just enumerate output rectangles; no devices, no threads.
    EnumerateOutputs_();
    return S_OK;
}

void OutputManager::PrewarmForSelection()
{
    // Refresh monitor list and ensure duplication threads are created before
    // the user completes selection. This shifts one-time setup cost earlier.
    m_outputsEnumerated = false;
    EnumerateOutputs_();
    EnsureThreadsCreated_();
}

void OutputManager::EnumerateOutputs_()
{
    if (m_outputsEnumerated) return;
    winvert4::Log("OM: EnumerateOutputs start");
    m_outputRects.clear();
    ::Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { winvert4::Logf("OM.CreateDXGIFactory1 FAILED hr=0x%08X", hr); return; }
    for (UINT i = 0; ; ++i)
    {
        ::Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        HRESULT hrEnumA = factory->EnumAdapters1(i, &adapter);
        if (hrEnumA == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hrEnumA)) continue;
        for (UINT j = 0; ; ++j)
        {
            ::Microsoft::WRL::ComPtr<IDXGIOutput> output;
            HRESULT hrEnumO = adapter->EnumOutputs(j, &output);
            if (hrEnumO == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hrEnumO)) continue;
            ::Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1))) continue;
            DXGI_OUTPUT_DESC outputDesc{};
            if (FAILED(output1->GetDesc(&outputDesc))) continue;
            m_outputRects.push_back(outputDesc.DesktopCoordinates);
        }
    }
    m_outputsEnumerated = true;
    winvert4::Logf("OM: EnumerateOutputs done; %zu outputs", m_outputRects.size());
}

void OutputManager::EnsureThreadsCreated_()
{
    if (m_threadsInitialized) return;
    winvert4::Log("OM: ensuring duplication threads for current outputs");
    ::Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { winvert4::Logf("OM.CreateDXGIFactory1 FAILED hr=0x%08X", hr); return; }
    for (UINT i = 0; ; ++i)
    {
        ::Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        HRESULT hrEnumA = factory->EnumAdapters1(i, &adapter);
        if (hrEnumA == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hrEnumA)) continue;
        for (UINT j = 0; ; ++j)
        {
            ::Microsoft::WRL::ComPtr<IDXGIOutput> output;
            HRESULT hrEnumO = adapter->EnumOutputs(j, &output);
            if (hrEnumO == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hrEnumO)) continue;
            ::Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1))) continue;
            DXGI_OUTPUT_DESC outputDesc{};
            if (FAILED(output1->GetDesc(&outputDesc))) continue;
            std::wstring deviceName = outputDesc.DeviceName;
            if (m_duplicationThreads.find(deviceName) == m_duplicationThreads.end())
            {
                winvert4::Logf("OM: creating thread for output %ls", deviceName.c_str());
                auto thread = std::make_unique<DuplicationThread>(adapter.Get(), output1.Get());
                thread->Run();
                m_duplicationThreads[deviceName] = std::move(thread);
            }
        }
    }
    m_threadsInitialized = true;
}

DuplicationThread* OutputManager::GetThreadForRect(const RECT& rc)
{
    // Lazily create threads when the first effect window is shown.
    // Avoid re-enumerating outputs/threads on every call.
    auto findBest = [this, &rc](LONG& outArea) -> DuplicationThread*
    {
        DuplicationThread* bestThread = nullptr;
        LONG bestArea = 0;
        for (auto const& [key, val] : m_duplicationThreads)
        {
            const RECT& outputRect = val->GetOutputRect();
            RECT intersection{};
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
        outArea = bestArea;
        return bestThread;
    };

    if (m_duplicationThreads.empty())
    {
        EnsureThreadsCreated_();
    }
    LONG bestArea = 0;
    DuplicationThread* bestThread = findBest(bestArea);
    if (!bestThread)
    {
        // Display topology can change while stale duplication threads remain.
        // Rebuild once and retry thread selection.
        winvert4::Log("OM: no thread match; rebuilding duplication threads and retrying");
        m_duplicationThreads.clear();
        m_threadsInitialized = false;
        EnsureThreadsCreated_();
        bestThread = findBest(bestArea);
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

void OutputManager::GetIntersectingRects(const RECT& rc, std::vector<RECT>& outRects)
{
    outRects.clear();

    // Always refresh lightweight output rectangles so monitor hot-plug/reorder
    // is reflected immediately in region splitting.
    m_outputsEnumerated = false;
    EnumerateOutputs_();
    for (auto const& rOut : m_outputRects)
    {
        RECT intersection{};
        if (IntersectRect(&intersection, &rc, &rOut))
        {
            outRects.push_back(intersection);
        }
    }
    if (outRects.empty())
    {
        // Fallback: no outputs reported; return the original rect
        outRects.push_back(rc);
    }
}
