/*
 */
#define DISABLE_DEBUG
#include <serialise.hpp>
#include <serialiser_texttree.hpp>
#include "common.hpp"

Serialiser& Serialiser::operator<<(const Serialisable& subobj)
{
    start_object(subobj.serialise_tag());
    subobj.serialise(*this);
    end_object(subobj.serialise_tag());
    return *this;
}

void Deserialiser::item(Serialisable& s)
{
    DEBUG("Deserialise - '"<<s.serialise_tag()<<"'");
    start_object(s.serialise_tag());
    s.deserialise(*this);
    end_object(s.serialise_tag());
}
::std::string Deserialiser::start_object()
{
    ::std::string s = read_tag();
    DEBUG("tag = '" << s << "'");
    start_object(nullptr);
    return s;
}


// --------------------------------------------------------------------
Serialiser_TextTree::Serialiser_TextTree(::std::ostream& os):
    m_os(os),
    m_indent_level(0),
    m_array_was_empty(false)
{
}

void Serialiser_TextTree::start_object(const char *tag) {
    print_indent();
    m_os << tag << " {\n";
    indent();
}
void Serialiser_TextTree::end_object(const char *_tag) {
    unindent();
    print_indent();
    m_os << "}\n";
}
void Serialiser_TextTree::start_array(unsigned int size) {
    print_indent();
    if( size == 0 ) {
        m_os << "[]\n";
        m_array_was_empty = true;
    }
    else {
        m_os << "[" << size << "\n";
        indent();
    }
}
void Serialiser_TextTree::end_array() {
    if( m_array_was_empty ) {
        m_array_was_empty = false;
    }
    else {
        unindent();
        print_indent();
        m_os << "]\n";
    }
}
Serialiser& Serialiser_TextTree::operator<<(bool val)
{
    print_indent();
    m_os << (val ? "T" : "F") << "\n";
    return *this;
}
Serialiser& Serialiser_TextTree::operator<<(uint64_t val)
{
    print_indent();
    m_os << val << "\n";
    return *this;
}
Serialiser& Serialiser_TextTree::operator<<(int64_t val)
{
    print_indent();
    m_os << val << "\n";
    return *this;
}
Serialiser& Serialiser_TextTree::operator<<(double val)
{
    print_indent();
    m_os << val << "\n";
    return *this;
}

Serialiser& Serialiser_TextTree::operator<<(const char* s)
{
    print_indent();
    m_os << "\"" << s << "\"\n";
    return *this;
}
void Serialiser_TextTree::indent()
{
    m_indent_level ++;
}
void Serialiser_TextTree::unindent()
{
    m_indent_level --;
}
void Serialiser_TextTree::print_indent()
{
    for(int i = 0; i < m_indent_level; i ++)
        m_os << " ";
}

// --------------------------------------------------------------------
Deserialiser_TextTree::Deserialiser_TextTree(::std::istream& is):
    m_is(is)
{
}
bool Deserialiser_TextTree::is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
char Deserialiser_TextTree::getc()
{
    char c;
    m_is.get(c);
    return c;
}
char Deserialiser_TextTree::peekc()
{
    return m_is.peek();
}
void Deserialiser_TextTree::eat_ws()
{
    char c;
    do {
        m_is.get(c);
    } while( is_ws(c) );
    m_is.putback(c);
}
size_t Deserialiser_TextTree::start_array()
{
    eat_ws();
    char c = getc();
    if( c != '[' )
        throw ::std::runtime_error("TODO: Less shit exception, start_array");
    
    eat_ws();
    if( peekc() == ']' ) {
        DEBUG("len = 0");
        return 0;
    }
    
    size_t len;
    m_is >> len;
    if( !m_is.good() )
        throw ::std::runtime_error("TODO: Less shit exception, start_array");
    DEBUG("len = "<<len);
    return len;
}
void Deserialiser_TextTree::end_array()
{
    eat_ws();
    char c = getc();
    DEBUG("c = '"<<c<<"'");
    if( c != ']' )
        throw ::std::runtime_error("TODO: Less shit exception, end_array");
}
::std::string Deserialiser_TextTree::read_tag()
{
    ::std::string   tag;
    eat_ws();
    char c;
    do {
        m_is.get(c);
        tag.push_back(c);
    } while( !is_ws(c) );
    tag.pop_back();
    if( tag.size() == 0 )
        throw ::std::runtime_error("TODO: Less shit exception, read_tag");
    return tag;
}

void Deserialiser_TextTree::item(bool& b)
{
    eat_ws();
    switch( getc() )
    {
    case 'T': DEBUG("true");   b = true;   break;
    case 'F': DEBUG("false");  b = false;  break;
    default:
        throw ::std::runtime_error("TODO: Less shit exception, item(bool)");
    }
}
void Deserialiser_TextTree::item(uint64_t& v)
{
    eat_ws();
    m_is >> v;
    if( !m_is.good() )
        throw ::std::runtime_error("TODO: Less shit exception, item(uint64_t)");
}
void Deserialiser_TextTree::item(int64_t& v)
{
    eat_ws();
    m_is >> v;
    if( !m_is.good() )
        throw ::std::runtime_error("TODO: Less shit exception, item(int64_t)");
}
void Deserialiser_TextTree::item(double& v)
{
    eat_ws();
    m_is >> v;
    if( !m_is.good() )
        throw ::std::runtime_error("TODO: Less shit exception, item(double)");
}
void Deserialiser_TextTree::item(::std::string& s)
{
    eat_ws();
    
    ::std::string   rv;
    char c = getc();
    DEBUG("c = '"<<c<<"'");
    if( c != '"' )
        throw ::std::runtime_error("TODO: Less shit exception, item(::std::string)");
    
    while(peekc() != '"')
    {
        char c = getc();
        if( c == '\\' )
            c = getc();
        rv.push_back(c);
    }
    getc(); // eat "
    
    DEBUG("rv = '"<<rv<<"'");
    s = rv;
}

void Deserialiser_TextTree::start_object(const char *tag)
{
    eat_ws();
    if( tag != nullptr ) {
        ::std::string s = read_tag();
        DEBUG("s == " << s);
        if( s != tag )
            throw ::std::runtime_error("TODO: Less shit exception, start_object");
    }
    eat_ws();
    char c = getc();
    DEBUG("c = '" << c << "' (tag = " << (tag ? tag : "-NUL-"));
    if( c != '{' )
        throw ::std::runtime_error("TODO: Less shit exception, start_object");
}
void Deserialiser_TextTree::end_object(const char *tag)
{
    eat_ws();
    char c = getc();
    DEBUG("c = '"<<c<<"'");
    if( c != '}' ) {
        throw ::std::runtime_error("TODO: Less shit exception, end_object");
    }
}
