if(NOT DEFINED CANGJIE_ROOT)
  set(CANGJIE_ROOT ${CMAKE_SOURCE_DIR}/../../cangjie)
endif()
if(NOT DEFINED CANGJIE_FRONTEND_LIB)
  set(CANGJIE_FRONTEND_LIB ${CMAKE_SOURCE_DIR}/../../cangjie/build/build/lib/libcangjie-frontend${CMAKE_SHARED_LIBRARY_SUFFIX})
endif()
if(NOT DEFINED CANGJIE_FRONTEND_IMPLIB)
  set(CANGJIE_FRONTEND_IMPLIB ${CMAKE_SOURCE_DIR}/../../cangjie/build/build/lib/libcangjie-frontend.dll.a)
endif()
if(NOT DEFINED CANGJIE_LSP_LIB)
  set(CANGJIE_LSP_LIB ${CMAKE_SOURCE_DIR}/../../cangjie/build/build/lib/libcangjie-lsp${CMAKE_SHARED_LIBRARY_SUFFIX})
endif()
if(NOT DEFINED CANGJIE_LSP_IMPLIB)
  set(CANGJIE_LSP_IMPLIB ${CMAKE_SOURCE_DIR}/../../cangjie/build/build/lib/libcangjie-lsp.dll.a)
endif()
include_directories(${CANGJIE_ROOT}/include)
include_directories(${CANGJIE_ROOT}/third_party/flatbuffers/include)
include_directories(${CANGJIE_ROOT}/build/build/schema)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../utils/demangle)
set_property(SOURCE
  CangjieUserExpression.cpp
  CangjieCompilerInstance.cpp
  CangjieDeclMap.cpp
  CangjieASTUtils.cpp
  CangjieExpressionSourceCode.cpp
  CangjieExpressionParser.cpp
  CangjieIRForTarget.cpp
  CangjiePersistentVariables.cpp
  APPEND PROPERTY COMPILE_FLAGS "-fexceptions -DCANGJIE_CODEGEN_CJNATIVE_BACKEND")

if(WIN32 AND uppercase_CMAKE_BUILD_TYPE STREQUAL "DEBUG")
add_lldb_library(lldbPluginExpressionParserCangjie
  CangjieUserExpression.cpp
  CangjieCompilerInstance.cpp
  CangjieDeclMap.cpp
  CangjieASTUtils.cpp
  CangjieExpressionSourceCode.cpp
  CangjieExpressionParser.cpp
  CangjieIRForTarget.cpp
  CangjiePersistentVariables.cpp
  LINK_LIBS
    lldbExpression
    lldbTarget
    cangjie-frontend
    boundscheck
)
else()
add_lldb_library(lldbPluginExpressionParserCangjie
  CangjieUserExpression.cpp
  CangjieCompilerInstance.cpp
  CangjieDeclMap.cpp
  CangjieASTUtils.cpp
  CangjieExpressionSourceCode.cpp
  CangjieExpressionParser.cpp
  CangjieIRForTarget.cpp
  CangjiePersistentVariables.cpp
  LINK_LIBS
    lldbExpression
    lldbTarget
    cangjie-frontend
    cangjie-lsp
    boundscheck
)
endif()

string(REPLACE "AMD64" "x86_64" ARCH_NAME "${CMAKE_SYSTEM_PROCESSOR}")
target_link_directories(lldbPluginExpressionParserCangjie PUBLIC ${CMAKE_SOURCE_DIR}/../utils/boundscheck/lib/${ARCH_NAME})
add_compile_definitions(CANGJIE_CODEGEN_CJNATIVE_BACKEND)
add_compile_definitions(CANGJIE_AST2CHIR)

if(WIN32 AND uppercase_CMAKE_BUILD_TYPE STREQUAL "DEBUG")
  add_library(cangjie-frontend STATIC IMPORTED)
  set_target_properties(cangjie-frontend PROPERTIES
      IMPORTED_LOCATION ${CANGJIE_FRONTEND_LIB}
  )
else()
  add_library(cangjie-frontend SHARED IMPORTED)
  add_library(cangjie-lsp SHARED IMPORTED)
  set_target_properties(cangjie-frontend PROPERTIES
      IMPORTED_LOCATION ${CANGJIE_FRONTEND_LIB}
      IMPORTED_IMPLIB ${CANGJIE_FRONTEND_IMPLIB}
  )
  set_target_properties(cangjie-lsp PROPERTIES
      IMPORTED_LOCATION ${CANGJIE_LSP_LIB}
      IMPORTED_IMPLIB ${CANGJIE_LSP_IMPLIB}
  )
endif()
