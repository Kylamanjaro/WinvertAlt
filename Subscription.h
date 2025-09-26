#pragma once
#include "pch.h"

// Interface for an object that can receive desktop frames
struct ISubscriber
{
    virtual ~ISubscriber() = default;
    virtual void OnFrameReady(::Microsoft::WRL::ComPtr<ID3D11Texture2D> texture) = 0;
};

// Represents a request for a specific region of the desktop
struct Subscription
{
    ISubscriber* Subscriber = nullptr;
    RECT Region{};
};

