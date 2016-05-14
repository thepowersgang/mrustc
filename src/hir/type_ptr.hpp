/*
 */
#pragma once

namespace HIR {

class TypeRef;
class TypeRefPtr {
    TypeRef*    m_ptr;
public:
    TypeRefPtr(TypeRef _);
    ~TypeRefPtr();
};

}   // namespace HIR
