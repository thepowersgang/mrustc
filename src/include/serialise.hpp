/*
 */
#ifndef _SERIALSE_HPP_INCLUDED_
#define _SERIALSE_HPP_INCLUDED_

#include <vector>
#include <string>
#include <map>

class Serialiser;

#define SERIALISABLE_PROTOTYPES()\
    virtual const char* serialise_tag() const override; \
    virtual void serialise(::Serialiser& s) const override
#define SERIALISE_TYPE(method_prefix, tag_str, body) \
    const char* method_prefix serialise_tag() const { return tag_str; } \
    void method_prefix serialise(::Serialiser& s) const { body }

class Serialisable
{
public:
    virtual const char* serialise_tag() const = 0;
    virtual void serialise(Serialiser& s) const = 0;
};

class Serialiser
{
protected:
    virtual void start_object(const char *tag) = 0;
    virtual void end_object(const char *tag) = 0;
    virtual void start_array(unsigned int size) = 0;
    virtual void end_array() = 0;
public:
    virtual Serialiser& operator<<(bool val) = 0;
    virtual Serialiser& operator<<(unsigned int val) = 0;
    virtual Serialiser& operator<<(const ::std::string& s) = 0;
    Serialiser& operator<<(const Serialisable& subobj);

    template<typename T>
    Serialiser& operator<<(const ::std::vector<T>& v)
    {
        start_array(v.size());
        for(const auto& ent : v)
            *this << ent;
        end_array();
        return *this;
    }
    template<typename T1, typename T2>
    Serialiser& operator<<(const ::std::pair<T1,T2>& v)
    {
        start_array(2);
        *this << v.first;
        *this << v.second;
        end_array();
        return *this;
    }
    template<typename T1, typename T2>
    Serialiser& operator<<(const ::std::map<T1,T2>& v)
    {
        start_object("map");
        for(const auto& ent : v)
            *this << ent;
        end_object("map");
        return *this;
    }
};

#endif

