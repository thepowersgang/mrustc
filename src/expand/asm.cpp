/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/asm.cpp
 * - asm! macro
 */
#include <common.hpp>
#include <synext_macro.hpp>
#include <synext.hpp>   // for Expand_BareExpr
#include <parse/tokentree.hpp>
#include <parse/ttstream.hpp>
#include <parse/common.hpp>
#include <parse/parseerror.hpp>
#include <ast/expr.hpp>    // for ExprNode_*
#include <parse/interpolated_fragment.hpp>
#include <ast/crate.hpp>
#include <hir/asm.hpp>
#include <trans/target.hpp>
#include <cctype>

namespace
{
    ::std::string get_string(const Span& sp, TokenStream& lex, const ::AST::Crate& crate, AST::Module& mod)
    {
        auto n = Expand_ParseAndExpand_ExprVal(crate, mod, lex);

        auto* format_string_np = dynamic_cast<AST::ExprNode_String*>(&*n);
        if( !format_string_np ) {
            ERROR(sp, E0000, "asm! requires a string literal - got " << *n);
        }
        //const auto& format_string_sp = format_string_np->span();
        return mv$( format_string_np->m_value );
    }

    RcString get_tok_ident_rword(TokenStream& lex)
    {
        Token   tok;
        GET_TOK(tok, lex);
        if(tok.type() == TOK_IDENT)
            return tok.ident().name;
        if(tok.type() >= TOK_RWORD_PUB)
            return tok.to_str().c_str();
        throw ParseError::Unexpected(lex, tok, TOK_IDENT);
    }
}

class CLlvmAsmExpander:
    public ExpandProcMacro
{
public:
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;
        auto lex = TTStream(sp, ParseState(), tt);

        auto template_text = get_string(sp, lex,  crate, mod);
        ::std::vector<::AST::ExprNode_Asm::ValRef>  outputs;
        ::std::vector<::AST::ExprNode_Asm::ValRef>  inputs;
        ::std::vector<::std::string>    clobbers;
        ::std::vector<::std::string>    flags;

        // Outputs
        if( lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            GET_TOK(tok, lex);
            lex.putback(Token(TOK_COLON));
        }
        else if( lex.lookahead(0) == TOK_COLON )
        {
            GET_TOK(tok, lex);

            while( lex.lookahead(0) == TOK_STRING )
            {
                //auto name = get_string(sp, lex);
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                auto name = mv$(tok.str());

                GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
                auto val = Parse_Expr0(lex);
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);

                outputs.push_back( ::AST::ExprNode_Asm::ValRef { mv$(name), mv$(val) } );

                if( lex.lookahead(0) != TOK_COMMA )
                    break;

                GET_TOK(tok, lex);
            }
        }
        else
        {
        }

        // Inputs
        if( lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            GET_TOK(tok, lex);
            lex.putback(Token(TOK_COLON));
        }
        else if( lex.lookahead(0) == TOK_COLON )
        {
            GET_TOK(tok, lex);

            while( lex.lookahead(0) == TOK_STRING )
            {
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                auto name = mv$(tok.str());

                GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
                auto val = Parse_Expr0(lex);
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);

                inputs.push_back( ::AST::ExprNode_Asm::ValRef { mv$(name), mv$(val) } );

                if( lex.lookahead(0) != TOK_COMMA )
                    break;
                GET_TOK(tok, lex);
            }
        }
        else
        {
        }

        // Clobbers
        if( lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            GET_TOK(tok, lex);
            lex.putback(Token(TOK_COLON));
        }
        else if( lex.lookahead(0) == TOK_COLON )
        {
            GET_TOK(tok, lex);

            while( lex.lookahead(0) == TOK_STRING )
            {
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                clobbers.push_back( mv$(tok.str()) );

                if( lex.lookahead(0) != TOK_COMMA )
                    break;
                GET_TOK(tok, lex);
            }
        }
        else
        {
        }

        // Flags
        if( lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            GET_TOK(tok, lex);
            lex.putback(Token(TOK_COLON));
        }
        else if( lex.lookahead(0) == TOK_COLON )
        {
            GET_TOK(tok, lex);

            while( lex.lookahead(0) == TOK_STRING )
            {
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                flags.push_back( mv$(tok.str()) );

                if( lex.lookahead(0) != TOK_COMMA )
                    break;
                GET_TOK(tok, lex);
            }
        }
        else
        {
        }

        // trailing `: voltaile` - TODO: Is this valid?
        if( lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            GET_TOK(tok, lex);
            lex.putback(Token(TOK_COLON));
        }
        else if( lex.lookahead(0) == TOK_COLON )
        {
            GET_TOK(tok, lex);

            if( GET_TOK(tok, lex) == TOK_IDENT && tok.ident() == "volatile" )
            {
                flags.push_back( "volatile" );
            }
            else
            {
                PUTBACK(tok, lex);
            }
        }
        else
        {
        }

        // has to be the end
        if( lex.lookahead(0) != TOK_EOF )
        {
            ERROR(sp, E0000, "Unexpected token in asm! - " << lex.getToken());
        }

        // Convert this into an AST node and insert as an intepolated expression
        ::AST::ExprNodeP rv = ::AST::ExprNodeP( new ::AST::ExprNode_Asm { mv$(template_text), mv$(outputs), mv$(inputs), mv$(clobbers), mv$(flags) } );
        return box$( TTStreamO(sp, ParseState(), TokenTree(Token( InterpolatedFragment(InterpolatedFragment::EXPR, rv.release()) ))));
    }
};

namespace {
    AsmCommon::RegisterClass get_reg_class_x8664(const Span& sp, const RcString& str)
    {
        if(str == "reg")    return AsmCommon::RegisterClass::x86_reg;
        if(str == "reg_abcd")    return AsmCommon::RegisterClass::x86_reg_abcd;
        if(str == "reg_byte")    return AsmCommon::RegisterClass::x86_reg_byte;
        ERROR(sp, E0000, "Unknown register for x86");
    }

    AsmCommon::RegisterClass get_reg_class(const Span& sp, const RcString& str)
    {
        if(Target_GetCurSpec().m_arch.m_name == "x86_64")
            return get_reg_class_x8664(sp, str);
        ERROR(sp, E0000, "Unknown architecture for asm!");
    }
}

class CAsmExpander:
    public ExpandProcMacro
{
public:
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        if(TARGETVER_MOST_1_39)
            return CLlvmAsmExpander().expand(sp, crate, tt, mod);

        // Stabilisation-path `asm!`

        Token   tok;
        auto lex = TTStream(sp, ParseState(), tt);

        std::vector<std::pair<Span,std::string>>    raw_lines;
        do {
            auto ps = lex.start_span();
            auto text = get_string(sp, lex,  crate, mod);
            auto sp = lex.end_span(ps);
            raw_lines.push_back(std::make_pair(sp, std::move(text)));

            if( lex.lookahead(0) == TOK_EOF ) {
                GET_TOK(tok, lex);
                break;
            }
            GET_CHECK_TOK(tok, lex, TOK_COMMA);
        } while( lex.lookahead(0) == TOK_STRING );


        std::vector<AST::ExprNode_Asm2::Param>  params;
        std::vector<RcString>   names;
        AsmCommon::Options  options;
        while( tok.type() == TOK_COMMA )
        {
            if(lex.lookahead(0) == TOK_EOF) {
                GET_TOK(tok, lex);
                break;
            }

            RcString    binding_name;
            auto v = get_tok_ident_rword(lex);
            if(v == "options") {
                GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
                do {
                    GET_CHECK_TOK(tok, lex, TOK_IDENT);

                    if(tok.ident().name == "pure") {
                        if(options.pure)    ERROR(lex.point_span(), E0000, "Duplicate specification of option `" << tok.ident().name << "`");
                        options.pure = 1;
                    }
                    else if(tok.ident().name == "nomem") {
                        if(options.nomem)   ERROR(lex.point_span(), E0000, "Duplicate specification of option `" << tok.ident().name << "`");
                        options.nomem = 1;
                    }
                    else if(tok.ident().name == "readonly") {
                        if(options.readonly)    ERROR(lex.point_span(), E0000, "Duplicate specification of option `" << tok.ident().name << "`");
                        options.readonly = 1;
                    }
                    else if(tok.ident().name == "preserves_flags") {
                        if(options.preserves_flags) ERROR(lex.point_span(), E0000, "Duplicate specification of option `" << tok.ident().name << "`");
                        options.preserves_flags = 1;
                    }
                    else if(tok.ident().name == "noreturn") {
                        if(options.noreturn)    ERROR(lex.point_span(), E0000, "Duplicate specification of option `" << tok.ident().name << "`");
                        options.noreturn = 1;
                    }
                    else if(tok.ident().name == "nostack") {
                        if(options.nostack) ERROR(lex.point_span(), E0000, "Duplicate specification of option `" << tok.ident().name << "`");
                        options.nostack = 1;
                    }
                    else if(tok.ident().name == "att_syntax") {
                        if(options.att_syntax) ERROR(lex.point_span(), E0000, "Duplicate specification of option `" << tok.ident().name << "`");
                        // TODO: x86(-64) only
                        options.att_syntax = 1;
                    }
                    else {
                        ERROR(lex.point_span(), E0000, "Unknown asm option - " << tok.ident().name);
                    }

                    if(lex.lookahead(0) == TOK_PAREN_CLOSE) {
                        GET_TOK(tok, lex);
                        break;
                    }
                } while(GET_TOK(tok, lex) == TOK_COMMA);
                CHECK_TOK(tok, TOK_PAREN_CLOSE);

                GET_TOK(tok, lex);
                continue ;
            }

            if(lex.lookahead(0) == TOK_EQUAL) {
                GET_CHECK_TOK(tok, lex, TOK_EQUAL);
                binding_name = v;
                v = get_tok_ident_rword(lex);
            }

            AST::ExprNode_Asm2::Param   param_spec;
            if(v == "const") {
                auto e = Parse_Expr0(lex);
                param_spec = AST::ExprNode_Asm2::Param::make_Const(std::move(e));
            }
            else if(v == "sym") {
                auto p = Parse_Path(lex, PATH_GENERIC_EXPR);
                param_spec = AST::ExprNode_Asm2::Param::make_Sym(std::move(p));
            }
            else {
                AsmCommon::Direction    dir;
                if(v == "inlateout") {
                    dir = AsmCommon::Direction::InLateOut;
                }
                else if(v == "in") {
                    dir = AsmCommon::Direction::In;
                }
                else if(v == "out") {
                    dir = AsmCommon::Direction::Out;
                }
                else if(v == "lateout") {
                    dir = AsmCommon::Direction::LateOut;
                }
                else if(v == "inout") {
                    dir = AsmCommon::Direction::InOut;
                }
                else {
                    ERROR(sp, E0000, "Unknown asm fragment - `" << tok.ident().name << "`");
                }

                GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
                GET_TOK(tok, lex);
                AsmCommon::RegisterSpec reg_spec;
                if( tok.type() == TOK_IDENT ) {
                    //Target_GetCurSpec().m_arch
                    reg_spec = AsmCommon::RegisterSpec::make_Class(get_reg_class(lex.point_span(), tok.ident().name));
                }
                else if( tok.type() == TOK_STRING ) {
                    reg_spec = AsmCommon::RegisterSpec::make_Explicit(tok.str());
                }
                else {
                    throw ParseError::Unexpected(lex, tok, { TOK_IDENT, TOK_STRING });
                }
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);

                if(lex.lookahead(0) == TOK_UNDERSCORE) {
                    GET_TOK(tok, lex);
                    // out or lateout only
                    switch(dir)
                    {
                    case AsmCommon::Direction::LateOut:
                    case AsmCommon::Direction::Out:
                        break;
                    default:
                        ERROR(sp, E0000, "Invalid use of _ in asm!");
                    }
                    param_spec = AST::ExprNode_Asm2::Param::make_Reg({ dir, std::move(reg_spec), nullptr, nullptr });
                }
                else {
                    auto e  = Parse_Expr0(lex);

                    if(lex.lookahead(0) == TOK_FATARROW)
                    {
                        // inout or inlateout only
                        switch(dir)
                        {
                        case AsmCommon::Direction::InLateOut:
                        case AsmCommon::Direction::InOut:
                            break;
                        default:
                            ERROR(sp, E0000, "Invalid use of => in asm!");
                        }
                        GET_TOK(tok, lex);
                        if(lex.lookahead(0) == TOK_UNDERSCORE) {
                            GET_TOK(tok, lex);
                            param_spec = AST::ExprNode_Asm2::Param::make_Reg({ dir, std::move(reg_spec), mv$(e), nullptr });
                        }
                        else {
                            auto e2  = Parse_Expr0(lex);
                            param_spec = AST::ExprNode_Asm2::Param::make_Reg({ dir, std::move(reg_spec), mv$(e), mv$(e2) });
                        }
                    }
                    else
                    {
                        // Note: Different variant to handle `inout(reg) foo` without duplicating
                        param_spec = AST::ExprNode_Asm2::Param::make_RegSingle({ dir, std::move(reg_spec), mv$(e) });
                    }
                }
            }

            names.push_back(binding_name);
            params.push_back(std::move(param_spec));

            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_EOF);

        // - Sanity-check options
        if( options.nomem && options.readonly ) {
            ERROR(sp, E0000, "asm! options `nomem` and `readonly` are mutually exclusive");
        }
        if( options.pure && !(options.nomem || options.readonly) ) {
            ERROR(sp, E0000, "asm! marked `pure` without `nomem` or `readonly`");
        }
        //if( options.pure && /* has no saved outputs */ ) {
        //}
        //if( options.noreturn && /* has outputs */ ) {
        //}

        unsigned next_index = 0;
        std::vector<AsmCommon::Line>    lines;
        for(const auto& e : raw_lines)
        {
            const auto& sp = e.first;
            const auto& text = e.second;

            AsmCommon::Line line;

            const char* c = text.c_str();
            std::string cur_string;
            while(*c)
            {
                if(*c == '{') {
                    c ++;
                    std::string name;
                    while(*c && *c != ':' && *c != '}')
                    {
                        name += *c;
                        c ++;
                    }
                    if(!*c)
                        ERROR(sp, E0000, "Unexpected EOF in asm! format string");
                    AsmCommon::LineFragment frag;
                    if(name.empty()) {
                        frag.index = next_index;
                        if(frag.index >= params.size()) {
                            ERROR(sp, E0000, "asm! format doesn't have enough arguments");
                        }
                        next_index ++;
                    }
                    else if(std::isdigit(name[0])) {
                        frag.index = std::stoul(name);
                        if(frag.index >= params.size()) {
                            ERROR(sp, E0000, "asm! format string index out of range - " << frag.index);
                        }
                    }
                    else {
                        auto it = std::find(names.begin(), names.end(), name);
                        if(it == names.end()) {
                            ERROR(sp, E0000, "asm! format string references undefined value - `" << name << "`");
                        }
                        frag.index = it - names.begin();
                    }
                    assert(*c == ':' || *c == '}');
                    if(*c == ':')
                    {
                        c ++;
                        if(!*c)
                            ERROR(sp, E0000, "Unexpected EOF in asm! format string");
                        if(*c != '}') {
                            frag.modifier = *c;
                            c ++;
                        }
                    }
                    if(!*c)
                        ERROR(sp, E0000, "Unexpected EOF in asm! format string");
                    if(*c != '}')
                        ERROR(sp, E0000, "Expected '}' in asm! format string");

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
            lines.push_back(std::move(line));
        }

        // - Sanity-check register modifiers
        for(const auto& line : lines)
        {
            for(const auto& frag : line.frags)
            {
                if(frag.index == UINT_MAX) {
                    ERROR(sp, E0000, "asm! marked `pure` without `nomem` or `readonly`");
                }
                if(frag.modifier != '\0')
                {
                    // TODO: Check that the modifier is valid for the specifier
                }
            }
        }

        // Convert this into an AST node and insert as an intepolated expression
        ::AST::ExprNodeP rv = ::AST::ExprNodeP( new ::AST::ExprNode_Asm2 { mv$(options), mv$(lines), mv$(params) } );
        return box$( TTStreamO(sp, ParseState(), TokenTree(Token( InterpolatedFragment(InterpolatedFragment::EXPR, rv.release()) ))));
    }
};

STATIC_MACRO("llvm_asm", CLlvmAsmExpander);
STATIC_MACRO("asm", CAsmExpander);
