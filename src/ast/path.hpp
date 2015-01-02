/*
 */
#ifndef AST_PATH_HPP_INCLUDED
#define AST_PATH_HPP_INCLUDED

#include "../common.hpp"
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
    ::std::vector<TypeRef>&   args() { return m_params; }
    const ::std::vector<TypeRef>&   args() const;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const PathNode& pn) {
        os << pn.m_name;
        if( pn.m_params.size() )
        {
            os << "<";
            os << pn.m_params;
            os << ">";
        }
        return os;
    }
};

class Path
{
    enum Class {
        RELATIVE,
        ABSOLUTE,
        LOCAL,
    };
    Class   m_class;
    ::std::vector<PathNode> m_nodes;
public:
    Path():
        m_class(RELATIVE)
    {}
    struct TagAbsolute {};
    Path(TagAbsolute):
        m_class(ABSOLUTE)
    {}
    struct TagLocal {};
    Path(TagLocal, ::std::string name):
        m_class(LOCAL),
        m_nodes({PathNode(name, {})})
    {}
    
    static Path add_tailing(const Path& a, const Path& b) {
        Path    ret(a);
        for(const auto& ent : b.m_nodes)
            ret.m_nodes.push_back(ent);
        return ret;
    }
    Path operator+(const Path& x) const {
        return Path(*this) += x;
    }
    Path& operator+=(const Path& x);

    void append(PathNode node) {
        m_nodes.push_back(node);
    }
    
    bool is_relative() const { return m_class == RELATIVE; }
    size_t size() const { return m_nodes.size(); }
    ::std::vector<PathNode>& nodes() { return m_nodes; }
    const ::std::vector<PathNode>& nodes() const { return m_nodes; }
    
    PathNode& operator[](size_t idx) { return m_nodes[idx]; }
    const PathNode& operator[](size_t idx) const { return m_nodes[idx]; }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const Path& path);
};

}   // namespace AST

#endif
