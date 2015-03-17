/*
 */

#ifndef _SERIALISER_TEXTTREE_HPP_INCLUDED_
#define _SERIALISER_TEXTTREE_HPP_INCLUDED_

#include <ostream>
#include <istream>
#include "serialise.hpp"

class Serialiser_TextTree:
    public Serialiser
{
    ::std::ostream& m_os;
     int    m_indent_level;
    bool    m_array_was_empty;
public:
    Serialiser_TextTree(::std::ostream& os);
    
    virtual Serialiser& operator<<(bool val) override;
    virtual Serialiser& operator<<(uint64_t val) override;
    virtual Serialiser& operator<<(int64_t val) override;
    virtual Serialiser& operator<<(double val) override;
    virtual Serialiser& operator<<(const char* s) override;
    
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


class Deserialiser_TextTree:
    public Deserialiser
{
    ::std::istream& m_is;
    
    static bool is_ws(char c);
    char getc();
    char peekc();
    void eat_ws();
public:
    Deserialiser_TextTree(::std::istream& is);
   
protected: 
    virtual size_t start_array() override;
    virtual void end_array() override;
    virtual ::std::string read_tag() override;

public:
    virtual void item(bool& b) override;
    virtual void item(uint64_t& v) override;
    virtual void item(int64_t& v) override;
    virtual void item(double& v) override;
    virtual void item(::std::string& s) override;

    virtual void start_object(const char *tag) override;
    virtual void end_object(const char *tag) override;
};

#endif

