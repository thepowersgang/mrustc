add_library(hir_typeck STATIC
  CMakeLists.txt
  common.cpp
  common.hpp
  expr_check.cpp
  expr_cs.cpp
  expr_visit.cpp
  expr_visit.hpp
  helpers.cpp
  helpers.hpp
  impl_ref.cpp
  impl_ref.hpp
  main_bindings.hpp
  outer.cpp
  static.cpp
  static.hpp)

target_link_libraries(hir_typeck PUBLIC shared_includes)
