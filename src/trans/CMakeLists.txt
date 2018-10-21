add_library(trans STATIC
  allocator.cpp
  allocator.hpp
  codegen_c.cpp
  codegen_c.hpp
  codegen.cpp
  codegen_c_structured.cpp
  codegen.hpp
  codegen_mmir.cpp
  enumerate.cpp
  main_bindings.hpp
  mangling.cpp
  mangling.hpp
  monomorphise.cpp
  monomorphise.hpp
  target.cpp
  target.hpp
  trans_list.cpp
  trans_list.hpp)

target_link_libraries(trans PUBLIC shared_includes)
target_link_libraries(trans PRIVATE common)
