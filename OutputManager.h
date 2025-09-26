#pragma once
#include "pch.h"
#include "DuplicationThread.h"
#include "Subscription.h"
#include <string>

class OutputManager
{
public:
    OutputManager();
    ~OutputManager();

    HRESULT Initialize();
    DuplicationThread* GetThreadForRect(const RECT& rc);

private:
    std::map<std::wstring, std::unique_ptr<DuplicationThread>> m_duplicationThreads;
};
