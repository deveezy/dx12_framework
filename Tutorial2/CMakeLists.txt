cmake_minimum_required( VERSION 3.10.1 ) # Latest version of CMake when this file was created.

set( HEADER_FILES
    inc/Tutorial2.h
)

set( SRC_FILES
    src/main.cpp
    src/Tutorial2.cpp
)

set( SHADER_FILES
    shaders/VertexShader.hlsl
    shaders/PixelShader.hlsl
)

# source_group( "Resources\\Shaders" FILES ${SHADER_FILES} )

set_source_files_properties( shaders/VertexShader.hlsl PROPERTIES 
    VS_SHADER_TYPE Vertex
    VS_SHADER_MODEL 5.1
)

set_source_files_properties( shaders/PixelShader.hlsl PROPERTIES 
    VS_SHADER_TYPE Pixel
    VS_SHADER_MODEL 5.1
)

add_executable( Tutorial2 WIN32
    ${HEADER_FILES} 
    ${SRC_FILES}
    ${SHADER_FILES})

target_include_directories( Tutorial2
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)

target_link_libraries( Tutorial2
    DX12Lib
    D3DCompiler.lib
    Shlwapi.lib
)

set(FXC "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.18362.0\\x64\\fxc.exe")

# generate shader output
function(netcode_compile_shader var_binary_path source_path shader_type entry_point)
  get_filename_component(source_filename ${source_path} NAME_WE)
  get_filename_component(source_path "${source_path}" ABSOLUTE)
  set(binary_path "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${source_filename}.cso")
  add_custom_command(OUTPUT ${binary_path}
    COMMENT "Generating ${binary_path}..."
    COMMAND ${FXC} /T \"${shader_type}\" /E\"${entry_point}\" /Od /Zi /Fo \"${binary_path}\" \"${source_path}\"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS "${source_path}"
  )
  set(${var_binary_path} "${binary_path}" PARENT_SCOPE)
endfunction()

# generate vs shader output
macro(netcode_compile_vs var_binary_path source_path)
  netcode_compile_shader(${var_binary_path} ${source_path} "vs_5_1" "main")
endmacro()
macro(netcode_compile_ps var_binary_path source_path)
  netcode_compile_shader(${var_binary_path} ${source_path} "ps_5_1" "main")
endmacro()

# create shader compiling target
set(NETCODE_VS 
  "shaders/VertexShader.hlsl")

set(NETCODE_PS 
  "shaders/PixelShader.hlsl"
)
foreach(vs ${NETCODE_VS})
  netcode_compile_vs(vs_out ${vs})
  list(APPEND NETCODE_VS_OUT ${vs_out})
endforeach()

foreach(ps ${NETCODE_PS})
  netcode_compile_ps(ps_out ${ps})
  list(APPEND NETCODE_PS_OUT ${ps_out})
endforeach()

add_custom_target(NetcodeClientShaders DEPENDS ${NETCODE_VS_OUT} ${NETCODE_PS_OUT})