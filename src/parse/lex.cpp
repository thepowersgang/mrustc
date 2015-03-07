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

Lexer::Lexer(::std::string filename):
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
  //TOKENT("$",  0),
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
  TOKENT("<=", TOK_LTE),
  TOKENT("=" , TOK_EQUAL),
  TOKENT("==", TOK_DOUBLE_EQUAL),
  TOKENT("=>", TOK_FATARROW),
  TOKENT(">",  TOK_GT),
  TOKENT(">>", TOK_DOUBLE_GT),
  TOKENT(">=", TOK_GTE),
  TOKENT("?",  TOK_QMARK),
  TOKENT("@",  TOK_AT),
  // A-Z :: Elsewhere
  TOKENT("[",  TOK_SQUARE_OPEN),
  TOKENT("\\", TOK_BACKSLASH),
  TOKENT("]",  TOK_SQUARE_CLOSE),
  TOKENT("^",  TOK_CARET),
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
            this->putback();
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

    this->putback();
    return best;
}

bool issym(char ch)
{
    if( ::std::isalnum(ch) )
        return true;
    if( ch == '_' )
        return true;
    if( ch == '$' )
        return true;
    return false;
}

Token Lexer::getToken()
{
    try
    {
        char ch = this->getc();

        if( ch == '\n' )
            return Token(TOK_NEWLINE);
        if( isspace(ch) )
        {
            while( isspace(this->getc()) )
                ;
            this->putback();
            return Token(TOK_WHITESPACE);
        }
        this->putback();

        const signed int sym = this->getSymbol();
        if( sym == 0 )
        {
            // No match at all, check for symbol
            char ch = this->getc();
            if( isdigit(ch) )
            {
                // TODO: handle integers/floats
                uint64_t    val = 0;
                if( ch == '0' ) {
                    // Octal/hex handling
                    ch = this->getc();
                    if( ch == 'x' ) {
                        while( isxdigit(ch = this->getc()) )
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
                    else if( isdigit(ch) ) {
                        throw ParseError::Todo("Lex octal numbers");
                    }
                    else {
                        val = 0;
                    }
                }
                else {
                    while( isdigit(ch) ) {
                        val *= val * 10;
                        val += ch - '0';
                        ch = this->getc();
                    }
                }

                if(ch == 'u' || ch == 'i') {
                    // Unsigned
                    throw ParseError::Todo("Lex number suffixes");
                }
                else if( ch == '.' ) {
                    throw ParseError::Todo("Lex floats");
                }
                else {
                    this->putback();
                    return Token(val, CORETYPE_ANY);
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
                    this->putback();
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
                this->putback();
                return Token(TOK_COMMENT, str); }
            case BLOCKCOMMENT: {
                ::std::string   str;
                while(true)
                {
                    if( ch == '*' ) {
                        ch = this->getc();
                        if( ch == '/' ) break;
                        this->putback();
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
                        return Token((uint64_t)ch, CORETYPE_CHAR);
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
                        this->putback();
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
            case DOUBLEQUOTE:
                throw ParseError::Todo("Strings");
                break;
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

uint32_t Lexer::parseEscape(char enclosing)
{
    char ch = this->getc();
    switch(ch)
    {
    case 'u': {
        // Unicode (up to six hex digits)
        uint32_t    val = 0;
        ch = this->getc();
        if( !isxdigit(ch) )
            throw ParseError::Todo("Proper lex error for escape sequences");
        while( isxdigit(ch) )
        {
            char    tmp[2] = {ch, 0};
            val *= 16;
            val += ::std::strtol(tmp, NULL, 16);
            ch = this->getc();
        }
        this->putback();
        return val; }
    case '\\':
        return '\\';
    default:
        throw ParseError::Todo("Proper lex error for escape sequences");
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

void Lexer::putback()
{
//    ::std::cout << "putback(): " << m_last_char_valid << " '" << m_last_char << "'" << ::std::endl;
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

const char* Token::typestr(enum eTokenType type)
{
    switch(type)
    {
    case TOK_NULL: return "TOK_NULL";
    case TOK_EOF: return "TOK_EOF";

    case TOK_NEWLINE:    return "TOK_NEWLINE";
    case TOK_WHITESPACE: return "TOK_WHITESPACE";
    case TOK_COMMENT: return "TOK_COMMENT";

    // Value tokens
    case TOK_IDENT: return "TOK_IDENT";
    case TOK_MACRO: return "TOK_MACRO";
    case TOK_LIFETIME: return "TOK_LIFETIME";
    case TOK_INTEGER: return "TOK_INTEGER";
    case TOK_CHAR: return "TOK_CHAR";
    case TOK_FLOAT: return "TOK_FLOAT";
    case TOK_STRING: return "TOK_STRING";

    case TOK_CATTR_OPEN: return "TOK_CATTR_OPEN";
    case TOK_ATTR_OPEN: return "TOK_ATTR_OPEN";

    // Symbols
    case TOK_PAREN_OPEN: return "TOK_PAREN_OPEN"; case TOK_PAREN_CLOSE: return "TOK_PAREN_CLOSE";
    case TOK_BRACE_OPEN: return "TOK_BRACE_OPEN"; case TOK_BRACE_CLOSE: return "TOK_BRACE_CLOSE";
    case TOK_LT: return "TOK_LT"; case TOK_GT: return "TOK_GT";
    case TOK_SQUARE_OPEN: return "TOK_SQUARE_OPEN";case TOK_SQUARE_CLOSE: return "TOK_SQUARE_CLOSE";
    case TOK_COMMA: return "TOK_COMMA";
    case TOK_SEMICOLON: return "TOK_SEMICOLON";
    case TOK_COLON: return "TOK_COLON";
    case TOK_DOUBLE_COLON: return "TOK_DOUBLE_COLON";
    case TOK_STAR: return "TOK_STAR"; case TOK_AMP: return "TOK_AMP";
    case TOK_PIPE: return "TOK_PIPE";

    case TOK_FATARROW: return "TOK_FATARROW";   // =>
    case TOK_THINARROW: return "TOK_THINARROW";  // ->

    case TOK_PLUS: return "TOK_PLUS"; case TOK_DASH: return "TOK_DASH";
    case TOK_EXCLAM: return "TOK_EXCLAM";
    case TOK_PERCENT: return "TOK_PERCENT";
    case TOK_SLASH: return "TOK_SLASH";

    case TOK_DOT: return "TOK_DOT";
    case TOK_DOUBLE_DOT: return "TOK_DOUBLE_DOT";
    case TOK_TRIPLE_DOT: return "TOK_TRIPLE_DOT";

    case TOK_EQUAL: return "TOK_EQUAL";
    case TOK_PLUS_EQUAL: return "TOK_PLUS_EQUAL";
    case TOK_DASH_EQUAL: return "TOK_DASH_EQUAL";
    case TOK_PERCENT_EQUAL: return "TOK_PERCENT_EQUAL";
    case TOK_SLASH_EQUAL: return "TOK_SLASH_EQUAL";
    case TOK_STAR_EQUAL: return "TOK_STAR_EQUAL";
    case TOK_AMP_EQUAL: return "TOK_AMP_EQUAL";
    case TOK_PIPE_EQUAL: return "TOK_PIPE_EQUAL";

    case TOK_DOUBLE_EQUAL: return "TOK_DOUBLE_EQUAL";
    case TOK_EXCLAM_EQUAL: return "TOK_EXCLAM_EQUAL";
    case TOK_GTE: return "TOK_GTE";
    case TOK_LTE: return "TOK_LTE";

    case TOK_DOUBLE_AMP: return "TOK_DOUBLE_AMP";
    case TOK_DOUBLE_PIPE: return "TOK_DOUBLE_PIPE";
    case TOK_DOUBLE_LT: return "TOK_DOUBLE_LT";
    case TOK_DOUBLE_GT: return "TOK_DOUBLE_GT";

    case TOK_QMARK: return "TOK_QMARK";
    case TOK_AT: return "TOK_AT";
    case TOK_TILDE: return "TOK_TILDE";
    case TOK_BACKSLASH: return "TOK_BACKSLASH";
    case TOK_CARET: return "TOK_CARET";
    case TOK_BACKTICK: return "TOK_BACKTICK";

    // Reserved Words
    case TOK_RWORD_PUB: return "TOK_RWORD_PUB";
    case TOK_RWORD_PRIV: return "TOK_RWORD_PRIV";
    case TOK_RWORD_MUT: return "TOK_RWORD_MUT";
    case TOK_RWORD_CONST: return "TOK_RWORD_CONST";
    case TOK_RWORD_STATIC: return "TOK_RWORD_STATIC";
    case TOK_RWORD_UNSAFE: return "TOK_RWORD_UNSAFE";
    case TOK_RWORD_EXTERN: return "TOK_RWORD_EXTERN";

    case TOK_RWORD_CRATE: return "TOK_RWORD_CRATE";
    case TOK_RWORD_MOD: return "TOK_RWORD_MOD";
    case TOK_RWORD_STRUCT: return "TOK_RWORD_STRUCT";
    case TOK_RWORD_ENUM: return "TOK_RWORD_ENUM";
    case TOK_RWORD_TRAIT: return "TOK_RWORD_TRAIT";
    case TOK_RWORD_FN: return "TOK_RWORD_FN";
    case TOK_RWORD_USE: return "TOK_RWORD_USE";
    case TOK_RWORD_IMPL: return "TOK_RWORD_IMPL";
    case TOK_RWORD_TYPE: return "TOK_RWORD_TYPE";

    case TOK_RWORD_WHERE: return "TOK_RWORD_WHERE";
    case TOK_RWORD_AS: return "TOK_RWORD_AS";

    case TOK_RWORD_LET: return "TOK_RWORD_LET";
    case TOK_RWORD_MATCH: return "TOK_RWORD_MATCH";
    case TOK_RWORD_IF: return "TOK_RWORD_IF";
    case TOK_RWORD_ELSE: return "TOK_RWORD_ELSE";
    case TOK_RWORD_LOOP: return "TOK_RWORD_LOOP";
    case TOK_RWORD_WHILE: return "TOK_RWORD_WHILE";
    case TOK_RWORD_FOR: return "TOK_RWORD_FOR";
    case TOK_RWORD_IN: return "TOK_RWORD_IN";
    case TOK_RWORD_DO: return "TOK_RWORD_DO";

    case TOK_RWORD_CONTINUE: return "TOK_RWORD_CONTINUE";
    case TOK_RWORD_BREAK: return "TOK_RWORD_BREAK";
    case TOK_RWORD_RETURN: return "TOK_RWORD_RETURN";
    case TOK_RWORD_YIELD: return "TOK_RWORD_YIELD";
    case TOK_RWORD_BOX: return "TOK_RWORD_BOX";
    case TOK_RWORD_REF: return "TOK_RWORD_REF";

    case TOK_RWORD_FALSE: return "TOK_RWORD_FALSE";
    case TOK_RWORD_TRUE: return "TOK_RWORD_TRUE";
    case TOK_RWORD_SELF: return "TOK_RWORD_SELF";
    case TOK_RWORD_SUPER: return "TOK_RWORD_SUPER";

    case TOK_RWORD_PROC: return "TOK_RWORD_PROC";
    case TOK_RWORD_MOVE: return "TOK_RWORD_MOVE";
    case TOK_RWORD_ONCE: return "TOK_RWORD_ONCE";

    case TOK_RWORD_ABSTRACT: return "TOK_RWORD_ABSTRACT";
    case TOK_RWORD_FINAL: return "TOK_RWORD_FINAL";
    case TOK_RWORD_PURE: return "TOK_RWORD_PURE";
    case TOK_RWORD_OVERRIDE: return "TOK_RWORD_OVERRIDE";
    case TOK_RWORD_VIRTUAL: return "TOK_RWORD_VIRTUAL";

    case TOK_RWORD_ALIGNOF: return "TOK_RWORD_ALIGNOF";
    case TOK_RWORD_OFFSETOF: return "TOK_RWORD_OFFSETOF";
    case TOK_RWORD_SIZEOF: return "TOK_RWORD_SIZEOF";
    case TOK_RWORD_TYPEOF: return "TOK_RWORD_TYPEOF";

    case TOK_RWORD_BE: return "TOK_RWORD_BE";
    case TOK_RWORD_UNSIZED: return "TOK_RWORD_UNSIZED";
    }
    return ">>BUGCHECK: BADTOK<<";
}

::std::ostream&  operator<<(::std::ostream& os, const Token& tok)
{
    os << Token::typestr(tok.type()) << "\"" << tok.str() << "\"";
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const Position& p)
{
    return os << p.filename << ":" << p.line;
}

TTStream::TTStream(const TokenTree& input_tt):
    m_input_tt(input_tt)
{
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

        if(idx == 0 && tree.size() == 0) {
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

Token TokenStream::getToken()
{
    if( m_cache_valid )
    {
        m_cache_valid = false;
        return m_cache;
    }
    else
    {
        Token ret = this->realGetToken();
        ::std::cout << "getToken[" << typeid(*this).name() << "] - " << ret << ::std::endl;
        return ret;
    }
}
void TokenStream::putback(Token tok)
{
    m_cache_valid = true;
    m_cache = tok;
}
