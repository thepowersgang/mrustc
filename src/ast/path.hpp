/*
 */
#ifndef AST_PATH_HPP_INCLUDED
#define AST_PATH_HPP_INCLUDED

#include <string>
#include <stdexcept>

class TypeRef;

namespace AST {

class TypeParam
{
public:
    TypeParam(bool is_lifetime, ::std::string name);
    void addLifetimeBound(::std::string name);
    void addTypeBound(TypeRef type);
};

typedef ::std::vector<TypeParam>    TypeParams;
typedef ::std::pair< ::std::string, TypeRef>    StructItem;

class PathNode
{
    ::std::string   m_name;
    ::std::vector<TypeRef>  m_params;
public:
    PathNode(::std::string name, ::std::vector<TypeRef> args);
    const ::std::string& name() const;
    const ::std::vector<TypeRef>&   args() const;
};

class Path
{
public:
    Path();
    struct TagAbsolute {};
    Path(TagAbsolute);

    void append(PathNode node) {}
    size_t length() const {return 0;}

    PathNode& operator[](size_t idx) { throw ::std::out_of_range("Path []"); }
};

}   // namespace AST

#endif
