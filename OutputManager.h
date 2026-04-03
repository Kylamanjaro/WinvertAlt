#pragma once
#include "pch.h"
#include "DuplicationThread.h"
#include "Subscription.h"
#include <string>
#include <vector>

class OutputManager
{
public:
    OutputManager();
    ~OutputManager();

    HRESULT Initialize();
    // Prepare duplication threads ahead of first region creation so capture
    // is warm when selection completes.
    void PrewarmForSelection();
    DuplicationThread* GetThreadForRect(const RECT& rc);
    // Enumerate all output sub-rectangles that intersect the given virtual-desktop rect
    void GetIntersectingRects(const RECT& rc, std::vector<RECT>& outRects);

private:
    // Lazy-created duplication threads (one per output)
    std::map<std::wstring, std::unique_ptr<DuplicationThread>> m_duplicationThreads;
    // Lightweight cache of output rectangles (populated without creating D3D devices)
    std::vector<RECT> m_outputRects;
    bool m_outputsEnumerated{ false };
    bool m_threadsInitialized{ false };

    void EnumerateOutputs_();
    void EnsureThreadsCreated_();
};
