/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * module_tree.cpp
 * - In-memory representation of a Monomorphised MIR executable
 * - Also handles parsing the .mir files
 */
#include "module_tree.hpp"
#include "lex.hpp"
#include "value.hpp"
#include <iostream>
#include <algorithm>    // std::find
#include "debug.hpp"

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
    ::HIR::PathParams parse_pathparams();
    RawType parse_core_type();
    ::HIR::TypeRef parse_type();

    const DataType* get_composite(RcString gp);
};

void ModuleTree::load_file(const ::std::string& path)
{
    if( !loaded_files.insert(path).second )
    {
        LOG_DEBUG("load_file(" << path << ") - Already loaded");
        return ;
    }

    TRACE_FUNCTION_R(path, "");
    auto parse = Parser { *this, path };

    while(parse.parse_one())
    {
        // Keep going!
    }
}
void ModuleTree::validate()
{
    TRACE_FUNCTION_R("", "");
    for(const auto& dt : this->data_types)
    {
        //LOG_ASSERT(dt.second->populated, "Type " << dt.first << " never defined");
    }

    for(const auto& fcn : this->functions)
    {
        // TODO: This doesn't actually happen yet (this combination can't be parsed)
        if( fcn.second.external.link_name != "" && !fcn.second.m_mir.blocks.empty() )
        {
            LOG_DEBUG(fcn.first << " = '" << fcn.second.external.link_name << "'");
            ext_functions.insert(::std::make_pair( fcn.second.external.link_name, &fcn.second ));
        }
    }
}
// Parse a single item from a .mir file
bool Parser::parse_one()
{
    //TRACE_FUNCTION_F("");
    if( lex.next() == TokenClass::Eof )
    {
        return false;
    }

    if( lex.consume_if("crate") )
    {
        // Import an external crate
        auto path = ::std::move(lex.check_consume(TokenClass::String).strval);
        //LOG_TRACE(lex << "crate '" << path << "'");

        lex.check_consume(';');


        this->tree.load_file(path);
    }
    else if( lex.consume_if("fn") )
    {
        auto p = RcString::new_interned(lex.check_consume(TokenClass::Ident).strval.c_str());
        //LOG_TRACE(lex << "fn " << p);

        lex.check_consume('(');
        ::std::vector<::HIR::TypeRef>  arg_tys;
        while(lex.next() != ')')
        {
            arg_tys.push_back( parse_type() );
            if( !lex.consume_if(',') )
                break;
        }
        lex.check_consume(')');
        ::HIR::TypeRef  rv_ty;
        if( lex.consume_if(':') )
        {
            rv_ty = parse_type();
        }

        Function::ExtInfo   ext;
        if( lex.consume_if('=') )
        {
            ext.link_name = ::std::move(lex.check_consume(TokenClass::String).strval);
            lex.check_consume(':');
            ext.link_abi = ::std::move(lex.check_consume(TokenClass::String).strval);
        }
        ::MIR::Function body;
        if( lex.consume_if(';') )
        {
            LOG_DEBUG(lex << "extern fn " << p);
        }
        else
        {
            body = parse_body();

            LOG_DEBUG(lex << "fn " << p);
        }
        auto p2 = p;
        tree.functions.insert( ::std::make_pair(::std::move(p), Function { ::std::move(p2), ::std::move(arg_tys), rv_ty, ::std::move(ext), ::std::move(body) }) );
    }
    else if( lex.consume_if("static") )
    {
        auto p = RcString::new_interned(lex.check_consume(TokenClass::Ident).strval.c_str());
        //LOG_TRACE(lex << "static " << p);
        lex.check_consume(':');
        auto ty = parse_type();
        // TODO: externs?
        lex.check_consume('=');
        lex.check(TokenClass::String);
        auto data = ::std::move(lex.consume().strval);

        Static s;
        s.ty = ty;
        s.init.bytes.insert(s.init.bytes.begin(), data.begin(), data.end());

        if( lex.consume_if('{') )
        {
            while( !lex.consume_if('}') )
            {
                lex.check_consume('@');
                lex.check(TokenClass::Integer);
                auto ofs = lex.consume().integer();
                lex.check_consume('+');
                lex.check(TokenClass::Integer);
                auto size = lex.consume().integer();
                lex.check_consume('=');
                if( lex.next() == TokenClass::String )
                {
                    auto reloc_str = ::std::move(lex.consume().strval);
                    s.init.relocs.push_back( Static::InitValue::Relocation::new_string(ofs, size, std::move(reloc_str)) );
                }
                else if( lex.next() == TokenClass::Ident )
                {
                    auto reloc_path = HIR::Path { RcString(lex.check_consume(TokenClass::Ident).strval.c_str()) };
                    s.init.relocs.push_back( Static::InitValue::Relocation::new_item(ofs, size, std::move(reloc_path)) );
                }
                else
                {
                    LOG_FATAL(lex << "Unexepcted token " << lex.next() << " in relocation value");
                    throw "ERROR";
                }
                if( ! lex.consume_if(',') ) {
                    lex.check_consume('}');
                    break ;
                }
            }
        }
        lex.check_consume(';');

        LOG_DEBUG(lex << "static " << p);
        tree.statics.insert(::std::make_pair( ::std::move(p), ::std::move(s) ));
    }
    else if( lex.consume_if("type") )
    {
        RcString p = lex.check_consume(TokenClass::Ident).strval.c_str();
        //LOG_TRACE("type " << p);

        auto rv = DataType {};
        rv.populated = true;
        rv.my_path = p.c_str();

        lex.check_consume('{');
        lex.check_consume("SIZE");
        rv.size = lex.consume().integer();
        lex.check_consume(',');
        lex.check_consume("ALIGN");
        rv.alignment = lex.consume().integer();
        lex.check_consume(';');

        // Drop glue (if present)
        if( lex.consume_if("DROP") )
        {
            rv.drop_glue = HIR::Path { lex.check_consume(TokenClass::Ident).strval.c_str() };
            lex.check_consume(';');
        }
        else
        {
            // No drop glue
        }

        // DST Meta type
        if( lex.consume_if("DSTMETA") )
        {
            rv.dst_meta = parse_type();
            lex.check_consume(';');
        }
        else
        {
            // Using ! as the metadata type means that the type is Sized (meanwhile, `()` means unsized with no meta)
            rv.dst_meta = ::HIR::TypeRef::diverge();
        }

        while( lex.next() != '}' )
        {
            // Data
            if(lex.next() == TokenClass::Integer)
            {
                size_t ofs = lex.consume().integer();
                lex.check_consume('=');
                auto ty = parse_type();
                lex.check_consume(';');
                //LOG_DEBUG(ofs << " " << ty);

                rv.fields.push_back(::std::make_pair(ofs, ::std::move(ty)));
            }
            // Variants
            else if( lex.next() == '@' )
            {
                lex.check_consume('@');
                lex.check_consume('[');
                rv.tag_path.base_field = lex.consume().integer();
                if( lex.consume_if(',') )
                {
                    while(lex.next() != ']')
                    {
                        lex.check(TokenClass::Integer);
                        rv.tag_path.other_indexes.push_back( lex.consume().integer() );
                        if( !lex.consume_if(',') )
                            break;
                    }
                }
                lex.check_consume(']');
                lex.check_consume('=');
                lex.check_consume('{');

                while( lex.next() != '}' )
                {
                    DataType::VariantValue  var;
                    if( lex.consume_if('*') ) {
                    }
                    else {
                        lex.check(TokenClass::String);
                        var.tag_data = ::std::move(lex.consume().strval);
                    }
                    if(lex.consume_if('=')) {
                        var.data_field = lex.check_consume(TokenClass::Integer).integer();
                    }
                    else {
                        var.data_field = SIZE_MAX;
                    }
                    rv.variants.push_back(std::move(var));

                    if( !lex.consume_if(',') )
                        break;
                }
                lex.check_consume('}');
            }
            else
            {
                LOG_BUG("Unexpected token in `type` - " << lex.next());
            }
        }
        lex.check_consume('}');

        if( rv.alignment == 0 && rv.fields.size() != 0 )
        {
            LOG_ERROR(lex << "Alignment of zero with fields is invalid, " << p);
        }

        LOG_DEBUG(lex << "type " << p);
        auto it = this->tree.data_types.find(p);
        if( it != this->tree.data_types.end() )
        {
            if( it->second->alignment == 0 )
            {
                *it->second = ::std::move(rv);
            }
            else
            {
                //LOG_ERROR(lex << "Duplicate definition of " << p);
            }
        }
        else
        {
            this->tree.data_types.insert(::std::make_pair( ::std::move(p), ::std::make_unique<DataType>(::std::move(rv)) ));
        }
    }
    else
    {
        LOG_ERROR(lex << "Unexpected token at root - " << lex.next());

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
        //
        // Parse a LValue
        //
        static ::MIR::LValue parse_lvalue(Parser& p, ::std::vector<::std::string>& var_names)
        {
            auto& lex = p.lex;
            int deref = 0;
            // Count up leading derefs
            while(lex.consume_if('*') ) {
                deref ++;
            }
            ::MIR::LValue   lv;
            if( lex.consume_if('(') ) {
                lv = parse_lvalue(p, var_names);
                lex.check_consume(')');
            }
            else if( lex.next() == TokenClass::Ident ) {
                auto name = ::std::move(lex.consume().strval);
                // TODO: Make arguments have custom names too
                if( name.substr(0,3) == "arg" ) {
                    try {
                        auto idx = static_cast<unsigned>( ::std::stol(name.substr(3)) );
                        lv = ::MIR::LValue::new_Argument( idx );
                    }
                    catch(const ::std::exception& e) {
                        LOG_ERROR(lex << "Invalid argument name - " << name << " - " << e.what());
                    }
                }
                // Hard-coded "RETURN" lvalue
                else if( name == "RETURN" ) {
                    lv = ::MIR::LValue::new_Return();
                }
                // Otherwise, look up variable names
                else {
                    auto it = ::std::find(var_names.begin(), var_names.end(), name);
                    if( it != var_names.end() ) {
                        lv = ::MIR::LValue::new_Local(static_cast<unsigned>(it - var_names.begin()));
                    }
                    else {
                        lv = ::MIR::LValue::new_Static( HIR::Path { RcString(name.c_str()) } );
                    }
                }
            }
            else {
                LOG_ERROR(lex << "Unexpected token in LValue - " << lex.next());
            }
            for(;;)
            {
                if( lex.consume_if('@') )
                {
                    lex.check(TokenClass::Integer);
                    auto idx = static_cast<unsigned>( lex.consume().integer() );
                    lv = ::MIR::LValue::new_Downcast(::std::move(lv), idx);
                }
                else if( lex.consume_if('.') )
                {
                    lex.check(TokenClass::Integer);
                    auto idx = static_cast<unsigned>( lex.consume().integer() );
                    lv = ::MIR::LValue::new_Field( ::std::move(lv), idx );
                }
                else if( lex.next() == '[' )
                {
                    lex.consume();
                    auto idx_lv = parse_lvalue(p, var_names);
                    lv = ::MIR::LValue::new_Index(::std::move(lv), idx_lv.as_Local());
                    lex.check_consume(']');
                }
                else
                {
                    break;
                }
            }
            while(deref --)
            {
                lv = ::MIR::LValue::new_Deref( ::std::move(lv) );
            }
            return lv;
        }

        static ::MIR::Constant parse_const(Parser& p)
        {
            if( p.lex.next() == TokenClass::Integer ) {
                auto v = p.lex.consume().integer();
                auto cty = p.parse_core_type();
                return ::MIR::Constant::make_Uint({ static_cast<uint64_t>(v), cty });
            }
            else if( p.lex.next() == TokenClass::String ) {
                auto v = ::std::move( p.lex.consume().strval );
                return ::MIR::Constant::make_StaticString(::std::move(v));
            }
            else if( p.lex.next() == TokenClass::ByteString ) {
                ::std::vector<uint8_t>  v;
                for(char c : p.lex.consume().strval )
                {
                    v.push_back( static_cast<uint8_t>(c) );
                }
                return ::MIR::Constant::make_Bytes(::std::move(v));
            }
            else if( p.lex.next() == '+' || p.lex.next() == '-' ) {
                bool is_neg = (p.lex.consume() == '-');
                if( p.lex.next() == TokenClass::Integer )
                {
                    auto v = static_cast<int64_t>(p.lex.consume().integer());
                    auto cty = p.parse_core_type();
                    return ::MIR::Constant::make_Int({ is_neg ? -v : v, cty });
                }
                else if( p.lex.next() == TokenClass::Real )
                {
                    auto v = p.lex.consume().real();
                    auto cty = p.parse_core_type();
                    return ::MIR::Constant::make_Float({ is_neg ? -v : v, cty });
                }
                else
                {
                    LOG_ERROR(p.lex << "Expected an integer or float, got " << p.lex.next());
                }
            }
            else if( p.lex.consume_if("true") ) {
                return ::MIR::Constant::make_Bool({ true });
            }
            else if( p.lex.consume_if("false") ) {
                return ::MIR::Constant::make_Bool({ false });
            }
            else if( p.lex.consume_if("ADDROF") ) {
                auto path = RcString(p.lex.check_consume(TokenClass::Ident).strval.c_str());

                return ::MIR::Constant::make_ItemAddr({ ::std::make_unique<HIR::Path>(HIR::Path { ::std::move(path) }) });
            }
            else {
                LOG_BUG(p.lex << "BUG? " << p.lex.next());
            }
        }

        // Parse a "Param" (constant or lvalue)
        static ::MIR::Param parse_param(Parser& p, ::std::vector<::std::string>& var_names)
        {
            if( p.lex.next() == TokenClass::Integer || p.lex.next() == TokenClass::String || p.lex.next() == TokenClass::ByteString
                || p.lex.next() == '+' || p.lex.next() == '-' || p.lex.next() == '&'
                || p.lex.next() == "true" || p.lex.next() == "false"
                || p.lex.next() == "ADDROF"
                )
            {
                return parse_const(p);
            }
            else
            {
                return parse_lvalue(p, var_names);
            }
        }
    };

    lex.check_consume('{');

    // 1. Locals + Drop flags
    while(lex.next() == "let")
    {
        lex.consume();
        auto name = ::std::move(lex.consume().strval);
        if(lex.consume_if('='))
        {
            rv.drop_flags.push_back(lex.consume().integer() != 0);
            drop_flag_names.push_back(::std::move(name));
        }
        else if(lex.consume_if(':'))
        {
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

        if( lex.next().integer() != rv.blocks.size() )
        {
            // TODO: Error.
        }

        lex.consume();
        lex.check_consume(':');
        lex.check_consume('{');
        for(;;)
        {
            lex.check(TokenClass::Ident);
            if( lex.consume_if("ASSIGN") )
            {
                auto dst_val = H::parse_lvalue(*this, var_names);
                lex.check_consume('=');
                ::MIR::RValue   src_rval;
                // Literals
                if( lex.next() == TokenClass::Integer || lex.next() == TokenClass::String || lex.next() == TokenClass::ByteString
                    || lex.next() == '+' || lex.next() == '-'
                    || lex.next() == "true" || lex.next() == "false"
                    || lex.next() == "ADDROF"
                    )
                {
                    src_rval = H::parse_const(*this);
                }
                // LValue (prefixed by =)
                else if( lex.consume_if('=') ) {
                    src_rval = H::parse_lvalue(*this, var_names);
                }
                else if( lex.consume_if('&') ) {
                    auto bt = ::HIR::BorrowType::Shared;
                    if( lex.consume_if("move") )
                        bt = ::HIR::BorrowType::Move;
                    else if( lex.consume_if("mut") )
                        bt = ::HIR::BorrowType::Unique;
                    else
                        ;
                    auto val = H::parse_lvalue(*this, var_names);
                    src_rval = ::MIR::RValue::make_Borrow({ bt, ::std::move(val) });
                }
                // Composites
                else if( lex.consume_if('(') ) {
                    ::std::vector<::MIR::Param> vals;
                    while( lex.next() != ')' )
                    {
                        vals.push_back( H::parse_param(*this, var_names) );
                        if( !lex.consume_if(',') )
                            break ;
                    }
                    lex.check_consume(')');
                    src_rval = ::MIR::RValue::make_Tuple({ ::std::move(vals) });
                }
                else if( lex.consume_if('[') ) {
                    ::std::vector<::MIR::Param> vals;
                    if( lex.consume_if(']') )
                    {
                        // Empty array
                        src_rval = ::MIR::RValue::make_Array({ ::std::move(vals) });
                    }
                    else
                    {
                        vals.push_back( H::parse_param(*this, var_names) );
                        if( lex.consume_if(';') )
                        {
                            // Sized array
                            lex.check(TokenClass::Integer);
                            auto size_val = static_cast<unsigned>(lex.consume().integer());
                            lex.check_consume(']');

                            src_rval = ::MIR::RValue::make_SizedArray({ ::std::move(vals[0]), size_val });
                        }
                        else
                        {
                            // List array
                            if( lex.consume_if(',') )
                            {
                                while( lex.next() != ']' )
                                {
                                    vals.push_back( H::parse_param(*this, var_names) );
                                    if( !lex.consume_if(',') )
                                        break ;
                                }
                            }
                            lex.check_consume(']');
                            src_rval = ::MIR::RValue::make_Array({ ::std::move(vals) });
                        }
                    }
                }
                else if( lex.consume_if('{') ) {
                    ::std::vector<::MIR::Param> vals;
                    while( lex.next() != '}' )
                    {
                        vals.push_back( H::parse_param(*this, var_names) );
                        if( !lex.consume_if(',') )
                            break ;
                    }
                    lex.check_consume('}');
                    lex.check_consume(':');
                    auto p = HIR::GenericPath { RcString( lex.check_consume(TokenClass::Ident).strval.c_str() ) };

                    src_rval = ::MIR::RValue::make_Struct({ ::std::move(p), ::std::move(vals) });
                }
                else if( lex.consume_if("UNION") ) {
                    auto path = HIR::GenericPath { RcString( lex.check_consume(TokenClass::Ident).strval.c_str() ) };
                    //auto idx = static_cast<unsigned>(lex.consume_integer());
                    lex.check(TokenClass::Integer);
                    auto idx = static_cast<unsigned>(lex.consume().integer());
                    auto val = H::parse_param(*this, var_names);

                    src_rval = ::MIR::RValue::make_UnionVariant({ ::std::move(path), idx, ::std::move(val) });
                }
                else if( lex.consume_if("ENUM") ) {
                    auto path = HIR::GenericPath { RcString( lex.check_consume(TokenClass::Ident).strval.c_str() ) };
                    //auto idx = static_cast<unsigned>(lex.consume_integer());
                    lex.check(TokenClass::Integer);
                    auto idx = static_cast<unsigned>(lex.consume().integer());

                    lex.check_consume('{');
                    ::std::vector<::MIR::Param> vals;
                    while( lex.next() != '}' )
                    {
                        vals.push_back( H::parse_param(*this, var_names) );
                        if( !lex.consume_if(',') )
                            break ;
                    }
                    lex.check_consume('}');

                    src_rval = ::MIR::RValue::make_EnumVariant({ ::std::move(path), idx, ::std::move(vals) });
                }
                // Operations
                else if( lex.consume_if("CAST") ) {
                    auto lv = H::parse_lvalue(*this, var_names);
                    lex.check_consume("as");
                    auto ty = parse_type();
                    src_rval = ::MIR::RValue::make_Cast({ ::std::move(lv), ::std::move(ty) });
                }
                else if( lex.consume_if("UNIOP") ) {

                    lex.check(TokenClass::Symbol);
                    ::MIR::eUniOp   op;
                    if( lex.consume_if('!') ) {
                        op = ::MIR::eUniOp::INV;
                    }
                    else if( lex.consume_if('-') ) {
                        op = ::MIR::eUniOp::NEG;
                    }
                    else {
                        LOG_ERROR(lex << "Unexpected token in uniop - " << lex.next());
                    }

                    auto lv = H::parse_lvalue(*this, var_names);

                    src_rval = ::MIR::RValue::make_UniOp({ ::std::move(lv), op });
                }
                else if( lex.consume_if("BINOP") ) {
                    auto lv1 = H::parse_param(*this, var_names);
                    lex.check(TokenClass::Symbol);
                    auto t = lex.consume();
                    ::MIR::eBinOp   op;
                    switch(t.strval[0])
                    {
                    case '+':   op = (lex.consume_if('^') ? ::MIR::eBinOp::ADD_OV : ::MIR::eBinOp::ADD);    break;
                    case '-':   op = (lex.consume_if('^') ? ::MIR::eBinOp::SUB_OV : ::MIR::eBinOp::SUB);    break;
                    case '*':   op = (lex.consume_if('^') ? ::MIR::eBinOp::MUL_OV : ::MIR::eBinOp::MUL);    break;
                    case '/':   op = (lex.consume_if('^') ? ::MIR::eBinOp::DIV_OV : ::MIR::eBinOp::DIV);    break;
                    case '|':   op = ::MIR::eBinOp::BIT_OR ; break;
                    case '&':   op = ::MIR::eBinOp::BIT_AND; break;
                    case '^':   op = ::MIR::eBinOp::BIT_XOR; break;
                    case '%':   op = ::MIR::eBinOp::MOD;    break;
                    case '<':
                        if( t.strval[1] == '<' )
                            op = ::MIR::eBinOp::BIT_SHL;
                        else if( t.strval[1] == '=' )
                            op = ::MIR::eBinOp::LE;
                        else
                            op = ::MIR::eBinOp::LT;
                        break;
                    case '>':
                        if( lex.consume_if('>') )
                            op = ::MIR::eBinOp::BIT_SHR;
                        else if( lex.consume_if('=') )
                            op = ::MIR::eBinOp::GE;
                        else
                            op = ::MIR::eBinOp::GT;
                        break;
                    case '=':
                        op = ::MIR::eBinOp::EQ;
                        lex.check_consume('=');
                        break;
                    case '!':
                        op = ::MIR::eBinOp::NE;
                        lex.check_consume('=');
                        break;
                    default:
                        LOG_ERROR(lex << "Unexpected token " << t << " in BINOP");
                    }
                    auto lv2 = H::parse_param(*this, var_names);

                    src_rval = ::MIR::RValue::make_BinOp({ ::std::move(lv1), op, ::std::move(lv2) });
                }
                else if( lex.consume_if("MAKEDST") ) {
                    auto lv_ptr = H::parse_param(*this, var_names);
                    lex.check_consume(',');
                    auto lv_meta = H::parse_param(*this, var_names);
                    src_rval = ::MIR::RValue::make_MakeDst({ ::std::move(lv_ptr), ::std::move(lv_meta) });
                }
                else if( lex.consume_if("DSTPTR") ) {
                    auto lv = H::parse_lvalue(*this, var_names);
                    src_rval = ::MIR::RValue::make_DstPtr({ ::std::move(lv) });
                }
                else if( lex.consume_if("DSTMETA") ) {
                    auto lv = H::parse_lvalue(*this, var_names);
                    src_rval = ::MIR::RValue::make_DstMeta({ ::std::move(lv) });
                }
                else {
                    LOG_ERROR(lex << "Unexpected token in RValue - " << lex.next());
                }

                stmts.push_back(::MIR::Statement::make_Assign({ ::std::move(dst_val), ::std::move(src_rval) }));
            }
            else if( lex.consume_if("SETFLAG") )
            {
                lex.check(TokenClass::Ident);
                auto name = ::std::move(lex.consume().strval);
                auto df_it = ::std::find(drop_flag_names.begin(), drop_flag_names.end(), name);
                if( df_it == drop_flag_names.end() ) {
                    LOG_ERROR(lex << "Unable to find drop flag '" << name << "'");
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
                    lex.check(TokenClass::Ident);
                    auto name = ::std::move(lex.consume().strval);
                    df_it = ::std::find(drop_flag_names.begin(), drop_flag_names.end(), name);
                    if( df_it == drop_flag_names.end() ) {
                        LOG_ERROR(lex << "Unable to find drop flag '" << name << "'");
                    }
                    auto other_idx = static_cast<unsigned>( df_it - drop_flag_names.begin() );

                    stmts.push_back(::MIR::Statement::make_SetDropFlag({ df_idx, inv, other_idx }));
                }
            }
            else if(lex.consume_if("DROP") )
            {
                auto slot = H::parse_lvalue(*this, var_names);
                auto kind = ::MIR::eDropKind::DEEP;
                if( lex.consume_if("SHALLOW") )
                {
                    kind = ::MIR::eDropKind::SHALLOW;
                }
                unsigned flag_idx = ~0u;
                if( lex.consume_if("IF") )
                {
                    lex.check(TokenClass::Ident);
                    auto name = ::std::move(lex.consume().strval);
                    auto df_it = ::std::find(drop_flag_names.begin(), drop_flag_names.end(), name);
                    if( df_it == drop_flag_names.end() ) {
                        LOG_ERROR(lex << "Unable to find drop flag '" << name << "'");
                    }
                    flag_idx = static_cast<unsigned>( df_it - drop_flag_names.begin() );
                }

                stmts.push_back(::MIR::Statement::make_Drop({  kind, ::std::move(slot), flag_idx }));
            }
            else if( lex.consume_if("ASM") )
            {
                lex.check_consume('(');
                ::std::vector<::std::pair<::std::string, ::MIR::LValue>>  out_vals;
                while(lex.next() != ')')
                {
                    auto cons = ::std::move(lex.check_consume(TokenClass::String).strval);
                    lex.check_consume(':');
                    auto lv = H::parse_lvalue(*this, var_names);
                    if(!lex.consume_if(','))
                        break;
                    out_vals.push_back(::std::make_pair(::std::move(cons), ::std::move(lv)));
                }
                lex.check_consume(')');
                lex.check_consume('=');
                auto tpl = ::std::move(lex.check_consume(TokenClass::String).strval);

                lex.check_consume('(');
                ::std::vector<::std::pair<::std::string, ::MIR::LValue>>  in_vals;
                while(lex.next() != ')')
                {
                    auto cons = ::std::move(lex.check_consume(TokenClass::String).strval);
                    lex.check_consume(':');
                    auto lv = H::parse_lvalue(*this, var_names);
                    if(!lex.consume_if(','))
                        break;
                    in_vals.push_back(::std::make_pair(::std::move(cons), ::std::move(lv)));
                }
                lex.check_consume(')');

                lex.check_consume('[');
                ::std::vector<::std::string>  clobbers;
                while(lex.next() != ':')
                {
                    clobbers.push_back( ::std::move(lex.check_consume(TokenClass::String).strval) );
                    if(!lex.consume_if(','))
                        break;
                }
                lex.check_consume(':');
                ::std::vector<::std::string>  flags;
                while(lex.next() != ']')
                {
                    flags.push_back( ::std::move(lex.check_consume(TokenClass::Ident).strval) );
                    if(!lex.consume_if(','))
                        break;
                }
                lex.check_consume(']');

                stmts.push_back(::MIR::Statement::make_Asm({
                    ::std::move(tpl), ::std::move(out_vals), ::std::move(in_vals), ::std::move(clobbers), ::std::move(flags)
                    }));
            }
            else if( lex.consume_if("ASM2") )
            {
                ::MIR::Statement::Data_Asm2 stmt_asm2;
                lex.check_consume('(');
                do {
                    auto text = std::move(lex.check_consume(TokenClass::String).strval);
                    AsmCommon::Line line;

                    // Stripped-down version of the parsing code from the main compiler
                    const char* c = text.c_str();
                    std::string cur_string;
                    while(*c)
                    {
                        if(*c == '{')
                        {
                            AsmCommon::LineFragment frag;

                            c ++;
                            unsigned idx = 0;
                            while(*c && *c != ':' && *c != '}')
                            {
                                LOG_ASSERT('0' <= *c && *c <= '9', lex << "Non-integer argument in asm! format string");
                                idx *= 10;
                                idx += *c - '0';
                                c ++;
                            }
                            LOG_ASSERT(*c, lex << "Unexpected EOF in asm! format string");
                            frag.index = idx;
                            assert(*c == ':' || *c == '}');
                            if(*c == ':')
                            {
                                c ++;
                                LOG_ASSERT(*c, lex << "Unexpected EOF in asm! format string");
                                if(*c != '}') {
                                    frag.modifier = *c;
                                    c ++;
                                }
                            }
                            LOG_ASSERT(*c, lex << "Unexpected EOF in asm! format string");
                            LOG_ASSERT(*c == '}', lex << "Expected '}' in asm! format string");

                            frag.before = std::move(cur_string);
                            cur_string.clear();
                            line.frags.push_back(std::move(frag));
                        }
                        else {
                            cur_string += *c;
                        }
                        c ++;
                    }
                    line.trailing = std::move(cur_string);
                    stmt_asm2.lines.push_back(std::move(line));

                    if( lex.next() == ')' )
                        break;
                } while( !lex.consume_if(',') );

                // Arguments
                while( !lex.consume_if(')') )
                {
                    if( lex.consume_if("const") )
                    {
                        stmt_asm2.params.push_back( H::parse_const(*this) );
                    }
                    else if( lex.consume_if("sym") )
                    {
                        stmt_asm2.params.push_back( HIR::Path { RcString(lex.check_consume(TokenClass::Ident).strval.c_str()) } );
                    }
                    else if( lex.consume_if("reg") )
                    {
                        MIR::AsmParam::Data_Reg param;
                        lex.check_consume('(');
                        // Direction
                        lex.check(TokenClass::Ident);
                        /**/ if( lex.consume_if("in" ) ) { param.dir = AsmCommon::Direction::In;  }
                        else if( lex.consume_if("out") ) { param.dir = AsmCommon::Direction::Out; }
                        else if( lex.consume_if("lateout") ) { param.dir = AsmCommon::Direction::LateOut; }
                        else if( lex.consume_if("inlateout") ) { param.dir = AsmCommon::Direction::InLateOut; }
                        else {
                            LOG_ERROR(lex << "Unexpected token in ASM2 direction - " << lex.next());
                        }
                        // Spec
                        if( lex.next() == TokenClass::String ) {
                            param.spec = lex.consume().strval;
                        }
                        else if( lex.next() == TokenClass::Ident ) {
#define GET(_suf, _name)  if( lex.consume_if(#_name) ) { param.spec = AsmCommon::RegisterClass::_suf ## _ ## _name; }
                            /**/ GET(x86, reg)
                            else GET(x86, reg_byte)
                            else {
                                LOG_ERROR(lex << "Unexpected token in ASM2 register class - " << lex.next());
                            }
#undef GET
                        }
                        else {
                            LOG_ERROR(lex << "Unexpected token in ASM2 reg spec - " << lex.next());
                        }
                        lex.check_consume(')');
                        if( !lex.consume_if('_') ) {
                            param.input = std::make_unique<MIR::Param>(H::parse_param(*this, var_names));
                        }
                        lex.check_consume('=');
                        lex.check_consume('>');
                        if( !lex.consume_if('_') ) {
                            param.output = std::make_unique<MIR::LValue>(H::parse_lvalue(*this, var_names));
                        }

                        stmt_asm2.params.push_back(std::move(param));
                    }
                    else if( lex.consume_if("options") )
                    {
                        lex.check_consume('(');
                        
                        do {
                            if( lex.next() == ')' )
                                break;
                            
                            lex.check(TokenClass::Ident);
#define FLAG(_name)  if( lex.consume_if(#_name) ) { stmt_asm2.options._name = true; }
                            /**/ FLAG(pure)
                            else FLAG(nomem)
                            else FLAG(readonly)
                            else FLAG(preserves_flags)
                            else FLAG(noreturn)
                            else FLAG(nostack)
                            else FLAG(att_syntax)
                            else {
                                LOG_ERROR(lex << "Unexpected token in ASM2 options - " << lex.next());
                            }
#undef FLAG

                        } while( lex.consume_if(',') );
                        lex.check_consume(')');
                    }
                    else
                    {
                        LOG_ERROR(lex << "Unexpected token in ASM2 argument - " << lex.next());
                    }
                    if( !lex.consume_if(',') ) {
                        lex.consume_if(')');
                        break;
                    }
                }

                stmts.push_back(std::move(stmt_asm2));
            }
            else
            {
                break;
            }
            lex.check_consume(';');
            //LOG_TRACE(stmts.back());
        }

        lex.check(TokenClass::Ident);
        if( lex.consume_if("GOTO") )
        {
            term = ::MIR::Terminator::make_Goto(static_cast<unsigned>(lex.consume().integer()));
        }
        else if( lex.consume_if("PANIC") )
        {
            term = ::MIR::Terminator::make_Panic({ static_cast<unsigned>(lex.consume().integer()) });
        }
        else if( lex.consume_if("RETURN") )
        {
            term = ::MIR::Terminator::make_Return({});
        }
        else if( lex.consume_if("DIVERGE") )
        {
            term = ::MIR::Terminator::make_Diverge({});
        }
        else if( lex.consume_if("INCOMPLETE") )
        {
            term = ::MIR::Terminator::make_Incomplete({});
        }
        else if( lex.consume_if("IF") )
        {
            auto val = H::parse_lvalue(*this, var_names);
            lex.check_consume("goto");
            auto tgt_true = static_cast<unsigned>(lex.consume().integer());
            lex.check_consume("else");
            auto tgt_false = static_cast<unsigned>(lex.consume().integer());
            term = ::MIR::Terminator::make_If({ ::std::move(val), tgt_true, tgt_false });
        }
        else if( lex.consume_if("SWITCH") )
        {
            auto val = H::parse_lvalue(*this, var_names);
            lex.check_consume('{');
            ::std::vector<unsigned> targets;
            while(lex.next() != '{')
            {
                targets.push_back( static_cast<unsigned>(lex.check_consume(TokenClass::Integer).integer()) );
                if( !lex.consume_if(',') )
                    break;
            }
            lex.check_consume('}');

            term = ::MIR::Terminator::make_Switch({ ::std::move(val), ::std::move(targets) });
        }
        else if( lex.consume_if("SWITCHVALUE") )
        {
            auto val = H::parse_lvalue(*this, var_names);
            ::std::vector<::MIR::BasicBlockId>  targets;
            lex.check_consume('{');
            ::MIR::SwitchValues vals;
            if( lex.next() == TokenClass::Integer ) {
                ::std::vector<uint64_t> values;
                while(lex.next() != '_')
                {
                    values.push_back( lex.check_consume(TokenClass::Integer).integer() );
                    lex.check_consume('=');
                    targets.push_back( static_cast<unsigned>( lex.check_consume(TokenClass::Integer).integer() ) );
                    lex.check_consume(',');
                }
                vals = ::MIR::SwitchValues::make_Unsigned(::std::move(values));
            }
            else if( lex.next() == '+' || lex.next() == '-' ) {
                ::std::vector<int64_t> values;
                while(lex.next() != '_')
                {
                    auto neg = lex.consume() == '-';
                    int64_t val = static_cast<int64_t>( lex.check_consume(TokenClass::Integer).integer() );
                    values.push_back( neg ? -val : val );
                    lex.check_consume('=');
                    targets.push_back( static_cast<unsigned>( lex.check_consume(TokenClass::Integer).integer() ) );
                    lex.check_consume(',');
                }
                vals = ::MIR::SwitchValues::make_Signed(::std::move(values));
            }
            else if( lex.next() == TokenClass::String ) {
                ::std::vector<::std::string> values;
                while(lex.next() != '_')
                {
                    values.push_back( ::std::move(lex.check_consume(TokenClass::String).strval) );
                    lex.check_consume('=');
                    targets.push_back( static_cast<unsigned>( lex.check_consume(TokenClass::Integer).integer() ) );
                    lex.check_consume(',');
                }
                vals = ::MIR::SwitchValues::make_String(::std::move(values));
            }
            else {
                LOG_ERROR(lex << "Unexpected token for SWITCHVALUE value - " << lex.next());
            }
            lex.check_consume('_');
            lex.check_consume('=');
            auto def_tgt = static_cast<unsigned>( lex.check_consume(TokenClass::Integer).integer() );
            lex.check_consume('}');

            term = ::MIR::Terminator::make_SwitchValue({ ::std::move(val), def_tgt, ::std::move(targets), ::std::move(vals) });
        }
        else if( lex.consume_if("CALL") )
        {
            auto dst = H::parse_lvalue(*this, var_names);
            lex.check_consume('=');
            ::MIR::CallTarget   ct;
            if(lex.consume_if('(')) {
                ct = H::parse_lvalue(*this, var_names);
                lex.check_consume(')');
            }
            else if( lex.next() == TokenClass::String ) {
                auto name = RcString::new_interned(lex.consume().strval);
                auto params = parse_pathparams();
                ct = ::MIR::CallTarget::make_Intrinsic({ ::std::move(name), ::std::move(params) });
            }
            else {
                ct = HIR::Path { RcString(lex.check_consume(TokenClass::Ident).strval.c_str()) };
            }
            lex.check_consume('(');
            ::std::vector<::MIR::Param> args;
            while(lex.next() != ')')
            {
                args.push_back(H::parse_param(*this, var_names));
                if( !lex.consume_if(',') )
                    break;
            }
            lex.check_consume(')');
            lex.check_consume("goto");
            //auto tgt_idx = lex.consume_integer();
            lex.check(TokenClass::Integer);
            auto tgt_block = static_cast<unsigned>(lex.consume().integer());
            lex.check_consume("else");
            lex.check(TokenClass::Integer);
            auto panic_block = static_cast<unsigned>(lex.consume().integer());

            term = ::MIR::Terminator::make_Call({ tgt_block, panic_block, ::std::move(dst), ::std::move(ct), ::std::move(args) });
        }
        else
        {
            LOG_ERROR(lex << "Unexpected token at terminator - " << lex.next());
        }

        lex.check_consume('}');

        rv.blocks.push_back(::MIR::BasicBlock { ::std::move(stmts), ::std::move(term) });
    }

    lex.check_consume('}');
    return rv;
}
::HIR::PathParams Parser::parse_pathparams()
{
    ::HIR::PathParams   params;
    if( lex.consume_if('<') )
    {
        while(lex.next() != '>')
        {
            params.tys.push_back( parse_type() );
            if( !lex.consume_if(',') )
                break ;
        }
        lex.check_consume('>');
    }
    return params;
}
namespace {
    const struct CtMapping {
        RcString    tok;
        RawType val;
    } S_RAWTPYE_MAPPINGS[] = {
        { RcString("u8"  ), RawType::U8   },
        { RcString("u16" ), RawType::U16  },
        { RcString("u32" ), RawType::U32  },
        { RcString("u64" ), RawType::U64  },
        { RcString("u128"), RawType::U128 },
        { RcString("i8"  ), RawType::I8   },
        { RcString("i16" ), RawType::I16  },
        { RcString("i32" ), RawType::I32  },
        { RcString("i64" ), RawType::I64  },
        { RcString("i128"), RawType::I128 },
        { RcString("usize"), RawType::USize },
        { RcString("isize"), RawType::ISize },
        { RcString("f32"), RawType::F32  },
        { RcString("f64"), RawType::F64  },
        { RcString("bool"), RawType::Bool },
        { RcString("char"), RawType::Char },
        { RcString("str" ), RawType::Str },
        };
}
RawType Parser::parse_core_type()
{
    lex.check(TokenClass::Ident);
    auto tok = RcString( lex.consume().strval.c_str() );
    for(size_t i = 0; i < sizeof(S_RAWTPYE_MAPPINGS)/sizeof(S_RAWTPYE_MAPPINGS[0]); i ++)
    {
        const auto& m = S_RAWTPYE_MAPPINGS[i];
        if( tok == m.tok ) {
            return m.val;
        }
    }
    LOG_ERROR(lex << "Unknown core type " << tok << "'");
}
::HIR::TypeRef Parser::parse_type()
{
    if( lex.consume_if('(') )
    {
        lex.check_consume(')');
        // Unit!
        return ::HIR::TypeRef::unit();
    }
    else if( lex.consume_if('[') )
    {
        auto rv = parse_type();
        if( lex.consume_if(';') )
        {
            size_t size = lex.check_consume(TokenClass::Integer).integer();
            lex.check_consume(']');
            return ::std::move(rv).wrap( TypeWrapper::Ty::Array, size );
        }
        else
        {
            lex.check_consume(']');
            return ::std::move(rv).wrap( TypeWrapper::Ty::Slice, 0 );
        }
    }
    else if( lex.consume_if('!') )
    {
        return ::HIR::TypeRef::diverge();
    }
    else if( lex.consume_if('&') )
    {
        if( lex.next() == TokenClass::Lifetime )
        {
            // TODO: Handle lifetime names (require them?)
            lex.consume();
        }
        auto bt = ::HIR::BorrowType::Shared;
        if( lex.consume_if("move") )
            bt = ::HIR::BorrowType::Move;
        else if( lex.consume_if("mut") )
            bt = ::HIR::BorrowType::Unique;
        else
            ; // keep as shared
        return parse_type().wrap( TypeWrapper::Ty::Borrow, static_cast<size_t>(bt) );
    }
    else if( lex.consume_if('*') )
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
        return parse_type().wrap( TypeWrapper::Ty::Pointer, static_cast<size_t>(bt) );
    }
    else if( lex.next() == "extern" || lex.next() == "fn" || lex.next() == "unsafe" )
    {
        bool is_unsafe = false;
        ::std::string abi = "Rust";
        if( lex.consume_if("unsafe") )
        {
            is_unsafe = true;
        }
        if( lex.consume_if("extern") )
        {
            abi = ::std::move(lex.check_consume(TokenClass::String).strval);
        }
        lex.check_consume("fn");
        lex.check_consume('(');
        ::std::vector<::HIR::TypeRef>   args;
        while( lex.next() != ')' )
        {
            args.push_back(parse_type());
            if( !lex.consume_if(',') )
                break;
        }
        lex.check_consume(')');
        ::HIR::TypeRef  ret_ty;
        if( lex.consume_if('-') )
        {
            lex.check_consume('>');
            ret_ty = parse_type();
        }
        else
        {
            ret_ty = ::HIR::TypeRef::unit();
        }
        auto ft = FunctionType {
            is_unsafe,
            ::std::move(abi),
            ::std::move(args),
            ::std::move(ret_ty)
            };
        const auto* ft_p = &*tree.function_types.insert(::std::move(ft)).first;
        return ::HIR::TypeRef(ft_p);
        // TODO: Use abi/ret_ty/args as part of that
    }
    else if( lex.consume_if("dyn") )
    {
        RcString vtable_ty = lex.check_consume(TokenClass::Ident).strval.c_str();

        auto rv = ::HIR::TypeRef(RawType::TraitObject);
        //if( base_trait != ::HIR::GenericPath() )
        {
            // Generate vtable path
            // - TODO: Associated types? (Need to ensure ordering is correct)
            rv.ptr.composite_type = this->get_composite(vtable_ty);
        }
        //else
        //{
        //    // TODO: vtable for empty trait?
        //}
        return rv;
    }
    else if( lex.next() == TokenClass::Ident )
    {
        auto name = RcString( lex.consume().strval.c_str() );
        for(size_t i = 0; i < sizeof(S_RAWTPYE_MAPPINGS)/sizeof(S_RAWTPYE_MAPPINGS[0]); i ++)
        {
            const auto& m = S_RAWTPYE_MAPPINGS[i];
            if( name == m.tok ) {
                return ::HIR::TypeRef(m.val);
            }
        }
        return ::HIR::TypeRef( this->get_composite(::std::move(name)));
    }
    else
    {
        LOG_ERROR(lex << "Unexpected token in type - " << lex.next());
    }
}
const DataType* Parser::get_composite(RcString gp)
{
    auto it = tree.data_types.find(gp);
    if( it == tree.data_types.end() )
    {
        // TODO: Later on need to check if the type is valid.
        auto v = ::std::make_unique<DataType>(DataType {});
        v->populated = false;
        v->my_path = gp;
        auto ir = tree.data_types.insert(::std::make_pair( ::std::move(gp), ::std::move(v)) );
        it = ir.first;
    }
    return it->second.get();
}

const Function& ModuleTree::get_function(const HIR::Path& p) const
{
    auto it = functions.find(p.n);
    if(it == functions.end())
    {
        LOG_ERROR("Unable to find function " << p << " for invoke");
    }
    return it->second;
}
const Function* ModuleTree::get_function_opt(const HIR::Path& p) const
{
    auto it = functions.find(p.n);
    if(it == functions.end())
    {
        return nullptr;
    }
    return &it->second;
}
const Function* ModuleTree::get_ext_function(const char* name) const
{
    auto it = ext_functions.find(name);
    if( it == ext_functions.end() )
    {
        return nullptr;
    }
    return it->second;
}
const Static& ModuleTree::get_static(const HIR::Path& p) const
{
    auto it = statics.find(p.n);
    if(it == statics.end())
    {
        LOG_ERROR("Unable to find static " << p << " for invoke");
    }
    return it->second;
}
const Static* ModuleTree::get_static_opt(const HIR::Path& p) const
{
    auto it = statics.find(p.n);
    if(it == statics.end())
    {
        return nullptr;
    }
    return &it->second;
}
