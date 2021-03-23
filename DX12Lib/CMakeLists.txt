cmake_minimum_required( VERSION 3.10.1 ) 

set( HEADER_FILES 
    inc/DX12LibPCH.h
    inc/Application.h
    inc/CommandQueue.h
    inc/Game.h
    inc/Utility.h
    inc/HighResolutionClock.h
    resource.h
    inc/Window.h
    inc/d3dx12.h
    inc/KeyCodes.h
    inc/Events.h
    inc/UploadBuffer.h
    inc/Defines.h
    inc/DescriptorAllocator.h
    inc/DescriptorAllocatorPage.h
    inc/DescriptorAllocation.h
)

set( SOURCE_FILES
    src/DX12LibPCH.cpp
    src/Application.cpp
    src/CommandQueue.cpp
    src/Game.cpp
    src/HighResolutionClock.cpp
    src/Window.cpp
    src/Utility.cpp
    src/UploadBuffer.cpp
    src/DescriptorAllocator.cpp
    src/DescriptorAllocatorPage.cpp
    src/DescriptorAllocation.cpp
)

add_library( DX12Lib STATIC
    ${HEADER_FILES}
    ${SOURCE_FILES})

target_include_directories( DX12Lib PUBLIC inc)

target_link_libraries( DX12Lib 
    PUBLIC d3d12.lib
    PUBLIC dxgi.lib
    PUBLIC dxguid.lib
)
