/*
 */
#ifndef _SERIALSE_HPP_INCLUDED_
#define _SERIALSE_HPP_INCLUDED_

#include <vector>
#include <string>
#include <map>
#include <memory>
#include <stdexcept>

class Serialiser;
class Deserialiser;

#define SERIALISABLE_PROTOTYPES()\
    const char* serialise_tag() const override; \
    void serialise(::Serialiser& s) const override; \
    void deserialise(::Deserialiser& s) override
#define SERIALISE_TYPE(method_prefix, tag_str, body, des_body) \
    const char* method_prefix serialise_tag() const { return tag_str; } \
    void method_prefix serialise(::Serialiser& s) const { body } \
    void method_prefix deserialise(::Deserialiser& s) { des_body }
#define SERIALISE_TYPE_A(method_prefix, tag_str, body)  SERIALISE_TYPE(method_prefix, tag_str, body, body)
#define SERIALISE_TYPE_S(class_, body)  SERIALISE_TYPE(class_::, #class_, body, body)
#define SERIALISE_TU(class_, tag_str, val_name, ...)  SERIALISE_TYPE(class_::, tag_str,\
    {\
        s << class_::tag_to_str(this->tag());\
        TU_MATCH(class_, (*this), (val_name), __VA_ARGS__)\
    },/*
*/  {\
        ::std::string   STU_tag_str;\
        s.item(STU_tag_str);\
        auto tag = class_::tag_from_str(STU_tag_str);\
        switch(tag) { \
        case class_::TAGDEAD:   break;\
        SERIALISE_TU_MATCH_ARMS(class_, val_name, __VA_ARGS__)\
        }\
    })
#define SERIALISE_TU_MATCH_ARM(CLASS, VAL_NAME, TAG, ...)  case CLASS::TAG_##TAG: {/*
*/    *this = CLASS::make_##TAG({});/*
*/    auto& VAL_NAME = this->as_##TAG(); /*
*/    __VA_ARGS__/*
*/} break;
#define SERIALISE_TU_MATCH_ARMS(CLASS, NAME, ...)    TU_EXP1( TU_GMA(__VA_ARGS__)(SERIALISE_TU_MATCH_ARM, (CLASS, NAME), __VA_ARGS__) )

class DeserialiseFailure:
    public ::std::runtime_error
{
    //const char *m_fcn;
    //const char *m_message;
public:
    DeserialiseFailure(const char *fcn, const char *message):
        ::std::runtime_error("Deserialise failure")//,
        //m_fcn(fcn),
        //m_message(message)
    {}
};

class Serialisable
{
public:
    virtual const char* serialise_tag() const = 0;
    virtual void serialise(Serialiser& s) const = 0;
    virtual void deserialise(Deserialiser& s) = 0;
};

class Serialiser
{
protected:
    virtual void start_object(const char *tag) = 0;
    virtual void end_object(const char *tag) = 0;
    virtual void start_array(unsigned int size) = 0;
    virtual void end_array() = 0;
public:
    template<typename T>
    inline void item(T& v) { *this << v; }

    virtual Serialiser& operator<<(bool val) = 0;
    virtual Serialiser& operator<<(uint64_t val) = 0;
    virtual Serialiser& operator<<(int64_t val) = 0;
    virtual Serialiser& operator<<(double val) = 0;
    Serialiser& operator<<(unsigned int val) { return *this << (uint64_t)val; };
    virtual Serialiser& operator<<(const char* s) = 0;
    Serialiser& operator<<(const ::std::string& s) {
        return *this << s.c_str();
    }
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
    template<typename T>
    Serialiser& operator<<(const ::std::shared_ptr<T>& v)
    {
        *this << v.get();
        if(v.get())
            *this << *v;
        return *this;
    }
    template<typename T>
    Serialiser& operator<<(const ::std::unique_ptr<T>& v)
    {
        *this << v.get();
        if(v.get())
            *this << *v;
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
        start_array(v.size());
        for(const auto& ent : v)
            *this << ent;
        end_array();
        return *this;
    }
};

class Deserialiser
{
protected:
    virtual size_t start_array() = 0;
    virtual void end_array() = 0;

    virtual ::std::string read_tag() = 0;
public:
    virtual void item(bool& b) = 0;
    virtual void item(uint64_t& v) = 0;
    void item(unsigned int& v) { uint64_t v1; this->item(v1); v = static_cast<unsigned int>(v1); }
    virtual void item(int64_t& val) = 0;
    virtual void item(double& v) = 0;
    virtual void item(::std::string& s) = 0;

    virtual void start_object(const char *tag) = 0;
    virtual void end_object(const char *tag) = 0;
    ::std::string start_object();

    void item(Serialisable& v);
    Deserialiser& operator>>(Serialisable& v) {
        this->item(v);
        return *this;
    }

    template<typename T>
    void item(::std::vector<T>& v) {
        size_t  size = start_array();
        v.reserve(size);
        for(size_t i = 0; i < size; i ++) {
            T item;
            this->item(item);
            v.emplace_back( ::std::move(item) );
        }
        end_array();
    }
    template<typename T>
    void item(::std::shared_ptr<T>& v)
    {
        bool present;

        item(present);

        if(present) {
            v.reset(new T);
            item(*v);
        }
        else {
            v.reset();
        }
    }
    template<typename T>
    void item(::std::unique_ptr<T>& v)
    {
        bool present;

        item(present);

        if(present) {
            v.reset( T::from_deserialiser(*this).release() );
        }
        else {
            v.reset();
        }
    }
    template<typename T1, typename T2>
    void item(::std::pair<T1,T2>& v)
    {
        if(2 != start_array())
            throw ::std::runtime_error("Invalid array size for pair");
        item(v.first);
        item(v.second);
        end_array();
    }
    template<typename T1, typename T2>
    void item(::std::map<T1,T2>& v)
    {
        size_t count = start_array();
        while(count--) {
            ::std::pair<T1,T2>  e;
            item(e);
            v.insert( ::std::move(e) );
        }
        end_array();
    }
};

#endif

