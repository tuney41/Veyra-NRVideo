cmake_minimum_required(VERSION 3.24)
project(VeyraAmdNrBackendChecks LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

get_filename_component(VEYRA_ROOT "../.." ABSOLUTE)

function(amd_nr_check_target target)
  target_include_directories(${target} PRIVATE "${VEYRA_ROOT}/include")
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor)
  endif()
endfunction()

add_library(veyra_fake_amd_ngx_core SHARED FakeAmdNgxCore.cpp)
amd_nr_check_target(veyra_fake_amd_ngx_core)
set_target_properties(veyra_fake_amd_ngx_core PROPERTIES OUTPUT_NAME dlssnr_core)

add_executable(amd_nr_backends_tests
  AmdNrBackendsTests.cpp
  "${VEYRA_ROOT}/src/pipeline/AmdNgxNrBackend.cpp"
  "${VEYRA_ROOT}/src/base/Log.cpp"
  "${VEYRA_ROOT}/src/base/RuntimePaths.cpp"
  "${VEYRA_ROOT}/src/engine/EffectChain.cpp"
)
amd_nr_check_target(amd_nr_backends_tests)
target_compile_options(amd_nr_backends_tests PRIVATE /WX-)
target_link_libraries(amd_nr_backends_tests PRIVATE d3d12 dxgi dxguid)
add_dependencies(amd_nr_backends_tests veyra_fake_amd_ngx_core)

add_executable(effect_availability_tests
  "${VEYRA_ROOT}/tests/unit/EffectAvailabilityTests.cpp"
)
amd_nr_check_target(effect_availability_tests)

add_test(NAME amd_nr_backends COMMAND amd_nr_backends_tests "$<TARGET_FILE:veyra_fake_amd_ngx_core>")
add_test(NAME effect_availability COMMAND effect_availability_tests)
set_tests_properties(amd_nr_backends effect_availability PROPERTIES TIMEOUT 120)
