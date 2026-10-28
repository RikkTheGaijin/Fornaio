cmake_minimum_required(VERSION 3.24)
project(Fornaio VERSION 1.3.0 LANGUAGES C CXX)
add_compile_definitions(FORNAIO_VERSION="${PROJECT_VERSION}")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network OpenGL OpenGLWidgets Test)
find_package(CUDAToolkit REQUIRED)
add_library(ufbx STATIC third_party/ufbx/ufbx.c)
target_include_directories(ufbx PUBLIC third_party/ufbx)
add_library(mikktspace STATIC third_party/mikktspace/mikktspace.c)
target_include_directories(mikktspace PUBLIC third_party/mikktspace)
add_library(baker STATIC src/core.cpp src/gpu.cpp src/transfer.cpp src/transfer_gpu.cpp src/fbx_binary.cpp
 src/uv_pack.cpp src/uv2.cpp)
target_include_directories(baker PUBLIC src PRIVATE third_party/optix/include third_party/stb)
target_link_libraries(baker PUBLIC Qt6::Core Qt6::Gui ufbx mikktspace CUDA::cuda_driver CUDA::cudart_static)
# cudart_static.lib requests the static CRT (LIBCMT); keep everything on the DLL CRT.
target_link_options(baker INTERFACE /NODEFAULTLIB:LIBCMT)
target_compile_definitions(baker PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set(PTX ${CMAKE_CURRENT_BINARY_DIR}/bake.ptx)
set(TRANSFER_PTX ${CMAKE_CURRENT_BINARY_DIR}/transfer.ptx)
add_custom_command(OUTPUT ${PTX}
 COMMAND ${CUDAToolkit_NVCC_EXECUTABLE} --ptx -std=c++17 -O3 --use_fast_math -arch=compute_75
 -I${CMAKE_CURRENT_SOURCE_DIR}/third_party/optix/include -I${CMAKE_CURRENT_SOURCE_DIR}/src
 ${CMAKE_CURRENT_SOURCE_DIR}/src/kernel.cu -o ${PTX}
 DEPENDS src/kernel.cu src/shared.h VERBATIM)
# No --use_fast_math: sRGB and normal re-encoding need full-precision pow/exp.
add_custom_command(OUTPUT ${TRANSFER_PTX}
 COMMAND ${CUDAToolkit_NVCC_EXECUTABLE} --ptx -std=c++17 -O3 -arch=compute_75
 -I${CMAKE_CURRENT_SOURCE_DIR}/third_party/optix/include -I${CMAKE_CURRENT_SOURCE_DIR}/src
 ${CMAKE_CURRENT_SOURCE_DIR}/src/transfer_kernel.cu -o ${TRANSFER_PTX}
 DEPENDS src/transfer_kernel.cu src/transfer_shared.h src/shared.h VERBATIM)
add_custom_target(device_code ALL DEPENDS ${PTX} ${TRANSFER_PTX})
set(UI_SOURCES src/window.cpp src/viewport.cpp src/transfer_panel.cpp src/transfer_viewport.cpp
 src/ai_texture.cpp src/ai_runtime.cpp src/ai_cloud.cpp src/ai_panel.cpp src/resources/fornaio.qrc)
add_executable(Fornaio WIN32 src/main.cpp ${UI_SOURCES} src/resources/fornaio.rc)
target_link_libraries(Fornaio PRIVATE baker Qt6::Widgets Qt6::Network Qt6::OpenGLWidgets Qt6::OpenGL)
add_executable(fornaio_cli src/cli.cpp src/transfer_tests.cpp src/uv2_tests.cpp src/resources/fornaio.rc)
target_link_libraries(fornaio_cli PRIVATE baker)
enable_testing()
add_test(NAME core_and_gpu COMMAND fornaio_cli --self-test)
add_executable(fornaio_ui_tests tests/ui_tests.cpp ${UI_SOURCES})
target_link_libraries(fornaio_ui_tests PRIVATE baker Qt6::Widgets Qt6::Network Qt6::OpenGLWidgets Qt6::OpenGL Qt6::Test)
add_test(NAME ui_regressions COMMAND fornaio_ui_tests)
add_executable(fornaio_ai_tests tests/ai_tests.cpp src/ai_texture.cpp src/ai_runtime.cpp src/ai_cloud.cpp src/ai_panel.cpp
 src/transfer_viewport.cpp)
target_link_libraries(fornaio_ai_tests PRIVATE baker Qt6::Widgets Qt6::Network Qt6::OpenGLWidgets Qt6::OpenGL Qt6::Test)
add_test(NAME ai_regressions COMMAND fornaio_ai_tests)
# Maintainer tool for scripted end-to-end generator evaluation (capture, project, render).
add_executable(fornaio_ai_eval tests/ai_eval.cpp src/ai_texture.cpp)
target_link_libraries(fornaio_ai_eval PRIVATE baker Qt6::OpenGLWidgets)
set_tests_properties(ai_regressions PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
foreach(t Fornaio fornaio_cli fornaio_ui_tests)
 add_dependencies(${t} device_code)
 add_custom_command(TARGET ${t} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PTX} $<TARGET_FILE_DIR:${t}>/bake.ptx
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TRANSFER_PTX} $<TARGET_FILE_DIR:${t}>/transfer.ptx)
endforeach()
# Refresh scripts even when a worker-only change does not relink the executable.
add_custom_target(fornaio_ai_assets ALL
 COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:Fornaio>/ai
 COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/ai/manifest.json $<TARGET_FILE_DIR:Fornaio>/ai/manifest.json
 COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/ai/AI_NOTICES.md $<TARGET_FILE_DIR:Fornaio>/ai/AI_NOTICES.md
 COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/ai/PROJECTION_BLENDING.md $<TARGET_FILE_DIR:Fornaio>/ai/PROJECTION_BLENDING.md
 COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/scripts/ai_worker.py $<TARGET_FILE_DIR:Fornaio>/ai/ai_worker.py
 COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/scripts/ai_extract.ps1 $<TARGET_FILE_DIR:Fornaio>/ai/ai_extract.ps1)
foreach(t Fornaio fornaio_ui_tests fornaio_ai_tests)
 add_dependencies(${t} fornaio_ai_assets)
endforeach()
