/*
 * "MRustC" - Primitive rust compiler in C++
 */
/**
 * \file parse/lex.cpp
 * \brief Low-level lexer
 */
#include "lex.hpp"
#include "parseerror.hpp"
#include <cassert>
#include <iostream>
#include <cstdlib>  // strtol

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
const struct {
    unsigned char len;
    const char* chars;
    signed int type;
} TOKENMAP[] = {
  TOKENT("!" , TOK_EXLAM),
  TOKENT("!=", TOK_EXLAM_EQUAL),
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

  TOKENT("as",    TOK_RWORD_AS),
  TOKENT("const", TOK_RWORD_CONST),
  TOKENT("fn",    TOK_RWORD_FN),
  TOKENT("for",   TOK_RWORD_FOR),
  TOKENT("static",TOK_RWORD_STATIC),
  TOKENT("use",   TOK_RWORD_USE),

  TOKENT("{",  TOK_BRACE_OPEN),
  TOKENT("|",  TOK_PIPE),
  TOKENT("|=", TOK_PIPE_EQUAL),
  TOKENT("||", TOK_DOUBLE_PIPE),
  TOKENT("}",  TOK_BRACE_CLOSE),
  TOKENT("~",  TOK_TILDE),
};
#define LEN(arr)    (sizeof(arr)/sizeof(arr[0]))

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
            if( issym(ch) )
            {
                ::std::string   str;
                while( issym(ch) )
                {
                    str.push_back(ch);
                    ch = this->getc();
                }
                this->putback();

                if( ch == '!' )
                {
                    return Token(TOK_MACRO, str);
                }
                else
                {
                    return Token(TOK_IDENT, str);
                }
            }
            else if( isdigit(ch) )
            {
                // TODO: handle integers/floats
                throw ParseError::Todo("Lex Numbers");
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
//    ::std::cout << "getc(): '" << m_last_char << "'" << ::std::endl;
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

    case TOK_WHITESPACE: return "TOK_WHITESPACE";
    case TOK_COMMENT: return "TOK_COMMENT";

    // Value tokens
    case TOK_IDENT: return "TOK_IDENT";
    case TOK_MACRO: return "TOK_MACRO";
    case TOK_LIFETIME: return "TOK_LIFETIME";
    case TOK_INTEGER: return "TOK_INTEGER";
    case TOK_CHAR: return "TOK_CHAR";
    case TOK_FLOAT: return "TOK_FLOAT";
    case TOK_UNDERSCORE: return "TOK_UNDERSCORE";

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
    case TOK_EXLAM: return "TOK_EXLAM";
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
    case TOK_EXLAM_EQUAL: return "TOK_EXLAM_EQUAL";
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
    case TOK_RWORD_MUT: return "TOK_RWORD_MUT";
    case TOK_RWORD_CONST: return "TOK_RWORD_CONST";
    case TOK_RWORD_STATIC: return "TOK_RWORD_STATIC";
    case TOK_RWORD_UNSAFE: return "TOK_RWORD_UNSAFE";

    case TOK_RWORD_STRUCT: return "TOK_RWORD_STRUCT";
    case TOK_RWORD_ENUM: return "TOK_RWORD_ENUM";
    case TOK_RWORD_TRAIT: return "TOK_RWORD_TRAIT";
    case TOK_RWORD_FN: return "TOK_RWORD_FN";
    case TOK_RWORD_USE: return "TOK_RWORD_USE";

    case TOK_RWORD_SELF: return "TOK_RWORD_SELF";
    case TOK_RWORD_AS: return "TOK_RWORD_AS";

    case TOK_RWORD_LET: return "TOK_RWORD_LET";
    case TOK_RWORD_MATCH: return "TOK_RWORD_MATCH";
    case TOK_RWORD_IF: return "TOK_RWORD_IF";
    case TOK_RWORD_ELSE: return "TOK_RWORD_ELSE";
    case TOK_RWORD_WHILE: return "TOK_RWORD_WHILE";
    case TOK_RWORD_FOR: return "TOK_RWORD_FOR";

    case TOK_RWORD_CONTINUE: return "TOK_RWORD_CONTINUE";
    case TOK_RWORD_BREAK: return "TOK_RWORD_BREAK";
    case TOK_RWORD_RETURN: return "TOK_RWORD_RETURN";
    }
    return ">>BUGCHECK: BADTOK<<";
}

::std::ostream&  operator<<(::std::ostream& os, Token& tok)
{
    os << Token::typestr(tok.type()) << "\"" << tok.str() << "\"";
    return os;
}

