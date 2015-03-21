/*
 * "MRustC" - Primitive rust compiler in C++
 */
/**
 * \file parse/lex.cpp
 * \brief Low-level lexer
 */
#include "lex.hpp"
#include "tokentree.hpp"
#include "parseerror.hpp"
#include <cassert>
#include <iostream>
#include <cstdlib>  // strtol
#include <typeinfo>
#include <algorithm>	// std::count

Lexer::Lexer(::std::string filename):
    m_path(filename),
    m_line(1),
    m_istream(filename.c_str()),
    m_last_char_valid(false)
{
    if( !m_istream.is_open() )
    {
        throw ::std::runtime_error("Unable to open file");
    }
}

#define LINECOMMENT -1
#define BLOCKCOMMENT -2
#define SINGLEQUOTE -3
#define DOUBLEQUOTE -4

// NOTE: This array must be kept reverse sorted
#define TOKENT(str, sym)    {sizeof(str)-1, str, sym}
static const struct {
    unsigned char len;
    const char* chars;
    signed int type;
} TOKENMAP[] = {
  TOKENT("!" , TOK_EXCLAM),
  TOKENT("!=", TOK_EXCLAM_EQUAL),
  TOKENT("\"", DOUBLEQUOTE),
  TOKENT("#",  0),
  TOKENT("#![",TOK_CATTR_OPEN),
  TOKENT("#[", TOK_ATTR_OPEN),
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
  TOKENT("..", TOK_DOUBLE_DOT),
  TOKENT("...",TOK_TRIPLE_DOT),
  TOKENT("/" , TOK_SLASH),
  TOKENT("/*", BLOCKCOMMENT),
  TOKENT("//", LINECOMMENT),
  TOKENT("/=", TOK_SLASH_EQUAL),
  // 0-9 :: Elsewhere
  TOKENT(":",  TOK_COLON),
  TOKENT("::", TOK_DOUBLE_COLON),
  TOKENT(";",  TOK_SEMICOLON),
  TOKENT("<",  TOK_LT),
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

  TOKENT("{",  TOK_BRACE_OPEN),
  TOKENT("|",  TOK_PIPE),
  TOKENT("|=", TOK_PIPE_EQUAL),
  TOKENT("||", TOK_DOUBLE_PIPE),
  TOKENT("}",  TOK_BRACE_CLOSE),
  TOKENT("~",  TOK_TILDE),
};
#define LEN(arr)    (sizeof(arr)/sizeof(arr[0]))
static const struct {
    unsigned char len;
    const char* chars;
    signed int type;
} RWORDS[] = {
  TOKENT("_", TOK_UNDERSCORE),
  TOKENT("abstract",TOK_RWORD_ABSTRACT),
  TOKENT("alignof", TOK_RWORD_ALIGNOF),
  TOKENT("as",      TOK_RWORD_AS),
  TOKENT("be",      TOK_RWORD_BE),
  TOKENT("box",     TOK_RWORD_BOX),
  TOKENT("break",   TOK_RWORD_BREAK),
  TOKENT("const",   TOK_RWORD_CONST),
  TOKENT("continue",TOK_RWORD_CONTINUE),
  TOKENT("crate",   TOK_RWORD_CRATE),
  TOKENT("do",      TOK_RWORD_DO),
  TOKENT("else",    TOK_RWORD_ELSE),
  TOKENT("enum",    TOK_RWORD_ENUM),
  TOKENT("extern",  TOK_RWORD_EXTERN),
  TOKENT("false",   TOK_RWORD_FALSE),
  TOKENT("final",   TOK_RWORD_FINAL),
  TOKENT("fn",      TOK_RWORD_FN),
  TOKENT("for",     TOK_RWORD_FOR),
  TOKENT("if",      TOK_RWORD_IF),
  TOKENT("impl",    TOK_RWORD_IMPL),
  TOKENT("in",      TOK_RWORD_IN),
  TOKENT("let",     TOK_RWORD_LET),
  TOKENT("loop",    TOK_RWORD_LOOP),
  TOKENT("match",   TOK_RWORD_MATCH),
  TOKENT("mod",     TOK_RWORD_MOD),
  TOKENT("move",    TOK_RWORD_MOVE),
  TOKENT("mut",     TOK_RWORD_MUT),
  TOKENT("offsetof",TOK_RWORD_OFFSETOF),
  TOKENT("once",    TOK_RWORD_ONCE),
  TOKENT("override",TOK_RWORD_OVERRIDE),
  TOKENT("priv",    TOK_RWORD_PRIV),
  TOKENT("proc",    TOK_RWORD_PROC),
  TOKENT("pub",     TOK_RWORD_PUB),
  TOKENT("pure",    TOK_RWORD_PURE),
  TOKENT("ref",     TOK_RWORD_REF),
  TOKENT("return",  TOK_RWORD_RETURN),
  TOKENT("self",    TOK_RWORD_SELF),
  TOKENT("sizeof",  TOK_RWORD_SIZEOF),
  TOKENT("static",  TOK_RWORD_STATIC),
  TOKENT("struct",  TOK_RWORD_STRUCT),
  TOKENT("super",   TOK_RWORD_SUPER),
  TOKENT("trait",   TOK_RWORD_TRAIT),
  TOKENT("true",    TOK_RWORD_TRUE),
  TOKENT("type",    TOK_RWORD_TYPE),
  TOKENT("typeof",  TOK_RWORD_TYPEOF),
  TOKENT("unsafe",  TOK_RWORD_UNSAFE),
  TOKENT("unsized", TOK_RWORD_UNSIZED),
  TOKENT("use",     TOK_RWORD_USE),
  TOKENT("virtual", TOK_RWORD_VIRTUAL),
  TOKENT("where",   TOK_RWORD_WHERE),
  TOKENT("while",   TOK_RWORD_WHILE),
  TOKENT("yield",   TOK_RWORD_YIELD),
};

signed int Lexer::getSymbol()
{
    char ch = this->getc();
    // 1. lsearch for character
    // 2. Consume as many characters as currently match
    // 3. IF: a smaller character or, EOS is hit - Return current best
    unsigned ofs = 0;
    signed int best = 0;
    for(unsigned i = 0; i < LEN(TOKENMAP); i ++)
    {
        const char* const chars = TOKENMAP[i].chars;
        const size_t len = TOKENMAP[i].len;

        //::std::cout << "ofs=" << ofs << ", chars[ofs] = " << chars[ofs] << ", ch = " << ch << ", len = " << len << ::std::endl;

        if( ofs >= len || chars[ofs] > ch ) {
            this->ungetc();
            return best;
        }

        while( chars[ofs] && chars[ofs] == ch )
        {
            ch = this->getc();
            ofs ++;
        }
        if( chars[ofs] == 0 )
        {
            best = TOKENMAP[i].type;
        }
    }

    this->ungetc();
    return best;
}

bool issym(char ch)
{
    if( ::std::isalnum(ch) )
        return true;
    if( ch == '_' )
        return true;
    return false;
}

Position Lexer::getPosition() const
{
    return Position(m_path, m_line);
}
Token Lexer::realGetToken()
{
    while(true)
    {
        Token tok = getTokenInt();
        //::std::cout << "getTokenInt: tok = " << tok << ::std::endl;
        switch(tok.type())
        {
        case TOK_NEWLINE:
            m_line ++;
            //DEBUG("m_line = " << m_line << " (NL)");
            continue;
        case TOK_WHITESPACE:
            continue;
        case TOK_COMMENT: {
            ::std::string comment = tok.str();
            unsigned int c = ::std::count(comment.begin(), comment.end(), '\n');
            m_line += c;
            //DEBUG("m_line = " << m_line << " (comment w/ "<<c<<")");
            continue; }
        default:
            return tok;
        }
    }
}

Token Lexer::getTokenInt()
{
    if( this->m_next_token.type() != TOK_NULL )
    {
        return ::std::move(this->m_next_token);
    }
    try
    {
        char ch = this->getc();

        if( ch == '\n' )
            return Token(TOK_NEWLINE);
        if( isspace(ch) )
        {
            while( isspace(ch = this->getc()) && ch != '\n' )
                ;
            this->ungetc();
            return Token(TOK_WHITESPACE);
        }
        this->ungetc();

        const signed int sym = this->getSymbol();
        if( sym == 0 )
        {
            // No match at all, check for symbol
            char ch = this->getc();
            if( isdigit(ch) )
            {
                enum eCoreType  num_type = CORETYPE_ANY;
                enum {
                    BIN,
                    OCT,
                    DEC,
                    HEX,
                } num_mode = DEC;
                // TODO: handle integers/floats
                uint64_t    val = 0;
                if( ch == '0' ) {
                    // Octal/hex handling
                    ch = this->getc();
                    if( ch == 'x' ) {
                        num_mode = HEX;
                        while( isxdigit(ch = this->getc_num()) )
                        {
                            val *= 16;
                            if(ch <= '9')
                                val += ch - '0';
                            else if( ch <= 'F' )
                                val += ch - 'A' + 10;
                            else if( ch <= 'f' )
                                val += ch - 'a' + 10;
                        }
                    }
                    else if( ch == 'b' ) {
                        num_mode = BIN;
                        while( isdigit(ch = this->getc_num()) )
                        {
                            val *= 2;
                            if(ch == '0')
                                val += 0;
                            else if( ch == '1' )
                                val += 1;
                            else
                                throw ParseError::Generic("Invalid digit in binary literal");
                        }
                    }
                    else if( isdigit(ch) ) {
                        num_mode = OCT;
                        throw ParseError::Todo("Lex octal numbers");
                    }
                    else {
                        num_mode = DEC;
                        val = 0;
                    }
                }
                else {
                    while( isdigit(ch) ) {
                        val *= 10;
                        val += ch - '0';
                        ch = this->getc();
                    }
                }

                if(ch == 'u' || ch == 'i') {
                    // Unsigned
                    throw ParseError::Todo("Lex number suffixes");
                }
                else if( ch == '.' ) {
                    if( num_mode != DEC )
                        throw ParseError::Todo("Non-decimal floats");
                    
                    ch = this->getc();
                    
                    // Double/Triple Dot
                    if( ch == '.' ) {
                        if( this->getc() == '.') {
                            this->m_next_token = Token(TOK_TRIPLE_DOT);
                        }
                        else {
                            this->ungetc();
                            this->m_next_token = Token(TOK_DOUBLE_DOT);
                        }
                        return Token(val, CORETYPE_ANY);
                    }
                    // Single dot
                    else if( !isdigit(ch) )
                    {
                        this->m_next_token = Token(TOK_DOT);
                        return Token(val, CORETYPE_ANY);
                    }
                    
                    this->ungetc();
                    double fval = this->parseFloat(val);
                    if( (ch = this->getc()) == 'f' )
                    {
                        ::std::string   suffix;
                        while( issym(ch) )
                        {
                            suffix.push_back(ch);
                            ch = this->getc();
                        }
                        this->ungetc();
                        if( suffix == "f32" ) {
                            num_type = CORETYPE_F32;
                        }
                        else if( suffix == "f64" ) {
                            num_type = CORETYPE_F64;
                        }
                        else {
                            throw ParseError::Generic( FMT("Unknown number suffix " << suffix) );
                        }
                    }
                    else
                    {
                        this->ungetc();
                    }
                    return Token( fval, num_type);

                }
                else {
                    this->ungetc();
                    return Token(val, num_type);
                }
            }
            else if( issym(ch) )
            {
                ::std::string   str;
                while( issym(ch) )
                {
                    str.push_back(ch);
                    ch = this->getc();
                }

                if( ch == '!' )
                {
                    return Token(TOK_MACRO, str);
                }
                else
                {
                    if( str == "b" && ch == '\'' ) {
                        // Byte constant
                        ch = this->getc();
                        if( ch == '\\' ) {
                            uint32_t val = this->parseEscape('\'');
                            if( this->getc() != '\'' )
                                throw ParseError::Generic(*this, "Multi-byte character literal");
                            return Token((uint64_t)val, CORETYPE_U8);
                        }
                        else {
                            if( this->getc() != '\'' )
                                throw ParseError::Generic(*this, "Multi-byte character literal");
                            return Token((uint64_t)ch, CORETYPE_U8);
                        }
                    }
                
                    this->ungetc();
                    for( unsigned int i = 0; i < LEN(RWORDS); i ++ )
                    {
                        if( str < RWORDS[i].chars ) break;
                        if( str == RWORDS[i].chars )    return Token((enum eTokenType)RWORDS[i].type);
                    }
                    return Token(TOK_IDENT, str);
                }
            }
            else
            {
                throw ParseError::BadChar(ch);
            }
        }
        else if( sym > 0 )
        {
            return Token((enum eTokenType)sym);
        }
        else
        {
            switch(sym)
            {
            case LINECOMMENT: {
                // Line comment
                ::std::string   str;
                char ch = this->getc();
                while(ch != '\n' && ch != '\r')
                {
                    str.push_back(ch);
                    ch = this->getc();
                }
                this->ungetc();
                return Token(TOK_COMMENT, str); }
            case BLOCKCOMMENT: {
                ::std::string   str;
                while(true)
                {
                    if( ch == '*' ) {
                        ch = this->getc();
                        if( ch == '/' ) break;
                        this->ungetc();
                    }
                    str.push_back(ch);
                    ch = this->getc();
                }
                return Token(TOK_COMMENT, str); }
            case SINGLEQUOTE: {
                char firstchar = this->getc();
                if( firstchar != '\\' ) {
                    ch = this->getc();
                    if( ch == '\'' ) {
                        // Character constant
                        return Token((uint64_t)firstchar, CORETYPE_CHAR);
                    }
                    else {
                        // Lifetime name
                        ::std::string   str;
                        str.push_back(firstchar);
                        while( issym(ch) )
                        {
                            str.push_back(ch);
                            ch = this->getc();
                        }
                        this->ungetc();
                        return Token(TOK_LIFETIME, str);
                    }
                }
                else {
                    // Character constant with an escape code
                    uint32_t val = this->parseEscape('\'');
                    if(this->getc() != '\'') {
                        throw ParseError::Todo("Proper error for lex failures");
                    }
                    return Token((uint64_t)val, CORETYPE_CHAR);
                }
                break; }
            case DOUBLEQUOTE: {
                ::std::string str;
                while( (ch = this->getc()) != '"' )
                {
                    if( ch == '\\' )
                        ch = this->parseEscape('"');
                    str.push_back(ch);
                }
                return Token(TOK_STRING, str);
                }
            default:
                assert(!"bugcheck");
            }
        }
    }
    catch(const Lexer::EndOfFile& e)
    {
        return Token(TOK_EOF);
    }
    //assert(!"bugcheck");
}

// Takes the VERY lazy way of reading the float into a string then passing to strtod
double Lexer::parseFloat(uint64_t whole)
{
    const int MAX_LEN = 63;
    const int MAX_SIG = MAX_LEN - 1 - 4;
    char buf[MAX_LEN+1];
    int ofs = snprintf(buf, MAX_LEN+1, "%llu", (unsigned long long)whole);

    char ch = this->getc_num();
    #define PUTC(ch)    do { if( ofs < MAX_SIG ) { buf[ofs] = ch; ofs ++; } else { throw ParseError::Generic("Oversized float"); } } while(0)
    while( isdigit(ch) )
    {
        PUTC(ch);
        ch = this->getc_num();
    }
    if( ch == 'e' || ch == 'E' )
    {
        PUTC(ch);
        ch = this->getc_num();
        if( ch == '-' || ch == '+' ) {
            PUTC(ch);
            ch = this->getc_num();
        }
        if( !isdigit(ch) )
            throw ParseError::Generic( FMT("Non-numeric '"<<ch<<"' in float exponent") );
        do {
            PUTC(ch);
            ch = this->getc_num();
        } while( isdigit(ch) );
    }
    this->ungetc();
    buf[ofs] = 0;
    
    return ::std::strtod(buf, NULL);
}

uint32_t Lexer::parseEscape(char enclosing)
{
    char ch = this->getc();
    switch(ch)
    {
    case 'x':
    case 'u': {
        // Unicode (up to six hex digits)
        uint32_t    val = 0;
        ch = this->getc();
        bool req_close_brace = false;
        if( ch == '{' ) {
            req_close_brace = true;
            ch = this->getc();
        }
        if( !isxdigit(ch) )
            throw ParseError::Generic(*this, FMT("Found invalid character '\\x" << ::std::hex << (int)ch << "' in \\u sequence" ) );
        while( isxdigit(ch) )
        {
            char    tmp[2] = {ch, 0};
            val *= 16;
            val += ::std::strtol(tmp, NULL, 16);
            ch = this->getc();
        }
        if( !req_close_brace )
                this->ungetc();
        else if( ch != '}' )
            throw ParseError::Generic(*this, "Expected terminating } in \\u sequence");
        else
            ;
        return val; }
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
    case '\n':
	    m_line ++;
        while( isspace(ch) )
            ch = this->getc();
        return ch;
    default:
        throw ParseError::Todo( FMT("Unknown escape sequence \\" << ch) );
    }
}

char Lexer::getc()
{
    if( m_last_char_valid )
    {
        m_last_char_valid = false;
    }
    else
    {
		m_last_char = m_istream.get();
		if( m_istream.eof() )
			throw Lexer::EndOfFile();
    }
    //::std::cout << "getc(): '" << m_last_char << "'" << ::std::endl;
    return m_last_char;
}

char Lexer::getc_num()
{
    char ch;
    do {
        ch = this->getc();
    } while( ch == '_' );
    return ch;
}

void Lexer::ungetc()
{
//    ::std::cout << "ungetc(): " << m_last_char_valid << " '" << m_last_char << "'" << ::std::endl;
    assert(!m_last_char_valid);
    m_last_char_valid = true;
}

Token::Token():
    m_type(TOK_NULL),
    m_str("")
{
}
Token::Token(enum eTokenType type):
    m_type(type),
    m_str("")
{
}
Token::Token(enum eTokenType type, ::std::string str):
    m_type(type),
    m_str(str)
{
}
Token::Token(uint64_t val, enum eCoreType datatype):
    m_type(TOK_INTEGER),
    m_datatype(datatype),
    m_intval(val)
{
}
Token::Token(double val, enum eCoreType datatype):
    m_type(TOK_FLOAT),
    m_datatype(datatype),
    m_floatval(val)
{
}

const char* Token::typestr(enum eTokenType type)
{
    switch(type)
    {
    #define _(t)    case t: return #t;
    #include "eTokenType.enum.h"
    #undef _
    }
    return ">>BUGCHECK: BADTOK<<";
}

enum eTokenType Token::typefromstr(const ::std::string& s)
{
    if(s == "")
        return TOK_NULL;
    #define _(t)    else if( s == #t ) return t;
    #include "eTokenType.enum.h"
    #undef _
    else
        return TOK_NULL;
}

void operator%(Serialiser& s, enum eTokenType c) {
    s << Token::typestr(c);
}
void operator%(::Deserialiser& s, enum eTokenType& c) {
    ::std::string   n;
    s.item(n);
    c = Token::typefromstr(n);
}
SERIALISE_TYPE_S(Token, {
    s % m_type;
    s.item(m_str);
});

::std::ostream&  operator<<(::std::ostream& os, const Token& tok)
{
    os << Token::typestr(tok.type());
    switch(tok.type())
    {
    case TOK_STRING:
    case TOK_IDENT:
    case TOK_MACRO:
    case TOK_LIFETIME:
        os << "\"" << tok.str() << "\"";
        break;
    case TOK_INTEGER:
        os << ":" << tok.intval();
        break;
    default:
        break;
    }
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const Position& p)
{
    return os << p.filename << ":" << p.line;
}

TTStream::TTStream(const TokenTree& input_tt):
    m_input_tt(input_tt)
{
    DEBUG("input_tt = [" << input_tt << "]");
    m_stack.push_back( ::std::make_pair(0, &input_tt) );
}
TTStream::~TTStream()
{
}
Token TTStream::realGetToken()
{
    while(m_stack.size() > 0)
    {
        // If current index is above TT size, go up
        unsigned int& idx = m_stack.back().first;
        const TokenTree& tree = *m_stack.back().second;

        if(idx == 0 && tree.is_token()) {
            idx ++;
            return tree.tok();
        }

        if(idx < tree.size())
        {
            const TokenTree&    subtree = tree[idx];
            idx ++;
            if( subtree.size() == 0 ) {
                return subtree.tok();
            }
            else {
                m_stack.push_back( ::std::make_pair(0, &subtree ) );
            }
        }
        else {
            m_stack.pop_back();
        }
    }
    return Token(TOK_EOF);
}
Position TTStream::getPosition() const
{
    return Position("TTStream", 0);
}

TokenStream::TokenStream():
    m_cache_valid(false)
{
}
TokenStream::~TokenStream()
{
}

Token TokenStream::innerGetToken()
{
    Token ret = this->realGetToken();
    if( ret.get_pos().filename.size() == 0 )
        ret.set_pos( this->getPosition() );
    //DEBUG("ret.get_pos() = " << ret.get_pos());
    return ret;
}
Token TokenStream::getToken()
{
    if( m_cache_valid )
    {
        m_cache_valid = false;
        return m_cache;
    }
    else if( m_lookahead.size() )
    {
        Token ret = m_lookahead.front();
        m_lookahead.erase(m_lookahead.begin());
        ::std::cout << "getToken[" << typeid(*this).name() << "] - " << ret << ::std::endl;
        return ret;
    }
    else
    {
        Token ret = this->innerGetToken();
        ::std::cout << "getToken[" << typeid(*this).name() << "] - " << ret << ::std::endl;
        return ret;
    }
}
void TokenStream::putback(Token tok)
{
    if( m_cache_valid )
    {
        DEBUG("" << getPosition());
        throw ParseError::BugCheck("Double putback");
    }
    else
    {
        m_cache_valid = true;
        m_cache = tok;
    }
}

eTokenType TokenStream::lookahead(unsigned int i)
{
    const unsigned int MAX_LOOKAHEAD = 3;
    
    if( m_cache_valid )
    {
        if( i == 0 )
            return m_cache.type();
        i --;
    }
    
    if( i >= MAX_LOOKAHEAD )
        throw ParseError::BugCheck("Excessive lookahead");
    
    while( i >= m_lookahead.size() )
    {
        DEBUG("lookahead - read #" << m_lookahead.size());
        m_lookahead.push_back( this->innerGetToken() );
    }
    
    DEBUG("lookahead(" << i << ") = " << m_lookahead[i]);
    return m_lookahead[i].type();
}

