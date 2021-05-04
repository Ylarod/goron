set(SOURCES
  libclang_cxx.cpp
  )

set(LIBS
  clangBasic
  clangCodeGen
  clangDriver
  clangFrontend
  clangFrontendTool
  clangIndex
  clangTooling
)

if (CLANG_ENABLE_ARCMT)
  list(APPEND LIBS clangARCMigrate)
endif ()

if (TARGET clangTidyPlugin)
  add_definitions(-DCLANG_TOOL_EXTRA_BUILD)
  list(APPEND LIBS clangTidyPlugin)
  list(APPEND LIBS clangIncludeFixerPlugin)
  if(LLVM_ENABLE_MODULES)
    list(APPEND LLVM_COMPILE_FLAGS "-fmodules-ignore-macro=CLANG_TOOL_EXTRA_BUILD")
  endif()
endif ()

find_library(DL_LIBRARY_PATH dl)
if (DL_LIBRARY_PATH)
  list(APPEND LIBS dl)
endif()

if(NOT WIN32)
  set(LLVM_EXPORTED_SYMBOL_FILE ${CMAKE_CURRENT_SOURCE_DIR}/libclang_cxx.exports)
endif()

if( LLVM_ENABLE_PIC )
  set(ENABLE_SHARED SHARED)
endif()

if(NOT LLVM_ENABLE_PIC AND NOT WIN32)
  set(ENABLE_STATIC STATIC)
endif()

if(WIN32)
  set(output_name "libclang_cxx")
else()
  set(output_name "clang_cxx")
endif()

if(("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux") OR (MINGW))
  set(LIBS -Wl,--whole-archive ${LIBS} -Wl,--no-whole-archive)
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
  set(LIBS -Wl,-all_load ${LIBS})
endif()

add_clang_library(libclang_cxx ${ENABLE_SHARED} ${ENABLE_STATIC}
  OUTPUT_NAME ${output_name}
  ${SOURCES}
  LINK_LIBS
  ${LIBS}
  )

if(ENABLE_SHARED)
  if(WIN32)
    set_target_properties(libclang_cxx
      PROPERTIES
      VERSION ${LIBCLANG_LIBRARY_VERSION}
      DEFINE_SYMBOL _CINDEX_LIB_)
  elseif(APPLE)
      set(LIBCLANG_CXX_LINK_FLAGS " -Wl,-compatibility_version -Wl,1")
      set(LIBCLANG_CXX_LINK_FLAGS "${LIBCLANG_CXX_LINK_FLAGS} -Wl,-current_version -Wl,${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}")

    set_property(TARGET libclang_cxx APPEND_STRING PROPERTY
        LINK_FLAGS ${LIBCLANG_CXX_LINK_FLAGS})
  else()
    set_target_properties(libclang_cxx
      PROPERTIES
      VERSION ${LIBCLANG_LIBRARY_VERSION}
      DEFINE_SYMBOL _CINDEX_LIB_)
    # FIXME: _CINDEX_LIB_ affects dllexport/dllimport on Win32.
    if(LLVM_ENABLE_MODULES AND NOT WIN32)
      target_compile_options(libclang_cxx PRIVATE
        "-fmodules-ignore-macro=_CINDEX_LIB_"
        )
    endif()
  endif()
endif()
