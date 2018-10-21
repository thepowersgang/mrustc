add_library(hir_expand STATIC
  annotate_value_usage.cpp
  closures.cpp
  erased_types.cpp
  main_bindings.hpp
  reborrow.cpp
  ufcs_everything.cpp
  vtable.cpp)

target_link_libraries(hir_expand PUBLIC shared_includes)
