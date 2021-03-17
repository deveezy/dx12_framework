#include <DX12LibPCH.h>
#include <Application.h>
#include <CommandQueue.h>
#include <Window.h>
#include <Game.h>

Window::Window(HWND hWnd, const std::wstring& windowName, int clientWidth, int clientHeight, bool vSync )
    : m_hWnd(hWnd)
    , m_WindowName(windowName)
    , m_ClientWidth(clientWidth)
    , m_ClientHeight(clientHeight)
    , m_VSync(vSync)
    , m_Fullscreen(false)
    , m_FrameCounter(0)
{
    Application& app = Application::Get();

    m_IsTearingSupported = app.IsTearingSupported();

    m_dxgiSwapChain = CreateSwapChain();
    m_d3d12RTVDescriptorHeap = app.CreateDescriptorHeap(BufferCount, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_RTVDescriptorSize = app.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    UpdateRenderTargetViews();
}
