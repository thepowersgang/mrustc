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
    ::HIR::PathParams parse_pathparams();
    ::HIR::GenericPath parse_genericpath();
    ::HIR::SimplePath parse_simplepath();
    RawType parse_core_type();
    ::HIR::TypeRef parse_type();
    ::HIR::GenericPath parse_tuple();
};

void ModuleTree::load_file(const ::std::string& path)
{
    ::std::cout << "DEBUG: load_file(" << path << ")" << ::std::endl;
    //TRACE_FUNCTION_F(path);
    auto parse = Parser { *this, path };

    while(parse.parse_one())
    {
        // Keep going!
    }
}
// Parse a single item from a .mir file
bool Parser::parse_one()
{
    //::std::cout << "DEBUG: parse_one" << ::std::endl;
    if( lex.next() == "" )  // EOF?
    {
        return false;
    }

    if( lex.consume_if("crate") )
    {
        // Import an external crate
        lex.check(TokenClass::String);
        auto path = ::std::move(lex.next().strval);
        lex.consume();
        //::std::cout << "DEBUG: parse_one - crate '" << path << "'" << ::std::endl;

        lex.check_consume(';');


        this->tree.load_file(path);
    }
    else if( lex.consume_if("fn") )
    {
        auto p = parse_path();
        //::std::cout << "DEBUG:p arse_one - fn " << p << ::std::endl;

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
        //::std::cout << "DEBUG: parse_one - static " << p << ::std::endl;
        lex.check_consume('=');
        // TODO: Body? Value?
        //auto body = parse_body();
        //auto data = ::std::move(lex.consume().strval);
        throw "TODO";
    }
    else if( lex.consume_if("type") )
    {
        auto p = (lex.consume_if('(')) ? parse_tuple() : parse_genericpath();
        //::std::cout << "DEBUG: parse_one - type " << p << ::std::endl;

        auto rv = DataType {};

        lex.check_consume('{');
        lex.check_consume("SIZE");
        rv.size = lex.consume().integer();
        lex.check_consume(',');
        lex.check_consume("ALIGN");
        rv.alignment = lex.consume().integer();
        if( rv.alignment == 0 )
        {
            ::std::cerr << lex << "Alignment of zero is invalid, " << p << ::std::endl;
            throw "ERROR";
        }
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
            lex.check_consume(';');
            //::std::cout << ofs << " " << ty << ::std::endl;

            rv.fields.push_back(::std::make_pair(ofs, ::std::move(ty)));
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
            lex.check_consume(';');

            rv.variants.push_back({ base_idx, other_idx, v });
        }
        lex.check_consume('}');

        auto it = this->tree.data_types.find(p);
        if( it != this->tree.data_types.end() )
        {
            if( it->second->alignment == 0 )
            {
                *it->second = ::std::move(rv);
            }
            else
            {
                ::std::cerr << lex << "Duplicate definition of " << p << ::std::endl;
                // Not really an error, can happen when loading crates
                //throw "ERROR";
            }
        }
        else
        {
            this->tree.data_types.insert(::std::make_pair( ::std::move(p), ::std::make_unique<DataType>(::std::move(rv)) ));
        }
    }
    else
    {
        ::std::cerr << lex << "Unexpected token at root - " << lex.next() << ::std::endl;

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
                        ::std::cerr << lex << "Invalid argument name - " << name << " - " << e.what() << ::std::endl;
                        throw "ERROR";
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
                        ::std::cerr << lex << "Cannot find variable named '" << name << "'" << ::std::endl;
                        throw "ERROR";
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
                ::std::cerr << lex << "Unexpected token in LValue - " << lex.next() << ::std::endl;
                throw "ERROR";
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
            else if( p.lex.next() == '+' || p.lex.next() == '-' ) {
                bool is_neg = (p.lex.consume() == '-');
                auto v = static_cast<int64_t>(p.lex.consume().integer());
                auto cty = p.parse_core_type();
                return ::MIR::Constant::make_Int({ is_neg ? -v : v, cty });
            }
            else if( p.lex.consume_if("true") ) {
                return ::MIR::Constant::make_Bool({ true });
            }
            else if( p.lex.consume_if("false") ) {
                return ::MIR::Constant::make_Bool({ false });
            }
            else if( p.lex.consume_if("&") ) {
                auto path = p.parse_path();

                return ::MIR::Constant::make_ItemAddr({ ::std::move(path) });
            }
            else {
                ::std::cerr << p.lex << "BUG? " << p.lex.next() << ::std::endl;
                throw "ERROR";
            }
        }

        // Parse a "Param" (constant or lvalue)
        static ::MIR::Param parse_param(Parser& p, ::std::vector<::std::string>& var_names)
        {
            if( p.lex.next() == TokenClass::Integer || p.lex.next() == '+' || p.lex.next() == '-' || p.lex.next() == '&' || p.lex.next() == "true" || p.lex.next() == "false" ) {
                return parse_const(p);
            }
            else {
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
                if( lex.next() == TokenClass::Integer || lex.next() == '+' || lex.next() == '-' || lex.next() == "true" || lex.next() == "false" ) {
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
                        ::std::cerr << lex << "Unexpected token in uniop - " << lex.next() << ::std::endl;
                        throw "ERROR";
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
                    case '<':
                        if( lex.consume_if('<') )
                            op = ::MIR::eBinOp::BIT_SHL;
                        else if( lex.consume_if('=') )
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
                        op = ::MIR::eBinOp::EQ; if(0)
                    case '!':
                        op = ::MIR::eBinOp::NE;
                        lex.check_consume('=');
                        break;
                    default:
                        ::std::cerr << lex << "Unexpected token " << t << " in BINOP" << ::std::endl;
                        throw "ERROR";
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
                    ::std::cerr << lex << "Unexpected token in RValue - " << lex.next() << ::std::endl;
                    throw "ERROR";
                }

                stmts.push_back(::MIR::Statement::make_Assign({ ::std::move(dst_val), ::std::move(src_rval) }));
            }
            else if( lex.consume_if("SETFLAG") )
            {
                lex.check(TokenClass::Ident);
                auto name = ::std::move(lex.consume().strval);
                auto df_it = ::std::find(drop_flag_names.begin(), drop_flag_names.end(), name);
                if( df_it == drop_flag_names.end() ) {
                    ::std::cerr << lex << "Unable to find drop flag '" << name << "'" << ::std::endl;
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
                    lex.check(TokenClass::Ident);
                    auto name = ::std::move(lex.consume().strval);
                    df_it = ::std::find(drop_flag_names.begin(), drop_flag_names.end(), name);
                    if( df_it == drop_flag_names.end() ) {
                        ::std::cerr << lex << "Unable to find drop flag '" << name << "'" << ::std::endl;
                        throw "ERROR";
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
                        ::std::cerr << lex << "Unable to find drop flag '" << name << "'" << ::std::endl;
                        throw "ERROR";
                    }
                    flag_idx = static_cast<unsigned>( df_it - drop_flag_names.begin() );
                }

                stmts.push_back(::MIR::Statement::make_Drop({  kind, ::std::move(slot), flag_idx }));
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
            //::std::cout << stmts.back() << ::std::endl;
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
        else if( lex.consume_if("SWITCHVAL") )
        {
            auto val = H::parse_lvalue(*this, var_names);
            throw "TODO";
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
            ::std::cerr << lex << "Unexpected token at terminator - " << lex.next() << ::std::endl;
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
        if( !lex.consume_if(',') )
            break;
    } while( lex.next() != ')' );
    lex.check_consume(')');

    return gp;
}
RawType Parser::parse_core_type()
{
    //::std::cout << lex.next() << ::std::endl;
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
        ::std::cerr << lex << "Unknown core type " << tok << "'" << ::std::endl;
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

        // Look up this type, then create a TypeRef referring to the type in the datastore
        // - May need to create an unpopulated type?
        auto it = tree.data_types.find(gp);
        if( it == tree.data_types.end() )
        {
            // TODO: Later on need to check if the type is valid.
            auto v = ::std::make_unique<DataType>(DataType {});
            auto ir = tree.data_types.insert(::std::make_pair( ::std::move(gp), ::std::move(v)) );
            it = ir.first;
        }
        // Good.
        return ::HIR::TypeRef(it->second.get());
    }
    else if( lex.consume_if('[') )
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
    else if( lex.consume_if('!') )
    {
        return ::HIR::TypeRef::diverge();
    }
    else if( lex.consume_if('&') )
    {
        auto bt = ::HIR::BorrowType::Shared;
        if( lex.consume_if("move") )
            bt = ::HIR::BorrowType::Move;
        else if( lex.consume_if("mut") )
            bt = ::HIR::BorrowType::Unique;
        else
            ; // keep as shared
        auto rv = parse_type();
        rv.wrappers.insert( rv.wrappers.begin(), { TypeWrapper::Ty::Borrow, static_cast<size_t>(bt) });
        return rv;
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
        auto rv = parse_type();
        rv.wrappers.insert( rv.wrappers.begin(), { TypeWrapper::Ty::Pointer, static_cast<size_t>(bt) });
        return rv;
    }
    else if( lex.next() == "::" )
    {
        auto path = parse_genericpath();
        // Look up this type, then create a TypeRef referring to the type in the datastore
        // - May need to create an unpopulated type?
        auto it = tree.data_types.find(path);
        if( it == tree.data_types.end() )
        {
            // TODO: Later on need to check if the type is valid.
            auto v = ::std::make_unique<DataType>(DataType {});
            auto ir = tree.data_types.insert(::std::make_pair( ::std::move(path), ::std::move(v)) );
            it = ir.first;
        }
        // Good.
        return ::HIR::TypeRef(it->second.get());
    }
    else if( lex.next() == "extern" || lex.next() == "fn" )
    {
        ::std::string abi = "Rust";
        if( lex.consume_if("extern") )
        {
            // TODO: Save the ABI
            lex.check(TokenClass::String);
            abi = lex.consume().strval;
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
        lex.check_consume('-');
        lex.check_consume('>');
        auto ret_ty = parse_type();
        return ::HIR::TypeRef(RawType::Function);
        // TODO: Use abi/ret_ty/args as part of that
    }
    else if( lex.consume_if("dyn") )
    {
        lex.consume_if('(');
        ::HIR::GenericPath  base_trait;
        if( lex.next() != '+' )
        {
            base_trait = parse_genericpath();
        }
        ::std::vector<::HIR::GenericPath>   markers;
        while(lex.consume_if('+'))
        {
            markers.push_back(parse_genericpath());
            // TODO: Lifetimes?
        }
        lex.consume_if(')');
        return ::HIR::TypeRef(RawType::TraitObject);
        // TODO: Figure out how to include the traits in this type.
    }
    else if( lex.next() == TokenClass::Ident )
    {
        return ::HIR::TypeRef(parse_core_type());
    }
    else
    {
        ::std::cerr << lex << "Unexpected token in type - " << lex.next() << ::std::endl;
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