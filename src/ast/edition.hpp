/*
 */
#pragma once
#include <iostream>

namespace AST {


    enum class Edition {
        Rust2015,
        Rust2018,
        Rust2021,
    };
    static inline std::ostream& operator<<(std::ostream& os, const Edition& e) {
        switch(e)
        {
        case Edition::Rust2015: os << "Rust2015";   break;
        case Edition::Rust2018: os << "Rust2018";   break;
        case Edition::Rust2021: os << "Rust2021";   break;
        }
        return os;
    }

}
