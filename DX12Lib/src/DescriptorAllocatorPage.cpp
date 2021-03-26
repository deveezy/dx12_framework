#include <DescriptorAllocatorPage.h>
#include <Application.h>
#include <d3dx12.h>
#include <DX12LibPCH.h>

#include <mutex>

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

DescriptorAllocation DescriptorAllocatorPage::Allocate(uint32_t numDescriptors) 
{
    std::lock_guard<std::mutex> lock(m_AllocationMutex);

    // There are less requsted number of descriptors left in the heap.
    // Return a NULL descriptor and try another heap.
    if ( numDescriptors > m_NumDescriptorsInHeap)
    {
        return DescriptorAllocation();
    }

    // Get the first block that is large enough to satisfy the request.
    auto smallestBlockIter = m_FreeListBySize.lower_bound( numDescriptors );
    if (smallestBlockIter == m_FreeListBySize.end())
    {
        // There was no free block that could satisfy the request.
        return DescriptorAllocation();
    }
    
    // The size of the smallest block that satisfies the request.
    uint32_t blockSize = smallestBlockIter->first;

    // The pointer to the same entry in the FreeListByOffset map.
    auto offsetIter = smallestBlockIter->second;

    // The offset in the descriptor heap.
    uint32_t offset = offsetIter->first;

    // Remove existing free block from the free list.
    m_FreeListBySize.erase(smallestBlockIter);
    m_FreeListByOffset.erase(offsetIter);

    // Compute the new free block that results from splitting this block.
    uint32_t newOffset = offset + numDescriptors;
    uint32_t newSize   = blockSize - numDescriptors;

    if ( newSize > 0)
    {
        // If the allocation didn't exactly match the requested size,
        // return the left-over to the free list.
        AddNewBlock(newOffset, newSize);
    }

    // Decrement free handles.
    m_NumFreeHandles -= numDescriptors;

    return DescriptorAllocation(
        CD3DX12_CPU_DESCRIPTOR_HANDLE(m_BaseDescriptor, offset, m_DescriptorHandleIncrementSize),
        numDescriptors, m_DescriptorHandleIncrementSize, shared_from_this());
}

uint32_t DescriptorAllocatorPage::ComputeOffset(D3D12_CPU_DESCRIPTOR_HANDLE handle) 
{
    return static_cast<uint32_t>(handle.ptr - m_BaseDescriptor.ptr) / m_DescriptorHandleIncrementSize;
}

void DescriptorAllocatorPage::Free(DescriptorAllocation&& descriptor, uint64_t frameNumber) 
{
    // Compute the offset of the descriptor within descriptor heap.
    uint32_t offset = ComputeOffset(descriptor.GetDescriptorHandle());

    std::lock_guard<std::mutex> lock(m_AllocationMutex);
    
    // Don't add the block directly to the free list until the frame has completed.
    m_StaleDescriptors.emplace(offset, descriptor.GetNumHandles(), frameNumber);
}

void DescriptorAllocatorPage::FreeBlock(uint32_t offset, uint32_t numDescriptors) 
{
    // Find the first element whose offset is greater than the specified offset.
    // This is the block that should appear after the block that is being freed.
    auto nextBlockIter = m_FreeListByOffset.upper_bound(offset);

    // Find the block that appears before the block being freed.
    auto prevBlockIter = nextBlockIter;
    
    // If it's not the first block in the list.
    if (prevBlockIter != m_FreeListByOffset.begin())
    {
        // Go to the previuos block in the list.
        --prevBlockIter;
    }
    else 
    {
        // Otherwise, just set it to the end of the list to indicate that no
        // block comes before the one being freed.
        prevBlockIter = m_FreeListByOffset.end();
    }

    // Add the number of free handles back to the heap.
    // This needs to be done before merging any blocks since merging
    // blocks modifies the numDescriptors variable.
    m_NumFreeHandles += numDescriptors;

    if ( prevBlockIter != m_FreeListByOffset.end() &&
        offset == prevBlockIter->first + prevBlockIter->second.Size )
    {
        // The previous block is exactly behind the block that is to be freed.
        //
        // PrevBlock.Offset           Offset
        // |                          |
        // |<-----PrevBlock.Size----->|<------Size-------->|
        //
    
        // Increase the block size by the size of merging with the previous block.
        offset = prevBlockIter->first;
        numDescriptors += prevBlockIter->second.Size;
    
        // Remove the previous block from the free list.
        m_FreeListBySize.erase( prevBlockIter->second.FreeListBySizeIt );
        m_FreeListByOffset.erase( prevBlockIter );
    }
   
    if ( nextBlockIter != m_FreeListByOffset.end() &&
        offset + numDescriptors == nextBlockIter->first )
    {
        // The next block is exactly in front of the block that is to be freed.
        //
        // Offset               NextBlock.Offset 
        // |                    |
        // |<------Size-------->|<-----NextBlock.Size----->|
    
        // Increase the block size by the size of merging with the next block.
        numDescriptors += nextBlockIter->second.Size;
    
        // Remove the next block from the free list.
        m_FreeListBySize.erase( nextBlockIter->second.FreeListBySizeIt );
        m_FreeListByOffset.erase( nextBlockIter );
    }

    AddNewBlock(offset, numDescriptors);
}

void DescriptorAllocatorPage::ReleaseStaleDescriptors(uint64_t frameNumber) 
{
    std::lock_guard<std::mutex> lock(m_AllocationMutex);

    while (!m_StaleDescriptors.empty())
    {
        StaleDescriptorInfo &staleDescriptor = m_StaleDescriptors.front();

        // The offset of the descriptor in the heap.
        uint32_t offset = staleDescriptor.Offset;
        uint32_t numDescriptors = staleDescriptor.Size;

        FreeBlock(offset, numDescriptors);
        m_StaleDescriptors.pop();
    }
}