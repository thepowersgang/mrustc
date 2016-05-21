/*
 */
#pragma once

#include <rc_string.hpp>
#include <tagged_union.hpp>
#include <serialise.hpp>
#include "../coretypes.hpp"

enum eTokenType
{
    #define _(t)    t,
    #include "eTokenType.enum.h"
    #undef _
};

class Position
{
public:
    RcString    filename;
    unsigned int    line;
    unsigned int    ofs;
    
    Position():
        filename(""),
        line(0),
        ofs(0)
    {}
    Position(RcString filename, unsigned int line, unsigned int ofs):
        filename(filename),
        line(line),
        ofs(ofs)
    {
    }
};
extern ::std::ostream& operator<<(::std::ostream& os, const Position& p);

class Token:
    public Serialisable
{
    TAGGED_UNION(Data, None,
    (None, struct {}),
    (String, ::std::string),
    (Integer, struct {
        enum eCoreType  m_datatype;
        uint64_t    m_intval;
        }),
    (Float, struct {
        enum eCoreType  m_datatype;
        double  m_floatval;
        })
    );
    
    enum eTokenType m_type;
    Data    m_data;
    Position    m_pos;
public:
    Token();
    Token& operator=(Token&& t)
    {
        m_type = t.m_type;  t.m_type = TOK_NULL;
        m_data = ::std::move(t.m_data);
        m_pos = ::std::move(t.m_pos);
        return *this;
    }
    Token(Token&& t):
        m_type(t.m_type),
        m_data( ::std::move(t.m_data) ),
        m_pos( ::std::move(t.m_pos) )
    {
        t.m_type = TOK_NULL;
    }
    Token(const Token& t):
        m_type(t.m_type),
        m_data( Data::make_None({}) ),
        m_pos( t.m_pos )
    {
        assert( t.m_data.tag() != Data::TAGDEAD );
        TU_MATCH(Data, (t.m_data), (e),
        (None,  ),
        (String,    m_data = Data::make_String(e); ),
        (Integer,   m_data = Data::make_Integer(e);),
        (Float, m_data = Data::make_Float(e);)
        )
    }
    
    Token(enum eTokenType type);
    Token(enum eTokenType type, ::std::string str);
    Token(uint64_t val, enum eCoreType datatype);
    Token(double val, enum eCoreType datatype);

    enum eTokenType type() const { return m_type; }
    const ::std::string& str() const { return m_data.as_String(); }
    enum eCoreType  datatype() const { TU_MATCH_DEF(Data, (m_data), (e), (assert(!"Getting datatype of invalid token type");), (Integer, return e.m_datatype;), (Float, return e.m_datatype;)) }
    uint64_t intval() const { return m_data.as_Integer().m_intval; }
    double floatval() const { return m_data.as_Float().m_floatval; }
    bool operator==(const Token& r) const {
        if(type() != r.type())
            return false;
        TU_MATCH(Data, (m_data, r.m_data), (e, re),
        (None, return true;),
        (String, return e == re;),
        (Integer, return e.m_datatype == re.m_datatype && e.m_intval == re.m_intval;),
        (Float, return e.m_datatype == re.m_datatype && e.m_floatval == re.m_floatval;)
        )
        throw "";
    }
    bool operator!=(const Token& r) { return !(*this == r); }

    ::std::string to_str() const;
    
    void set_pos(Position pos) { m_pos = pos; }
    const Position& get_pos() const { return m_pos; }
    
    static const char* typestr(enum eTokenType type);
    static eTokenType typefromstr(const ::std::string& s);
    
    SERIALISABLE_PROTOTYPES();
};
extern ::std::ostream&  operator<<(::std::ostream& os, const Token& tok);

