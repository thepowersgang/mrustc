//
//
//
#include "module_tree.hpp"
#include "lex.hpp"
#include <iostream>

ModuleTree::ModuleTree()
{
}

struct Parser
{
    ModuleTree& tree;
    Lexer  lex;
    Parser(ModuleTree& tree, const ::std::string& path):
        tree(tree),
        lex(path)
    {
    }

    bool parse_one();

    ::MIR::Function parse_body();
    ::HIR::Path parse_path();
    ::HIR::GenericPath parse_genericpath();
    ::HIR::SimplePath parse_simplepath();
    RawType parse_core_type();
    ::HIR::TypeRef parse_type();
    ::HIR::GenericPath parse_tuple();
};

void ModuleTree::load_file(const ::std::string& path)
{
    auto parse = Parser { *this, path };

    while(parse.parse_one())
    {
        // Keep going!
    }
}
// Parse a single item from a .mir file
bool Parser::parse_one()
{
    if( lex.next() == "" )  // EOF?
    {
        return false;
    }

    if( lex.consume_if("fn") )
    {
        auto p = parse_path();
        lex.check_consume('(');
        ::std::vector<::HIR::TypeRef>  arg_tys;
        while(lex.next() != ')')
        {
            arg_tys.push_back( parse_type() );
            lex.check_consume(',');
        }
        lex.consume();
        ::HIR::TypeRef  rv_ty;
        if( lex.consume_if(':') )
        {
            rv_ty = parse_type();
        }
        auto body = parse_body();

        tree.functions.insert( ::std::make_pair(::std::move(p), Function { ::std::move(arg_tys), rv_ty, ::std::move(body) }) );
    }
    else if( lex.consume_if("static") )
    {
        auto p = parse_path();
        lex.check_consume('=');
        // TODO: Body? Value?
        //auto body = parse_body();
        //auto data = ::std::move(lex.consume().strval);
        throw "TODO";
    }
    else if( lex.consume_if("type") )
    {
        auto p = (lex.consume_if('(')) ? parse_tuple() : parse_genericpath();

        auto rv = ::std::make_unique<DataType>();

        lex.check_consume('{');
        lex.check_consume("SIZE");
        rv->size = lex.consume().integer();
        lex.check_consume(',');
        lex.check_consume("ALIGN");
        rv->alignment = lex.consume().integer();
        lex.check_consume(';');

        // TODO: DST Meta
        if( lex.consume_if("DSTMETA") )
        {
            //rv->dst_meta = parse_type();
            lex.check_consume(';');
            throw "TODO";
        }
        else
        {
            //rv->dst_meta = ::HIR::TypeRef::diverge();
        }

        // Data
        while(lex.next() == TokenClass::Integer)
        {
            size_t ofs = lex.consume().integer();
            lex.check_consume('=');
            auto ty = parse_type();
            lex.check_consume(',');

            rv->fields.push_back(::std::make_pair(ofs, ::std::move(ty)));
        }
        // Variants
        while(lex.next() == '[')
        {
            lex.consume();
            size_t base_idx = lex.consume().integer();
            ::std::vector<size_t>   other_idx;
            while(lex.next() == ',')
            {
                lex.consume();
                other_idx.push_back( lex.consume().integer() );
            }
            lex.check_consume(']');
            lex.check_consume('=');
            lex.check(TokenClass::String);
            uint64_t    v = 0;
            int pos = 0;
            for(auto ch : lex.next().strval)
            {
                if(pos < 64)
                {
                    v |= static_cast<uint64_t>(ch) << pos;
                }
                pos += 8;
            }
            lex.consume();
            lex.check_consume(',');

            rv->variants.push_back({ base_idx, other_idx, v });
        }
        lex.check_consume('}');

        auto r = this->tree.data_types.insert(::std::make_pair( ::std::move(p), ::std::move(rv) ));
        if( !r.second )
        {
            // Duplicate definition of a type
            throw "ERROR";
        }
    }
    else
    {
        // Unknown item type
        throw "ERROR";
    }

    return true;
}
::MIR::Function Parser::parse_body()
{
    ::MIR::Function rv;
    ::std::vector<::std::string>    drop_flag_names;
    ::std::vector<::std::string>    var_names;

    struct H
    {
        static ::std::unique_ptr<::MIR::LValue> make_lvp(::MIR::LValue&& lv) {
            return ::std::unique_ptr<::MIR::LValue>(new ::MIR::LValue(::std::move(lv)));
        }
        static ::MIR::LValue parse_lvalue(Lexer& lex, ::std::vector<::std::string>& var_names)
        {
            int deref = 0;
            while(lex.consume_if('*') ) {
                deref ++;
            }
            ::MIR::LValue   lv;
            if( lex.consume_if('(') ) {
                lv = parse_lvalue(lex, var_names);
                lex.check_consume(')');
            }
            else if( lex.next() == TokenClass::Ident ) {
                auto name = ::std::move(lex.consume().strval);
                //::std::cout << "name=" << name << "\n";
                if( name.substr(0,3) == "arg" ) {
                    auto idx = static_cast<unsigned>( ::std::stol(name.substr(4)) );

                    lv = ::MIR::LValue::make_Argument({ idx });
                }
                else if( name == "RETURN" ) {
                    lv = ::MIR::LValue::make_Return({});
                }
                else {
                    auto it = ::std::find(var_names.begin(), var_names.end(), name);
                    if( it == var_names.end() ) {
                        throw "ERROR";
                    }
                    lv = ::MIR::LValue::make_Local(static_cast<unsigned>(it - var_names.begin()));
                }
            }
            else {
                throw "ERROR";
            }
            for(;;)
            {
                if( lex.consume_if('.') )
                {
                    auto idx = static_cast<unsigned>( lex.consume().integer() );
                    lv = ::MIR::LValue::make_Field({ make_lvp(::std::move(lv)), idx });
                }
                else if( lex.next() == '[' )
                {
                    lex.consume();
                    auto idx_lv = parse_lvalue(lex, var_names);
                    lv = ::MIR::LValue::make_Index({ make_lvp(::std::move(lv)), make_lvp(::std::move(idx_lv)) });
                    lex.check_consume(']');
                }
                else
                {
                    break;
                }
            }
            while(deref --)
            {
                lv = ::MIR::LValue::make_Deref({ make_lvp(::std::move(lv)) });
            }
            return lv;
        }

        static ::MIR::Param parse_param(Parser& p, ::std::vector<::std::string>& var_names)
        {
            if( p.lex.next() == TokenClass::Integer ) {
                auto v = p.lex.consume().integer();
                auto cty = p.parse_core_type();
                return ::MIR::Constant::make_Int({ static_cast<int64_t>(v), cty });
            }
            else {
                return parse_lvalue(p.lex, var_names);
            }
        }
    };

    lex.check_consume('{');

    // 1. Locals + Drop flags
    while(lex.next() == "let")
    {
        lex.consume();
        auto name = ::std::move(lex.consume().strval);
        if(lex.next() == '=')
        {
            lex.consume();
            rv.drop_flags.push_back(lex.consume().integer() != 0);
            drop_flag_names.push_back(::std::move(name));
        }
        else if(lex.next() == ':')
        {
            lex.consume();
            var_names.push_back(::std::move(name));
            rv.locals.push_back( parse_type() );
        }
        else
        {
            throw "ERROR";
        }
        lex.check_consume(';');
    }

    // 3. BBs + statements
    while(lex.next() == TokenClass::Integer)
    {
        ::std::vector<::MIR::Statement> stmts;
        ::MIR::Terminator   term;

        lex.consume();
        lex.check_consume(':');
        lex.check_consume('{');
        for(;;)
        {
            lex.check(TokenClass::Ident);
            if( lex.consume_if("ASSIGN") )
            {
                auto dst_val = H::parse_lvalue(lex, var_names);
                lex.check_consume('=');
                ::MIR::RValue   src_rval;
                if( lex.next() == TokenClass::Integer ) {
                    auto v = lex.consume().integer();
                    auto cty = parse_core_type();
                    src_rval = ::MIR::Constant::make_Int({ static_cast<int64_t>(v), cty });
                }
                else if( lex.consume_if('(') ) {
                    ::std::vector<::MIR::Param> vals;
                    while( lex.next() != ')' )
                    {
                        vals.push_back( H::parse_param(*this, var_names) );
                        lex.check_consume(',');
                    }
                    lex.consume();
                    src_rval = ::MIR::RValue::make_Tuple({ ::std::move(vals) });
                }
                else if( lex.consume_if("USE") ) {
                    src_rval = H::parse_lvalue(lex, var_names);
                }
                else {
                    throw "";
                }

                stmts.push_back(::MIR::Statement::make_Assign({ ::std::move(dst_val), ::std::move(src_rval) }));
            }
            else if( lex.consume_if("SETFLAG") )
            {
                auto df_it = ::std::find(drop_flag_names.begin(), drop_flag_names.end(), lex.next().strval);
                if( df_it == drop_flag_names.end() ) {
                    throw "ERROR";
                }
                auto df_idx = static_cast<unsigned>( df_it - drop_flag_names.begin() );
                lex.check_consume('=');
                if( lex.next() == TokenClass::Integer ) {
                    bool val = lex.consume().integer() != 0;
                    stmts.push_back(::MIR::Statement::make_SetDropFlag({ df_idx, val, ~0u }));
                }
                else {
                    bool inv = false;
                    if( lex.next() == '!' ) {
                        inv = true;
                        lex.consume();
                    }
                    df_it = ::std::find(drop_flag_names.begin(), drop_flag_names.end(), lex.next().strval);
                    if( df_it == drop_flag_names.end() ) {
                        throw "ERROR";
                    }
                    auto other_idx = static_cast<unsigned>( df_it - drop_flag_names.begin() );

                    stmts.push_back(::MIR::Statement::make_SetDropFlag({ df_idx, inv, other_idx }));
                }
            }
            else if(lex.next() == "DROP")
            {
                throw "TODO";
            }
            else if(lex.next() == "ASM")
            {
                throw "TODO";
            }
            else
            {
                break;
            }
            lex.check_consume(';');
        }

        lex.check(TokenClass::Ident);
        if( lex.consume_if("GOTO") )
        {
            term = ::MIR::Terminator::make_Goto(static_cast<unsigned>(lex.consume().integer()));
        }
        else if( lex.consume_if("RETURN") )
        {
            term = ::MIR::Terminator::make_Return({});
        }
        else if(lex.next() == "IF")
        {
            auto val = H::parse_lvalue(lex, var_names);
            lex.check_consume("goto");
            auto tgt_true = static_cast<unsigned>(lex.consume().integer());
            lex.check_consume("else");
            auto tgt_false = static_cast<unsigned>(lex.consume().integer());
            term = ::MIR::Terminator::make_If({ ::std::move(val), tgt_true, tgt_false });
        }
        else if(lex.next() == "SWITCH")
        {
            auto val = H::parse_lvalue(lex, var_names);
            throw "TODO";
        }
        else if(lex.next() == "SWITCHVAL")
        {
            auto val = H::parse_lvalue(lex, var_names);
            throw "TODO";
        }
        else if(lex.next() == "CALL")
        {
            throw "TODO";
        }
        else
        {
            throw "ERROR";
        }

        lex.check_consume('}');

        rv.blocks.push_back(::MIR::BasicBlock { ::std::move(stmts), ::std::move(term) });
    }

    lex.check_consume('}');
    return rv;
}
::HIR::Path Parser::parse_path()
{
    if( lex.consume_if('<') )
    {
        auto ty = parse_type();
        ::HIR::GenericPath  trait;
        if( lex.consume_if("as") )
        {
            trait = parse_genericpath();
        }
        lex.check_consume('>');
        lex.check_consume("::");
        lex.check(TokenClass::Ident);
        auto item_name = ::std::move(lex.consume().strval);

        ::HIR::PathParams   params;
        if( lex.consume_if('<') )
        {
            do
            {
                params.tys.push_back( parse_type() );
            } while( lex.consume_if(',') );
            lex.check_consume('>');
        }
        throw "TODO";
    }
    else
    {
        return parse_genericpath();
    }
}
::HIR::GenericPath Parser::parse_genericpath()
{
    ::HIR::GenericPath  rv;
    rv.m_simplepath = parse_simplepath();
    if( lex.consume_if('<') )
    {
        do
        {
            rv.m_params.tys.push_back( parse_type() );
        } while( lex.consume_if(',') );
        lex.check_consume('>');
    }
    return rv;
}
::HIR::SimplePath Parser::parse_simplepath()
{
    lex.check_consume("::");
    lex.check(TokenClass::String);
    auto crate = lex.consume().strval;
    lex.check_consume("::");
    ::std::vector<::std::string>    ents;
    do
    {
        lex.check(TokenClass::Ident);
        ents.push_back( lex.consume().strval );
    } while(lex.consume_if("::"));

    return ::HIR::SimplePath { ::std::move(crate), ::std::move(ents) };
}
::HIR::GenericPath Parser::parse_tuple()
{
    // Tuples! Should point to a composite
    ::HIR::GenericPath  gp;
    do
    {
        gp.m_params.tys.push_back(parse_type());
        lex.check_consume(',');
    } while( lex.next() != ')' );
    lex.consume();

    return gp;
}
RawType Parser::parse_core_type()
{
    auto tok = lex.consume();
    // Primitive type.
    if( tok == "u8" ) {
        return RawType::U8;
    }
    else if( tok == "u16" ) {
        return RawType::U16;
    }
    else if( tok == "u32" ) {
        return RawType::U32;
    }
    else if( tok == "u64" ) {
        return RawType::U64;
    }
    else if( tok == "u128" ) {
        return RawType::U128;
    }
    else if( tok == "i8" ) {
        return RawType::I8;
    }
    else if( tok == "i16" ) {
        return RawType::I16;
    }
    else if( tok == "i32" ) {
        return RawType::I32;
    }
    else if( tok == "i64" ) {
        return RawType::I64;
    }
    else if( tok == "i128" ) {
        return RawType::I128;
    }
    else if( tok == "f32" ) {
        return RawType::F32;
    }
    else if( tok == "f64" ) {
        return RawType::F64;
    }
    else if( tok == "bool" ) {
        return RawType::Bool;
    }
    else if( tok == "str" ) {
        return RawType::Str;
    }
    else {
        throw "ERROR";
    }
}
::HIR::TypeRef Parser::parse_type()
{
    if( lex.consume_if('(') )
    {
        if( lex.consume_if(')') ) {
            // Unit!
            return ::HIR::TypeRef::unit();
        }
        // Tuples! Should point to a composite
        ::HIR::GenericPath  gp = parse_tuple();

        auto rv = this->tree.data_types.find(gp);
        if( rv == this->tree.data_types.end() )
        {
            throw "ERROR";
        }

        return ::HIR::TypeRef(rv->second.get());
    }
    else if( lex.next() == '[' )
    {
        auto rv = parse_type();
        if( lex.consume_if(';') )
        {
            size_t size = lex.next().integer();
            lex.consume();
            rv.wrappers.insert( rv.wrappers.begin(), { TypeWrapper::Ty::Array, size });
        }
        else
        {
            // TODO: How to handle arrays?
            rv.wrappers.insert( rv.wrappers.begin(), { TypeWrapper::Ty::Slice, 0 });
        }
        lex.check_consume(']');
        return rv;
    }
    else if( lex.next() == '!' )
    {
        return ::HIR::TypeRef::diverge();
    }
    else if( lex.next() == '&' )
    {
        auto bt = ::HIR::BorrowType::Shared;
        if( lex.consume_if("move") )
            bt = ::HIR::BorrowType::Move;
        else if( lex.consume_if("mut") )
            bt = ::HIR::BorrowType::Unique;
        else
            ; // keep as shared
        auto rv = parse_type();
        rv.wrappers.insert( rv.wrappers.begin(), { TypeWrapper::Ty::Borrow, 0 });
        return rv;
    }
    else if( lex.next() == '*' )
    {
        auto bt = ::HIR::BorrowType::Shared;
        if( lex.consume_if("move") )
            bt = ::HIR::BorrowType::Move;
        else if( lex.consume_if("mut") )
            bt = ::HIR::BorrowType::Unique;
        else if( lex.consume_if("const") )
            ; // keep as shared
        else
            throw "ERROR";
        auto rv = parse_type();
        rv.wrappers.insert( rv.wrappers.begin(), { TypeWrapper::Ty::Pointer, 0 });
        return rv;
    }
    else if( lex.next() == "::" )
    {
        auto path = parse_genericpath();
        // Look up this type, then create a TypeRef referring to the type in the datastore
        // - May need to create an unpopulated type?
        throw "TODO";
    }
    else if( lex.next() == TokenClass::Ident )
    {
        return ::HIR::TypeRef(parse_core_type());
    }
    else
    {
        throw "ERROR";
    }
}

::HIR::SimplePath ModuleTree::find_lang_item(const char* name) const
{
    return ::HIR::SimplePath({ "core", { "start" } });
}
const Function& ModuleTree::get_function(const ::HIR::Path& p) const
{
    auto it = functions.find(p);
    if(it == functions.end())
        throw "";
    return it->second;
}