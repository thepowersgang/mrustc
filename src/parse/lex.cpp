/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/lex.cpp
 * - Lexer (converts input file to token stream)
 */
#include "lex.hpp"
#include "tokentree.hpp"
#include "parseerror.hpp"
#include "../common.hpp"
#include <cassert>
#include <iostream>
#include <cstdlib>  // strtol
#include <typeinfo>
#include <algorithm>    // std::count
#include <limits>       // std::numeric_limits
#include <cctype>
//#define TRACE_CHARS
//#define TRACE_RAW_TOKENS

Lexer::Lexer(const ::std::string& filename, AST::Edition edition, ParseState ps):
    TokenStream(ps),
    m_path(filename.c_str()),
    m_line(1),
    m_line_ofs(0),
    m_istream_fp(filename != "-" ? new std::ifstream(filename.c_str()) : nullptr),
    m_istream(filename != "-" ? *m_istream_fp : std::cin),
    m_last_char_valid(false),
    m_edition(edition),
    m_hygiene( Ident::Hygiene::new_scope() )
{
    if( m_istream_fp )
    {
        if( !m_istream_fp->is_open() )
        {
            throw ::std::runtime_error("Unable to open file '" + filename + "'");
        }
        // Consume the BOM
        if( this->getc_byte() == '\xef' )
        {
            if( this->getc_byte() != '\xbb' ) {
                throw ::std::runtime_error("Incomplete BOM - missing \\xBB in second position");
            }
            if( this->getc_byte() != '\xbf' ) {
                throw ::std::runtime_error("Incomplete BOM - missing \\xBF in second position");
            }
            m_line_ofs = 0;
        }
        else
        {
            m_istream.unget();
        }
    }
}


#define LINECOMMENT -1
#define BLOCKCOMMENT -2
#define SINGLEQUOTE -3
#define DOUBLEQUOTE -4
#define SHEBANG -5

// NOTE: This array must be kept sorted, or symbols are will be skipped
#define TOKENT(str, sym)    {sizeof(str)-1, str, sym}
static const struct {
    unsigned char len;
    const char* chars;
    signed int type;
} TOKENMAP[] = {
  TOKENT("!" , TOK_EXCLAM),
  TOKENT("!=", TOK_EXCLAM_EQUAL),
  TOKENT("\"", DOUBLEQUOTE),
  TOKENT("#",  TOK_HASH),
  TOKENT("$",  TOK_DOLLAR),
  TOKENT("%" , TOK_PERCENT),
  TOKENT("%=", TOK_PERCENT_EQUAL),
  TOKENT("&" , TOK_AMP),
  TOKENT("&&", TOK_DOUBLE_AMP),
  TOKENT("&=", TOK_AMP_EQUAL),
  TOKENT("'" , SINGLEQUOTE),
  TOKENT("(" , TOK_PAREN_OPEN),
  TOKENT(")" , TOK_PAREN_CLOSE),
  TOKENT("*" , TOK_STAR),
  TOKENT("*=", TOK_STAR_EQUAL),
  TOKENT("+" , TOK_PLUS),
  TOKENT("+=", TOK_PLUS_EQUAL),
  TOKENT("," , TOK_COMMA),
  TOKENT("-" , TOK_DASH),
  TOKENT("-=", TOK_DASH_EQUAL),
  TOKENT("->", TOK_THINARROW),
  TOKENT(".",  TOK_DOT),
  // NOTE: These have special handling when following numbers
  TOKENT("..", TOK_DOUBLE_DOT),
  TOKENT("...",TOK_TRIPLE_DOT),
  TOKENT("..=",TOK_DOUBLE_DOT_EQUAL),
  TOKENT("/" , TOK_SLASH),
  TOKENT("/*", BLOCKCOMMENT),
  TOKENT("//", LINECOMMENT),
  TOKENT("/=", TOK_SLASH_EQUAL),
  // 0-9 :: Elsewhere
  TOKENT(":",  TOK_COLON),
  TOKENT("::", TOK_DOUBLE_COLON),
  TOKENT(";",  TOK_SEMICOLON),
  TOKENT("<",  TOK_LT),
  TOKENT("<-", TOK_THINARROW_LEFT),
  TOKENT("<<", TOK_DOUBLE_LT),
  TOKENT("<<=",TOK_DOUBLE_LT_EQUAL),
  TOKENT("<=", TOK_LTE),
  TOKENT("=" , TOK_EQUAL),
  TOKENT("==", TOK_DOUBLE_EQUAL),
  TOKENT("=>", TOK_FATARROW),
  TOKENT(">",  TOK_GT),
  TOKENT(">=", TOK_GTE),
  TOKENT(">>", TOK_DOUBLE_GT),
  TOKENT(">>=",TOK_DOUBLE_GT_EQUAL),
  TOKENT("?",  TOK_QMARK),
  TOKENT("@",  TOK_AT),
  // A-Z :: Elsewhere
  TOKENT("[",  TOK_SQUARE_OPEN),
  TOKENT("\\", TOK_BACKSLASH),
  TOKENT("]",  TOK_SQUARE_CLOSE),
  TOKENT("^",  TOK_CARET),
  TOKENT("^=", TOK_CARET_EQUAL),
  TOKENT("`",  TOK_BACKTICK),
  // a-z :: Elsewhere
  //TOKENT("b\"", DOUBLEQUOTE),

  TOKENT("{",  TOK_BRACE_OPEN),
  TOKENT("|",  TOK_PIPE),
  TOKENT("|=", TOK_PIPE_EQUAL),
  TOKENT("||", TOK_DOUBLE_PIPE),
  TOKENT("}",  TOK_BRACE_CLOSE),
  TOKENT("~",  TOK_TILDE),
};
#define LEN(arr)    (sizeof(arr)/sizeof(arr[0]))
struct sRWORD {
    unsigned char len;
    const char* chars;
    signed int type;
};
static const sRWORD RWORDS_2015[] = {
  TOKENT("_", TOK_UNDERSCORE),
  TOKENT("abstract",TOK_RWORD_ABSTRACT), // Reserved 2015+
  TOKENT("as",      TOK_RWORD_AS),
  TOKENT("become",  TOK_RWORD_BECOME),  //  // Reserved 2015+
  TOKENT("box",     TOK_RWORD_BOX),
  TOKENT("break",   TOK_RWORD_BREAK),
  TOKENT("const",   TOK_RWORD_CONST),
  TOKENT("continue",TOK_RWORD_CONTINUE),
  TOKENT("crate",   TOK_RWORD_CRATE),
  TOKENT("do",      TOK_RWORD_DO),  // Reserved 2015+
  TOKENT("else",    TOK_RWORD_ELSE),
  TOKENT("enum",    TOK_RWORD_ENUM),
  TOKENT("extern",  TOK_RWORD_EXTERN),
  TOKENT("false",   TOK_RWORD_FALSE),
  TOKENT("final",   TOK_RWORD_FINAL), // Reserved 2015+
  TOKENT("fn",      TOK_RWORD_FN),
  TOKENT("for",     TOK_RWORD_FOR),
  TOKENT("if",      TOK_RWORD_IF),
  TOKENT("impl",    TOK_RWORD_IMPL),
  TOKENT("in",      TOK_RWORD_IN),
  TOKENT("let",     TOK_RWORD_LET),
  TOKENT("loop",    TOK_RWORD_LOOP),
  TOKENT("macro",   TOK_RWORD_MACRO),
  TOKENT("match",   TOK_RWORD_MATCH),
  TOKENT("mod",     TOK_RWORD_MOD),
  TOKENT("move",    TOK_RWORD_MOVE),
  TOKENT("mut",     TOK_RWORD_MUT),
  TOKENT("override",TOK_RWORD_OVERRIDE),    // Reserved 2015+
  TOKENT("priv",    TOK_RWORD_PRIV),
  TOKENT("pub",     TOK_RWORD_PUB),
  TOKENT("ref",     TOK_RWORD_REF),
  TOKENT("return",  TOK_RWORD_RETURN),
  TOKENT("self",    TOK_RWORD_SELF),
  TOKENT("static",  TOK_RWORD_STATIC),
  TOKENT("struct",  TOK_RWORD_STRUCT),
  TOKENT("super",   TOK_RWORD_SUPER),
  TOKENT("trait",   TOK_RWORD_TRAIT),
  TOKENT("true",    TOK_RWORD_TRUE),
  TOKENT("type",    TOK_RWORD_TYPE),
  TOKENT("typeof",  TOK_RWORD_TYPEOF), // Reserved 2015+
  TOKENT("unsafe",  TOK_RWORD_UNSAFE),
  TOKENT("unsized", TOK_RWORD_UNSIZED), // Reserved 2015+
  TOKENT("use",     TOK_RWORD_USE),
  TOKENT("virtual", TOK_RWORD_VIRTUAL), // Reserved 2015+
  TOKENT("where",   TOK_RWORD_WHERE),
  TOKENT("while",   TOK_RWORD_WHILE),
  TOKENT("yield",   TOK_RWORD_YIELD), // Reserved 2015+
};

static const sRWORD RWORDS_2018[] = {
    TOKENT("_", TOK_UNDERSCORE),
    TOKENT("abstract",TOK_RWORD_ABSTRACT), // Reserved 2015+
    TOKENT("as",      TOK_RWORD_AS),
    TOKENT("async",   TOK_RWORD_ASYNC), // Added 2018
    TOKENT("await",   TOK_RWORD_AWAIT), // Added 2018
    TOKENT("become",  TOK_RWORD_BECOME),  //  // Reserved 2015+
    TOKENT("box",     TOK_RWORD_BOX),
    TOKENT("break",   TOK_RWORD_BREAK),
    TOKENT("const",   TOK_RWORD_CONST),
    TOKENT("continue",TOK_RWORD_CONTINUE),
    TOKENT("crate",   TOK_RWORD_CRATE),
    TOKENT("do",      TOK_RWORD_DO),  // Reserved 2015+
    TOKENT("dyn",     TOK_RWORD_DYN), // Added 2018
    TOKENT("else",    TOK_RWORD_ELSE),
    TOKENT("enum",    TOK_RWORD_ENUM),
    TOKENT("extern",  TOK_RWORD_EXTERN),
    TOKENT("false",   TOK_RWORD_FALSE),
    TOKENT("final",   TOK_RWORD_FINAL), // Reserved 2015+
    TOKENT("fn",      TOK_RWORD_FN),
    TOKENT("for",     TOK_RWORD_FOR),
    TOKENT("if",      TOK_RWORD_IF),
    TOKENT("impl",    TOK_RWORD_IMPL),
    TOKENT("in",      TOK_RWORD_IN),
    TOKENT("let",     TOK_RWORD_LET),
    TOKENT("loop",    TOK_RWORD_LOOP),
    TOKENT("macro",   TOK_RWORD_MACRO),
    TOKENT("match",   TOK_RWORD_MATCH),
    TOKENT("mod",     TOK_RWORD_MOD),
    TOKENT("move",    TOK_RWORD_MOVE),
    TOKENT("mut",     TOK_RWORD_MUT),
    TOKENT("override",TOK_RWORD_OVERRIDE),    // Reserved 2015+
    TOKENT("priv",    TOK_RWORD_PRIV),
    TOKENT("pub",     TOK_RWORD_PUB),
    TOKENT("ref",     TOK_RWORD_REF),
    TOKENT("return",  TOK_RWORD_RETURN),
    TOKENT("self",    TOK_RWORD_SELF),
    TOKENT("static",  TOK_RWORD_STATIC),
    TOKENT("struct",  TOK_RWORD_STRUCT),
    TOKENT("super",   TOK_RWORD_SUPER),
    TOKENT("trait",   TOK_RWORD_TRAIT),
    TOKENT("true",    TOK_RWORD_TRUE),
    TOKENT("try",     TOK_RWORD_TRY), // Reserved 2018+
    TOKENT("type",    TOK_RWORD_TYPE),
    TOKENT("typeof",  TOK_RWORD_TYPEOF), // Reserved 2015+
    TOKENT("unsafe",  TOK_RWORD_UNSAFE),
    TOKENT("unsized", TOK_RWORD_UNSIZED), // Reserved 2015+
    TOKENT("use",     TOK_RWORD_USE),
    TOKENT("virtual", TOK_RWORD_VIRTUAL), // Reserved 2015+
    TOKENT("where",   TOK_RWORD_WHERE),
    TOKENT("while",   TOK_RWORD_WHILE),
    TOKENT("yield",   TOK_RWORD_YIELD), // Reserved 2015+
};

signed int Lexer::getSymbol()
{
    Codepoint ch = this->getc();
    // 1. lsearch for character
    // 2. Consume as many characters as currently match
    // 3. IF: a smaller character or, EOS is hit - Return current best
    unsigned ofs = 0;
    signed int best = 0;
    bool hit_eof = false;
    for(unsigned i = 0; i < LEN(TOKENMAP); i ++)
    {
        const char* const chars = TOKENMAP[i].chars;
        const size_t len = TOKENMAP[i].len;

        if( ofs >= len || static_cast<uint32_t>(chars[ofs]) > ch.v ) {
            break ;
        }

        while( chars[ofs] && ch == chars[ofs] )
        {
            try {
                ch = this->getc();
            }
            catch(Lexer::EndOfFile) {
                ch = 0;
                // Prevent `ungetc` if EOF was hit
                hit_eof = true;
            }
            ofs ++;
        }
        if( chars[ofs] == 0 )
        {
            best = TOKENMAP[i].type;
        }
    }

    if( !hit_eof )
    {
        this->ungetc();
    }
    return best;
}

bool issym(Codepoint ch)
{
    if('0' <= ch.v && ch.v <= '9')
        return true;
    if( ::std::isalpha(ch.v) )
        return true;
    if( ch == '_' )
        return true;
    if( ch.v >= 128 )
        return !ch.isspace();
    return false;
}

Position Lexer::getPosition() const
{
    return Position(m_path, m_line, m_line_ofs);
}
Ident::Hygiene Lexer::realGetHygiene() const
{
    return m_hygiene;
}
Token Lexer::realGetToken()
{
    while(true)
    {
        Token tok = getTokenInt();
#ifdef TRACE_RAW_TOKENS
        ::std::cout << "getTokenInt: tok = " << tok << ::std::endl;
#endif
        switch(tok.type())
        {
        case TOK_NEWLINE:
            continue;
        case TOK_WHITESPACE:
            continue;
        case TOK_COMMENT: {
            continue; }
        default:
            return tok;
        }
    }
}

Token Lexer::getTokenInt()
{
    if( ! this->m_next_tokens.empty() )
    {
        auto rv = ::std::move(this->m_next_tokens.back());
        m_next_tokens.pop_back();
        return rv;
    }
    try
    {
        Codepoint ch = this->getc();

        if( m_line == 1 && m_line_ofs == 1 && ch == '#') {
            switch( (ch = this->getc()).v )
            {
            case '!':
                switch( (ch = this->getc()).v )
                {
                case '/':
                    // SHEBANG!
                    while( ch != '\n' )
                        ch = this->getc();
                    return Token(TOK_NEWLINE);
                case '[':
                    this->ungetc();
                    this->m_next_tokens.push_back(TOK_EXCLAM);
                    return Token(TOK_HASH);
                default:
                    throw ParseError::BadChar(*this, ch.v);
                }
            case '[':
                this->ungetc();
                return Token(TOK_HASH);
            default:
                this->ungetc();
                //return Token(TOK_HASH);
                throw ParseError::BadChar(*this, ch.v);
            }
        }

        if( ch == '\n' )
            return Token(TOK_NEWLINE);
        if( ch.isspace() )
        {
            while( (ch = this->getc()).isspace() && ch != '\n' )
                ;
            this->ungetc();
            return Token(TOK_WHITESPACE);
        }
        this->ungetc();

        const signed int sym = this->getSymbol();
        if( sym == 0 )
        {
            // No match at all, check for symbol
            auto ch = this->getc();
            if( ch.isdigit() )
            {
                enum eCoreType  num_type = CORETYPE_ANY;
                NumMode num_mode = NumMode::DEC;

                // Handle integers/floats
                this->ungetc();
                auto val = this->parseInt(&num_mode);
                ch = this->getc();

                if( ch == 'e' || ch == 'E' || ch == '.' ) {
                    // Special handling for `.` - for `..` and method access
                    if( ch == '.' )
                    {
                        ch = this->getc();

                        // Double/Triple Dot
                        if( ch == '.' )
                        {
                            ch = this->getc();
                            if( ch == '.') {
                                this->m_next_tokens.push_back(TOK_TRIPLE_DOT);
                            }
                            else if( ch == '=') {
                                this->m_next_tokens.push_back(TOK_DOUBLE_DOT_EQUAL);
                            }
                            else {
                                this->ungetc();
                                this->m_next_tokens.push_back(TOK_DOUBLE_DOT);
                            }
                            return Token(val, CORETYPE_ANY);
                        }

                        // Single dot followed by a non-digit, could be a float or an integer with a method/field access
                        // NOTE: `1.e1` is not a float
                        if( !ch.isdigit() )
                        {
                            this->ungetc();
                            if( issym(ch) )
                            {
                                this->m_next_tokens.push_back(TOK_DOT);
                                return Token(val, CORETYPE_ANY);
                            }
                            else
                            {
                                double fval = val.to_double();
                                return Token::make_float(fval, CORETYPE_ANY);
                            }
                        }
                        else
                        {
                            // Digit, continue
                            // NOTE: parseFloat assumes that the '.' has been consumed, and reads digits until it hits a non-digit and then parses exponents
                            // - Thus, continuing here and letting the below 'ungetc' push a digit back is correct.
                        }
                    }
                    if( num_mode != NumMode::DEC )
                        TODO(this->point_span(), "Non-decimal floats");


                    this->ungetc();
                    double fval = this->parseFloat(val);
                    if( fval != fval )
                    {
                        assert(!this->m_next_tokens.empty());
                        auto t = std::move( this->m_next_tokens.back() );
                        this->m_next_tokens.pop_back();
                        return t;
                    }
                    if( issym(ch = this->getc()) )
                    {
                        ::std::string   suffix;
                        while( issym(ch) )
                        {
                            suffix += ch;
                            ch = this->getc();
                        }
                        this->ungetc();

                        if(0)   ;
                        else if(suffix == "f32") num_type = CORETYPE_F32;
                        else if(suffix == "f64") num_type = CORETYPE_F64;
                        else
                            throw ParseError::Generic( FMT("Unknown number suffix " << suffix) );
                    }
                    else
                    {
                        this->ungetc();
                    }
                    return Token::make_float(fval, num_type);

                }
                else if( issym(ch)) {
                    // Unsigned
                    ::std::string   suffix;
                    while( issym(ch) )
                    {
                        suffix += ch;
                        ch = this->getc();
                    }
                    this->ungetc();

                    if(0)   ;
                    else if(suffix == "i8")  num_type = CORETYPE_I8;
                    else if(suffix == "i16") num_type = CORETYPE_I16;
                    else if(suffix == "i32") num_type = CORETYPE_I32;
                    else if(suffix == "i64") num_type = CORETYPE_I64;
                    else if(suffix == "i128") num_type = CORETYPE_I128;
                    else if(suffix == "isize") num_type = CORETYPE_INT;
                    else if(suffix == "u8")  num_type = CORETYPE_U8;
                    else if(suffix == "u16") num_type = CORETYPE_U16;
                    else if(suffix == "u32") num_type = CORETYPE_U32;
                    else if(suffix == "u64") num_type = CORETYPE_U64;
                    else if(suffix == "u128") num_type = CORETYPE_U128;
                    else if(suffix == "usize") num_type = CORETYPE_UINT;
                    else if(suffix == "f32") num_type = CORETYPE_F32;
                    else if(suffix == "f64") num_type = CORETYPE_F64;
                    else
                        throw ParseError::Generic(*this, FMT("Unknown integer suffix '" << suffix << "'"));
                    return Token(val, num_type);
                }
                else {
                    this->ungetc();
                    return Token(val, num_type);
                }
            }
            // Byte/Raw strings
            else if( ch == 'b' || ch == 'r' )
            {
                bool is_byte = false;
                if(ch == 'b') {
                    is_byte = true;
                    ch = this->getc();
                }

                if(ch == 'r') {
                    return this->getTokenInt_RawString(is_byte);
                }
                else {
                    assert(is_byte);

                    // Byte string
                    if( ch == '"' ) {
                        ::std::string str;
                        while( (ch = this->getc()) != '"' )
                        {
                            if( ch == '\\' ) {
                                auto v = this->parseEscape('"');
                                if( v != ~0u ) {
                                    if( v > 256 )
                                        throw ParseError::Generic(*this, "Value out of range for byte literal");
                                    str += (char)v;
                                }
                            }
                            else {
                                str += ch;
                            }
                        }
                        return Token(TOK_BYTESTRING, mv$(str));
                    }
                    // Byte constant
                    else if( ch == '\'' ) {
                        // Byte constant
                        ch = this->getc();
                        if( ch == '\\' ) {
                            uint32_t val = this->parseEscape('\'');
                            if( this->getc() != '\'' )
                                throw ParseError::Generic(*this, "Multi-byte character literal");
                            return Token( U128(val), CORETYPE_U8);
                        }
                        else {
                            if( this->getc() != '\'' )
                                throw ParseError::Generic(*this, "Multi-byte character literal");
                            return Token( U128(ch.v), CORETYPE_U8);
                        }
                    }
                    else {
                        assert(is_byte);
                        this->ungetc();
                        return this->getTokenInt_Identifier('b');
                    }
                }
            }
            // Symbols
            else if( issym(ch) )
            {
                return this->getTokenInt_Identifier(ch);
            }
            else
            {
                throw ParseError::BadChar(*this, ch.v);
            }
        }
        else if( sym > 0 )
        {
            // If the symbol is `TOK_DOT`, check if the next character is a digit and consume an integer
            if(sym == TOK_DOT) {
                auto ch = this->getc();
                this->ungetc();
                if( ch.isdigit() ) {
                    auto val = this->parseInt(nullptr);
                    m_next_tokens.push_back(Token(val, CORETYPE_ANY));
                }
                else {
                }
            }
            return Token((enum eTokenType)sym);
        }
        else
        {
            switch(sym)
            {
            case LINECOMMENT: {
                // Line comment
                ::std::string   str;
                auto ch = this->getc();
                bool is_doc = false;
                bool is_pdoc = false;
                if( ch == '/' ) {
                    ch = this->getc();
                    if( ch == '/' )
                        str += "/";
                    else
                        is_doc = true;
                }
                else if( ch == '!' ) {
                    is_pdoc = true;
                    ch = this->getc();
                }
                while(ch != '\n' && ch != '\r')
                {
                    str += ch;
                    ch = this->getc();
                }
                this->ungetc();
                if( is_doc || is_pdoc )
                {
                    //# [ doc = "commment data" ]
                    m_next_tokens.push_back(TOK_SQUARE_CLOSE);
                    m_next_tokens.push_back(Token(TOK_STRING, mv$(str)));
                    m_next_tokens.push_back(TOK_EQUAL);
                    m_next_tokens.push_back(Token(TOK_IDENT, RcString::new_interned("doc")));
                    m_next_tokens.push_back(TOK_SQUARE_OPEN);
                    if(is_pdoc)
                        m_next_tokens.push_back(TOK_EXCLAM);
                    return TOK_HASH;
                }
                return Token(TOK_COMMENT, str); }
            case BLOCKCOMMENT: {
                ::std::string   str;
                bool is_doc = false;
                bool is_pdoc = false;
                ch = this->getc();
                if( ch == '*' ) {
                    ch = this->getc();
                    if( ch == '*' )
                    {
                        str += "*";
                    }
                    else
                        is_doc = true;
                }
                else if( ch == '!' ) {
                    is_pdoc = true;
                    ch = this->getc();
                }
                unsigned int level = 0;
                while(true)
                {
                    if( ch == '/' ) {
                        str += ch;
                        ch = this->getc();
                        if( ch == '*' ) {
                            level ++;
                        }
                        str += ch;
                    }
                    else {
                        if( ch == '*' ) {
                            ch = this->getc();
                            if( ch == '/' ) {
                                if( level == 0 )
                                    break;
                                level --;
                                str.push_back('*');
                                str.push_back('/');
                            }
                            else {
                                str.push_back('*');
                                str += ch;
                            }
                        }
                        else {
                            str += ch;
                        }
                    }
                    ch = this->getc();
                }
                if( is_doc || is_pdoc )
                {
                    //# [ doc = "commment data" ]
                    m_next_tokens.push_back(TOK_SQUARE_CLOSE);
                    m_next_tokens.push_back(Token(TOK_STRING, mv$(str)));
                    m_next_tokens.push_back(TOK_EQUAL);
                    m_next_tokens.push_back(Token(TOK_IDENT, RcString::new_interned("doc")));
                    m_next_tokens.push_back(TOK_SQUARE_OPEN);
                    if(is_pdoc)
                        m_next_tokens.push_back(TOK_EXCLAM);
                    return TOK_HASH;
                }
                return Token(TOK_COMMENT, str); }
            case SINGLEQUOTE: {
                auto firstchar = this->getc();
                if( firstchar.v == '\\' ) {
                    // Character constant with an escape code
                    uint32_t val = this->parseEscape('\'');
                    if(this->getc() != '\'') {
                        throw ParseError::Todo("Proper error for lex failures");
                    }
                    return Token( U128(val), CORETYPE_CHAR);
                }
                else {
                    ch = this->getc();
                    if( ch == '\'' ) {
                        // Character constant
                        return Token( U128(firstchar.v), CORETYPE_CHAR);
                    }
                    else if( issym(firstchar.v) ) {
                        // Lifetime name
                        ::std::string   str;
                        str += firstchar;
                        while( issym(ch) )
                        {
                            str += ch;
                            ch = this->getc();
                        }
                        this->ungetc();
                        return Token(TOK_LIFETIME, Ident(this->get_hygiene(), RcString::new_interned(str)));
                    }
                    else {
                        throw ParseError::Todo("Lex Fail - Expected ' after character constant");
                    }
                }
                break; }
            case DOUBLEQUOTE: {
                ::std::string str;
                while( (ch = this->getc()) != '"' )
                {
                    if( ch == '\\' )
                    {
                        auto v = this->parseEscape('"');
                        if( v != ~0u )
                        {
                            str += Codepoint(v);
                        }
                    }
                    else
                    {
                        str += ch;
                    }
                }
                return Token(TOK_STRING, mv$(str));
                }
            default:
                assert(!"bugcheck");
            }
        }
    }
    catch(const Lexer::EndOfFile& /*e*/)
    {
        return Token(TOK_EOF);
    }
    throw "Fell off the end of getTokenInt";
}

Token Lexer::getTokenInt_RawString(bool is_byte)
{
    // Raw string (possibly byte)
    Codepoint ch = this->getc();
    unsigned int hashes = 0;
    while(ch == '#')
    {
        hashes ++;
        ch = this->getc();
    }
    if( ch != '"' ) {
        // 'b' or 'br' identifier
        if( hashes == 0 ) {
            this->ungetc(); // Unget the not '"'
            if( is_byte )
                return this->getTokenInt_Identifier('b', 'r');
            else
                return this->getTokenInt_Identifier('r');
        }
        // Raw identifier
        else if( hashes == 1 ) {
            return this->getTokenInt_Identifier(ch, Codepoint(), /*parse_reserved_word*/false);
        }
        else {
            throw ParseError::Generic(*this, "Expected '\"' after hashes following `r`");
        }
    }
    auto terminator = ch;
    ::std::string   val;
    DEBUG("terminator = '" << terminator << "', hashes = " << hashes);

    unsigned terminating_hashes = 0;
    for(;;)
    {
        try {
            ch = this->getc();
        }
        catch( const Lexer::EndOfFile& /*e*/ ) {
            throw ParseError::Generic(*this, "EOF reached in raw string");
        }

        if( terminating_hashes > 0 )
        {
            assert(terminating_hashes > 0);
            if( ch != '#' ) {
                val += terminator;
                while( terminating_hashes < hashes ) {
                    val += '#';
                    terminating_hashes += 1;
                }
                terminating_hashes = 0;

                this->ungetc();
            }
            else {
                terminating_hashes -= 1;
                if( terminating_hashes == 0 ) {
                    break;
                }
            }
        }
        else
        {
            if( ch == terminator ) {
                if( hashes == 0 ) {
                    break;
                }
                terminating_hashes = hashes;
            }
            else {
                val += ch;
            }
        }
    }
    return Token(is_byte ? TOK_BYTESTRING : TOK_STRING, mv$(val));
}
Token Lexer::getTokenInt_Identifier(Codepoint leader, Codepoint leader2, bool parse_reserved_word)
{
    ::std::string   str;
    if( leader2 != '\0' )
        str += leader;
    auto ch = leader2 == '\0' ? leader : leader2;
    while( issym(ch) )
    {
        str += ch;
        ch = this->getc();
    }

    this->ungetc();
    if(parse_reserved_word)
    {
        auto v = Lex_FindReservedWord(str, this->m_edition);
        if( v != TOK_NULL)
        {
            return Token(v);
        }
    }
    return Token(TOK_IDENT, Ident(this->realGetHygiene(), RcString::new_interned(str)));
}

/// Parse an integer from the input stream
U128 Lexer::parseInt(NumMode* num_mode_out)
{
    auto num_mode = NumMode::DEC;

    U128    val(0);
    auto ch = this->getc();
    // Leading zero could be part of a base suffix
    // - `0x` Hex
    // - `0o` Octal
    // - `0b` Binary
    // - Any other character: decimal
    if( ch == '0' ) {
        ch = this->getc_num();
        if( ch == 'x' ) {
            num_mode = NumMode::HEX;
            while( (ch = this->getc_num()).isxdigit() )
            {
                val *= 16;
                if(ch.v <= '9')
                    val += U128(ch.v - '0');
                else if( ch.v <= 'F' )
                    val += U128(ch.v - 'A' + 10);
                else if( ch.v <= 'f' )
                    val += U128(ch.v - 'a' + 10);
            }
        }
        else if( ch == 'b' ) {
            num_mode = NumMode::BIN;
            while( (ch = this->getc_num()).isdigit() )
            {
                val *= 2;
                if(ch.v == '0')
                    val += 0;
                else if( ch.v == '1' )
                    val += 1;
                else
                    throw ParseError::Generic("Invalid digit in binary literal");
            }
        }
        else if( ch == 'o' ) {
            num_mode = NumMode::OCT;
            while( (ch = this->getc_num()).isdigit() ) {
                val *= 8;
                if('0' <= ch.v && ch.v <= '7')
                    val += U128(ch.v - '0');
                else
                    throw ParseError::Generic("Invalid digit in octal literal");
            }
        }
        else {
            num_mode = NumMode::DEC;
            while( ch.isdigit() ) {
                val *= 10;
                val += U128(ch.v - '0');
                ch = this->getc_num();
            }
        }
    }
    else {
        num_mode = NumMode::DEC;
        while( ch.isdigit() ) {
            val *= 10;
            val += U128(ch.v - '0');
            ch = this->getc_num();
        }
    }

    this->ungetc();
    if( num_mode_out )
        *num_mode_out = num_mode;
    return val;
}

// Takes the VERY lazy way of reading the float into a string then passing to strtod
double Lexer::parseFloat(U128 whole)
{
    const int MAX_LEN = 63;
    const int MAX_SIG = MAX_LEN - 1 - 4;
    std::string  sbuf = FMT(whole << ".");
    //char buf[MAX_LEN+1];
    //int ofs = snprintf(buf, MAX_LEN+1, "%llu.", (unsigned long long)whole);

    auto ch = this->getc_num();
    #define PUTC(ch)    do { assert(ch.v < 127); sbuf += char(ch.v); /* if( ofs < MAX_SIG ) { buf[ofs] = ch.v; ofs ++; } else { throw ParseError::Generic("Oversized float"); } */ } while(0)
    while( ch.isdigit() )
    {
        PUTC(ch);
        ch = this->getc_num();
    }
    // If the current char is a `.`
    if( ch == '.' )
    {
        // TODO: `0.0..` should be a range, so if the next character is `.`, then unget and continue
        ch = this->getc();
        if( ch == '.' ) {
            // Double-dot, need to unget twice
            // OR: Explicitly handle the symbols
            switch(this->getc().v)
            {
            case '.':
                m_next_tokens.push_back(TOK_TRIPLE_DOT);
                break;
            case '=':
                m_next_tokens.push_back(TOK_DOUBLE_DOT_EQUAL);
                break;
            default:
                this->ungetc();
                m_next_tokens.push_back(TOK_DOUBLE_DOT);
                break;
            }
            //buf[ofs] = 0;
            //m_next_tokens.push_back(Token::make_float(::std::strtod(buf, NULL), CORETYPE_ANY));
            m_next_tokens.push_back(Token::make_float(::std::strtod(sbuf.c_str(), NULL), CORETYPE_ANY));

            return std::numeric_limits<double>::quiet_NaN();
        }
        else {
            this->ungetc();

            //buf[ofs] = '\0';
            //assert( buf[ofs-1] != '.' );    // Shouldn't be possible (as that would have been handled by the caller as `<int> '..'`
            DEBUG("Detected double tuple indexing (trailing `.` after a float - " << sbuf << ")");
            // x.y. -> This should be two integers.
            // - Parse into `<int> '.' <int>` (ungetting the final `.`)
            const char* buf = sbuf.data();
            auto cit = std::find(buf, buf+sbuf.size(), '.');
            sbuf[cit - sbuf.data()] = '\0';
            //auto cit = std::find(buf, buf+sizeof(buf), '.');
            //*cit = '\0';
            // - Push these in reverse order (as they're popped off the back)
            m_next_tokens.push_back(TOK_DOT);
            m_next_tokens.push_back(Token(U128(std::strtoull(cit+1, nullptr, 10)), CORETYPE_ANY));
            m_next_tokens.push_back(TOK_DOT);
            m_next_tokens.push_back(Token(U128(std::strtoull(buf  , nullptr, 10)), CORETYPE_ANY));

            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    else
    {
        if( ch == 'e' || ch == 'E' )
        {
            PUTC(ch);
            ch = this->getc_num();
            if( ch == '-' || ch == '+' ) {
                PUTC(ch);
                ch = this->getc_num();
            }
            if( !ch.isdigit() )
                throw ParseError::Generic( FMT("Non-numeric '"<<ch<<"' in float exponent") );
            do {
                PUTC(ch);
                ch = this->getc_num();
            } while( ch.isdigit() );
        }
        this->ungetc();
        //buf[ofs] = 0;
        //DEBUG("buf = " << buf << ", ch = '" << ch << "'");
        DEBUG("buf = " << sbuf << ", ch = '" << ch << "'");

        //return ::std::strtod(buf, NULL);
        return ::std::strtod(sbuf.c_str(), NULL);
    }
}

uint32_t Lexer::parseEscape(char enclosing)
{
    auto ch = this->getc();
    switch(ch.v)
    {
    case 'x': {
        ch = this->getc();
        if( !ch.isxdigit() )
            throw ParseError::Generic(*this, FMT("Found invalid character '\\x" << ::std::hex << ch.v << "' in \\u sequence" ) );
        char tmp[3] = {static_cast<char>(ch.v), 0, 0};
        ch = this->getc();
        if( !ch.isxdigit() )
            throw ParseError::Generic(*this, FMT("Found invalid character '\\x" << ::std::hex << ch.v << "' in \\u sequence" ) );
        tmp[1] = static_cast<char>(ch.v);
        return ::std::strtol(tmp, NULL, 16);
        } break;
    case 'u': {
        // Unicode (up to six hex digits)
        uint32_t    val = 0;
        ch = this->getc();
        bool req_close_brace = false;
        if( ch == '{' ) {
            req_close_brace = true;
            ch = this->getc();
        }
        if( !ch.isxdigit() )
            throw ParseError::Generic(*this, FMT("Found invalid character '\\x" << ::std::hex << ch.v << "' in \\u sequence" ) );
        while( ch.isxdigit() )
        {
            char    tmp[2] = {static_cast<char>(ch.v), 0};
            val *= 16;
            val += ::std::strtol(tmp, NULL, 16);
            ch = this->getc();
        }
        if( !req_close_brace )
            this->ungetc();
        else if( ch != '}' )
            throw ParseError::Generic(*this, "Expected terminating } in \\u sequence");
        else {
        }
        return val; }
    case '0':
        return '\0';
    case '\\':
        return '\\';
    case '\'':
        return '\'';
    case '"':
        return '"';
    case 'r':
        return '\r';
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case '\r':
    case '\n':
        while( ch.isspace() )
            ch = this->getc();
        if(ch == '\\' )
            return parseEscape(enclosing);
        else if( ch == enclosing )
        {
            this->ungetc();
            return ~0;
        }
        else
            return ch.v;
    default:
        throw ParseError::Todo( FMT("Unknown escape sequence \\" << ch) );
    }
}

char Lexer::getc_byte()
{
    int rv = m_istream.get();
    if( rv == EOF )
        throw Lexer::EndOfFile();

    if( rv == '\r' )
    {
        if( m_istream.get() != '\n' )
        {
            m_istream.unget();
        }
        else
        {
            rv = '\n';
        }
    }
    if( rv == '\n' )
    {
        m_line ++;
        m_line_ofs = 0;
    }

    return rv;
}
Codepoint Lexer::getc()
{
    if( m_last_char_valid )
    {
        m_last_char_valid = false;
#ifdef TRACE_CHARS
        ::std::cout << "getc(): U+" << ::std::hex << m_last_char.v << " (cached)" << ::std::endl;
#endif
    }
    else
    {
        m_last_char = this->getc_cp();
        m_line_ofs += 1;
#ifdef TRACE_CHARS
        ::std::cout << "getc(): U+" << ::std::hex << m_last_char.v << ::std::endl;
#endif
    }
    return m_last_char;
}

Codepoint Lexer::getc_num()
{
    Codepoint ch;
    do {
        ch = this->getc();
    } while( ch == '_' );
    return ch;
}
Codepoint Lexer::getc_cp()
{
    uint8_t v1 = this->getc_byte();
    if( v1 < 128 ) {
        return {v1};
    }
    else if( (v1 & 0xC0) == 0x80 ) {
        // Invalid (continuation)
        return {0xFFFE};
    }
    else if( (v1 & 0xE0) == 0xC0 ) {
        // Two bytes
        uint8_t e1 = this->getc_byte();
        if( (e1 & 0xC0) != 0x80 )  return {0xFFFE};

        uint32_t outval
            = ((v1 & 0x1F) << 6)
            | ((e1 & 0x3F) <<0)
            ;
        return {outval};
    }
    else if( (v1 & 0xF0) == 0xE0 ) {
        // Three bytes
        uint8_t e1 = this->getc_byte();
        if( (e1 & 0xC0) != 0x80 )  return {0xFFFE};
        uint8_t e2 = this->getc_byte();
        if( (e2 & 0xC0) != 0x80 )  return {0xFFFE};

        uint32_t outval
            = ((v1 & 0x0F) << 12)
            | ((e1 & 0x3F) <<  6)
            | ((e2 & 0x3F) <<  0)
            ;
        return {outval};
    }
    else if( (v1 & 0xF8) == 0xF0 ) {
        // Four bytes
        uint8_t e1 = this->getc_byte();
        if( (e1 & 0xC0) != 0x80 )  return {0xFFFE};
        uint8_t e2 = this->getc_byte();
        if( (e2 & 0xC0) != 0x80 )  return {0xFFFE};
        uint8_t e3 = this->getc_byte();
        if( (e3 & 0xC0) != 0x80 )  return {0xFFFE};

        uint32_t outval
            = ((v1 & 0x07) << 18)
            | ((e1 & 0x3F) << 12)
            | ((e2 & 0x3F) <<  6)
            | ((e3 & 0x3F) <<  0)
            ;
        return {outval};
    }
    else {
        throw ParseError::Generic("Invalid UTF-8 (too long)");
    }
}

void Lexer::ungetc()
{
#ifdef TRACE_CHARS
    ::std::cout << "ungetc(): cache U+" << ::std::hex << m_last_char.v << ::std::endl;
#endif
    assert(!m_last_char_valid);
    m_last_char_valid = true;
}

// --------------------------------------------------------------------
// Codepoint - Unicode codepoint.
// --------------------------------------------------------------------

bool Codepoint::isspace() const {
    switch(this->v)
    {
    case '\t':
    case '\r':
    case '\n':
    case ' ':
    case 0xC:   // ^L
    case 0x85:
    case 0x200E: case 0x200F: // LTR / RTL markers
    case 0x2028:    // Line Separator
    case 0x2029:    // Paragrah Separator
        return true;
    default:
        return false;
    }
}
bool Codepoint::isdigit() const {
    return this->v < 128 && std::isdigit(static_cast<int>(this->v));
}
bool Codepoint::isxdigit() const {
    return this->v < 128 && std::isxdigit(static_cast<int>(this->v));
}

::std::string& operator+=(::std::string& s, const Codepoint& cp)
{
    if( cp.v < 0x80 ) {
        s += (char)cp.v;
    }
    else if( cp.v < (0x1F+1)<<(1*6) ) {

        s += (char)(0xC0 | ((cp.v >> 6) & 0x1F));
        s += (char)(0x80 | ((cp.v >> 0) & 0x3F));
    }
    else if( cp.v < (0x0F+1)<<(2*6) ) {
        s += (char)(0xE0 | ((cp.v >> 12) & 0x0F));
        s += (char)(0x80 | ((cp.v >>  6) & 0x3F));
        s += (char)(0x80 | ((cp.v >>  0) & 0x3F));
    }
    else if( cp.v < (0x07+1)<<(3*6) ) {
        s += (char)(0xF0 | ((cp.v >> 18) & 0x07));
        s += (char)(0x80 | ((cp.v >> 12) & 0x3F));
        s += (char)(0x80 | ((cp.v >>  6) & 0x3F));
        s += (char)(0x80 | ((cp.v >>  0) & 0x3F));
    }
    else {
        throw ::std::runtime_error( FMT("BUGCHECK: Bad unicode codepoint encountered - " << ::std::hex << cp.v) );
    }
    return s;
}
::std::ostream& operator<<(::std::ostream& os, const Codepoint& cp)
{
    if( cp.v < 0x80 ) {
        os << (char)cp.v;
    }
    else if( cp.v < (0x1F+1)<<(1*6) ) {
        os << (char)(0xC0 | ((cp.v >> 6) & 0x1F));
        os << (char)(0x80 | ((cp.v >> 0) & 0x3F));
    }
    else if( cp.v < (0x0F+1)<<(2*6) ) {
        os << (char)(0xE0 | ((cp.v >> 12) & 0x0F));
        os << (char)(0x80 | ((cp.v >>  6) & 0x3F));
        os << (char)(0x80 | ((cp.v >>  0) & 0x3F));
    }
    else if( cp.v < (0x07+1)<<(2*6) ) {
        os << (char)(0xF0 | ((cp.v >> 18) & 0x07));
        os << (char)(0x80 | ((cp.v >> 12) & 0x3F));
        os << (char)(0x80 | ((cp.v >>  6) & 0x3F));
        os << (char)(0x80 | ((cp.v >>  0) & 0x3F));
    }
    else {
        throw ::std::runtime_error("BUGCHECK: Bad unicode codepoint encountered");
    }
    return os;
}

Token Lex_FindOperator(const ::std::string& s)
{
    if( s == "_" )
        return TOK_UNDERSCORE;
    for(size_t i = 0; i < LEN(TOKENMAP); i++)
    {
        const auto& e = TOKENMAP[i];
        if( s < e.chars )
            break;
        if( s == e.chars )
        {
            if( e.type < 0 )
                break ;
            return static_cast<eTokenType>(e.type);
        }
    }
    return TOK_NULL;
}
Token Lex_FindReservedWord(const ::std::string& s, AST::Edition edition)
{
    size_t len = 0;
    const sRWORD* RWORDS = nullptr;
    switch(edition)
    {
    case AST::Edition::Rust2015:
        len = LEN(RWORDS_2015);
        RWORDS = RWORDS_2015;
        break;
    case AST::Edition::Rust2018:
    case AST::Edition::Rust2021:
        len = LEN(RWORDS_2018);
        RWORDS = RWORDS_2018;
        break;
    }
    assert(len > 0);
    for(size_t i = 0; i < len; i++)
    {
        const auto& e = RWORDS[i];
        if( s < e.chars )
            break;
        if( s == e.chars )
        {
            assert(e.type > 0);
            return static_cast<eTokenType>(e.type);
        }
    }
    return TOK_NULL;
}
