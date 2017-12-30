/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/format_args.cpp
 * - format_args! syntax extension handling
 */
#include <synext_macro.hpp>
#include <synext.hpp>   // for Expand_BareExpr
#include "../parse/common.hpp"
#include "../parse/parseerror.hpp"
#include "../parse/tokentree.hpp"
#include "../parse/ttstream.hpp"
#include "../parse/interpolated_fragment.hpp"
#include <ast/crate.hpp>    // for m_load_std
#include <ast/expr.hpp>    // for ExprNode_*
#include <cctype>

namespace {

    /// Options for a formatting fragment
    struct FmtArgs
    {
        enum class Align {
            Unspec,
            Left,
            Center,
            Right,
        };
        enum class Sign {
            Unspec,
            Plus,
            Minus,
        };

        Align   align = Align::Unspec;
        char    align_char = ' ';

        Sign    sign = Sign::Unspec;
        bool    alternate = false;
        bool    zero_pad = false;

        bool width_is_arg = false;
        unsigned int width = 0;

        bool prec_is_arg = false;
        unsigned int prec = 0;

        bool operator==(const FmtArgs& x) const { return ::std::memcmp(this, &x, sizeof(*this)) == 0; }
        bool operator!=(const FmtArgs& x) const {
            #define CMP(f)  if(f != x.f)    return true
            CMP(align);
            CMP(align_char);
            CMP(sign);
            CMP(alternate);
            CMP(zero_pad);
            CMP(width_is_arg);
            CMP(width);
            CMP(prec_is_arg);
            CMP(prec);
            return false;
        }
        friend ::std::ostream& operator<<(::std::ostream& os, const FmtArgs& x) {
            os << "Align(";
            switch(x.align) {
            case Align::Unspec: os << "-"; break;
            case Align::Left:   os << "<"; break;
            case Align::Center: os << "^"; break;
            case Align::Right:  os << ">"; break;
            }
            os << "'" << x.align_char << "'";
            os << ")";
            os << "Sign(";
            switch(x.sign) {
            case Sign::Unspec:  os << " ";  break;
            case Sign::Plus:    os << "+";  break;
            case Sign::Minus:   os << "-";  break;
            }
            if(x.alternate) os << "#";
            if(x.zero_pad) os << "0";
            os << ")";
            os << "Width(" << (x.width_is_arg ? "$" : "") << x.width << ")";
            os << "Prec(" << (x.prec_is_arg ? "$" : "") << x.prec << ")";
            return os;
        }
    };

    /// A single formatting fragment
    struct FmtFrag
    {
        /// Literal text preceding the fragment
        ::std::string   leading_text;

        /// Argument index used
        unsigned int    arg_index;

        /// Trait to use for formatting
        const char* trait_name;

        // TODO: Support case where this hasn't been edited (telling the formatter that it has nothing to apply)
        /// Options
        FmtArgs     args;
    };


    class string_view {
        const char* s;
        const char* e;
    public:
        string_view(const char* s, const char* e):
            s(s), e(e)
        {}

        friend ::std::ostream& operator<<(::std::ostream& os, const string_view& x) {
            for(const char* p = x.s; p != x.e; p++)
                os << *p;
            return os;
        }
    };

    /// Parse a format string into a sequence of fragments.
    ///
    /// Returns a list of fragments, and the remaining free text after the last format sequence
    ::std::tuple< ::std::vector<FmtFrag>, ::std::string> parse_format_string(
        const Span& sp,
        const ::std::string& format_string,
        const ::std::map< ::std::string,unsigned int>& named,
        unsigned int n_free
        )
    {
        unsigned int n_named = named.size();
        unsigned int next_free = 0;

        ::std::vector<FmtFrag>  frags;
        ::std::string   cur_literal;

        const char* s = format_string.c_str();
        for( ; *s; s ++)
        {
            if( *s != '{' )
            {
                if( *s == '}' ) {
                    s ++;
                    if( *s != '}' ) {
                        // TODO: Error? Warning?
                        s --;   // Step backwards, just in case
                    }
                    // Doesn't need escaping
                    cur_literal += '}';
                }
                else
                {
                    cur_literal += *s;
                }
            }
            else
            {
                s ++;
                // Escaped '{' as "{{"
                if( *s == '{' ) {
                    cur_literal += '{';
                    continue ;
                }

                // Debugging: A view of the formatting fragment
                const char* s2 = s;
                while(*s2 && *s2 != '}')
                    s2 ++;
                auto fmt_frag_str = string_view { s, s2 };

                unsigned int index = ~0u;
                const char* trait_name;
                FmtArgs args;

                // Formatting parameter
                if( *s != ':' && *s != '}' ) {
                    // Parse either an integer or an identifer
                    if( isdigit(*s) ) {
                        unsigned int arg_idx = 0;
                        do {
                            arg_idx *= 10;
                            arg_idx += *s - '0';
                            s ++;
                        } while(isdigit(*s));
                        if( arg_idx >= n_free )
                            ERROR(sp, E0000, "Positional argument " << arg_idx << " out of range");
                        index = arg_idx;
                    }
                    else {
                        const char* start = s;
                        while( isalnum(*s) || *s == '_' || (*s < 0 || *s > 127) ) {
                            s ++;
                        }
                        ::std::string   ident { start, s };
                        auto it = named.find(ident);
                        if( it == named.end() )
                            ERROR(sp, E0000, "Named argument '"<<ident<<"' not found");
                        index = n_free + it->second;
                    }
                }
                else {
                    // Leave (for now)
                    // - If index is ~0u at the end of this block, it's set to the next arg
                    // - This allows {:.*} to format correctly (taking <prec> then <arg>)
                }

                // If next character is ':', parse extra information
                if( *s == ':' ) {
                    s ++;   // eat ':'

                    // Alignment
                    if( s[0] != '\0' && (s[1] == '<' || s[1] == '^' || s[1] == '>') ) {
                        args.align_char = s[0];
                        s ++;
                    }
                    if( *s == '<' ) {
                        args.align = FmtArgs::Align::Left;
                        s ++;
                    }
                    else if( *s == '^' ) {
                        args.align = FmtArgs::Align::Center;
                        s ++;
                    }
                    else if( *s == '>' ) {
                        args.align = FmtArgs::Align::Right;
                        s ++;
                    }
                    else {
                        //args.align = FmtArgs::Align::Unspec;
                    }

                    // Sign
                    if( *s == '+' ) {
                        args.sign = FmtArgs::Sign::Plus;
                        s ++;
                    }
                    else if( *s == '-' ) {
                        args.sign = FmtArgs::Sign::Minus;
                        s ++;
                    }
                    else {
                        args.sign = FmtArgs::Sign::Unspec;
                    }

                    if( *s == '#' ) {
                        args.alternate = true;
                        s ++;
                    }
                    else {
                        //args.alternate = false;
                    }

                    if( *s == '0' ) {
                        args.zero_pad = true;
                        args.align_char = '0';
                        s ++;
                    }
                    else {
                        //args.zero_pad = false;
                    }

                    // Padded width
                    if( ::std::isdigit(*s) /*|| *s == '*'*/ ) {
                        unsigned int val = 0;
                        while( ::std::isdigit(*s) )
                        {
                            val *= 10;
                            val += *s - '0';
                            s ++;
                        }
                        args.width = val;

                        if( *s == '$' ) {
                            args.width_is_arg = true;
                            s ++;
                        }
                        else {
                            //args.width_is_arg = false;
                        }
                    }
                    else if( ::std::isalpha(*s) ) {
                        // Parse an ident and if the next character is $, convert to named
                        // - Otherwise keep the ident around for the formatter

                        const char* start = s;
                        while( isalnum(*s) || *s == '_' || (*s < 0 || *s > 127) ) {
                            s ++;
                        }
                        if( *s == '$' )
                        {
                            ::std::string   ident { start, s };
                            auto it = named.find(ident);
                            if( it == named.end() )
                                ERROR(sp, E0000, "Named argument '"<<ident<<"' not found");
                            args.width = n_free + it->second;
                            args.width_is_arg = true;

                            s ++;
                        }
                        else {
                            s = start;
                        }
                    }
                    else {
                    }
                    // Precision
                    if( *s == '.' ) {
                        s ++;
                        // '*' - Use next argument
                        if( *s == '*' ) {
                            args.prec_is_arg = true;
                            if( next_free == n_free ) {
                                ERROR(sp, E0000, "Not enough arguments passed, expected at least " << n_free+1);
                            }
                            args.prec = next_free + n_named;
                            next_free ++;
                        }
                        else if( ::std::isdigit(*s) ) {
                            unsigned int val = 0;
                            while( ::std::isdigit(*s) )
                            {
                                val *= 10;
                                val += *s - '0';
                                s ++;
                            }
                            args.prec = val;

                            if( *s == '$' ) {
                                args.prec_is_arg = true;
                                s ++;
                            }
                            else {
                                //args.prec_is_arg = false;
                            }
                        }
                        else {
                            // Wut?
                        }
                    }

                    // Parse ident?
                    // - Lazy way is to just handle a single char and ensure that it is just a single char
                    if( s[0] != '}' && s[0] != '\0' && s[1] != '}' ) {
                        TODO(sp, "Parse formatting fragment at \"" << fmt_frag_str << "\" (long type)");
                    }

                    switch(s[0])
                    {
                    case '\0':
                        ERROR(sp, E0000, "Unexpected end of formatting string");
                    default:
                        ERROR(sp, E0000, "Unknown formatting type specifier '" << *s << "'");
                    case '}':      trait_name = "Display"; break;
                    case '?': s++; trait_name = "Debug"  ; break;
                    case 'b': s++; trait_name = "Binary"; break;
                    case 'o': s++; trait_name = "Octal" ; break;
                    case 'x': s++; trait_name = "LowerHex"; break;
                    case 'X': s++; trait_name = "UpperHex"; break;
                    case 'p': s++; trait_name = "Pointer" ; break;
                    case 'e': s++; trait_name = "LowerExp"; break;
                    case 'E': s++; trait_name = "UpperExp"; break;
                    }
                    assert(*s == '}');
                }
                else {
                    if( *s != '}' )
                        ERROR(sp, E0000, "Malformed formatting fragment, unexpected " << *s);
                    // Otherwise, it's just a trivial Display call
                    trait_name = "Display";
                }

                // Set index if unspecified
                if( index == ~0u )
                {
                    if( next_free == n_free ) {
                        ERROR(sp, E0000, "Not enough arguments passed, expected at least " << n_free+1);
                    }
                    index = next_free + n_named;
                    next_free ++;
                }

                frags.push_back( FmtFrag {
                    mv$(cur_literal),
                    index, trait_name,
                    mv$(args)
                    });
            }
        }

        return ::std::make_tuple( mv$(frags), mv$(cur_literal) );
    }
}

namespace {
    void push_path(::std::vector<TokenTree>& toks, const AST::Crate& crate, ::std::initializer_list<const char*> il)
    {
        switch(crate.m_load_std)
        {
        case ::AST::Crate::LOAD_NONE:
            break;
        case ::AST::Crate::LOAD_CORE:
            toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
            toks.push_back( Token(TOK_IDENT, "core") );
            break;
        case ::AST::Crate::LOAD_STD:
            toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
            toks.push_back( Token(TOK_IDENT, "std") );
            break;
        }
        for(auto ent : il)
        {
            toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
            toks.push_back( Token(TOK_IDENT, ent) );
        }
    }
    void push_toks(::std::vector<TokenTree>& toks, Token t1) {
        toks.push_back( mv$(t1) );
    }
    void push_toks(::std::vector<TokenTree>& toks, Token t1, Token t2) {
        toks.push_back( mv$(t1) );
        toks.push_back( mv$(t2) );
    }
    //void push_toks(::std::vector<TokenTree>& toks, Token t1, Token t2, Token t3) {
    //    toks.push_back( mv$(t1) );
    //    toks.push_back( mv$(t2) );
    //    toks.push_back( mv$(t3) );
    //}
    void push_toks(::std::vector<TokenTree>& toks, Token t1, Token t2, Token t3, Token t4) {
        toks.push_back( mv$(t1) );
        toks.push_back( mv$(t2) );
        toks.push_back( mv$(t3) );
        toks.push_back( mv$(t4) );
    }
}

class CFormatArgsExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;

        auto lex = TTStream(sp, tt);
        lex.parse_state().module = &mod;
        if( ident != "" )
            ERROR(sp, E0000, "format_args! doesn't take an ident");

        auto n = Parse_ExprVal(lex);
        ASSERT_BUG(sp, n, "No expression returned");
        Expand_BareExpr(crate, mod, n);

        auto* format_string_np = dynamic_cast<AST::ExprNode_String*>(&*n);
        if( !format_string_np ) {
            ERROR(sp, E0000, "format_args! requires a string literal - got " << *n);
        }
        const auto& format_string_sp = format_string_np->span();
        const auto& format_string = format_string_np->m_value;

        ::std::map< ::std::string, unsigned int>   named_args_index;
        ::std::vector<TokenTree>    named_args;
        ::std::vector<TokenTree>    free_args;

        // - Parse the arguments
        while( GET_TOK(tok, lex) == TOK_COMMA )
        {
            if( lex.lookahead(0) == TOK_EOF ) {
                GET_TOK(tok, lex);
                break;
            }

            // - Named parameters
            if( lex.lookahead(0) == TOK_IDENT && lex.lookahead(1) == TOK_EQUAL )
            {
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                auto name = mv$(tok.str());

                GET_CHECK_TOK(tok, lex, TOK_EQUAL);

                auto expr_tt = TokenTree(Token( InterpolatedFragment(InterpolatedFragment::EXPR, Parse_Expr0(lex).release()) ));

                auto ins_rv = named_args_index.insert( ::std::make_pair(mv$(name), static_cast<unsigned>(named_args.size())) );
                if( ins_rv.second == false ) {
                    ERROR(sp, E0000, "Duplicate definition of named argument `" << ins_rv.first->first << "`");
                }
                named_args.push_back( mv$(expr_tt) );
            }
            // - Free parameters
            else
            {
                auto expr_tt = TokenTree(Token( InterpolatedFragment(InterpolatedFragment::EXPR, Parse_Expr0(lex).release()) ));
                free_args.push_back( mv$(expr_tt) );
            }
        }
        CHECK_TOK(tok, TOK_EOF);

        // - Parse the format string
        ::std::vector< FmtFrag> fragments;
        ::std::string   tail;
        ::std::tie( fragments, tail ) = parse_format_string(format_string_sp, format_string,  named_args_index, free_args.size());

        bool is_simple = true;
        for(unsigned int i = 0; i < fragments.size(); i ++)
        {
            if( fragments[i].arg_index != i ) {
                DEBUG(i << "Ordering mismach");
                is_simple = false;
            }
            if( fragments[i].args != FmtArgs {} ) {
                DEBUG(i << " Args changed - " << fragments[i].args << " != " << FmtArgs {});
                is_simple = false;
            }
        }

        ::std::vector<TokenTree> toks;
        // This should expand to a `match (a, b, c) { (ref _0, ref _1, ref _2) => ... }` to ensure that the values live long enough?
        // - Also avoids name collisions
        toks.push_back( TokenTree(TOK_RWORD_MATCH) );
        toks.push_back( TokenTree(TOK_PAREN_OPEN) );
        for(auto& arg : free_args)
        {
            toks.push_back( TokenTree(TOK_AMP) );
            toks.push_back( mv$(arg) );
            toks.push_back( TokenTree(TOK_COMMA) );
        }
        for(auto& arg : named_args)
        {
            toks.push_back( TokenTree(TOK_AMP) );
            toks.push_back( mv$(arg) );
            toks.push_back( TokenTree(TOK_COMMA) );
        }
        toks.push_back( TokenTree(TOK_PAREN_CLOSE) );
        toks.push_back( TokenTree(TOK_BRACE_OPEN) );
        toks.push_back( TokenTree(TOK_PAREN_OPEN) );
        for(unsigned int i = 0; i < free_args.size() + named_args.size(); i ++ )
        {
            toks.push_back( Token(TOK_IDENT, FMT("a" << i)) );
            toks.push_back( TokenTree(TOK_COMMA) );
        }
        toks.push_back( TokenTree(TOK_PAREN_CLOSE) );
        toks.push_back( TokenTree(TOK_FATARROW) );
        toks.push_back( TokenTree(TOK_BRACE_OPEN) );

        // Save fragments into a static
        // `static FRAGMENTS: [&'static str; N] = [...];`
        // - Contains N+1 entries, where N is the number of fragments
        {
            toks.push_back( TokenTree(TOK_RWORD_STATIC) );
            toks.push_back( Token(TOK_IDENT, "FRAGMENTS") );
            toks.push_back( TokenTree(TOK_COLON) );

            toks.push_back( TokenTree(TOK_SQUARE_OPEN) );
            toks.push_back( Token(TOK_AMP) );
            toks.push_back( Token(TOK_LIFETIME, "static") );
            toks.push_back( Token(TOK_IDENT, "str") );
            toks.push_back( Token(TOK_SEMICOLON) );
            toks.push_back( Token(static_cast<uint64_t>(fragments.size() + 1), CORETYPE_UINT) );
            toks.push_back( TokenTree(TOK_SQUARE_CLOSE) );

            toks.push_back( Token(TOK_EQUAL) );

            toks.push_back( TokenTree(TOK_SQUARE_OPEN) );
            for(const auto& frag : fragments ) {
                toks.push_back( Token(TOK_STRING, frag.leading_text) );
                toks.push_back( TokenTree(TOK_COMMA) );
            }
            toks.push_back( Token(TOK_STRING, tail) );
            toks.push_back( TokenTree(TOK_SQUARE_CLOSE) );

            toks.push_back( Token(TOK_SEMICOLON) );
        }

        if( is_simple )
        {
            // ::fmt::Arguments::new_v1
            push_path(toks, crate, {"fmt", "Arguments", "new_v1"});
            // (
            toks.push_back( TokenTree(TOK_PAREN_OPEN) );
            {
                toks.push_back( TokenTree(TOK_AMP) );
                toks.push_back( Token(TOK_IDENT, "FRAGMENTS") );
                toks.push_back( TokenTree(TOK_COMMA) );

                toks.push_back( TokenTree(TOK_AMP) );
                toks.push_back( TokenTree(TOK_SQUARE_OPEN) );
                for(const auto& frag : fragments )
                {
                    push_path(toks, crate, {"fmt", "ArgumentV1", "new"});
                    toks.push_back( Token(TOK_PAREN_OPEN) );
                    toks.push_back( Token(TOK_IDENT, FMT("a" << frag.arg_index)) );

                    toks.push_back( TokenTree(TOK_COMMA) );

                    push_path(toks, crate, {"fmt", frag.trait_name, "fmt"});
                    toks.push_back( TokenTree(TOK_PAREN_CLOSE) );
                    toks.push_back( TokenTree(TOK_COMMA) );
                }
                toks.push_back( TokenTree(TOK_SQUARE_CLOSE) );
            }
            // )
            toks.push_back( TokenTree(TOK_PAREN_CLOSE) );
        }
        else    // if(is_simple)
        {
            // 1. Generate a set of arguments+formatters
            // > Each combination of argument index and fragment type needs a unique entry in the `args` array

            // Use new_v1_formatted
            // - requires creating more entries in the `args` list to cover multiple formatters for one value
            push_path(toks, crate,  {"fmt", "Arguments", "new_v1_formatted"});
            // (
            toks.push_back( TokenTree(TOK_PAREN_OPEN) );
            {
                toks.push_back( TokenTree(TOK_AMP) );
                toks.push_back( Token(TOK_IDENT, "FRAGMENTS") );
                toks.push_back( TokenTree(TOK_COMMA) );

                // TODO: Fragments to format
                // - The format stored by mrustc doesn't quite work with how rustc (and fmt::rt::v1) works
                toks.push_back( TokenTree(TOK_AMP) );
                toks.push_back( TokenTree(TOK_SQUARE_OPEN) );
                for(const auto& frag : fragments )
                {
                    push_path(toks, crate, {"fmt", "ArgumentV1", "new"});
                    toks.push_back( Token(TOK_PAREN_OPEN) );
                    toks.push_back( Token(TOK_IDENT, FMT("a" << frag.arg_index)) );

                    toks.push_back( TokenTree(TOK_COMMA) );

                    push_path(toks, crate, {"fmt", frag.trait_name, "fmt"});
                    toks.push_back( TokenTree(TOK_PAREN_CLOSE) );
                    toks.push_back( TokenTree(TOK_COMMA) );
                }
                toks.push_back( TokenTree(TOK_SQUARE_CLOSE) );
                toks.push_back( TokenTree(TOK_COMMA) );

                toks.push_back( TokenTree(TOK_AMP) );
                toks.push_back( TokenTree(TOK_SQUARE_OPEN) );
                for(const auto& frag : fragments)
                {
                    push_path(toks, crate, {"fmt", "rt", "v1", "Argument"});
                    toks.push_back( TokenTree(TOK_BRACE_OPEN) );

                    push_toks(toks, Token(TOK_IDENT, "position"), TOK_COLON );
                    push_path(toks, crate, {"fmt", "rt", "v1", "Position", "Next"});
                    push_toks(toks, TOK_COMMA);

                    push_toks(toks, Token(TOK_IDENT, "format"), TOK_COLON );
                    push_path(toks, crate, {"fmt", "rt", "v1", "FormatSpec"});
                    toks.push_back( TokenTree(TOK_BRACE_OPEN) );
                    {
                        push_toks(toks, Token(TOK_IDENT, "fill"), TOK_COLON, Token(uint64_t(frag.args.align_char), CORETYPE_CHAR), TOK_COMMA );

                        push_toks(toks, Token(TOK_IDENT, "align"), TOK_COLON);
                        const char* align_var_name = nullptr;
                        switch( frag.args.align )
                        {
                        case FmtArgs::Align::Unspec:    align_var_name = "Unknown"; break;
                        case FmtArgs::Align::Left:      align_var_name = "Left";    break;
                        case FmtArgs::Align::Center:    align_var_name = "Center";  break;
                        case FmtArgs::Align::Right:     align_var_name = "Right";   break;
                        }
                        push_path(toks, crate, {"fmt", "rt", "v1", "Alignment", align_var_name});
                        push_toks(toks, TOK_COMMA);

                        push_toks(toks, Token(TOK_IDENT, "flags"), TOK_COLON);
                        uint64_t flags = 0;
                        if(frag.args.alternate)
                            flags |= 1 << 2;
                        push_toks(toks, Token(uint64_t(flags), CORETYPE_U32));
                        push_toks(toks, TOK_COMMA);

                        push_toks(toks, Token(TOK_IDENT, "precision"), TOK_COLON );
                        if( frag.args.prec_is_arg || frag.args.prec != 0 ) {
                            push_path(toks, crate, {"fmt", "rt", "v1", "Count", "Is"});
                            push_toks(toks, TOK_PAREN_OPEN);
                            if( frag.args.prec_is_arg ) {
                                push_toks(toks, TOK_STAR, Token(TOK_IDENT, FMT("a" << frag.args.prec)) );
                            }
                            else {
                                push_toks(toks, Token(uint64_t(frag.args.prec), CORETYPE_UINT) );
                            }
                            toks.push_back( TokenTree(TOK_PAREN_CLOSE) );
                        }
                        else {
                            push_path(toks, crate, {"fmt", "rt", "v1", "Count", "Implied"});
                        }
                        toks.push_back( TokenTree(TOK_COMMA) );

                        push_toks(toks, Token(TOK_IDENT, "width"), TOK_COLON );
                        if( frag.args.width_is_arg || frag.args.width != 0 ) {
                            push_path(toks, crate, {"fmt", "rt", "v1", "Count", "Is"});
                            push_toks(toks, TOK_PAREN_OPEN);
                            if( frag.args.width_is_arg ) {
                                push_toks(toks, TOK_STAR, Token(TOK_IDENT, FMT("a" << frag.args.width)) );
                            }
                            else {
                                push_toks(toks, Token(uint64_t(frag.args.width), CORETYPE_UINT) );
                            }
                            toks.push_back( TokenTree(TOK_PAREN_CLOSE) );
                        }
                        else {
                            push_path(toks, crate, {"fmt", "rt", "v1", "Count", "Implied"});
                        }
                        toks.push_back( TokenTree(TOK_COMMA) );
                    }
                    toks.push_back( TokenTree(TOK_BRACE_CLOSE) );

                    toks.push_back( TokenTree(TOK_BRACE_CLOSE) );
                    toks.push_back( TokenTree(TOK_COMMA) );
                }
                toks.push_back( TokenTree(TOK_SQUARE_CLOSE) );
            }
            // )
            toks.push_back( TokenTree(TOK_PAREN_CLOSE) );
        }   // if(is_simple) else

        toks.push_back( TokenTree(TOK_BRACE_CLOSE) );
        toks.push_back( TokenTree(TOK_BRACE_CLOSE) );

        return box$( TTStreamO(sp, TokenTree(Ident::Hygiene::new_scope(), mv$(toks))) );
    }
};

STATIC_MACRO("format_args", CFormatArgsExpander);

