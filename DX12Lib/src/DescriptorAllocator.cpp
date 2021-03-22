#include <DescriptorAllocator.h>
#include <DescriptorAllocatorPage.h>
#include <DX12LibPCH.h>

DescriptorAllocator::DescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptorsPerHeap)
    : m_HeapType(type)
    , m_NumDescriptorsPerHeap(numDescriptorsPerHeap) {} 

DescriptorAllocator::~DescriptorAllocator() {}

DescriptorAllocation DescriptorAllocator::Allocate(uint32_t numDescriptors)
{
    std::lock_guard<std::mutex> lock( m_AllocationMutex );

    DescriptorAllocation allocation;

    for ( auto iter = m_AvailableHeaps.begin(); iter != m_AvailableHeaps.end(); ++iter )
    {
        std::shared_ptr<DescriptorAllocatorPage> allocatorPage = m_HeapPool[*iter];

        allocation = allocatorPage->Allocate( numDescriptors );

        if ( allocatorPage->NumFreeHandles() == 0 )
        {
            iter = m_AvailableHeaps.erase( iter );
        }

        // A valid allocation has been found.
        if ( !allocation.IsNull() )
        {
            break;
        }
    }

    // No available heap could satisfy the requested number of descriptors.
    if ( allocation.IsNull() )
    {
        m_NumDescriptorsPerHeap = std::max( m_NumDescriptorsPerHeap, numDescriptors );
        std::shared_ptr<DescriptorAllocatorPage> newPage = CreateAllocatorPage();

        allocation = newPage->Allocate( numDescriptors );
    }

    return allocation;
}

std::shared_ptr<DescriptorAllocatorPage> DescriptorAllocator::CreateAllocatorPage()
{
    std::shared_ptr<DescriptorAllocatorPage> newPage = 
        std::make_shared<DescriptorAllocatorPage>( m_HeapType, m_NumDescriptorsPerHeap );

    m_HeapPool.emplace_back( newPage );
    m_AvailableHeaps.insert( m_HeapPool.size() - 1 );

    return newPage;
}

void DescriptorAllocator::ReleaseStaleDescriptors( uint64_t frameNumber )
{
    std::lock_guard<std::mutex> lock( m_AllocationMutex );
 
    for ( size_t i = 0; i < m_HeapPool.size(); ++i )
    {
        std::shared_ptr<DescriptorAllocatorPage> page = m_HeapPool[i];
 
        page->ReleaseStaleDescriptors( frameNumber );
 
        if ( page->NumFreeHandles() > 0 )
        {
            m_AvailableHeaps.insert( i );
        }
    }
}