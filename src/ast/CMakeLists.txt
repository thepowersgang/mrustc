add_library(ast STATIC
            "ast.cpp"
            "ast.hpp"
            "attrs.hpp"
            "crate.cpp"
            "crate.hpp"
            "dump.cpp"
            "expr.cpp"
            "expr.hpp"
            "expr_ptr.hpp"
            "generics.hpp"
            "item.hpp"
            "macro.hpp"
            "path.cpp"
            "path.hpp"
            "pattern.cpp"
            "pattern.hpp"
            "types.cpp"
            "types.hpp")

target_link_libraries(ast PUBLIC shared_includes)
