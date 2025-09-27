#pragma once
#include "pch.h"
#include "Subscription.h"

class DuplicationThread
{
public:
    DuplicationThread(IDXGIAdapter1* adapter, IDXGIOutput1* output);
    ~DuplicationThread();

    void Run();
    void Stop();

    void AddSubscription(const Subscription& sub);
    void RemoveSubscriber(ISubscriber* sub);
    const RECT& GetOutputRect() const { return m_outputRect; }
    ID3D11Device* GetDevice() { return m_device.Get(); }

private:
    void ThreadProc();

    std::thread m_thread;
    std::atomic<bool> m_isRunning = false;

    RECT m_outputRect{};
    ::Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    ::Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    ::Microsoft::WRL::ComPtr<IDXGIOutput1> m_output;
    ::Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;

    std::vector<Subscription> m_subscriptions;
    std::mutex m_subMutex;
    std::condition_variable m_subCv;
    // Shared full-frame texture for this output (sampled by all subscribers)
    ::Microsoft::WRL::ComPtr<ID3D11Texture2D> m_fullTexture;
};
