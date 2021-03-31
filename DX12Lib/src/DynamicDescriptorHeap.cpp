#include "d3dx12.h"
#include <DynamicDescriptorHeap.h>
#include <DX12LibPCH.h>
#include <Application.h>
#include <CommandList.h>
#include <RootSignature.h>
#include <Utility.h>

DynamicDescriptorHeap::DynamicDescriptorHeap(
    D3D12_DESCRIPTOR_HEAP_TYPE heapType,
    uint32_t numDescriptorsPerHeap) 
    : m_DescriptorHeapType(heapType)
    , m_NumDescriptorsPerHeap(numDescriptorsPerHeap)
    , m_DescriptorTableBitMask(0)
    , m_StaleDescriptorTableBitMask(0)
    , m_CurrentCPUDescriptorHandle(D3D12_DEFAULT)
    , m_CurrentGPUDescriptorHandle(D3D12_DEFAULT)
    , m_NumFreeHandles(0)
{
    m_DescriptorHandleIncrementSize = Application::Get().GetDescriptorHandleIncrementSize(heapType);

    // Allocate space for staging CPU visible descriptors.
    m_DescriptorHandleCache = std::make_unique<D3D12_CPU_DESCRIPTOR_HANDLE[]>(m_NumDescriptorsPerHeap);
}

DynamicDescriptorHeap::~DynamicDescriptorHeap() {}

void DynamicDescriptorHeap::ParseRootSignature(const RootSignature& rootSignature) 
{
    // If the root signature changes, all descriptors 
    // must be (re)bound to the command list.
    m_StaleDescriptorTableBitMask = 0;

    const D3D12_ROOT_SIGNATURE_DESC1& rootSignatureDesc = rootSignature.GetRootSignatureDesc();

    // Get a bit mask that represents the root parameter indices that match the
    // descriptor heap type for this dynamic descriptor heap.
    m_DescriptorTableBitMask = rootSignature.GetDescriptorTableBitMask(m_DescriptorHeapType);
    uint32_t descriptorTableBitMask = m_DescriptorTableBitMask;

    uint32_t currentOffset = 0;
    DWORD rootIndex;
    while (_BitScanForward(&rootIndex, descriptorTableBitMask) && rootIndex < rootSignatureDesc.NumParameters)
    {
        uint32_t numDescriptors = rootSignature.GetNumDescriptors(rootIndex);
        DescriptorTableCache& descriptorTableCache = m_DescriptorTableCache[rootIndex];
        descriptorTableCache.NumDescriptors = numDescriptors;
        descriptorTableCache.BaseDescriptor = m_DescriptorHandleCache.get() + currentOffset;

        currentOffset += numDescriptors;

        // Flip the descriptor table bit so it's not scanned again for the current index.
        descriptorTableBitMask ^= (1 << rootIndex);
    }
    // Make sure the maximum number of descriptors per descriptor heap has not been exceeded.
    assert(currentOffset <= m_NumDescriptorsPerHeap &&
        "The root signature requires more than the maximum number of descriptors per descriptor heap. Consider increasing the maximum number of descriptors per descriptor heap.");
}

void DynamicDescriptorHeap::StageDescriptors(
    uint32_t rootParameterIndex,
    uint32_t offset, 
    uint32_t numDescriptors, 
    const D3D12_CPU_DESCRIPTOR_HANDLE srcDescriptors) 
{
    // Cannot stage more than the maximum number of descriptors per heap.
    // Cannot stage more than MaxDescriptorTables root parameters.
    if (numDescriptors > m_NumDescriptorsPerHeap || rootParameterIndex >= MaxDescriptorTables)
    {
        throw std::bad_alloc();
    }

    DescriptorTableCache& descriptorTableCache = m_DescriptorTableCache[rootParameterIndex];

    // Check that the number of descriptors to copy does not exceed the number
    // of descriptors expected in the descriptor table.
    if ((offset + numDescriptors) > descriptorTableCache.NumDescriptors)
    {
        throw std::length_error("Number of descriptors exceeds the number of descriptors in the descriptor table.");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE* dstDescriptor = (descriptorTableCache.BaseDescriptor + offset);
    for (uint32_t i = 0; i < numDescriptors; ++i)
    {
        dstDescriptor[i] = CD3DX12_CPU_DESCRIPTOR_HANDLE(srcDescriptors, i, m_DescriptorHandleIncrementSize);
    }

    // Set the root parameter index bit to make sure the descriptor table
    // at the index is bound to the command list.
    m_StaleDescriptorTableBitMask |= (1 << rootParameterIndex);
}
    
uint32_t DynamicDescriptorHeap::ComputeStaleDescriptorCount() const
{
    uint32_t numStaleDescriptors = 0;
    DWORD i;
    DWORD staleDescriptorsBitMask = m_StaleDescriptorTableBitMask;

    while (_BitScanForward(&i, staleDescriptorsBitMask))
    {
        numStaleDescriptors += m_DescriptorTableCache[i].NumDescriptors;
        staleDescriptorsBitMask ^= (1 << i);
    }
    return numStaleDescriptors;
}

ComPtr<ID3D12DescriptorHeap> DynamicDescriptorHeap::RequestDescriptorHeap() 
{
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    if (!m_AvailableDescriptorHeaps.empty())
    {
        descriptorHeap = m_AvailableDescriptorHeaps.front();
        m_AvailableDescriptorHeaps.pop();
    }
    else
    {
        descriptorHeap = CreateDescriptorHeap();
        m_DescriptorHeapPool.push(descriptorHeap);
    }
    return descriptorHeap;
}

ComPtr<ID3D12DescriptorHeap> DynamicDescriptorHeap::CreateDescriptorHeap() 
{
    const ComPtr<ID3D12Device2> device = Application::Get().GetDevice();
    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
    descriptorHeapDesc.Type = m_DescriptorHeapType;
    descriptorHeapDesc.NumDescriptors = m_NumDescriptorsPerHeap;
    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    ThrowIfFailed(device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&descriptorHeap)));

    return descriptorHeap;
}

void DynamicDescriptorHeap::CommitStagedDescriptors(CommandList& commandList, 
    std::function<void(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)> setFunc) 
{
    // Compute the number of descriptors that need to be copied.
    const uint32_t numDescriptorsToCommit = ComputeStaleDescriptorCount();

    if (numDescriptorsToCommit > 0)
    {
        const ComPtr<ID3D12Device2> device = Application::Get().GetDevice();
        auto graphicsCommandList = commandList.GetGraphicsCommandList().Get(); 
        ASSERT(graphicsCommandList != nullptr);

        if (!m_CurrentDescriptorHeap || m_NumFreeHandles < numDescriptorsToCommit)
        {
            m_CurrentDescriptorHeap = RequestDescriptorHeap();
            m_CurrentCPUDescriptorHandle = m_CurrentDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
            m_CurrentGPUDescriptorHandle = m_CurrentDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
            m_NumFreeHandles = m_NumDescriptorsPerHeap;

            commandList.SetDescriptorHeap(m_DescriptorHeapType, m_CurrentDescriptorHeap.Get());

            // When updating the descriptor heap on the command list, all descriptor
            // tables must be (re)recopied to the new descriptor heap (not just
            // the stale descriptor tables).
            m_StaleDescriptorTableBitMask = m_DescriptorTableBitMask;
        }

        DWORD rootIndex;

        // Scan from LSB to MSB for a bit set in staleDescriptorsBitMask
        while (_BitScanForward(&rootIndex, m_StaleDescriptorTableBitMask))
        {
            UINT numSrcDescriptors = m_DescriptorTableCache[rootIndex].NumDescriptors;
            D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorHandles = m_DescriptorTableCache[rootIndex].BaseDescriptor;

            D3D12_CPU_DESCRIPTOR_HANDLE pDestDescriptorRangeStarts[] =
            {
                m_CurrentCPUDescriptorHandle
            };
            UINT pDestDescriptorRangeSizes[] =
            {
                numSrcDescriptors
            };

            // Copy the staged CPU visible descriptors to the GPU visible descriptor heap.
            device->CopyDescriptors(1, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                numSrcDescriptors, pSrcDescriptorHandles, nullptr, m_DescriptorHeapType);

            // Set the descriptors on the command list using the passed-in setter function.
            setFunc(graphicsCommandList, rootIndex, m_CurrentGPUDescriptorHandle);

            // Offset current CPU and GPU descriptor handles.
            m_CurrentCPUDescriptorHandle.Offset(numSrcDescriptors, m_DescriptorHandleIncrementSize);
            m_CurrentGPUDescriptorHandle.Offset(numSrcDescriptors, m_DescriptorHandleIncrementSize);
            m_NumFreeHandles -= numSrcDescriptors;

            // Flip the stale bit so the descriptor table is not recopied again unless it is updated with a new descriptor.
            m_StaleDescriptorTableBitMask ^= (1 << rootIndex);
        }
    }
}

void DynamicDescriptorHeap::CommitStagedDescriptorsForDraw(CommandList& commandList) 
{
    CommitStagedDescriptors(commandList, &ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable);
}

void DynamicDescriptorHeap::CommitStagedDescriptorsForDispatch(CommandList& commandList) 
{
    CommitStagedDescriptors(commandList, &ID3D12GraphicsCommandList::SetComputeRootDescriptorTable);
}

D3D12_GPU_DESCRIPTOR_HANDLE DynamicDescriptorHeap::CopyDescriptor(CommandList& commandList, D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor) 
{
    if (!m_CurrentDescriptorHeap || m_NumFreeHandles < 1)
    {
        m_CurrentDescriptorHeap = RequestDescriptorHeap();
        m_CurrentCPUDescriptorHandle = m_CurrentDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        m_CurrentGPUDescriptorHandle = m_CurrentDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
        m_NumFreeHandles = m_NumDescriptorsPerHeap;

        commandList.SetDescriptorHeap(m_DescriptorHeapType, m_CurrentDescriptorHeap.Get());

        // When updating the descriptor heap on the command list, all descriptor
        // tables must be (re)recopied to the new descriptor heap (not just
        // the stale descriptor tables).
        m_StaleDescriptorTableBitMask = m_DescriptorTableBitMask;
    }

    const ComPtr<ID3D12Device2> device = Application::Get().GetDevice();
    D3D12_GPU_DESCRIPTOR_HANDLE hGPU = m_CurrentGPUDescriptorHandle;
    device->CopyDescriptorsSimple(1, m_CurrentCPUDescriptorHandle, cpuDescriptor, m_DescriptorHeapType);

    m_CurrentCPUDescriptorHandle.Offset(1, m_DescriptorHandleIncrementSize);
    m_CurrentGPUDescriptorHandle.Offset(1, m_DescriptorHandleIncrementSize);
    m_NumFreeHandles -= 1;

    return hGPU;
}

void DynamicDescriptorHeap::Reset() 
{
    m_AvailableDescriptorHeaps = m_DescriptorHeapPool;
    m_CurrentDescriptorHeap.Reset();
    m_CurrentCPUDescriptorHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(D3D12_DEFAULT);
    m_CurrentGPUDescriptorHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(D3D12_DEFAULT);
    m_NumFreeHandles = 0;
    m_DescriptorTableBitMask = 0;
    m_StaleDescriptorTableBitMask = 0;

    // Reset the table cache 
    for (int i = 0; i < MaxDescriptorTables; ++i)
    {
        m_DescriptorTableCache[i].Reset();
    }
}

// void sfjkafja()
// {
//     CD3DX12_DESCRIPTOR_RANGE1 DescRange[6];

//     DescRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,6,2); // t2-t7
//     DescRange[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0); // u0-u3
//     DescRange[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,2,0); // s0-s1
//     DescRange[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,-1,8, 0,
//                     D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE); // t8-unbounded
//     DescRange[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,-1,0,1,
//                     D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE); 
//                                                                 // (t0,space1)-unbounded
//     DescRange[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,1,
//                     D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC); // b1

//     CD3DX12_ROOT_PARAMETER1 RP[7];

//     RP[0].InitAsConstants(3,2); // 3 constants at b2
//     RP[1].InitAsDescriptorTable(2,&DescRange[0]); // 2 ranges t2-t7 and u0-u3
//     RP[2].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC); // b0
//     RP[3].InitAsDescriptorTable(1,&DescRange[2]); // s0-s1
//     RP[4].InitAsDescriptorTable(1,&DescRange[3]); // t8-unbounded
//     RP[5].InitAsDescriptorTable(1,&DescRange[4]); // (t0,space1)-unbounded
//     RP[6].InitAsDescriptorTable(1,&DescRange[5]); // b1 
//     CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[1];
//     StaticSamplers[0].Init(3, D3D12_FILTER_ANISOTROPIC); // s3
//     CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSig(7,RP,1,StaticSamplers);
//     ID3DBlob* pSerializedRootSig;
//     HRESULT hr = (D3D12SerializeVersionedRootSignature(&RootSig,pSerializedRootSig)); 

//     ID3D12RootSignature* pRootSignature;
//     hr = CheckHR(pDevice->CreateRootSignature(
//         pSerializedRootSig->GetBufferPointer(),pSerializedRootSig->GetBufferSize(),
//         __uuidof(ID3D12RootSignature),
//         &pRootSignature));
// }

