/*
 */

#ifndef _SERIALISER_TEXTTREE_HPP_INCLUDED_
#define _SERIALISER_TEXTTREE_HPP_INCLUDED_

#include <ostream>
#include "serialise.hpp"

class Serialiser_TextTree:
    public Serialiser
{
    ::std::ostream& m_os;
     int    m_indent_level;
public:
    Serialiser_TextTree(::std::ostream& os);
    
    virtual Serialiser& operator<<(bool val) override;
    virtual Serialiser& operator<<(unsigned int val) override;
    virtual Serialiser& operator<<(const ::std::string& s) override;
    
protected:
    virtual void start_object(const char *tag) override;
    virtual void end_object(const char* tag) override;
    virtual void start_array(unsigned int size) override;
    virtual void end_array() override;
private:
    void indent();
    void unindent();
    void print_indent();
};

#endif

