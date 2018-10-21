
# Build outline for mrustc:
#
# Sourcefiles:  
# Dependencies: 
# Notes:        Needs compiler capable of C++14

add_subdirectory(include)
add_subdirectory(ast)
add_subdirectory(expand)
add_subdirectory(hir)
add_subdirectory(hir_conv)
add_subdirectory(hir_expand)
add_subdirectory(hir_typeck)
add_subdirectory(macro_rules)
add_subdirectory(mir)
add_subdirectory(parse)
add_subdirectory(resolve)
add_subdirectory(trans)

add_executable(mrustc
  main.cpp
  debug.cpp
  ident.cpp
  rc_string.cpp
  span.cpp
  version.cpp)

target_link_libraries(mrustc
   PRIVATE
    common # tools-directory
    ast
    expand
    hir
    hir_conv
    hir_expand
    hir_typeck
    macro_rules
    mir
    parse
    resolve
    trans
  INTERFACE
    include)

target_compile_definitions(mrustc PRIVATE
  -DVERSION_GIT_FULLHASH="${GIT_HEAD_HASH}"
  -DVERSION_GIT_SHORTHASH="${GIT_HEAD_HASH_SHORT}"
  -DVERSION_GIT_BRANCH="${GIT_BRANCH}"
  -DVERSION_GIT_ISDIRTY=$<BOOL:${GIT_IS_DIRTY}>
  -DVERSION_BUILDTIME="${BUILD_TIMESTAMP}")
