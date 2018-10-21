add_library(resolve STATIC
  absolute.cpp
  index.cpp
  main_bindings.hpp
  use.cpp)

target_link_libraries(resolve PUBLIC shared_includes)
