cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(engram C CXX ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

add_executable(engram main.cpp config.cpp effects.cpp font.cpp shell.cpp settings.cpp paf_stubs.S)

# Paf's C++ headers (ScePaf-RE) aren't part of the SDK, or of this repository.
set(PAF_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../refs/sdk/paf_headers/include" CACHE PATH
    "ScePaf-RE's include folder")
if(NOT EXISTS "${PAF_HEADERS_DIR}/paf.h")
  message(FATAL_ERROR "Paf headers not found in ${PAF_HEADERS_DIR}: pass -DPAF_HEADERS_DIR=<ScePaf-RE>/include")
endif()
target_include_directories(engram PRIVATE ${PAF_HEADERS_DIR})

# Paf uses 16-bit wchar_t. With no C runtime, nothing would run static constructors.
target_compile_options(engram PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:-O2;-Wall;-fno-rtti;-fno-exceptions;-fno-builtin;-fshort-wchar;-fno-threadsafe-statics;-fno-sized-deallocation;-fpermissive;-Wno-unused-function;-Wno-sign-compare;-Wno-template-body;-D__declspec(x)=>")
target_link_options(engram PRIVATE -nostartfiles -nostdlib)

# The same module loads into SceShell and the Settings app. A strong import of a
# library the host process doesn't have makes taiHEN refuse the module silently,
# so everything but taiHEN itself is weak.
target_link_libraries(engram
  taihen_stub
  SceCtrl_stub_weak
  SceIofilemgr_stub_weak
  SceKernelThreadMgr_stub_weak
  SceLibKernel_stub_weak
  SceMotion_stub_weak
  SceSysmem_stub_weak
  SceAppMgr_stub_weak
  ScePvf_stub_weak
  SceKernelModulemgr_stub_weak
  gcc
)

vita_create_self(engram.suprx engram CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/engram.yml UNSAFE)
