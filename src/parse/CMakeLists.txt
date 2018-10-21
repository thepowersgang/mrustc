add_library(parse STATIC
  common.hpp
  eTokenType.enum.h
  expr.cpp
  interpolated_fragment.cpp
  interpolated_fragment.hpp
  lex.cpp
  lex.hpp
  parseerror.cpp
  parseerror.hpp
  paths.cpp
  pattern.cpp
  root.cpp
  token.cpp
  token.hpp
  tokenstream.cpp
  tokenstream.hpp
  tokentree.cpp
  tokentree.hpp
  ttstream.cpp
  ttstream.hpp
  types.cpp)

target_link_libraries(parse PUBLIC shared_includes)
