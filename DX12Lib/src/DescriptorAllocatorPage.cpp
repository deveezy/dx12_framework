#include "Utility.h"
#include <DX12LibPCH.h>

#include <DescriptorAllocatorPage.h>
#include <Application.h>

DescriptorAllocatorPage::DescriptorAllocatorPage(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors) 
    : m_HeapType(type)
    , m_NumDescriptorsInHeap(numDescriptors)
{
    Microsoft::WRL::ComPtr<ID3D12Device2> device = Application::Get().GetDevice();

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = m_HeapType;
    heapDesc.NumDescriptors = m_NumDescriptorsInHeap;

    ThrowIfFailed( device->CreateDescriptorHeap( &heapDesc, IID_PPV_ARGS( &m_d3d12DescriptorHeap ) ) );

    m_BaseDescriptor = m_d3d12DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    m_DescriptorHandleIncrementSize = device->GetDescriptorHandleIncrementSize( m_HeapType );
    m_NumFreeHandles = m_NumDescriptorsInHeap;

    // Initialize the free lists
    AddNewBlock( 0, m_NumFreeHandles );
}

D3D12_DESCRIPTOR_HEAP_TYPE DescriptorAllocatorPage::GetHeapType() const
{
    return m_HeapType;
}

uint32_t DescriptorAllocatorPage::NumFreeHandles() const
{
    return m_NumFreeHandles;
}

bool DescriptorAllocatorPage::HasSpace(uint32_t numDescriptors) const
{
    // lower_bound - return first element that is not less than key. (greater or equal)
    return m_FreeListBySize.lower_bound(numDescriptors) != m_FreeListBySize.end();
}

void DescriptorAllocatorPage::AddNewBlock(uint32_t offset, uint32_t numDescriptors)
{
    auto offsetIter = m_FreeListByOffset.emplace(offset, FreeBlockInfo{numDescriptors});
    auto sizeIter   = m_FreeListBySize.emplace(numDescriptors, offsetIter.first);
    offsetIter.first->second.FreeListBySizeIt = sizeIter;
}
