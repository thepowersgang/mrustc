add_library(hir_conv STATIC
  bind.cpp
  constant_evaluation.cpp
  expand_type.cpp
  main_bindings.hpp
  markings.cpp
  resolve_ufcs.cpp)

target_link_libraries(hir_conv PUBLIC shared_includes)
