#ifndef AST_HPP_INCLUDED
#define AST_HPP_INCLUDED

#include <string>
#include <vector>
#include <stdexcept>
#include "../coretypes.hpp"

class TypeRef;

namespace AST {

class ExprNode;

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

class Pattern
{
public:
    Pattern();

    struct TagMaybeBind {};
    Pattern(TagMaybeBind, ::std::string name);

    struct TagValue {};
    Pattern(TagValue, ExprNode node);

    struct TagEnumVariant {};
    Pattern(TagEnumVariant, Path path, ::std::vector<Pattern> sub_patterns);
};

class ExprNode
{
public:
    ExprNode();

    struct TagBlock {};
    ExprNode(TagBlock, ::std::vector<ExprNode> nodes);

    struct TagLetBinding {};
    ExprNode(TagLetBinding, Pattern pat, ExprNode value);

    struct TagReturn {};
    ExprNode(TagReturn, ExprNode val);

    struct TagAssign {};
    ExprNode(TagAssign, ExprNode slot, ExprNode value) {}

    struct TagCast {};
    ExprNode(TagCast, ExprNode value, TypeRef dst_type);

    struct TagInteger {};
    ExprNode(TagInteger, uint64_t value, enum eCoreType datatype);

    struct TagStructLiteral {};
    ExprNode(TagStructLiteral, Path path, ExprNode base_value, ::std::vector< ::std::pair< ::std::string,ExprNode> > values );

    struct TagCallPath {};
    ExprNode(TagCallPath, Path path, ::std::vector<ExprNode> args);

    struct TagCallObject {};
    ExprNode(TagCallObject, ExprNode val, ::std::vector<ExprNode> args);

    struct TagMatch {};
    ExprNode(TagMatch, ExprNode val, ::std::vector< ::std::pair<Pattern,ExprNode> > arms);

    struct TagIf {};
    ExprNode(TagIf, ExprNode cond, ExprNode true_code, ExprNode false_code);

    struct TagNamedValue {};
    ExprNode(TagNamedValue, Path path);

    struct TagField {};
    ExprNode(TagField, ::std::string name);

    enum BinOpType {
        BINOP_CMPEQU,
        BINOP_CMPNEQU,

        BINOP_BITAND,
        BINOP_BITOR,
        BINOP_BITXOR,

        BINOP_SHL,
        BINOP_SHR,
    };
    struct TagBinOp {};
    ExprNode(TagBinOp, BinOpType type, ExprNode left, ExprNode right);
};

class Expr
{
public:
    Expr() {}
    Expr(ExprNode node) {}
};

class Function
{
public:

    enum Class
    {
        CLASS_UNBOUND,
        CLASS_REFMETHOD,
        CLASS_MUTMETHOD,
        CLASS_VALMETHOD,
    };

    Function(::std::string name, TypeParams params, Class fcn_class, TypeRef ret_type, ::std::vector<StructItem> args, Expr code);
};

class Impl
{
public:
    Impl(TypeRef impl_type, TypeRef trait_type);

    void add_function(bool is_public, Function fcn);
};

class Module
{
public:
    void add_alias(bool is_public, Path path) {}
    void add_constant(bool is_public, ::std::string name, TypeRef type, Expr val);
    void add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val);
    void add_struct(bool is_public, ::std::string name, TypeParams params, ::std::vector<StructItem> items);
    void add_function(bool is_public, Function func);
    void add_impl(Impl impl);
};

}

#endif // AST_HPP_INCLUDED
