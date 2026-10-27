# SMLib (from NVIDIA's open-source usd-brep) as one WebAssembly module.
#
#   node build.mjs            (sets up the Emscripten environment, then runs this)
#
# Two static libraries, as BREPcode's binding.gyp has them: SMLib's StdAfx.h
# and SM_API's are different headers with the same name, so each library gets
# its own include order. Single threaded: SMLib's non-Windows build without
# SM_USE_THREADS keeps its "thread local" state in plain statics.

cmake_minimum_required(VERSION 3.20)
project(smlib_wasm CXX)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Configure with emcmake (node build.mjs does).")
endif()

set(SMLIB_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upstream/source" CACHE PATH "usd-brep's source/ folder (SMLib and SM_API)")
# wasm: native WebAssembly exception handling (-fwasm-exceptions), needs
# Chrome 95 / Firefox 100 / Safari 15.2 / Node 17 or later. Measured: 25%
# smaller and twice as fast on boolean-heavy models as js, about native speed.
# js: Emscripten's JavaScript-based C++ exceptions (-fexceptions), runs anywhere.
set(SMLIB_EH "wasm" CACHE STRING "C++ exception handling: wasm or js")
set(SMLIB_OPT "-O2" CACHE STRING "optimisation level for the kernel")
set(SMLIB_DIST "${CMAKE_CURRENT_SOURCE_DIR}/dist" CACHE PATH "where smlib.mjs and smlib.wasm go")

if(SMLIB_EH STREQUAL "wasm")
  set(EH_FLAGS -fwasm-exceptions)
else()
  set(EH_FLAGS -fexceptions)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(COMMON_DEFS NDEBUG SM_API_EXPORTS)
set(COMMON_FLAGS ${SMLIB_OPT} ${EH_FLAGS} -frtti -w)

file(GLOB SMLIB_SRC "${SMLIB_SOURCE}/SMLib/src/*.cpp")
list(FILTER SMLIB_SRC EXCLUDE REGEX "/StdAfx\\.cpp$")
file(GLOB SMAPI_SRC "${SMLIB_SOURCE}/SM_API/src/*.cpp")
list(FILTER SMAPI_SRC EXCLUDE REGEX "/StdAfx\\.cpp$")
if(NOT SMLIB_SRC)
  message(FATAL_ERROR "No SMLib sources under ${SMLIB_SOURCE}: run node fetch-upstream.mjs first.")
endif()

add_library(smlib STATIC ${SMLIB_SRC})
target_include_directories(smlib PRIVATE "${SMLIB_SOURCE}/SMLib/inc")
target_compile_definitions(smlib PRIVATE ${COMMON_DEFS})
target_compile_options(smlib PRIVATE ${COMMON_FLAGS})

add_library(smapi STATIC ${SMAPI_SRC})
target_include_directories(smapi PRIVATE "${SMLIB_SOURCE}/SM_API/inc" "${SMLIB_SOURCE}/SMLib/inc")
target_compile_definitions(smapi PRIVATE ${COMMON_DEFS})
target_compile_options(smapi PRIVATE ${COMMON_FLAGS})

add_executable(smlib_wasm src/smlib_wasm.cc)
target_include_directories(smlib_wasm PRIVATE "${SMLIB_SOURCE}/SM_API/inc" "${SMLIB_SOURCE}/SMLib/inc")
target_compile_definitions(smlib_wasm PRIVATE ${COMMON_DEFS})
target_compile_options(smlib_wasm PRIVATE ${COMMON_FLAGS})
target_link_libraries(smlib_wasm PRIVATE smapi smlib)
set_target_properties(smlib_wasm PROPERTIES
  OUTPUT_NAME smlib
  SUFFIX ".mjs"
  RUNTIME_OUTPUT_DIRECTORY "${SMLIB_DIST}")
target_link_options(smlib_wasm PRIVATE
  ${SMLIB_OPT} ${EH_FLAGS}
  -lembind
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createSmlib
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=32MB
  -sMAXIMUM_MEMORY=4GB
  # SMLib recurses (booleans, the tessellator); Emscripten's default is 64 KB
  -sSTACK_SIZE=4MB
  -sFILESYSTEM=0
  -sASSERTIONS=0)
