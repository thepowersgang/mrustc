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
    ::HIR::Path parse_path();
    ::HIR::PathParams parse_pathparams();
    ::HIR::GenericPath parse_genericpath();
    ::HIR::SimplePath parse_simplepath();
    RawType parse_core_type();
    ::HIR::TypeRef parse_type();
    ::HIR::GenericPath parse_tuple();

    const DataType* get_composite(::HIR::GenericPath gp);
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
        lex.check(TokenClass::String);
        auto path = ::std::move(lex.next().strval);
        lex.consume();
        //LOG_TRACE(lex << "crate '" << path << "'");

        lex.check_consume(';');


        this->tree.load_file(path);
    }
    else if( lex.consume_if("fn") )
    {
        auto p = parse_path();
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

        if( lex.consume_if('=') )
        {
            auto link_name = ::std::move(lex.check_consume(TokenClass::String).strval);
            lex.check_consume(':');
            auto abi = ::std::move(lex.check_consume(TokenClass::String).strval);
            lex.check_consume(';');

            LOG_DEBUG(lex << "extern fn " << p);
            auto p2 = p;
            tree.functions.insert( ::std::make_pair(::std::move(p), Function { ::std::move(p2), ::std::move(arg_tys), rv_ty, {link_name, abi}, {} }) );
        }
        else
        {
            auto body = parse_body();

            LOG_DEBUG(lex << "fn " << p);
            auto p2 = p;
            tree.functions.insert( ::std::make_pair(::std::move(p), Function { ::std::move(p2), ::std::move(arg_tys), rv_ty, {}, ::std::move(body) }) );
        }
    }
    else if( lex.consume_if("static") )
    {
        auto p = parse_path();
        //LOG_TRACE(lex << "static " << p);
        lex.check_consume(':');
        auto ty = parse_type();
        // TODO: externs?
        lex.check_consume('=');
        lex.check(TokenClass::String);
        auto data = ::std::move(lex.consume().strval);

        Static s;
        s.val = Value(ty);
        // - Statics need to always have an allocation (for references)
        if( !s.val.allocation )
            s.val.create_allocation();
        s.val.write_bytes(0, data.data(), data.size());
        s.ty = ty;

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

                    auto a = Allocation::new_alloc( reloc_str.size() );
                    //a.alloc().set_tag();
                    a->write_bytes(0, reloc_str.data(), reloc_str.size());
                    s.val.allocation->relocations.push_back({ static_cast<size_t>(ofs), /*size,*/ RelocationPtr::new_alloc(::std::move(a)) });
                }
                else if( lex.next() == "::" || lex.next() == "<" )
                {
                    auto reloc_path = parse_path();
                    s.val.allocation->relocations.push_back({ static_cast<size_t>(ofs), /*size,*/ RelocationPtr::new_fcn(reloc_path) });
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
        auto p = (lex.consume_if('(')) ? parse_tuple() : parse_genericpath();
        //LOG_TRACE("type " << p);

        auto rv = DataType {};
        rv.my_path = p;

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
            rv.drop_glue = parse_path();
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
            else if(lex.next() == TokenClass::Ident && lex.next().strval[0] == '#' )
            {
                size_t var_idx = ::std::stoi(lex.consume().strval.substr(1));

                size_t data_fld = SIZE_MAX;
                if( lex.consume_if('=') )
                {
                    data_fld = lex.consume().integer();
                }

                size_t tag_ofs = SIZE_MAX;

                size_t base_idx = SIZE_MAX;
                ::std::vector<size_t>   other_idx;
                ::std::string tag_value;
                if( lex.consume_if('@') )
                {
                    lex.check_consume('[');
                    base_idx = lex.consume().integer();
                    if( lex.consume_if(',') )
                    {
                        while(lex.next() != ']')
                        {
                            lex.check(TokenClass::Integer);
                            other_idx.push_back( lex.consume().integer() );
                            if( !lex.consume_if(',') )
                                break;
                        }
                    }
                    lex.check_consume(']');

                    lex.check_consume('=');
                    lex.check(TokenClass::String);
                    tag_value = ::std::move(lex.consume().strval);

                    //tag_ofs = rv.fields.at(base_idx).first;
                    //const auto* tag_ty = &rv.fields.at(base_idx).second;
                    //for(auto idx : other_idx)
                    //{
                    //    assert(tag_ty->get_wrapper() == nullptr);
                    //    assert(tag_ty->inner_type == RawType::Composite);
                    //    LOG_TODO(lex << "Calculate tag offset with nested tag - " << idx << " ty=" << *tag_ty);
                    //}
                }
                lex.check_consume(';');

                if( rv.variants.size() <= var_idx )
                {
                    rv.variants.resize(var_idx+1);
                }
                rv.variants[var_idx] = { data_fld, base_idx, other_idx, tag_value };
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
        static ::std::unique_ptr<::MIR::LValue> make_lvp(::MIR::LValue&& lv) {
            return ::std::unique_ptr<::MIR::LValue>(new ::MIR::LValue(::std::move(lv)));
        }
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
                        lv = ::MIR::LValue::make_Argument({ idx });
                    }
                    catch(const ::std::exception& e) {
                        LOG_ERROR(lex << "Invalid argument name - " << name << " - " << e.what());
                    }
                }
                // Hard-coded "RETURN" lvalue
                else if( name == "RETURN" ) {
                    lv = ::MIR::LValue::make_Return({});
                }
                // Otherwise, look up variable names
                else {
                    auto it = ::std::find(var_names.begin(), var_names.end(), name);
                    if( it == var_names.end() ) {
                        LOG_ERROR(lex << "Cannot find variable named '" << name << "'");
                    }
                    lv = ::MIR::LValue::make_Local(static_cast<unsigned>(it - var_names.begin()));
                }
            }
            else if( lex.next() == "::" || lex.next() == '<' )
            {
                auto path = p.parse_path();
                lv = ::MIR::LValue( ::std::move(path) );
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
                    lv = ::MIR::LValue::make_Downcast({ make_lvp(::std::move(lv)), idx });
                }
                else if( lex.consume_if('.') )
                {
                    lex.check(TokenClass::Integer);
                    auto idx = static_cast<unsigned>( lex.consume().integer() );
                    lv = ::MIR::LValue::make_Field({ make_lvp(::std::move(lv)), idx });
                }
                else if( lex.next() == '[' )
                {
                    lex.consume();
                    auto idx_lv = parse_lvalue(p, var_names);
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
                auto path = p.parse_path();

                return ::MIR::Constant::make_ItemAddr({ ::std::move(path) });
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
                    src_rval = ::MIR::RValue::make_Borrow({ 0, bt, ::std::move(val) });
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
                    auto p = parse_genericpath();

                    src_rval = ::MIR::RValue::make_Struct({ ::std::move(p), ::std::move(vals) });
                }
                else if( lex.consume_if("VARIANT") ) {
                    auto path = parse_genericpath();
                    //auto idx = static_cast<unsigned>(lex.consume_integer());
                    lex.check(TokenClass::Integer);
                    auto idx = static_cast<unsigned>(lex.consume().integer());
                    auto val = H::parse_param(*this, var_names);

                    src_rval = ::MIR::RValue::make_Variant({ ::std::move(path), idx, ::std::move(val) });
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
                targets.push_back( static_cast<unsigned>(lex.consume().integer()) );
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
                auto name = ::std::move(lex.consume().strval);
                auto params = parse_pathparams();
                ct = ::MIR::CallTarget::make_Intrinsic({ ::std::move(name), ::std::move(params) });
            }
            else {
                ct = parse_path();
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

        ::HIR::PathParams   params = parse_pathparams();

        return ::HIR::Path( ::std::move(ty), ::std::move(trait), ::std::move(item_name), ::std::move(params) );
    }
    else
    {
        return parse_genericpath();
    }
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
::HIR::GenericPath Parser::parse_genericpath()
{
    ::HIR::GenericPath  rv;
    rv.m_simplepath = parse_simplepath();
    rv.m_params = parse_pathparams();
    return rv;
}
::HIR::SimplePath Parser::parse_simplepath()
{
    lex.check_consume("::");
    ::std::string   crate;
    if( lex.next() == TokenClass::String )
    {
        crate = lex.consume().strval;
        lex.check_consume("::");
    }
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
        if( !lex.consume_if(',') )
            break;
    } while( lex.next() != ')' );
    lex.check_consume(')');

    return gp;
}
RawType Parser::parse_core_type()
{
    //LOG_TRACE(lex.next());
    lex.check(TokenClass::Ident);
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
    else if( tok == "usize" ) {
        return RawType::USize;
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
    else if( tok == "isize" ) {
        return RawType::ISize;
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
    else if( tok == "char" ) {
        return RawType::Char;
    }
    else if( tok == "str" ) {
        return RawType::Str;
    }
    else {
        LOG_ERROR(lex << "Unknown core type " << tok << "'");
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

        // Good.
        return ::HIR::TypeRef( this->get_composite(::std::move(gp)) );
    }
    else if( lex.consume_if('[') )
    {
        auto rv = parse_type();
        if( lex.consume_if(';') )
        {
            size_t size = lex.next().integer();
            lex.consume();
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
    else if( lex.next() == "::" )
    {
        auto path = parse_genericpath();
        return ::HIR::TypeRef( this->get_composite(::std::move(path)));
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
        return ::HIR::TypeRef(RawType::Function);
        // TODO: Use abi/ret_ty/args as part of that
    }
    else if( lex.consume_if("dyn") )
    {
        lex.consume_if('(');
        ::HIR::GenericPath  base_trait;
        ::std::vector<::std::pair<::std::string, ::HIR::TypeRef>>   atys;
        if( lex.next() != '+' )
        {
            // Custom TraitPath parsing.
            base_trait.m_simplepath = parse_simplepath();
            if( lex.consume_if('<') )
            {
                while(lex.next() != '>')
                {
                    if( lex.next() == TokenClass::Ident && lex.lookahead() == '=' )
                    {
                        auto name = ::std::move(lex.consume().strval);
                        lex.check_consume('=');
                        auto ty = parse_type();
                        atys.push_back(::std::make_pair( ::std::move(name), ::std::move(ty) ));
                    }
                    else
                    {
                        base_trait.m_params.tys.push_back( parse_type() );
                    }
                    if( !lex.consume_if(',') )
                        break ;
                }
                lex.check_consume('>');
            }
        }
        ::std::vector<::HIR::GenericPath>   markers;
        while(lex.consume_if('+'))
        {
            if( lex.next() == TokenClass::Lifetime )
            {
                // TODO: Include lifetimes in output?
                lex.consume();
            }
            else
            {
                markers.push_back(parse_genericpath());
            }
        }
        lex.consume_if(')');

        auto rv = ::HIR::TypeRef(RawType::TraitObject);
        if( base_trait != ::HIR::GenericPath() )
        {
            // Generate vtable path
            auto vtable_path = base_trait;
            vtable_path.m_simplepath.ents.back() += "#vtable";
            // - TODO: Associated types?
            rv.composite_type = this->get_composite( ::std::move(vtable_path) );
        }
        return rv;
    }
    else if( lex.next() == TokenClass::Ident )
    {
        return ::HIR::TypeRef(parse_core_type());
    }
    else
    {
        LOG_ERROR(lex << "Unexpected token in type - " << lex.next());
    }
}
const DataType* Parser::get_composite(::HIR::GenericPath gp)
{
    auto it = tree.data_types.find(gp);
    if( it == tree.data_types.end() )
    {
        // TODO: Later on need to check if the type is valid.
        auto v = ::std::make_unique<DataType>(DataType {});
        v->my_path = gp;
        auto ir = tree.data_types.insert(::std::make_pair( ::std::move(gp), ::std::move(v)) );
        it = ir.first;
    }
    return it->second.get();
}

::HIR::SimplePath ModuleTree::find_lang_item(const char* name) const
{
    return ::HIR::SimplePath({ "", { "main#" } });
}
const Function& ModuleTree::get_function(const ::HIR::Path& p) const
{
    auto it = functions.find(p);
    if(it == functions.end())
    {
        LOG_ERROR("Unable to find function " << p << " for invoke");
    }
    return it->second;
}
const Function* ModuleTree::get_function_opt(const ::HIR::Path& p) const
{
    auto it = functions.find(p);
    if(it == functions.end())
    {
        return nullptr;
    }
    return &it->second;
}
Static& ModuleTree::get_static(const ::HIR::Path& p)
{
    auto it = statics.find(p);
    if(it == statics.end())
    {
        LOG_ERROR("Unable to find static " << p << " for invoke");
    }
    return it->second;
}
Static* ModuleTree::get_static_opt(const ::HIR::Path& p)
{
    auto it = statics.find(p);
    if(it == statics.end())
    {
        return nullptr;
    }
    return &it->second;
}
