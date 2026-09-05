option(RT64_FAST_VITAGL "Build the vitaGL draw backend" OFF)
option(RT64_FAST_GLES2 "Build the same draw path against host GLES2 for validation" OFF)
option(RT64_FAST_TESTS "Build the portable renderer tests" OFF)
option(RT64_FAST_VALIDATE_UPLOADS "Validate vitaGL texture uploads against CPU data" OFF)

add_library(rt64 STATIC
    rt64_fast_state.cpp
    rt64_fast_rsp.cpp
    rt64_fast_rdp.cpp
    rt64_fast_interpreter.cpp
    rt64_fast_unsupported.cpp
    rt64_fast_shader.cpp
    rt64_fast_batch.cpp
    rt64_fast_presentation.cpp
    ../hle/rt64_vi.cpp
    ../gbi/rt64_gbi.cpp
    ../gbi/rt64_gbi_f3d.cpp
    ../gbi/rt64_gbi_f3dex.cpp
    ../gbi/rt64_gbi_f3dex2.cpp
    ../gbi/rt64_gbi_f3dzex2.cpp
    ../gbi/rt64_gbi_f3dgolden.cpp
    ../gbi/rt64_gbi_f3dwave.cpp
    ../gbi/rt64_gbi_rdp.cpp
)
target_compile_features(rt64 PUBLIC cxx_std_17)
target_compile_definitions(rt64 PUBLIC RT64_FAST HLSL_CPU)
target_include_directories(rt64 PUBLIC
    .. ../contrib ../contrib/hlslpp/include)
target_compile_options(rt64 PRIVATE -fno-strict-aliasing)

if(RT64_FAST_VITAGL AND RT64_FAST_GLES2)
    message(FATAL_ERROR "Choose either vitaGL or host GLES2")
endif()
if(RT64_FAST_VITAGL OR RT64_FAST_GLES2)
    target_sources(rt64 PRIVATE rt64_fast_vitagl.cpp)
    if(RT64_FAST_VALIDATE_UPLOADS)
        target_compile_definitions(rt64 PRIVATE RT64_FAST_VALIDATE_UPLOADS)
    endif()
endif()
if(RT64_FAST_VITAGL)
    target_compile_definitions(rt64 PUBLIC RT64_FAST_VITAGL)
    target_link_libraries(rt64 PUBLIC vitaGL vitashark SceShaccCgExt
        taihen_stub mathneon SceGxm_stub SceDisplay_stub SceSysmodule_stub SceAppMgr_stub SceCommonDialog_stub
        SceShaccCg_stub SceKernelDmacMgr_stub)
endif()
if(RT64_FAST_GLES2)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLES2 REQUIRED IMPORTED_TARGET glesv2)
    target_compile_definitions(rt64 PUBLIC RT64_FAST_GLES2)
    target_link_libraries(rt64 PUBLIC PkgConfig::GLES2)
endif()

if(RT64_FAST_TESTS)
    enable_testing()
    add_executable(rt64_fast_tests tests/rt64_fast_tests.cpp)
    target_link_libraries(rt64_fast_tests PRIVATE rt64)
    add_test(NAME rt64_fast_tests COMMAND rt64_fast_tests)
endif()
