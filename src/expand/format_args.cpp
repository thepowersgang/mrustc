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
        enum class Debug {
            Normal,
            LowerHex,
            UpperHex,
        };

        Align   align = Align::Unspec;
        uint32_t align_char = ' ';

        Sign    sign = Sign::Unspec;
        bool    alternate = false;
        bool    zero_pad = false;

        Debug   debug_ty = Debug::Normal;

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

    uint32_t parse_utf8(const char* s, int& out_len)
    {
        uint8_t v1 = s[0];
        if( v1 < 0x80 )
        {
            out_len = 1;
            return v1;
        }
        else if( (v1 & 0xC0) == 0x80 )
        {
            // Invalid (continuation)
            out_len = 1;
            return 0xFFFE;
        }
        else if( (v1 & 0xE0) == 0xC0 ) {
            // Two bytes
            out_len = 2;

            uint8_t e1 = s[1];
            if( (e1 & 0xC0) != 0x80 )  return 0xFFFE;

            uint32_t outval
                = ((v1 & 0x1F) << 6)
                | ((e1 & 0x3F) <<0)
                ;
            return outval;
        }
        else if( (v1 & 0xF0) == 0xE0 ) {
            // Three bytes
            out_len = 3;
            uint8_t e1 = s[1];
            if( (e1 & 0xC0) != 0x80 )  return 0xFFFE;
            uint8_t e2 = s[2];
            if( (e2 & 0xC0) != 0x80 )  return 0xFFFE;

            uint32_t outval
                = ((v1 & 0x0F) << 12)
                | ((e1 & 0x3F) <<  6)
                | ((e2 & 0x3F) <<  0)
                ;
            return outval;
        }
        else if( (v1 & 0xF8) == 0xF0 ) {
            // Four bytes
            out_len = 4;
            uint8_t e1 = s[1];
            if( (e1 & 0xC0) != 0x80 )  return 0xFFFE;
            uint8_t e2 = s[2];
            if( (e2 & 0xC0) != 0x80 )  return 0xFFFE;
            uint8_t e3 = s[3];
            if( (e3 & 0xC0) != 0x80 )  return 0xFFFE;

            uint32_t outval
                = ((v1 & 0x07) << 18)
                | ((e1 & 0x3F) << 12)
                | ((e2 & 0x3F) <<  6)
                | ((e3 & 0x3F) <<  0)
                ;
            return outval;
        }
        else {
            throw "";   // Should be impossible.
        }
    }

    /// Parse a format string into a sequence of fragments.
    ///
    /// Returns a list of fragments, and the remaining free text after the last format sequence
    ::std::tuple< ::std::vector<FmtFrag>, ::std::string> parse_format_string(
        const Span& sp,
        const ::std::string& format_string,
        ::std::map<RcString,unsigned int>& named,
        unsigned int n_free,
        std::vector<TokenTree>& named_args,
        const Ident::Hygiene& hygiene
        )
    {
        //unsigned int n_named = named.size();
        unsigned int next_free = 0;

        ::std::vector<FmtFrag>  frags;
        ::std::string   cur_literal;

        const char* s = format_string.c_str();
        const char* const s_end = s + format_string.length();
        for( ; s < s_end; s ++)
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
                while(s2 < s_end && *s2 != '}')
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
                            ERROR(sp, E0000, "Positional argument " << arg_idx << " out of range in \"" << format_string << "\"");
                        index = arg_idx;
                    }
                    else {
                        const char* start = s;
                        while( isalnum(*s) || *s == '_' || (*s < 0 || *s > 127) ) {
                            s ++;
                        }
                        auto ident = RcString(start, s - start);
                        auto it = named.find(ident);
                        if( it == named.end() ) {
                            // Add an implicit named argument
                            it = named.insert(std::make_pair(ident, static_cast<unsigned>(named_args.size()))).first;
                            // TODO: Create a token with span information pointing to this location in the string.
                            named_args.push_back(Token(TOK_IDENT, Ident(hygiene, ident)));
                        }
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
                    // - Padding character, a single unicode codepoint followed by '<'/'^'/'>'
                    {
                        int next_c_i;
                        uint32_t ch = parse_utf8(s, next_c_i);
                        char next_c = s[next_c_i];
                        if( ch != '}' && ch != '\0' && (next_c == '<' || next_c == '^' || next_c == '>') ) {
                            args.align_char = ch;
                            s += next_c_i;
                        }
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
                            auto ident = RcString(start, s - start);
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
                            args.prec = next_free;
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

                    if( s[0] == '\0' )
                        ERROR(sp, E0000, "Unexpected end of formatting string");

                    // Parse ident?
                    // - Lazy way is to just handle a single char and ensure that it is just a single char
                    if( s[0] == '}' )
                    {
                        trait_name = "Display";
                    }
                    else if( s[1] == '}' )
                    {
                        switch(s[0])
                        {
                        default:
                            ERROR(sp, E0000, "Unknown formatting type specifier '" << *s << "'");
                        case '?': s++; trait_name = "Debug" ; break;
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
                    else
                    {
                        if( strcmp(s, "x?}") == 0 ) {
                            args.debug_ty = FmtArgs::Debug::LowerHex;
                            trait_name = "Debug";
                        }
                        else if( strcmp(s, "X?}") == 0 ) {
                            args.debug_ty = FmtArgs::Debug::UpperHex;
                            trait_name = "Debug";
                        }
                        else {
                            TODO(sp, "Parse formatting fragment at \"" << fmt_frag_str << "\" (long type) - s=...\"" << s << "\"");
                        }
                    }
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
                    index = next_free;
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
    Token ident(const char* s) {
        return Token(TOK_IDENT, RcString::new_interned(s));
    }
    void push_path(::std::vector<TokenTree>& toks, const AST::Crate& crate, ::std::initializer_list<const char*> il)
    {
        switch(crate.m_load_std)
        {
        case ::AST::Crate::LOAD_NONE:
            toks.push_back( TokenTree(TOK_RWORD_CRATE) );
            break;
        case ::AST::Crate::LOAD_CORE:
            toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
            toks.push_back( ident("core") );
            break;
        case ::AST::Crate::LOAD_STD:
            toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
            toks.push_back( ident("std") );
            break;
        }
        for(auto ent : il)
        {
            toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
            toks.push_back( ident(ent) );
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

    ::std::unique_ptr<TokenStream> expand_format_args(const Span& sp, const ::AST::Crate& crate, TTStream& lex, bool add_newline)
    {
        Token   tok;

        auto format_string_node = Parse_ExprVal(lex);
        auto h = lex.get_hygiene();
        ASSERT_BUG(sp, format_string_node, "No expression returned");
        Expand_BareExpr(crate, lex.parse_state().get_current_mod(), format_string_node);

        auto* format_string_np = dynamic_cast<AST::ExprNode_String*>(&*format_string_node);
        if( !format_string_np ) {
            ERROR(sp, E0000, "format_args! requires a string literal - got " << *format_string_node);
        }
        const auto& format_string_sp = format_string_np->span();
        const auto& format_string = format_string_np->m_value;

        ::std::map<RcString, unsigned int>   named_args_index;
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
            if( (lex.lookahead(0) == TOK_IDENT || Token::type_is_rword(lex.lookahead(0))) && lex.lookahead(1) == TOK_EQUAL )
            {
                GET_TOK(tok, lex);
                auto name = tok.type() == TOK_IDENT ? tok.ident().name : RcString::new_interned(tok.to_str());
                DEBUG("Named `" << name << "`");

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
                DEBUG("Free");
                auto expr_tt = TokenTree(Token( InterpolatedFragment(InterpolatedFragment::EXPR, Parse_Expr0(lex).release()) ));
                free_args.push_back( mv$(expr_tt) );
            }
        }
        CHECK_TOK(tok, TOK_EOF);

        // - Parse the format string
        ::std::vector< FmtFrag> fragments;
        ::std::string   tail;
        ::std::tie( fragments, tail ) = parse_format_string(format_string_sp, format_string,  named_args_index, free_args.size(), named_args, h);
        if( add_newline )
        {
            tail += "\n";
        }

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
            toks.push_back( ident(FMT("a" << i).c_str()) );
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
            toks.push_back( ident("FRAGMENTS") );
            toks.push_back( TokenTree(TOK_COLON) );

            toks.push_back( TokenTree(TOK_SQUARE_OPEN) );
            toks.push_back( Token(TOK_AMP) );
            toks.push_back( Token(TOK_LIFETIME, RcString::new_interned("static")) );
            toks.push_back( ident("str") );
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
                toks.push_back( ident("FRAGMENTS") );
                toks.push_back( TokenTree(TOK_COMMA) );

                toks.push_back( TokenTree(TOK_AMP) );
                toks.push_back( TokenTree(TOK_SQUARE_OPEN) );
                for(const auto& frag : fragments )
                {
                    push_path(toks, crate, {"fmt", "ArgumentV1", "new"});
                    toks.push_back( Token(TOK_PAREN_OPEN) );
                    toks.push_back( ident( FMT("a" << frag.arg_index).c_str() ) );

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
                toks.push_back( ident("FRAGMENTS") );
                toks.push_back( TokenTree(TOK_COMMA) );

                // TODO: Fragments to format
                // - The format stored by mrustc doesn't quite work with how rustc (and fmt::rt::v1) works
                toks.push_back( TokenTree(TOK_AMP) );
                toks.push_back( TokenTree(TOK_SQUARE_OPEN) );
                for(const auto& frag : fragments )
                {
                    push_path(toks, crate, {"fmt", "ArgumentV1", "new"});
                    toks.push_back( Token(TOK_PAREN_OPEN) );
                    toks.push_back( ident(FMT("a" << frag.arg_index).c_str()) );

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

                    push_toks(toks, ident("position"), TOK_COLON );
                    if(TARGETVER_MOST_1_39) {
                        push_path(toks, crate, {"fmt", "rt", "v1", "Position", "Next"});
                    }
                    else {
                        push_toks(toks, Token(static_cast<uint64_t>(&frag - fragments.data()), CORETYPE_UINT));
                    }
                    push_toks(toks, TOK_COMMA);

                    push_toks(toks, ident("format"), TOK_COLON );
                    push_path(toks, crate, {"fmt", "rt", "v1", "FormatSpec"});
                    toks.push_back( TokenTree(TOK_BRACE_OPEN) );
                    {
                        push_toks(toks, ident("fill"), TOK_COLON, Token(uint64_t(frag.args.align_char), CORETYPE_CHAR), TOK_COMMA );

                        push_toks(toks, ident("align"), TOK_COLON);
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

                        push_toks(toks, ident("flags"), TOK_COLON);
                        uint64_t flags = 0;
                        // ::core::fmt::FlagV1 (private)
                        switch(frag.args.sign)
                        {
                        case FmtArgs::Sign::Unspec: break;
                        case FmtArgs::Sign::Plus:   flags |= 1 << 0;    break;
                        case FmtArgs::Sign::Minus:  flags |= 1 << 1;    break;
                        }
                        if(frag.args.alternate)
                            flags |= 1 << 2;
                        switch(frag.args.debug_ty)
                        {
                        case FmtArgs::Debug::Normal:    break;
                        case FmtArgs::Debug::LowerHex:  flags |= 1 << 4;    break;
                        case FmtArgs::Debug::UpperHex:  flags |= 1 << 5;    break;
                        }
                        push_toks(toks, Token(uint64_t(flags), CORETYPE_U32));
                        push_toks(toks, TOK_COMMA);

                        push_toks(toks, ident("precision"), TOK_COLON );
                        if( frag.args.prec_is_arg || frag.args.prec != 0 ) {
                            push_path(toks, crate, {"fmt", "rt", "v1", "Count", "Is"});
                            push_toks(toks, TOK_PAREN_OPEN);
                            if( frag.args.prec_is_arg ) {
                                push_toks(toks, TOK_STAR, ident(FMT("a" << frag.args.prec).c_str()) );
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

                        push_toks(toks, ident("width"), TOK_COLON );
                        if( frag.args.width_is_arg || frag.args.width != 0 ) {
                            push_path(toks, crate, {"fmt", "rt", "v1", "Count", "Is"});
                            push_toks(toks, TOK_PAREN_OPEN);
                            if( frag.args.width_is_arg ) {
                                push_toks(toks, TOK_STAR, ident(FMT("a" << frag.args.width).c_str()) );
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

        return box$( TTStreamO(sp, ParseState(), TokenTree(lex.get_edition(), Ident::Hygiene::new_scope(), mv$(toks))) );
    }
}

class CFormatArgsExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;

        auto lex = TTStream(sp, ParseState(), tt);
        lex.parse_state().module = &mod;

        return expand_format_args(sp, crate, lex, /*add_newline=*/false);
    }
};

class CFormatArgsNlExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;

        auto lex = TTStream(sp, ParseState(), tt);
        lex.parse_state().module = &mod;

        return expand_format_args(sp, crate, lex, /*add_newline=*/true);
    }
};

STATIC_MACRO("format_args", CFormatArgsExpander);
STATIC_MACRO("format_args_nl", CFormatArgsNlExpander);

