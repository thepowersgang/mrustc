
find_package(ZLIB REQUIRED)


add_library(hir STATIC
  crate_post_load.cpp
  crate_ptr.cpp
  crate_ptr.hpp
  deserialise.cpp
  dump.cpp
  expr.cpp
  expr.hpp
  expr_ptr.cpp
  expr_ptr.hpp
  expr_state.hpp
  from_ast.cpp
  from_ast_expr.cpp
  from_ast.hpp
  generic_params.cpp
  generic_params.hpp
  hir.cpp
  hir.hpp
  item_path.hpp
  main_bindings.hpp
  path.cpp
  path.hpp
  pattern.cpp
  pattern.hpp
  serialise.cpp
  serialise_lowlevel.cpp
  serialise_lowlevel.hpp
  type.cpp
  type.hpp
  visitor.cpp
  visitor.hpp
  )

target_link_libraries(hir
  PUBLIC
    shared_includes
  PRIVATE
    ZLIB::ZLIB)

