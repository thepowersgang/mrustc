/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/lex.cpp
 * - Lexer (converts input file to token stream)
 *
 * Provides:
 * - Lexer : The file->token lexer
 * - TTStream : A stream of tokens from a TokenTree
 * - TokenStream : Common interface for all token streams
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
    m_line_ofs(0),
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

bool issym(int ch)
{
    if( ::std::isalnum(ch) )
        return true;
    if( ch == '_' )
        return true;
    if( ch >= 128 || ch < 0 )
        return true;
    return false;
}

Position Lexer::getPosition() const
{
    return Position(m_path, m_line, m_line_ofs);
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
            m_line_ofs = 0;
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
                    ch = this->getc_num();
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
                    else if( ch == 'o' ) {
                        num_mode = OCT;
                        while( isdigit(ch = this->getc_num()) ) {
                            val *= 8;
                            if('0' <= ch && ch <= '7')
                                val += ch - '0';
                            else
                                throw ParseError::Generic("Invalid digit in octal literal");
                        }
                    }
                    else {
                        num_mode = DEC;
                        while( isdigit(ch) ) {
                            val *= 10;
                            val += ch - '0';
                            ch = this->getc_num();
                        }
                    }
                }
                else {
                    while( isdigit(ch) ) {
                        val *= 10;
                        val += ch - '0';
                        ch = this->getc_num();
                    }
                }

                if( ch == 'e' || ch == 'E' || ch == '.' ) {
                    if( num_mode != DEC )
                        throw ParseError::Todo("Non-decimal floats");
                    
                    if( ch == '.' )
                    {
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
                            this->ungetc();
                            this->m_next_token = Token(TOK_DOT);
                            return Token(val, CORETYPE_ANY);
                        }
                    }
                    
                    this->ungetc();
                    double fval = this->parseFloat(val);
                    if( issym(ch = this->getc()) )
                    {
                        ::std::string   suffix;
                        while( issym(ch) )
                        {
                            suffix.push_back(ch);
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
                    return Token( fval, num_type);

                }
                else if( issym(ch)) {
                    // Unsigned
                    ::std::string   suffix;
                    while( issym(ch) )
                    {
                        suffix.push_back(ch);
                        ch = this->getc();
                    }
                    this->ungetc();
                    
                    if(0)   ;
                    else if(suffix == "i8")  num_type = CORETYPE_I8;
                    else if(suffix == "i16") num_type = CORETYPE_I16;
                    else if(suffix == "i32") num_type = CORETYPE_I32;
                    else if(suffix == "i64") num_type = CORETYPE_I64;
                    else if(suffix == "isize") num_type = CORETYPE_INT;
                    else if(suffix == "u8")  num_type = CORETYPE_U8;
                    else if(suffix == "u16") num_type = CORETYPE_U16;
                    else if(suffix == "u32") num_type = CORETYPE_U32;
                    else if(suffix == "u64") num_type = CORETYPE_U64;
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
                                str.push_back(ch);
                            }
                        }
                        return Token(TOK_BYTESTRING, str);
                    }
                    // Byte constant
                    else if( ch == '\'' ) {
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
                auto firstchar = this->getc_codepoint();
                if( firstchar.v == '\\' ) {
                    // Character constant with an escape code
                    uint32_t val = this->parseEscape('\'');
                    if(this->getc() != '\'') {
                        throw ParseError::Todo("Proper error for lex failures");
                    }
                    return Token((uint64_t)val, CORETYPE_CHAR);
                }
                else {
                    ch = this->getc();
                    if( ch == '\'' ) {
                        // Character constant
                        return Token((uint64_t)firstchar.v, CORETYPE_CHAR);
                    }
                    else if( issym(firstchar.v) ) {
                        // Lifetime name
                        ::std::string   str;
                        str += firstchar;
                        while( issym(ch) )
                        {
                            str.push_back(ch);
                            ch = this->getc();
                        }
                        this->ungetc();
                        return Token(TOK_LIFETIME, str);
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

Token Lexer::getTokenInt_RawString(bool is_byte)
{
    // Raw string (possibly byte)
    char ch = this->getc();
    unsigned int hashes = 0;
    while(ch == '#')
    {
        hashes ++;
        ch = this->getc();
    }
    if( hashes == 0 && ch != '"' ) {
        this->ungetc(); // Unget the not '"'
        if( is_byte )
            return this->getTokenInt_Identifier('b', 'r');
        else
            return this->getTokenInt_Identifier('r');
    }
    char terminator = ch;
    ::std::string   val;
    DEBUG("terminator = '" << terminator << "', hashes = " << hashes);

    unsigned terminating_hashes = 0;
    for(;;)
    {
        try {
            ch = this->getc();
        }
        catch( Lexer::EndOfFile e ) {
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
    return Token(is_byte ? TOK_BYTESTRING : TOK_STRING, val);
}
Token Lexer::getTokenInt_Identifier(char leader, char leader2)
{
    ::std::string   str;
    if( leader2 != '\0' )
        str += leader;
    char ch = leader2 == '\0' ? leader : leader2;
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
        this->ungetc();
        for( unsigned int i = 0; i < LEN(RWORDS); i ++ )
        {
            if( str < RWORDS[i].chars ) break;
            if( str == RWORDS[i].chars )    return Token((enum eTokenType)RWORDS[i].type);
        }
        return Token(TOK_IDENT, str);
    }
}

// Takes the VERY lazy way of reading the float into a string then passing to strtod
double Lexer::parseFloat(uint64_t whole)
{
    const int MAX_LEN = 63;
    const int MAX_SIG = MAX_LEN - 1 - 4;
    char buf[MAX_LEN+1];
    int ofs = snprintf(buf, MAX_LEN+1, "%llu.", (unsigned long long)whole);

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
    case '\n':
        m_line ++;
        while( isspace(ch) )
            ch = this->getc();
        this->ungetc();
        if( ch == enclosing )
            return ~0;
        else
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
        m_line_ofs += 1;
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
Codepoint Lexer::getc_codepoint()
{
    uint8_t v1 = this->getc();
    if( v1 < 128 ) {
        return {v1};
    }
    else if( (v1 & 0xC0) == 0x80 ) {
        // Invalid (continuation)
        return {0xFFFE};
    }
    else if( (v1 & 0xE0) == 0xC0 ) {
        // Two bytes
        uint8_t e1 = this->getc();
        if( (e1 & 0xC0) != 0x80 )  return {0xFFFE};
        
        uint32_t outval
            = ((v1 & 0x1F) << 6)
            | ((e1 & 0x3F) <<0)
            ;
        return {outval};
    }
    else if( (v1 & 0xF0) == 0xE0 ) {
        // Three bytes
        uint8_t e1 = this->getc();
        if( (e1 & 0xC0) != 0x80 )  return {0xFFFE};
        uint8_t e2 = this->getc();
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
        uint8_t e1 = this->getc();
        if( (e1 & 0xC0) != 0x80 )  return {0xFFFE};
        uint8_t e2 = this->getc();
        if( (e2 & 0xC0) != 0x80 )  return {0xFFFE};
        uint8_t e3 = this->getc();
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

::std::string Token::to_str() const
{
    switch(m_type)
    {
    case TOK_NULL:  return "/*null*/"; 
    case TOK_EOF:   return "/*eof*/";

    case TOK_NEWLINE:    return "\n";
    case TOK_WHITESPACE: return " ";
    case TOK_COMMENT:    return "/*" + m_str + "*/";
    // Value tokens
    case TOK_IDENT:     return m_str;
    case TOK_MACRO:     return m_str + "!";
    case TOK_LIFETIME:  return "'" + m_str;
    case TOK_INTEGER:   return FMT(m_intval);    // TODO: suffix for type
    case TOK_CHAR:      return FMT("'\\u{"<< ::std::hex << m_intval << "}");
    case TOK_FLOAT:     return FMT(m_floatval);
    case TOK_STRING:    return "\"" + m_str + "\"";
    case TOK_BYTESTRING:return "b\"" + m_str + "\"";
    case TOK_CATTR_OPEN:return "#![";
    case TOK_ATTR_OPEN: return "#[";
    case TOK_UNDERSCORE:return "_";
    // Symbols
    case TOK_PAREN_OPEN:    return "(";
    case TOK_PAREN_CLOSE:   return ")";
    case TOK_BRACE_OPEN:    return "{";
    case TOK_BRACE_CLOSE:   return "}";
    case TOK_LT:    return "<";
    case TOK_GT:    return ">";
    case TOK_SQUARE_OPEN:   return "[";
    case TOK_SQUARE_CLOSE:  return "]";
    case TOK_COMMA:     return ",";
    case TOK_SEMICOLON: return ";";
    case TOK_COLON:     return ":";
    case TOK_DOUBLE_COLON:  return ":";
    case TOK_STAR:  return "*";
    case TOK_AMP:   return "&";
    case TOK_PIPE:  return "|";

    case TOK_FATARROW:  return "=>";       // =>
    case TOK_THINARROW: return "->";      // ->

    case TOK_PLUS:  return "+";
    case TOK_DASH:  return "-";
    case TOK_EXCLAM:    return "!";
    case TOK_PERCENT:   return "%";
    case TOK_SLASH:     return "/";

    case TOK_DOT:   return ".";
    case TOK_DOUBLE_DOT:    return "...";
    case TOK_TRIPLE_DOT:    return "..";

    case TOK_EQUAL: return "=";
    case TOK_PLUS_EQUAL:    return "+=";
    case TOK_DASH_EQUAL:    return "-";
    case TOK_PERCENT_EQUAL: return "%=";
    case TOK_SLASH_EQUAL:   return "/=";
    case TOK_STAR_EQUAL:    return "*=";
    case TOK_AMP_EQUAL:     return "&=";
    case TOK_PIPE_EQUAL:    return "|=";

    case TOK_DOUBLE_EQUAL:  return "==";
    case TOK_EXCLAM_EQUAL:  return "!=";
    case TOK_GTE:    return ">=";
    case TOK_LTE:    return "<=";

    case TOK_DOUBLE_AMP:    return "&&";
    case TOK_DOUBLE_PIPE:   return "||";
    case TOK_DOUBLE_LT:     return "<<";
    case TOK_DOUBLE_GT:     return ">>";
    case TOK_DOUBLE_LT_EQUAL:   return "<=";
    case TOK_DOUBLE_GT_EQUAL:   return ">=";

    case TOK_DOLLAR:    return "$";

    case TOK_QMARK: return "?";
    case TOK_AT:    return "@";
    case TOK_TILDE:     return "~";
    case TOK_BACKSLASH: return "\\";
    case TOK_CARET:     return "^";
    case TOK_CARET_EQUAL:   return "^=";
    case TOK_BACKTICK:  return "`";

    // Reserved Words
    case TOK_RWORD_PUB:     return "pub";
    case TOK_RWORD_PRIV:    return "priv";
    case TOK_RWORD_MUT:     return "mut";
    case TOK_RWORD_CONST:   return "const";
    case TOK_RWORD_STATIC:  return "static";
    case TOK_RWORD_UNSAFE:  return "unsafe";
    case TOK_RWORD_EXTERN:  return "extern";

    case TOK_RWORD_CRATE:   return "crate";
    case TOK_RWORD_MOD:     return "mod";
    case TOK_RWORD_STRUCT:  return "struct";
    case TOK_RWORD_ENUM:    return "enum";
    case TOK_RWORD_TRAIT:   return "trait";
    case TOK_RWORD_FN:      return "fn";
    case TOK_RWORD_USE:     return "use";
    case TOK_RWORD_IMPL:    return "impl";
    case TOK_RWORD_TYPE:    return "type";

    case TOK_RWORD_WHERE:   return "where";
    case TOK_RWORD_AS:      return "as";

    case TOK_RWORD_LET:     return "let";
    case TOK_RWORD_MATCH:   return "match";
    case TOK_RWORD_IF:      return "if";
    case TOK_RWORD_ELSE:    return "else";
    case TOK_RWORD_LOOP:    return "loop";
    case TOK_RWORD_WHILE:   return "while";
    case TOK_RWORD_FOR:     return "for";
    case TOK_RWORD_IN:      return "in";
    case TOK_RWORD_DO:      return "do";

    case TOK_RWORD_CONTINUE:return "continue";
    case TOK_RWORD_BREAK:   return "break";
    case TOK_RWORD_RETURN:  return "return";
    case TOK_RWORD_YIELD:   return "yeild";
    case TOK_RWORD_BOX:     return "box";
    case TOK_RWORD_REF:     return "ref";

    case TOK_RWORD_FALSE:   return "false";
    case TOK_RWORD_TRUE:    return "true";
    case TOK_RWORD_SELF:    return "self";
    case TOK_RWORD_SUPER:   return "super";

    case TOK_RWORD_PROC:    return "proc";
    case TOK_RWORD_MOVE:    return "move";

    case TOK_RWORD_ABSTRACT:return "abstract";
    case TOK_RWORD_FINAL:   return "final";
    case TOK_RWORD_PURE:    return "pure";
    case TOK_RWORD_OVERRIDE:return "override";
    case TOK_RWORD_VIRTUAL: return "virtual";

    case TOK_RWORD_ALIGNOF: return "alignof";
    case TOK_RWORD_OFFSETOF:return "offsetof";
    case TOK_RWORD_SIZEOF:  return "sizeof";
    case TOK_RWORD_TYPEOF:  return "typeof";

    case TOK_RWORD_BE:      return "be";
    case TOK_RWORD_UNSIZED: return "unsized";
    }
    throw ParseError::BugCheck("Reached end of Token::to_str");
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
    case TOK_BYTESTRING:
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
    return Position("TTStream", 0,0);
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
        DEBUG("" << getPosition() << " - Double putback: " << tok << " but " << m_cache);
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

ProtoSpan TokenStream::start_span() const
{
    auto p = this->getPosition();
    return ProtoSpan {
        .filename = p.filename,
        .start_line = p.line,
        .start_ofs = p.ofs,
        };
}
Span TokenStream::end_span(ProtoSpan ps) const
{
    auto p = this->getPosition();
    return Span(
        ps.filename,
        ps.start_line, ps.start_ofs,
        p.line, p.ofs
        );
}


SERIALISE_TYPE_A(TokenTree::, "TokenTree", {
    s.item(m_tok);
    s.item(m_subtrees);
})

