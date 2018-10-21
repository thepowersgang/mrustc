add_library(macro_rules STATIC
  eval.cpp
  macro_rules.hpp
  macro_rules_ptr.hpp
  mod.cpp
  parse.cpp
  pattern_checks.hpp)

target_link_libraries(macro_rules PUBLIC shared_includes)
