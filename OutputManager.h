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
    DuplicationThread* GetThreadForRect(const RECT& rc);
    // Enumerate all output sub-rectangles that intersect the given virtual-desktop rect
    void GetIntersectingRects(const RECT& rc, std::vector<RECT>& outRects);

private:
    std::map<std::wstring, std::unique_ptr<DuplicationThread>> m_duplicationThreads;
};
