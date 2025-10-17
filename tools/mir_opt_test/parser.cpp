/*
 */
#include "test_desc.h"
#include <parse/lex.hpp>
#include <parse/common.hpp>
#include <parse/parseerror.hpp>
#include <climits>	// UINT_MAX

#include <hir/hir.hpp>
#include <mir/mir.hpp>

#include <trans/target.hpp>

namespace {
    HIR::Function parse_function(TokenStream& lex, RcString& out_name);
    HIR::PathParams parse_params(TokenStream& lex);
    HIR::Path parse_path(TokenStream& lex);
    HIR::TypeRef get_core_type(const RcString& s);
    HIR::TypeRef parse_type(TokenStream& lex);
    MIR::LValue parse_lvalue(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map);
    MIR::Param parse_param(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map);

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
    MirOptTestFile  rv;
    rv.m_filename = p.basename();
    rv.m_crate = HIR::CratePtr(::HIR::Crate());
    //rv.m_crate->m_crate_name = RcString(p.str());

    Lexer   lex(p.str(), AST::Edition::Rust2015, ParseState());
    Token   tok;

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
        else if( consume_if(lex, TOK_RWORD_STATIC) )
        {
            auto is_mut = consume_if(lex, TOK_RWORD_MUT);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.ident().name;
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto type = parse_type(lex);
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);

            auto st_decl = ::HIR::Static(HIR::Linkage(), is_mut, std::move(type), HIR::ExprPtr());

            if( consume_if(lex, TOK_AT) ) {
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                st_decl.m_linkage.name = tok.str();
            }
            else {
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                EncodedLiteral value;
                for(auto b : tok.str())
                    value.bytes.push_back(b);
                if( consume_if(lex, TOK_BRACE_OPEN) ) {
                    while( lex.lookahead(0) != TOK_BRACE_CLOSE ) {
                        GET_CHECK_TOK(tok, lex, TOK_AT);
                        GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                        auto ofs = tok.intval();
                        GET_CHECK_TOK(tok, lex, TOK_PLUS);
                        GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                        auto len = tok.intval();
                        GET_CHECK_TOK(tok, lex, TOK_EQUAL);

                        GET_TOK(tok, lex);
                        if( tok.type() == TOK_IDENT ) {
                            auto path = ::HIR::SimplePath("", { tok.ident().name });
                            value.relocations.push_back(Reloc::new_named(ofs.get_lo(), len.get_lo(), path));
                        }
                        else if( tok.type() == TOK_STRING ) {
                            value.relocations.push_back(Reloc::new_bytes(ofs.get_lo(), len.get_lo(), tok.str()));
                        }
                        else {
                            ERROR(lex.point_span(), E0000, "Expected ident or string");
                        }
                        if( !consume_if(lex, TOK_COMMA) ) {
                            continue;
                        }
                    }
                    GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
                }

                st_decl.m_value_res = std::move(value);
                st_decl.m_value_generated = true;
            }
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            auto vi = ::HIR::VisEnt<HIR::ValueItem> {
                HIR::Publicity::new_global(), ::HIR::ValueItem(mv$(st_decl))
                };
            rv.m_crate->m_root_module.m_value_items.insert(::std::make_pair(name,
                ::std::make_unique<decltype(vi)>(mv$(vi))
                ));
        }
        else if( consume_if(lex, TOK_RWORD_CRATE) )
        {
            GET_CHECK_TOK(tok, lex, TOK_STRING);
            auto path = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

            // TODO: Load extern crates (if not loaded)
            rv.m_crate->m_ext_crates.insert(std::make_pair(RcString(), HIR::ExternCrate { HIR::CratePtr(), "", path} ));
        }
        else if( consume_if(lex, TOK_RWORD_TYPE) )
        {
            TypeRepr    repr;

            // Parse the type representation, then generate a struct/union/enum from that

            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.ident().name;
            if( lex.lookahead(0) == TOK_SEMICOLON ) {
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

                HIR::ExternType et;
                auto vi = ::HIR::VisEnt<HIR::TypeItem> {
                    HIR::Publicity::new_global(), ::HIR::TypeItem(mv$(et))
                    };
                rv.m_crate->m_root_module.m_mod_items.insert(::std::make_pair(name,
                    ::std::make_unique<decltype(vi)>(mv$(vi))
                    ));
                continue ;
            }
            GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
            GET_CHECK_TOK(tok, lex, TOK_IDENT); // `SIZE`
            GET_CHECK_TOK(tok, lex, TOK_INTEGER);
            auto size = tok.intval();
            if( size > UINT_MAX ) {
                ERROR(lex.point_span(), E0000, "Structure size out of bounds");
            }
            repr.size = size.truncate_u64();
            GET_CHECK_TOK(tok, lex, TOK_COMMA);
            GET_CHECK_TOK(tok, lex, TOK_IDENT); // `ALIGN`
            GET_CHECK_TOK(tok, lex, TOK_INTEGER);
            auto align = tok.intval();
            if( !(/*1 <= align &&*/ align <= UINT_MAX) ) {
                ERROR(lex.point_span(), E0000, "Structure alignment out of bounds");
            }
            repr.align = size.truncate_u64();
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

            // TODO: Drop glue and DST meta

            // Fields
            while(lex.lookahead(0) == TOK_INTEGER)
            {
                GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                auto ofs = tok.intval();
                if( ofs > size ) {
                    ERROR(lex.point_span(), E0000, "Field offset out of bounds");
                }
                GET_CHECK_TOK(tok, lex, TOK_EQUAL);
                auto ty = parse_type(lex);
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
                repr.fields.push_back({static_cast<size_t>(ofs.truncate_u64()), std::move(ty) });
            }
            while(lex.lookahead(0) == TOK_AT)
            {
                TODO(lex.point_span(), "Parse `type " << name << "` - variants");
            }
            GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

            // If there's only one field, or all fields have different offsets - it's a struct
            if( repr.fields.size() <= 1 || std::all_of(repr.fields.begin(), repr.fields.end(), [&](const TypeRepr::Field& f){
                return std::none_of(repr.fields.begin(), repr.fields.end(), [&](const TypeRepr::Field& f2) { return &f != &f2 && f.offset == f2.offset; });
                }) )
            {
                // Struct
                ::HIR::t_tuple_fields   str_fields;
                for(const auto& f : repr.fields) {
                    str_fields.push_back(HIR::VisEnt<HIR::TypeRef>{ HIR::Publicity::new_global(), f.ty.clone() });
                }
                auto str = HIR::Struct(HIR::GenericParams(), HIR::Struct::Repr::C, mv$(str_fields));
                str.m_markings.is_copy = true;
                auto vi = ::HIR::VisEnt<HIR::TypeItem> {
                    HIR::Publicity::new_global(), ::HIR::TypeItem(mv$(str))
                    };
                rv.m_crate->m_root_module.m_mod_items.insert(::std::make_pair(name,
                    ::std::make_unique<decltype(vi)>(mv$(vi))
                    ));
            }
            else if( std::all_of(repr.fields.begin(), repr.fields.end(), [&](const TypeRepr::Field& f){ return f.offset == repr.fields.front().offset; }) )
            {
                // Union/enum
                if( repr.variants.is_None() ) {
                    HIR::Union  unn;
                    for(const auto& f : repr.fields) {
                        unn.m_variants.push_back(std::make_pair( RcString(), HIR::VisEnt<HIR::TypeRef>{ HIR::Publicity::new_global(), f.ty.clone() } ));
                    }
                    unn.m_markings.is_copy = true;
                    auto vi = ::HIR::VisEnt<HIR::TypeItem> {
                        HIR::Publicity::new_global(), ::HIR::TypeItem(mv$(unn))
                        };
                    rv.m_crate->m_root_module.m_mod_items.insert(::std::make_pair(name,
                        ::std::make_unique<decltype(vi)>(mv$(vi))
                        ));
                }
                else {
                    TODO(lex.point_span(), "Parse `type " << name << "` - enum");
                }
            }
            else {
                TODO(lex.point_span(), "Parse `type " << name << "` - What type?");
            }

            //Target_ForceTypeRepr(lex.point_span(), ty, std::move(repr));
        }
        else
        {
            GET_TOK(tok, lex);
            TODO(lex.point_span(), "Unexpected token at root: " << tok);
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
            while(lex.lookahead(0) == TOK_LIFETIME)
            {
                GET_TOK(tok, lex);
                rv.m_lifetimes.push_back(HIR::LifetimeDef());
                rv.m_lifetimes.back().m_name = tok.ident().name;

                if( lex.getTokenIf(TOK_COLON) )
                {
                    TODO(lex.point_span(), "parse_genericdef - bounds");
                }
                if( !lex.getTokenIf(TOK_COMMA) )
                    break;
            }
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
                if( !lex.getTokenIf(TOK_COMMA) )
                    break;
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
        val_name_map.insert(::std::make_pair("RETURN", MIR::LValue::Storage::new_Return()));
        ::std::map<RcString, unsigned>  dropflag_names;
        ::std::map<RcString, unsigned>  real_bb_name_map;
        ::std::map<RcString, unsigned>  lookup_bb_name_map;
        ::std::vector<RcString> lookup_bb_names;

        ::HIR::Function fcn_decl;

        // Name
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto fcn_name = tok.ident().name;
        if( lex.lookahead(0) == TOK_HASH ) {
            lex.getToken();
            fcn_name = RcString(FMT(fcn_name << "#"));
        }
        DEBUG("fn " << fcn_name);

        fcn_decl.m_params = parse_genericdef(lex);
        g_item_params = &fcn_decl.m_params;

        // Arguments
        auto& args = fcn_decl.m_args;
        GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
        while( lex.lookahead(0) != TOK_PAREN_CLOSE )
        {
            if( lex.lookahead(0) == TOK_TRIPLE_DOT ) {
                GET_TOK(tok, lex);
                fcn_decl.m_variadic = true;
                break;
            }
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

        // mmir format: linking parameters
        // `=` str:LinkName `:` str:LinkAbi
        if( consume_if(lex, TOK_EQUAL) )
        {
            GET_CHECK_TOK(tok, lex, TOK_STRING);
            fcn_decl.m_linkage.name = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            GET_CHECK_TOK(tok, lex, TOK_STRING);
            //fcn_decl.m_abi = tok.str();
        }

        if( consume_if(lex, TOK_SEMICOLON) ) {
            g_item_params = nullptr;
            out_name = mv$(fcn_name);
            return fcn_decl;
        }

        fcn_decl.m_code.m_mir = ::MIR::FunctionPointer(new ::MIR::Function());
        auto& mir_fcn = *fcn_decl.m_code.m_mir;

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
            if( lex.lookahead(0) == TOK_INTEGER) {
                GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                auto name = FMT("_" << tok.intval());
                real_bb_name_map.insert( ::std::make_pair(name, static_cast<unsigned>(mir_fcn.blocks.size())) );
            }
            else {
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                auto name = tok.ident().name;
                real_bb_name_map.insert( ::std::make_pair(name, static_cast<unsigned>(mir_fcn.blocks.size())) );
            }
            mir_fcn.blocks.push_back(::MIR::BasicBlock());
            auto& bb = mir_fcn.blocks.back();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
            bool is_smiri_format = false;
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
                            src = MIR::Constant::make_Int({ is_neg ? -S128(v) : S128(v), ct });
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
                        case HIR::CoreType::I8:
                        case HIR::CoreType::I16:
                        case HIR::CoreType::I32:
                        case HIR::CoreType::I64:
                        case HIR::CoreType::I128:
                        case HIR::CoreType::Isize:
                            ERROR(lex.point_span(), E0000, "Unexpected signed integer type in unsigned literal");
                        default:
                            // bad type
                            throw ParseError::Unexpected(lex, tok);
                        }
                        src = MIR::Constant::make_Uint({ v, ct });
                        } break;
                    case TOK_STRING:
                        src = MIR::Constant::make_StaticString(tok.str());
                        break;

                    case TOK_RWORD_CONST:
                        GET_CHECK_TOK(tok, lex, TOK_AMP);
                        src = MIR::Constant::make_ItemAddr({ box$(parse_path(lex)) });
                        break;

                    case TOK_AMP:
                        if( consume_if(lex, TOK_RWORD_MOVE) )
                            src = MIR::RValue::make_Borrow({ HIR::BorrowType::Owned, parse_lvalue(lex, val_name_map) });
                        else if( consume_if(lex, TOK_RWORD_MUT) )
                            src = MIR::RValue::make_Borrow({ HIR::BorrowType::Unique, parse_lvalue(lex, val_name_map) });
                        else
                            src = MIR::RValue::make_Borrow({ HIR::BorrowType::Shared, parse_lvalue(lex, val_name_map) });
                        break;

                    // mmir format: Explicit lvalue
                    case TOK_EQUAL:
                        src = parse_lvalue(lex, val_name_map);
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
                        else if( tok.ident().name == "ADDROF" )
                        {
                            auto path = parse_path(lex);
                            src= ::MIR::Constant::make_ItemAddr({ ::std::make_unique<HIR::Path>( ::std::move(path) ) });
                        }
                        else if( tok.ident().name == "UNIOP" )
                        {
                            GET_TOK(tok, lex);
                            MIR::eUniOp op;
                            switch(tok.type())
                            {
                            case TOK_EXCLAM:    op = MIR::eUniOp::INV;  break;
                            case TOK_DASH  :    op = MIR::eUniOp::NEG;  break;
                            default:
                                TODO(lex.point_span(), "MIR assign UniOp - " << tok);
                            }
                            auto r = parse_lvalue(lex, val_name_map);
                            src = MIR::RValue::make_UniOp({ mv$(r), op });
                        }
                        else if( tok.ident().name == "BINOP" )
                        {
                            auto l = parse_param(lex, val_name_map);
                            MIR::eBinOp op;
                            GET_TOK(tok, lex);
                            switch(tok.type())
                            {
                            case TOK_DOUBLE_EQUAL: op = MIR::eBinOp::EQ;   break;
                            case TOK_EXCLAM_EQUAL: op = MIR::eBinOp::NE;   break;
                            case TOK_LT:    op = MIR::eBinOp::LT;   break;
                            case TOK_GT:    op = MIR::eBinOp::GT;   break;
                            case TOK_LTE:   op = MIR::eBinOp::LE;   break;
                            case TOK_GTE:   op = MIR::eBinOp::GE;   break;

                            case TOK_PLUS:  op = MIR::eBinOp::ADD;  break;
                            case TOK_DASH:  op = MIR::eBinOp::SUB;  break;
                            case TOK_STAR:  op = MIR::eBinOp::MUL;  break;
                            case TOK_SLASH: op = MIR::eBinOp::DIV;  break;
                            case TOK_PERCENT: op = MIR::eBinOp::MOD;  break;

                            case TOK_AMP  : op = MIR::eBinOp::BIT_AND;  break;
                            case TOK_PIPE : op = MIR::eBinOp::BIT_OR ;  break;
                            case TOK_CARET: op = MIR::eBinOp::BIT_XOR;  break;
                            case TOK_DOUBLE_LT: op = MIR::eBinOp::BIT_SHL;  break;
                            case TOK_DOUBLE_GT: op = MIR::eBinOp::BIT_SHR;  break;
                            default:
                                TODO(lex.point_span(), "MIR assign BinOp - " << tok);
                            }
                            auto r = parse_param(lex, val_name_map);
                            src = MIR::RValue::make_BinOp({ mv$(l), op, mv$(r) });
                        }
                        else if( tok.ident() == "DSTPTR" )
                        {
                            auto v = parse_lvalue(lex, val_name_map);
                            src = MIR::RValue::make_DstPtr({ mv$(v) });
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
                            else if(tok.ident().name == "MUL")
                                src = parse_binop(lex, MIR::eBinOp::MUL);
                            else if(tok.ident().name == "DIV")
                                src = parse_binop(lex, MIR::eBinOp::DIV);
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
                            else if(tok.ident().name == "EQ")
                                src = parse_binop(lex, MIR::eBinOp::EQ);
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
                    case TOK_SQUARE_OPEN: {
                        if( lex.lookahead(0) != TOK_SQUARE_CLOSE ) {
                            auto val1 = parse_param(lex, val_name_map);
                            if( consume_if(lex, TOK_SEMICOLON) ) {
                                GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                                src = MIR::RValue::make_SizedArray({
                                    mv$(val1),
                                    ::HIR::ArraySize( tok.intval().truncate_u64() )
                                    });
                            }
                            else {
                                src = MIR::RValue::make_Array({});
                                auto& vals = src.as_Array().vals;
                                vals.push_back(mv$(val1));
                                while( consume_if(lex, TOK_COMMA) ) {
                                    if(lex.lookahead(0) == TOK_SQUARE_CLOSE )
                                        break;
                                    vals.push_back(parse_param(lex, val_name_map));
                                }
                            }
                        }
                        else {
                            src = MIR::RValue::make_Array({});
                        }
                        GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
                        } break;
                    default:
                        TODO(lex.point_span(), "MIR assign - " << tok);
                    }

                    bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(dst), mv$(src) }));
                }
                else
                {
                    is_smiri_format = true;
                    lex.putback(tok);
                    break;
                }
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            }
            if(!is_smiri_format) {
                GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
            }

            auto parse_bb_name = [&](TokenStream& lex) {
                Token   tok;
                RcString bb_name;
                if( lex.lookahead(0) == TOK_INTEGER) {
                    GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                    bb_name = RcString::new_interned(FMT("_" << tok.intval()));
                }
                else {
                    GET_CHECK_TOK(tok, lex, TOK_IDENT);
                    bb_name = tok.ident().name;
                }
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
            else if( tok.ident().name == "INCOMPLETE" )
            {
                bb.terminator = ::MIR::Terminator::make_Incomplete({});
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
                    GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
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
                if( lex.lookahead(0) == TOK_IDENT ) {
                    GET_CHECK_TOK(tok, lex, TOK_IDENT);
                    if( tok.ident().name != "goto" ) {
                        throw ParseError::Unexpected(lex, tok);
                    }
                }
                else {
                    GET_CHECK_TOK(tok, lex, TOK_FATARROW);
                }
                auto ret_bb = parse_bb_name(lex);
                GET_CHECK_TOK(tok, lex, TOK_RWORD_ELSE);
                auto panic_bb = parse_bb_name(lex);

                bb.terminator = ::MIR::Terminator::make_Call({ ret_bb, panic_bb, mv$(dst), mv$(target), mv$(args) });
            }
            else if( tok.ident().name == "IF" )
            {
                auto v = parse_lvalue(lex, val_name_map);
                if( lex.lookahead(0) == TOK_IDENT ) {
                    GET_CHECK_TOK(tok, lex, TOK_IDENT);
                    if( tok.ident().name != "goto" ) {
                        throw ParseError::Unexpected(lex, tok);
                    }
                }
                else {
                    GET_CHECK_TOK(tok, lex, TOK_FATARROW);
                }
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
            else if( tok.ident().name == "SWITCHVALUE" )
            {
                auto val = parse_lvalue(lex, val_name_map);
                GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
                ::std::vector<::MIR::BasicBlockId>  targets;
                ::MIR::SwitchValues vals;
                switch(lex.lookahead(0))
                {
                case TOK_INTEGER: {
                    ::std::vector<uint64_t> values;
                    while(lex.lookahead(0) != TOK_UNDERSCORE ) {
                        GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                        values.push_back( tok.intval().truncate_u64() );
                        GET_CHECK_TOK(tok, lex, TOK_EQUAL);
                        targets.push_back(parse_bb_name(lex));
                        GET_CHECK_TOK(tok, lex, TOK_COMMA);
                    }
                    vals = ::MIR::SwitchValues::make_Unsigned(::std::move(values));
                    } break;
                case TOK_PLUS:
                case TOK_DASH: {
                    ::std::vector<int64_t> values;
                    while(lex.lookahead(0) != TOK_UNDERSCORE ) {
                        GET_TOK(tok, lex);
                        bool is_neg;
                        switch(tok.type())
                        {
                        case TOK_PLUS:  is_neg = false; break;
                        case TOK_DASH:  is_neg = true ; break;
                        default:
                            throw ParseError::Unexpected(lex, tok, { TOK_PLUS, TOK_DASH });
                        }
                        GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                        values.push_back( static_cast<int64_t>(tok.intval().truncate_u64()) * (is_neg ? -1 : 1) );
                        GET_CHECK_TOK(tok, lex, TOK_EQUAL);
                        targets.push_back(parse_bb_name(lex));
                        GET_CHECK_TOK(tok, lex, TOK_COMMA);
                    }
                    vals = ::MIR::SwitchValues::make_Signed(::std::move(values));
                    } break;
                case TOK_STRING:
                    TODO(lex.point_span(), "MIR terminator - SwitchValue - string");
                    break;
                default:
                    TODO(lex.point_span(), "MIR terminator - SwitchValue - unknown " << lex.getToken());
                }
                GET_CHECK_TOK(tok, lex, TOK_UNDERSCORE);
                GET_CHECK_TOK(tok, lex, TOK_EQUAL);
                auto def_tgt = parse_bb_name(lex);
                GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
                bb.terminator = ::MIR::Terminator::make_SwitchValue({ ::std::move(val), def_tgt, ::std::move(targets), ::std::move(vals) });
            }
            else
            {
                TODO(lex.point_span(), "MIR terminator - " << tok.ident().name);
            }
            if(!is_smiri_format) {
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            }
            else {
                GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
            }
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
                e.bb_true = translate_bb(e.bb_true);
                e.bb_false = translate_bb(e.bb_false);
                }
            TU_ARMA(Switch, e) {
                for(auto& tgt : e.targets)
                    tgt = translate_bb(tgt);
                }
            TU_ARMA(SwitchValue, e) {
                for(auto& tgt : e.targets)
                    tgt = translate_bb(tgt);
                e.def_target = translate_bb(e.def_target);
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
        std::vector<HIR::TypeRef>   tys;
        if( lex.lookahead(0) == TOK_LT || lex.lookahead(0) == TOK_DOUBLE_LT )
        {
            if( !consume_if(lex, TOK_LT) ) {
                lex.getToken();
                lex.putback(TOK_LT);
            }

            while( !(lex.lookahead(0) == TOK_GT || lex.lookahead(0) == TOK_DOUBLE_GT) )
            {
                // TODO: Lifetimes?
                tys.push_back( parse_type(lex) );
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
        HIR::PathParams rv;
        rv.m_types.reserve_init(tys.size());
        for(auto& v : tys) {
            rv.m_types.push_back(std::move(v));
        }
        return rv;
    }

    HIR::SimplePath parse_simplepath(TokenStream& lex)
    {
        Token   tok;
        if( lex.lookahead(0) == TOK_IDENT ) {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            return ::HIR::SimplePath(RcString(), {tok.ident().name});
        }
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        GET_CHECK_TOK(tok, lex, TOK_STRING);
        auto crate_name = RcString::new_interned(tok.str());

        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        lex.putback(mv$(tok));

        std::vector<RcString>   components;
        while( consume_if(lex, TOK_DOUBLE_COLON) )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            components.push_back( tok.ident().name );
        }
        return ::HIR::SimplePath(std::move(crate_name), std::move(components));
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
    HIR::LifetimeRef parse_lifetime(TokenStream& lex)
    {
        auto n = lex.getTokenCheck(TOK_LIFETIME).ident();
        if( n.name == "static" ) {
            return HIR::LifetimeRef::new_static();
        }
        else {
            if( g_item_params )
            {
                for(size_t i = 0; i < g_item_params->m_lifetimes.size(); i ++)
                {
                    if( g_item_params->m_lifetimes[i].m_name == n.name )
                    {
                        return HIR::LifetimeRef(256 + static_cast<unsigned>(i));
                    }
                }
            }
            TODO(lex.point_span(), "Look up lifetime name - " << n);
        }
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
                GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
                ASSERT_BUG(lex.point_span(), size < UINT_MAX, "");
                return HIR::TypeRef::new_array(mv$(ity), static_cast<unsigned>(size.truncate_u64()));
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
                TODO(lex.point_span(), "Trait objects - " << tok);
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

                if( true )
                {
                    lex.putback(tok);
                    return HIR::TypeRef::new_path(parse_path(lex), {});
                }
                TODO(lex.point_span(), "Get named type - " << tok);
            }
            break;
        case TOK_DOUBLE_AMP:
            lex.putback(TOK_AMP);
        case TOK_AMP: {
            auto lft = lex.lookahead(0) == TOK_LIFETIME ? parse_lifetime(lex) : HIR::LifetimeRef();
            if( consume_if(lex, TOK_RWORD_MOVE) )
                return HIR::TypeRef::new_borrow(HIR::BorrowType::Owned, parse_type(lex), lft);
            else if( consume_if(lex, TOK_RWORD_MUT) )
                return HIR::TypeRef::new_borrow(HIR::BorrowType::Unique, parse_type(lex), lft);
            else
                return HIR::TypeRef::new_borrow(HIR::BorrowType::Shared, parse_type(lex), lft);
            }
        case TOK_STAR:
            if( consume_if(lex, TOK_RWORD_MOVE) )
                return HIR::TypeRef::new_pointer(HIR::BorrowType::Owned, parse_type(lex));
            else if( consume_if(lex, TOK_RWORD_MUT) )
                return HIR::TypeRef::new_pointer(HIR::BorrowType::Unique, parse_type(lex));
            else if( consume_if(lex, TOK_RWORD_CONST) )
                return HIR::TypeRef::new_pointer(HIR::BorrowType::Shared, parse_type(lex));
            else
                throw ParseError::Unexpected(lex, lex.getToken(), { TOK_RWORD_MOVE, TOK_RWORD_MUT, TOK_RWORD_CONST });
        case TOK_RWORD_FN: {
            HIR::TypeData_FunctionPointer   ft;
            ft.is_unsafe = false;
            GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
            while( lex.lookahead(0) != TOK_PAREN_CLOSE )
            {
                if( lex.lookahead(0) == TOK_TRIPLE_DOT ) {
                    GET_TOK(tok, lex);
                    TODO(lex.point_span(), "Variadic function pointers");
                    break;
                }
                ft.m_arg_types.push_back( parse_type(lex) );
                if( !consume_if(lex, TOK_COMMA) )
                    break;
            }
            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
            if( consume_if(lex, TOK_THINARROW) ) {
                ft.m_rettype = parse_type(lex);
            }
            else {
                ft.m_rettype = HIR::TypeRef::new_unit();
            }
            return HIR::TypeRef(mv$(ft));
            }
        default:
            TODO(lex.point_span(), tok);
        }
        throw "";
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
            auto tok = lex.getTokenCheck(TOK_IDENT);
            auto name = tok.ident().name;
            auto it = name_map.find(name);
            if( it == name_map.end() ) {
                lex.putback(tok);
                return MIR::LValue::Storage::new_Static( parse_path(lex) );
                //ERROR(lex.point_span(), E0000, "Unable to find value " << name);
            }
            return it->second.clone();
        }
    }
    MIR::LValue parse_lvalue(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map)
    {
        Token   tok;

        int deref = 0;
        // Count up leading derefs
        while(lex.lookahead(0) == TOK_STAR) {
            lex.getToken();
            deref ++;
        }

        MIR::LValue rv;
        if( lex.lookahead(0) == TOK_PAREN_OPEN) {
            lex.getToken();
            rv = parse_lvalue(lex, name_map);
            lex.getTokenCheck(TOK_PAREN_CLOSE);
        }
        else {
            rv.m_root = parse_lvalue_root(lex, name_map);
        }

        while(true)
        {
            GET_TOK(tok, lex);
            switch( tok.type() )
            {
            case TOK_SQUARE_OPEN: {
                auto r = parse_lvalue_root(lex, name_map);
                ASSERT_BUG(lex.point_span(), r.is_Local(), "Indexing needs a local variable, got " << r);
                rv.m_wrappers.push_back( ::MIR::LValue::Wrapper::new_Index(r.as_Local()) );
                GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
                } break;
            case TOK_DOT:
                if( lex.lookahead(0) == TOK_STAR ) {
                    lex.getToken();
                    rv.m_wrappers.push_back(::MIR::LValue::Wrapper::new_Deref());
                    break;
                }
                GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                ASSERT_BUG(lex.point_span(), tok.intval() < UINT_MAX, "");
                rv.m_wrappers.push_back(::MIR::LValue::Wrapper::new_Field( static_cast<unsigned>(tok.intval().truncate_u64()) ));
                break;
            case TOK_HASH:
                GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                ASSERT_BUG(lex.point_span(), tok.intval() < UINT_MAX, "");
                rv.m_wrappers.push_back(::MIR::LValue::Wrapper::new_Downcast( static_cast<unsigned>(tok.intval().truncate_u64()) ));
                break;
            //case TOK_STAR:
            //    wrappers.push_back(::MIR::LValue::Wrapper::new_Deref());
            //    break;
            default:
                lex.putback(mv$(tok));
                while(deref--) {
                    rv.m_wrappers.push_back(::MIR::LValue::Wrapper::new_Deref());
                }
                return rv;
            }
        }
    }

    MIR::Param parse_param(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map)
    {
        struct H {
            static HIR::CoreType get_lit_type(const Span& sp, TokenStream& lex, eCoreType ct) {
                switch(ct)
                {
                case CORETYPE_ANY: {
                    return parse_type(lex).data().as_Primitive();
                    } break;
                case CORETYPE_INT : return HIR::CoreType::Isize;
                case CORETYPE_I8  : return HIR::CoreType::I8;
                case CORETYPE_I16 : return HIR::CoreType::I16;
                case CORETYPE_I32 : return HIR::CoreType::I32;
                case CORETYPE_I64 : return HIR::CoreType::I64;
                case CORETYPE_I128: return HIR::CoreType::I128;
                case CORETYPE_UINT: return HIR::CoreType::Usize;
                case CORETYPE_U8  : return HIR::CoreType::U8;
                case CORETYPE_U16 : return HIR::CoreType::U16;
                case CORETYPE_U32 : return HIR::CoreType::U32;
                case CORETYPE_U64 : return HIR::CoreType::U64;
                case CORETYPE_U128: return HIR::CoreType::U128;
                default:
                    BUG(sp, "Incorrect type suffix for literal - " << coretype_name(ct));
                }
            }
            static HIR::CoreType to_type_float(const Span& sp, TokenStream& lex, eCoreType ct) {
                auto rv = get_lit_type(sp, lex, ct);
                switch(rv)
                {
                case HIR::CoreType::F32:
                case HIR::CoreType::F64:
                    return rv;
                default:
                    BUG(sp, "Incorrect type suffix for float - " << rv);
                }
            }
            static HIR::CoreType to_type_signed(const Span& sp, TokenStream& lex, eCoreType ct) {
                auto rv = get_lit_type(sp, lex, ct);
                switch(rv)
                {
                case HIR::CoreType::Isize:
                case HIR::CoreType::I8:
                case HIR::CoreType::I16:
                case HIR::CoreType::I32:
                case HIR::CoreType::I64:
                case HIR::CoreType::I128:
                    return rv;
                default:
                    BUG(sp, "Incorrect type suffix for integer - " << rv);
                }
            }
        };
        // Can be any constant, or an LValue
        auto sp = lex.point_span();
        switch(lex.lookahead(0))
        {
        case TOK_INTEGER: {
            auto v = lex.getTokenCheck(TOK_INTEGER);
            auto t = H::get_lit_type(sp, lex, v.datatype());
            switch( t )
            {
            case HIR::CoreType::Usize:
            case HIR::CoreType::U8:
            case HIR::CoreType::U16:
            case HIR::CoreType::U32:
            case HIR::CoreType::U64:
            case HIR::CoreType::U128:
                return MIR::Constant::make_Uint({ v.intval(), t });
            case HIR::CoreType::Isize:
            case HIR::CoreType::I8:
            case HIR::CoreType::I16:
            case HIR::CoreType::I32:
            case HIR::CoreType::I64:
            case HIR::CoreType::I128:
                return MIR::Constant::make_Int({ S128(v.intval()), t });
            default:
                BUG(sp, "Incorrect type suffix for integer");
            }
            }
        case TOK_FLOAT: {
            auto v = lex.getTokenCheck(TOK_FLOAT);
            return MIR::Constant::make_Float({ v.floatval(), H::to_type_float(sp, lex, v.datatype()) });
            }
        case TOK_PLUS:
            lex.getToken();
            switch(lex.lookahead(0))
            {
            case TOK_INTEGER: {
                auto v = lex.getTokenCheck(TOK_INTEGER);
                return MIR::Constant::make_Int({ S128(v.intval()), H::to_type_signed(sp, lex, v.datatype()) });
            }
            case TOK_FLOAT: {
                auto v = lex.getTokenCheck(TOK_FLOAT);
                return MIR::Constant::make_Float({ v.floatval(), H::to_type_float(sp, lex, v.datatype()) });
            }
            default:
                throw ParseError::Unexpected(lex, lex.getToken(), {TOK_INTEGER, TOK_FLOAT});
            }
        case TOK_DASH:
            lex.getToken();
            switch(lex.lookahead(0))
            {
            case TOK_INTEGER: {
                auto v = lex.getTokenCheck(TOK_INTEGER);
                return MIR::Constant::make_Int({ -S128(v.intval()), H::to_type_signed(sp, lex, v.datatype()) });
                }
            case TOK_FLOAT: {
                auto v = lex.getTokenCheck(TOK_FLOAT);
                return MIR::Constant::make_Float({ -v.floatval(), H::to_type_float(sp, lex, v.datatype()) });
                }
            default:
                throw ParseError::Unexpected(lex, lex.getToken(), {TOK_INTEGER, TOK_FLOAT});
            }
        case TOK_PAREN_OPEN:
        case TOK_IDENT:
        case TOK_DOUBLE_COLON:
            return parse_lvalue(lex, name_map);
        default:
            throw ParseError::Unexpected(lex, lex.getToken(), {TOK_INTEGER, TOK_FLOAT, TOK_PLUS, TOK_DASH, TOK_IDENT, TOK_DOUBLE_COLON});
        }
    }
}
