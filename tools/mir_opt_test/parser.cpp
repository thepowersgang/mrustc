/*
 */
#include "test_desc.h"
#include <parse/lex.hpp>
#include <parse/common.hpp>
#include <parse/parseerror.hpp>
#include <climits>	// UINT_MAX

#include <hir/hir.hpp>
#include <mir/mir.hpp>

namespace {
    HIR::Function parse_function(TokenStream& lex, RcString& out_name);
    HIR::PathParams parse_params(TokenStream& lex);
    HIR::Path parse_path(TokenStream& lex);
    HIR::TypeRef get_core_type(const RcString& s);
    HIR::TypeRef parse_type(TokenStream& lex);
    MIR::LValue parse_lvalue(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map);
    MIR::LValue parse_param(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map);

    bool consume_if(TokenStream& lex, eTokenType tok) {
        if( lex.lookahead(0) == tok ) {
            lex.getToken(); // eat token
            return true;
        }
        else {
            return false;
        }
    }
}

MirOptTestFile  MirOptTestFile::load_from_file(const helpers::path& p)
{
    Lexer   lex(p.str(), AST::Edition::Rust2015, ParseState());
    Token   tok;

    MirOptTestFile  rv;
    rv.m_filename = p.basename();
    rv.m_crate = HIR::CratePtr(::HIR::Crate());
    //rv.m_crate->m_crate_name = RcString(p.str());

    while(lex.lookahead(0) != TOK_EOF)
    {
        std::map<RcString, std::string>  attrs;
        while( consume_if(lex, TOK_HASH) )
        {
            //bool is_outer = consume_if(lex, TOK_EXCLAM);
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_OPEN);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.ident().name;
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            GET_CHECK_TOK(tok, lex, TOK_STRING);
            auto value = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);

            attrs.insert( std::make_pair(name, value) );
        }

        if( consume_if(lex, TOK_RWORD_FN) )
        {
            RcString    fcn_name;
            auto fcn_decl = parse_function(lex, fcn_name);

            auto vi = ::HIR::VisEnt<HIR::ValueItem> {
                HIR::Publicity::new_global(), ::HIR::ValueItem(mv$(fcn_decl))
                };
            rv.m_crate->m_root_module.m_value_items.insert(::std::make_pair(fcn_name,
                ::std::make_unique<decltype(vi)>(mv$(vi))
                ));

            // Attributes
            for(const auto& attr : attrs)
            {
                if( attr.first == "test" )
                {
                    MirOptTestFile::Test    t;
                    t.input_function = ::HIR::SimplePath("", { fcn_name });
                    t.output_template_function = ::HIR::SimplePath("", { RcString(attr.second) });

                    rv.m_tests.push_back(mv$(t));
                }
                else
                {
                    // Ignore? Support other forms of tests?
                }
            }
        }
        //else if( lex.lookahead(0) == "INCLUDE" )
        //{
        //    auto path = lex.check_consume(TokenClass::String).strval;
        //}
        else
        {
            TODO(lex.point_span(), "Error");
        }
    }

    return rv;
}
namespace {
    const HIR::GenericParams* g_item_params;

    HIR::GenericParams parse_genericdef(TokenStream& lex)
    {
        Token   tok;
        HIR::GenericParams  rv;
        if(consume_if(lex, TOK_LT) )
        {
            while( lex.lookahead(0) != TOK_GT )
            {
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                auto name = tok.ident().name;

                rv.m_types.push_back(HIR::TypeParamDef());
                rv.m_types.back().m_name = name;

                if( consume_if(lex, TOK_COLON) )
                {
                    TODO(lex.point_span(), "parse_genericdef - bounds");
                }
            }
            GET_CHECK_TOK(tok, lex, TOK_GT);
        }
        return rv;
    }
    HIR::Function parse_function(TokenStream& lex, RcString& out_name)
    {
        Token   tok;
        ::std::map<RcString, MIR::LValue::Storage>  val_name_map;
        val_name_map.insert(::std::make_pair("retval", MIR::LValue::Storage::new_Return()));
        ::std::map<RcString, unsigned>  dropflag_names;
        ::std::map<RcString, unsigned>  real_bb_name_map;
        ::std::map<RcString, unsigned>  lookup_bb_name_map;
        ::std::vector<RcString> lookup_bb_names;

        ::HIR::Function fcn_decl;
        fcn_decl.m_code.m_mir = ::MIR::FunctionPointer(new ::MIR::Function());
        auto& mir_fcn = *fcn_decl.m_code.m_mir;

        // Name
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto fcn_name = tok.ident().name;
        DEBUG("fn " << fcn_name);

        fcn_decl.m_params = parse_genericdef(lex);
        g_item_params = &fcn_decl.m_params;

        // Arguments
        auto& args = fcn_decl.m_args;
        GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
        while( lex.lookahead(0) != TOK_PAREN_CLOSE )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.ident().name;
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto var_ty = parse_type(lex);

            auto arg_idx = static_cast<unsigned>(args.size());
            val_name_map.insert( ::std::make_pair(name, ::MIR::LValue::Storage::new_Argument(arg_idx)) );
            args.push_back( ::std::make_pair(HIR::Pattern(), mv$(var_ty)) );

            if( !consume_if(lex, TOK_COMMA) )
                break;
        }
        GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
        // Return type
        if( consume_if(lex, TOK_THINARROW) )
        {
            // Parse return type
            fcn_decl.m_return = parse_type(lex);
        }
        else
        {
            fcn_decl.m_return = HIR::TypeRef::new_unit();
        }

        // Body.
        GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);

        // 1. Variable list
        while( consume_if(lex, TOK_RWORD_LET) )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.ident().name;
            if( consume_if(lex, TOK_EQUAL) ) {
                GET_TOK(tok, lex);
                bool v = tok.type() == TOK_RWORD_TRUE ? true
                    : tok.type() == TOK_RWORD_FALSE ? false
                    : throw ParseError::Unexpected(lex, tok, { TOK_RWORD_TRUE, TOK_RWORD_FALSE });
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

                dropflag_names.insert(::std::make_pair( name, static_cast<unsigned>(mir_fcn.drop_flags.size()) ));
                mir_fcn.drop_flags.push_back(v);
            }
            else {
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                auto var_ty = parse_type(lex);
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

                auto var_idx = static_cast<unsigned>(mir_fcn.locals.size());
                val_name_map.insert( ::std::make_pair(name, ::MIR::LValue::Storage::new_Local(var_idx)) );
                mir_fcn.locals.push_back( mv$(var_ty) );
            }
        }
        // 2. List of BBs arranged with 'ident: { STMTS; TERM }'
        while( lex.lookahead(0) != TOK_BRACE_CLOSE )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.ident().name;
            real_bb_name_map.insert( ::std::make_pair(name, static_cast<unsigned>(mir_fcn.blocks.size())) );
            mir_fcn.blocks.push_back(::MIR::BasicBlock());
            auto& bb = mir_fcn.blocks.back();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
            while( lex.lookahead(0) != TOK_BRACE_CLOSE )
            {
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                if( tok.ident().name == "DROP" )
                {
                    auto slot = parse_lvalue(lex, val_name_map);
                    if( consume_if(lex, TOK_RWORD_IF) )
                    {
                        TODO(lex.point_span(), "MIR statement - DROP if");
                    }
                    else
                    {
                        bb.statements.push_back(::MIR::Statement::make_Drop({ MIR::eDropKind::DEEP, mv$(slot), ~0u }));
                    }
                }
                else if( tok.ident().name == "ASM" )
                {
                    TODO(lex.point_span(), "MIR statement - " << tok);
                }
                else if( tok.ident().name == "SETDROP" )
                {
                    TODO(lex.point_span(), "MIR statement - " << tok);
                }
                else if( tok.ident().name == "ASSIGN" )
                {
                    // parse a lvalue
                    auto dst = parse_lvalue(lex, val_name_map);
                    GET_CHECK_TOK(tok, lex, TOK_EQUAL);

                    MIR::RValue src;
                    GET_TOK(tok, lex);
                    switch(tok.type())
                    {
                    // Constant boolean
                    case TOK_RWORD_TRUE:    src = MIR::Constant::make_Bool({true });   break;
                    case TOK_RWORD_FALSE:   src = MIR::Constant::make_Bool({false});   break;

                    case TOK_PLUS:
                    case TOK_DASH: {
                        bool is_neg = (tok.type() == TOK_DASH);
                        GET_TOK(tok, lex);
                        Token   ty_tok;
                        GET_CHECK_TOK(ty_tok, lex, TOK_IDENT);
                        auto t = get_core_type(ty_tok.ident().name);
                        if(t == HIR::TypeRef())
                            throw ParseError::Unexpected(lex, ty_tok);
                        auto ct = t.data().as_Primitive();
                        switch(tok.type())
                        {
                        case TOK_INTEGER: {
                            auto v = tok.intval();
                            switch(ct)
                            {
                            case HIR::CoreType::I8:
                            case HIR::CoreType::I16:
                            case HIR::CoreType::I32:
                            case HIR::CoreType::I64:
                            case HIR::CoreType::I128:
                            case HIR::CoreType::Isize:
                                break;
                            default:
                                throw ParseError::Unexpected(lex, ty_tok);
                            }
                            src = MIR::Constant::make_Int({ is_neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v), ct });
                            } break;
                        case TOK_FLOAT: {
                            auto v = tok.floatval();
                            switch(ct)
                            {
                            case HIR::CoreType::F32:
                            case HIR::CoreType::F64:
                                break;
                            default:
                                throw ParseError::Unexpected(lex, ty_tok);
                            }
                            src = MIR::Constant::make_Float({ (is_neg ? -1 : 1) * v, ct });
                            } break;
                        default:
                            throw ParseError::Unexpected(lex, tok, { TOK_INTEGER, TOK_FLOAT });
                        }
                        } break;
                    case TOK_INTEGER: {
                        auto v = tok.intval();
                        GET_CHECK_TOK(tok, lex, TOK_IDENT);
                        auto t = get_core_type(tok.ident().name);
                        if(t == HIR::TypeRef())
                            throw ParseError::Unexpected(lex, tok);
                        auto ct = t.data().as_Primitive();
                        switch(ct)
                        {
                        case HIR::CoreType::U8:
                        case HIR::CoreType::U16:
                        case HIR::CoreType::U32:
                        case HIR::CoreType::U64:
                        case HIR::CoreType::U128:
                        case HIR::CoreType::Usize:
                            break;
                        default:
                            throw ParseError::Unexpected(lex, tok);
                        }
                        src = MIR::Constant::make_Uint({ v, ct });
                        } break;

                    case TOK_AMP:
                        if( consume_if(lex, TOK_RWORD_MOVE) )
                            src = MIR::RValue::make_Borrow({ HIR::BorrowType::Owned, parse_lvalue(lex, val_name_map) });
                        else if( consume_if(lex, TOK_RWORD_MUT) )
                            src = MIR::RValue::make_Borrow({ HIR::BorrowType::Unique, parse_lvalue(lex, val_name_map) });
                        else
                            src = MIR::RValue::make_Borrow({ HIR::BorrowType::Shared, parse_lvalue(lex, val_name_map) });
                        break;

                    // Operator (e.g. `ADD(...)`) or an lvalue
                    case TOK_IDENT:
                        if( tok.ident().name == "CAST" )
                        {
                            auto v = parse_lvalue(lex, val_name_map);
                            GET_CHECK_TOK(tok, lex, TOK_RWORD_AS);
                            auto ty = parse_type(lex);
                            src = MIR::RValue::make_Cast({ mv$(v), mv$(ty) });
                        }
                        else if( consume_if(lex, TOK_PAREN_OPEN) )
                        {
                            auto parse_binop = [&](TokenStream& lex, MIR::eBinOp op) {
                                Token   tok;
                                auto l = parse_param(lex, val_name_map);
                                GET_CHECK_TOK(tok, lex, TOK_COMMA);
                                auto r = parse_param(lex, val_name_map);
                                return MIR::RValue::make_BinOp({ mv$(l), op, mv$(r) });
                                };
                            if(tok.ident().name == "ADD")
                                src = parse_binop(lex, MIR::eBinOp::ADD);
                            else if(tok.ident().name == "SUB")
                                src = parse_binop(lex, MIR::eBinOp::SUB);
                            else if(tok.ident().name == "BIT_SHL")
                                src = parse_binop(lex, MIR::eBinOp::BIT_SHL);
                            else if(tok.ident().name == "BIT_SHR")
                                src = parse_binop(lex, MIR::eBinOp::BIT_SHR);
                            else if(tok.ident().name == "BIT_AND")
                                src = parse_binop(lex, MIR::eBinOp::BIT_AND);
                            else if(tok.ident().name == "BIT_OR")
                                src = parse_binop(lex, MIR::eBinOp::BIT_OR);
                            else if(tok.ident().name == "BIT_XOR")
                                src = parse_binop(lex, MIR::eBinOp::BIT_XOR);
                            else
                            {
                                TODO(lex.point_span(), "MIR assign operator - " << tok.ident().name);
                            }
                            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
                        }
                        else
                        {
                            lex.putback(mv$(tok));
                            src = parse_lvalue(lex, val_name_map);
                        }
                        break;
                    // Start of a path (lvalue)
                    case TOK_LT:
                    case TOK_DOUBLE_LT:
                    case TOK_DOUBLE_COLON:
                        lex.putback(mv$(tok));
                        src = parse_lvalue(lex, val_name_map);
                        break;
                    // Tuple literal
                    case TOK_PAREN_OPEN: {
                        src = MIR::RValue::make_Tuple({});
                        auto& vals = src.as_Tuple().vals;
                        while( lex.lookahead(0) != TOK_PAREN_CLOSE )
                        {
                            vals.push_back(parse_param(lex, val_name_map));
                            if( !consume_if(lex, TOK_COMMA) )
                                break;
                        }
                        GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
                        } break;
                    default:
                        TODO(lex.point_span(), "MIR assign - " << tok);
                    }

                    bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(dst), mv$(src) }));
                }
                else
                {
                    TODO(lex.point_span(), "MIR statement - " << tok);
                }
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            }
            GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

            auto parse_bb_name = [&](TokenStream& lex) {
                Token   tok;
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                auto& bb_name = tok.ident().name;
                if( lookup_bb_name_map.count(bb_name) ) {
                    return lookup_bb_name_map[bb_name];
                }
                else {
                    unsigned idx = static_cast<unsigned>(lookup_bb_names.size());
                    lookup_bb_names.push_back(bb_name);
                    lookup_bb_name_map[bb_name] = idx;
                    return idx;
                }
                };

            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            if( tok.ident().name == "RETURN" )
            {
                bb.terminator = ::MIR::Terminator::make_Return({});
            }
            else if( tok.ident().name == "DIVERGE" )
            {
                bb.terminator = ::MIR::Terminator::make_Diverge({});
            }
            else if( tok.ident().name == "GOTO" )
            {
                bb.terminator = ::MIR::Terminator::make_Goto(parse_bb_name(lex));
            }
            else if( tok.ident().name == "PANIC" )
            {
                bb.terminator = ::MIR::Terminator::make_Panic({ parse_bb_name(lex) });
            }
            else if( tok.ident().name == "CALL" )
            {
                auto dst = parse_lvalue(lex, val_name_map);
                GET_CHECK_TOK(tok, lex, TOK_EQUAL);

                // - Call target
                MIR::CallTarget target;
                if( lex.lookahead(0) == TOK_PAREN_OPEN )
                {
                    GET_TOK(tok, lex);
                    target = ::MIR::CallTarget::make_Value(parse_lvalue(lex, val_name_map));
                }
                else if( lex.lookahead(0) == TOK_STRING )
                {
                    GET_TOK(tok, lex);
                    auto int_name = RcString(tok.str());
                    auto params = parse_params(lex);
                    target = ::MIR::CallTarget::make_Intrinsic({ int_name, mv$(params) });
                }
                else
                {
                    target = ::MIR::CallTarget::make_Path( parse_path(lex) );
                }

                // - Arguments
                ::std::vector<MIR::Param>   args;
                GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
                while( lex.lookahead(0) != TOK_PAREN_CLOSE )
                {
                    args.push_back(parse_param(lex, val_name_map));
                    if( !consume_if(lex, TOK_COMMA) )
                        break;
                }
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);

                // - Target blocks
                GET_CHECK_TOK(tok, lex, TOK_FATARROW);
                auto ret_bb = parse_bb_name(lex);
                GET_CHECK_TOK(tok, lex, TOK_RWORD_ELSE);
                auto panic_bb = parse_bb_name(lex);

                bb.terminator = ::MIR::Terminator::make_Call({ ret_bb, panic_bb, mv$(dst), mv$(target), mv$(args) });
            }
            else if( tok.ident().name == "IF" )
            {
                auto v = parse_lvalue(lex, val_name_map);
                GET_CHECK_TOK(tok, lex, TOK_FATARROW);
                auto bb0 = parse_bb_name(lex);
                GET_CHECK_TOK(tok, lex, TOK_RWORD_ELSE);
                auto bb1 = parse_bb_name(lex);

                bb.terminator = ::MIR::Terminator::make_If({ mv$(v), bb0, bb1 });
            }
            else if( tok.ident().name == "SWITCH" )
            {
                auto v = parse_lvalue(lex, val_name_map);
                ::std::vector<unsigned> targets;

                if( lex.lookahead(0) == TOK_IDENT )
                {
                    GET_CHECK_TOK(tok, lex, TOK_IDENT);
                    if( tok.ident().name == "str" )
                    {
                        TODO(lex.point_span(), "MIR terminator - SwitchValue - str");
                    }
                    else if( tok.ident().name == "sint" )
                    {
                        TODO(lex.point_span(), "MIR terminator - SwitchValue - signed");
                    }
                    else if( tok.ident().name == "uint" )
                    {
                        TODO(lex.point_span(), "MIR terminator - SwitchValue - unsigned");
                    }
                    else
                    {
                        TODO(lex.point_span(), "MIR terminator - SwitchValue - unknown " << tok);
                    }
                }
                else
                {
                    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
                    while( lex.lookahead(0) != TOK_BRACE_CLOSE )
                    {
                        targets.push_back(parse_bb_name(lex));
                        if( !consume_if(lex, TOK_COMMA) )
                            break;
                    }
                    GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

                    bb.terminator = ::MIR::Terminator::make_Switch({ mv$(v), targets });
                }
            }
            else
            {
                TODO(lex.point_span(), "MIR terminator - " << tok.ident().name);
            }
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        }
        GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

        // 3. Convert BB indexes into correct numbering
        for(auto& blk : mir_fcn.blocks)
        {
            auto bb_idx = &blk - mir_fcn.blocks.data();
            auto translate_bb = [&](unsigned e)->unsigned {
                const auto& name = lookup_bb_names[e];
                auto it = real_bb_name_map.find(name);
                if(it == real_bb_name_map.end())
                    throw std::runtime_error(FMT(lex.point_span() << ": BB" << bb_idx << "/TERM Unable to find basic block name `" << name << "`")); 
                return it->second;
                };
            TU_MATCH_HDRA( (blk.terminator), {)
            TU_ARMA(Diverge, e) {
                }
            TU_ARMA(Return, e) {
                }
            TU_ARMA(Incomplete, e) {
                }
            TU_ARMA(Goto, e) {
                e = translate_bb(e);
                }
            TU_ARMA(Panic, e) {
                e.dst = translate_bb(e.dst);
                }
            TU_ARMA(Call, e) {
                e.ret_block = translate_bb(e.ret_block);
                e.panic_block = translate_bb(e.panic_block);
                }
            TU_ARMA(If, e) {
                e.bb0 = translate_bb(e.bb0);
                e.bb1 = translate_bb(e.bb1);
                }
            TU_ARMA(Switch, e) {
                for(auto& tgt : e.targets)
                    tgt = translate_bb(tgt);
                }
            TU_ARMA(SwitchValue, e) {
                for(auto& tgt : e.targets)
                    tgt = translate_bb(tgt); 
                }
            }
        }

        DEBUG(fcn_decl.m_args << " -> " << fcn_decl.m_return);
        out_name = mv$(fcn_name);
        g_item_params = nullptr;
        return fcn_decl;
    }
    HIR::PathParams parse_params(TokenStream& lex)
    {
        HIR::PathParams rv;
        if( lex.lookahead(0) == TOK_LT || lex.lookahead(0) == TOK_DOUBLE_LT )
        {
            if( !consume_if(lex, TOK_LT) ) {
                lex.getToken();
                lex.putback(TOK_LT);
            }

            while( !(lex.lookahead(0) == TOK_GT || lex.lookahead(0) == TOK_DOUBLE_GT) )
            {
                // TODO: Lifetimes?
                rv.m_types.push_back( parse_type(lex) );
                if( !consume_if(lex, TOK_COMMA) )
                    break;
            }

            if( consume_if(lex, TOK_GT) )
            {
            }
            else if( consume_if(lex, TOK_DOUBLE_GT) )
            {
                lex.putback(TOK_GT);
            }
            else
            {
                auto tok = lex.getToken();
                throw ParseError::Unexpected(lex, tok, { TOK_GT, TOK_DOUBLE_GT });
            }
        }
        return rv;
    }

    HIR::SimplePath parse_simplepath(TokenStream& lex)
    {
        Token   tok;
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        GET_CHECK_TOK(tok, lex, TOK_STRING);
        auto rv = ::HIR::SimplePath(RcString::new_interned(tok.str()));

        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        lex.putback(mv$(tok));

        while( consume_if(lex, TOK_DOUBLE_COLON) )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            rv.m_components.push_back( tok.ident().name );
        }
        return rv;
    }
    HIR::GenericPath parse_genericpath(TokenStream& lex)
    {
        auto sp = parse_simplepath(lex);
        return ::HIR::GenericPath(mv$(sp), parse_params(lex));
    }
    HIR::Path parse_path(TokenStream& lex)
    {
        if( lex.lookahead(0) == TOK_LT || lex.lookahead(0) == TOK_DOUBLE_LT )
        {
            TODO(lex.point_span(), "parse_path - ufcs");
        }
        else
        {
            return parse_genericpath(lex);
        }
    }
    HIR::TypeRef get_core_type(const RcString& s)
    {
             if( s == "bool"  ) return HIR::TypeRef(HIR::CoreType::Bool);
        else if( s == "str"   ) return HIR::TypeRef(HIR::CoreType::Str );
        else if( s == "u8"    ) return HIR::TypeRef(HIR::CoreType::U8);
        else if( s == "i8"    ) return HIR::TypeRef(HIR::CoreType::I8);
        else if( s == "u16"   ) return HIR::TypeRef(HIR::CoreType::U16);
        else if( s == "i16"   ) return HIR::TypeRef(HIR::CoreType::I16);
        else if( s == "u32"   ) return HIR::TypeRef(HIR::CoreType::U32);
        else if( s == "i32"   ) return HIR::TypeRef(HIR::CoreType::I32);
        else if( s == "u64"   ) return HIR::TypeRef(HIR::CoreType::U64);
        else if( s == "i64"   ) return HIR::TypeRef(HIR::CoreType::I64);
        else if( s == "u128"  ) return HIR::TypeRef(HIR::CoreType::U128);
        else if( s == "i128"  ) return HIR::TypeRef(HIR::CoreType::I128);
        else if( s == "usize" ) return HIR::TypeRef(HIR::CoreType::Usize);
        else if( s == "isize" ) return HIR::TypeRef(HIR::CoreType::Isize);
        else
            return HIR::TypeRef();
    }
    HIR::TypeRef parse_type(TokenStream& lex)
    {
        Token   tok;
        GET_TOK(tok, lex);
        switch( tok.type() )
        {
        case TOK_PAREN_OPEN: {
            ::std::vector<HIR::TypeRef> tys;
            while( lex.lookahead(0) != TOK_PAREN_CLOSE )
            {
                tys.push_back( parse_type(lex) );
                if( !consume_if(lex, TOK_COMMA) )
                    break;
            }
            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
            return HIR::TypeRef(mv$(tys));
            } break;
        case TOK_SQUARE_OPEN: {
            auto ity = parse_type(lex);
            GET_TOK(tok, lex);
            if( tok == TOK_SEMICOLON )
            {
                GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                auto size = tok.intval();
                ASSERT_BUG(lex.point_span(), size < UINT_MAX, "");
                return HIR::TypeRef::new_array(mv$(ity), static_cast<unsigned>(size));
            }
            else if( tok == TOK_SQUARE_CLOSE )
            {
                return HIR::TypeRef::new_slice(mv$(ity));
            }
            else
            {
                throw ParseError::Unexpected(lex, tok, {TOK_SEMICOLON, TOK_SQUARE_CLOSE});
            }
            } break;
        case TOK_IDENT:
            if( tok.ident().name == "dyn" )
            {
                TODO(lex.point_span(), tok);
            }
            else
            {
                auto t = get_core_type(tok.ident().name);
                if( t != HIR::TypeRef() )
                {
                    return t;
                }
                if( g_item_params )
                {
                    for(size_t i = 0; i < g_item_params->m_types.size(); i ++)
                    {
                        if( g_item_params->m_types[i].m_name == tok.ident().name )
                        {
                            return HIR::TypeRef(tok.ident().name, 256 + static_cast<unsigned>(i));
                        }
                    }
                }
                TODO(lex.point_span(), tok);
            }
            break;
        case TOK_DOUBLE_AMP:
            lex.putback(TOK_AMP);
        case TOK_AMP:
            if( consume_if(lex, TOK_RWORD_MOVE) )
                return HIR::TypeRef::new_borrow(HIR::BorrowType::Owned, parse_type(lex));
            else if( consume_if(lex, TOK_RWORD_MUT) )
                return HIR::TypeRef::new_borrow(HIR::BorrowType::Unique, parse_type(lex));
            else
                return HIR::TypeRef::new_borrow(HIR::BorrowType::Shared, parse_type(lex));
        case TOK_STAR:
            if( consume_if(lex, TOK_RWORD_MOVE) )
                return HIR::TypeRef::new_pointer(HIR::BorrowType::Owned, parse_type(lex));
            else if( consume_if(lex, TOK_RWORD_MUT) )
                return HIR::TypeRef::new_pointer(HIR::BorrowType::Unique, parse_type(lex));
            else if( consume_if(lex, TOK_RWORD_CONST) )
                return HIR::TypeRef::new_pointer(HIR::BorrowType::Shared, parse_type(lex));
            else
                throw ParseError::Unexpected(lex, lex.getToken(), { TOK_RWORD_MOVE, TOK_RWORD_MUT, TOK_RWORD_CONST });
        default:
            TODO(lex.point_span(), tok);
        }
    }
    MIR::LValue::Storage parse_lvalue_root(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map)
    {
        if( lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            // Static
            return MIR::LValue::Storage::new_Static( parse_path(lex) );
        }
        else
        {
            Token   tok;
            GET_TOK(tok, lex);
            CHECK_TOK(tok, TOK_IDENT);
            auto it = name_map.find(tok.ident().name);
            if( it == name_map.end() )
                ERROR(lex.point_span(), E0000, "Unable to find value " << tok.ident().name);
            return it->second.clone();
        }
    }
    MIR::LValue parse_lvalue(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map)
    {
        Token   tok;
        std::vector<MIR::LValue::Wrapper>   wrappers;
        auto root = parse_lvalue_root(lex, name_map);

        while(true)
        {
            GET_TOK(tok, lex);
            switch( tok.type() )
            {
            case TOK_SQUARE_OPEN:
                TODO(lex.point_span(), "parse_lvalue - indexing");
                break;
            case TOK_DOT:
                GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                ASSERT_BUG(lex.point_span(), tok.intval() < UINT_MAX, "");
                wrappers.push_back(::MIR::LValue::Wrapper::new_Field( static_cast<unsigned>(tok.intval()) ));
                break;
            case TOK_HASH:
                GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                ASSERT_BUG(lex.point_span(), tok.intval() < UINT_MAX, "");
                wrappers.push_back(::MIR::LValue::Wrapper::new_Downcast( static_cast<unsigned>(tok.intval()) ));
                break;
            case TOK_STAR:
                wrappers.push_back(::MIR::LValue::Wrapper::new_Deref());
                break;
            default:
                lex.putback(mv$(tok));
                return MIR::LValue(mv$(root), mv$(wrappers));
            }
        }
    }

    MIR::LValue parse_param(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map)
    {
        // Can be any constant, or an LValue
        return parse_lvalue(lex, name_map);
    }
}
