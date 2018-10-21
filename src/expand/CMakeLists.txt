add_library(expand STATIC
  asm.cpp
  cfg.cpp
  cfg.hpp
  concat.cpp
  crate_tags.cpp
  derive.cpp
  env.cpp
  file_line.cpp
  format_args.cpp
  include.cpp
  lang_item.cpp
  macro_rules.cpp
  mod.cpp
  proc_macro.cpp
  proc_macro.hpp
  rustc_diagnostics.cpp
  std_prelude.cpp
  stringify.cpp
  test.cpp
  test_harness.cpp)

target_link_libraries(expand PUBLIC shared_includes)
