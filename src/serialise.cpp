/*
 */
#include <serialise.hpp>
#include <serialiser_texttree.hpp>

Serialiser& Serialiser::operator<<(const Serialisable& subobj)
{
    start_object(subobj.serialise_tag());
    subobj.serialise(*this);
    end_object(subobj.serialise_tag());
    return *this;
}


Serialiser_TextTree::Serialiser_TextTree(::std::ostream& os):
    m_os(os),
    m_indent_level(0)
{
}

void Serialiser_TextTree::start_object(const char *tag) {
    print_indent();
    m_os << tag << "{\n";
    indent();
}
void Serialiser_TextTree::end_object(const char *_tag) {
    unindent();
    print_indent();
    m_os << "}\n";
}
void Serialiser_TextTree::start_array(unsigned int size) {
    print_indent();
    if( size == 0 )
        m_os << "[";
    else
        m_os << "[\n";
    indent();
}
void Serialiser_TextTree::end_array() {
    unindent();
    print_indent();
    m_os << "]\n";
}
Serialiser& Serialiser_TextTree::operator<<(bool val)
{
    print_indent();
    m_os << (val ? "true" : "false") << "\n";
    return *this;
}
Serialiser& Serialiser_TextTree::operator<<(unsigned int val)
{
    print_indent();
    m_os << val << "\n";
    return *this;
}

Serialiser& Serialiser_TextTree::operator<<(const ::std::string& s)
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


