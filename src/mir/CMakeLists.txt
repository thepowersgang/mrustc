add_library(mir STATIC
  check.cpp
  check_full.cpp
  cleanup.cpp
  dump.cpp
  from_hir.cpp
  from_hir.hpp
  from_hir_match.cpp
  helpers.cpp
  helpers.hpp
  main_bindings.hpp
  mir_builder.cpp
  mir.cpp
  mir.hpp
  mir_ptr.cpp
  mir_ptr.hpp
  operations.hpp
  optimise.cpp
  visit_crate_mir.cpp
  visit_crate_mir.hpp)

target_link_libraries(mir PUBLIC shared_includes)
