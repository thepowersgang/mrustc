/*
 * The most evil CPP abuse I have ever written
 */
#ifndef INCLUDED_TAGGED_UNION_H_
#define INCLUDED_TAGGED_UNION_H_

#define TE_DATANAME(name)   Data_##name
#define ENUM_CONS(__tag, __type) \
    static self_t make_null_##__tag() { self_t ret; ret.m_tag = __tag; new (ret.m_data) __type; return ::std::move(ret); } \
    static self_t make_##__tag(__type v) \
    {\
        self_t  ret;\
        ret.m_tag = __tag;\
        new (ret.m_data) __type( ::std::move(v) ); \
        return ::std::move(ret); \
    }\
    bool is_##__tag() const { return m_tag == __tag; } \
    const __type& as_##__tag() const { return reinterpret_cast<const __type&>(m_data); } \
    __type& as_##__tag() { return reinterpret_cast<__type&>(m_data); } \
    __type unwrap_##__tag() { return ::std::move(reinterpret_cast<__type&>(m_data)); } \

// Argument iteration
#define _DISP2(n, _1, _2)   n _1 n _2
#define _DISP3(n, v, v2, v3)   n v n v2 n v3    // _DISP2(n, __VA_ARGS__)
#define _DISP4(n, v, v2, v3, v4)   n v n v2 n v3 n v4    // #define _DISP4(n, v, ...)   n v _DISP3(n, __VA_ARGS__)
#define _DISP5(n, v, ...)   n v _DISP4(n, __VA_ARGS__)
#define _DISP6(n, v, ...)   n v _DISP5(n, __VA_ARGS__)
#define _DISP7(n, v, ...)   n v _DISP6(n, __VA_ARGS__)
#define _DISP8(n, v, ...)   n v _DISP7(n, __VA_ARGS__)
#define _DISP9(n, v, ...)   n v _DISP8(n, __VA_ARGS__)
#define _DISP10(n, v, ...)   n v _DISP9(n, __VA_ARGS__)
#define _DISP11(n, v, ...)   n v _DISP10(n, __VA_ARGS__)
#define _DISP12(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3,c4)   _DISP4(n, a1,a2,a3,a4) _DISP4(n, b1,b2,b3,b4) _DISP4(n, c1,c2,c3,c4)    //n v _DISP11(n, __VA_ARGS__)

// Macro to obtain a numbered macro for argument counts
// - Raw variant
#define TE_GM_I(SUF,_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,COUNT,...) SUF##COUNT
#define TE_GM(SUF,...) TE_GM_I(SUF,__VA_ARGS__,12,11,10,9,8,7,6,5,4,3,2,x)
// - _DISP based variant (for iteration)
#define TE_GMX(...) TE_GM(_DISP,__VA_ARGS__)


// "
#define TE_CONS(name, _) ENUM_CONS(name, TE_DATANAME(name))

// Sizes of structures
#define TE_SO(name, _)   sizeof(TE_DATANAME(name))
#define MAX2(a, b)  (a < b ? b : a)
#define MAXS2(a, b)  (TE_SO a < TE_SO b ? TE_SO b : TE_SO a)
#define MAXS3(a, b, c)   MAX2(MAXS2(a, b), TE_SO c)
#define MAXS4(a, b, c, d)   MAX2(MAXS2(a, b), MAXS2(c, d))
#define MAXS5(a, b, c, d, e)   MAX2(MAXS3(a, b, c), MAXS2(d, e))
#define MAXS6(a, b, c, d, e, f)  MAX2(MAXS3(a, b, c), MAXS3(d, e, f))
#define MAXS7(a, b, c, d, e, f, g)  MAX2(MAXS3(a, b, c), MAXS4(d, e, f, g))
#define MAXS8(a, b, c, d, e, f, g, h)  MAX2(MAXS4(a, b, c, d), MAXS4(e, f, g, h))
#define MAXS9(a, b, c, d, e, f, g, h, i)  MAX2(MAXS4(a, b, c, d), MAXS5(e, f, g, h, i))
#define MAXS10(a, b, c, d, e, f, g, h, i, j)  MAX2(MAXS5(a, b, c, d, e), MAXS5(f, g, h, i, j))
#define MAXS11(a, b, c, d, e, f, g, h, i, j, k)  MAX2(MAXS6(a, b, c, d, e, f), MAXS5(g, h, i, j, k))
#define MAXS12(a, b, c, d, e, f, g, h, i, j, k, l)  MAX2(MAXS6(a, b, c, d, e, f), MAXS6(g, h, i, j, k, l))

// Type definitions
#define TE_EXP(...)  __VA_ARGS__
#define TE_TYPEDEF(name, content)    struct TE_DATANAME(name) { TE_EXP content; };/*
*/

#define TE_TAG(name, _)  name,

// Destructor internals
#define TE_DEST_CASE(tag, _)  case tag: as_##tag().~TE_DATANAME(tag)(); break;/*
*/

// move constructor internals
#define TE_MOVE_CASE(tag, _)  case tag: new(m_data) TE_DATANAME(tag)(x.unwrap_##tag()); break;/*
*/

// "tag_to_str" internals
#define TE_TOSTR_CASE(tag,_)    case tag: return #tag;/*
*/
// "tag_from_str" internals
#define TE_FROMSTR_CASE(tag,_)    else if(str == #tag) return tag;/*
*/

#define MAXS(...)          TE_GM(MAXS,__VA_ARGS__)(__VA_ARGS__)
#define TE_CONSS(...)      TE_GMX(__VA_ARGS__)(TE_CONS     , __VA_ARGS__)
#define TE_TYPEDEFS(...)   TE_GMX(__VA_ARGS__)(TE_TYPEDEF  ,__VA_ARGS__)
#define TE_TAGS(...)       TE_GMX(__VA_ARGS__)(TE_TAG      ,__VA_ARGS__)
#define TE_DEST_CASES(...) TE_GMX(__VA_ARGS__)(TE_DEST_CASE,__VA_ARGS__)
#define TE_MOVE_CASES(...) TE_GMX(__VA_ARGS__)(TE_MOVE_CASE,__VA_ARGS__)
#define TE_TOSTR_CASES(...)   TE_GMX(__VA_ARGS__)(TE_TOSTR_CASE  ,__VA_ARGS__)
#define TE_FROMSTR_CASES(...) TE_GMX(__VA_ARGS__)(TE_FROMSTR_CASE,__VA_ARGS__)

#define TAGGED_ENUM(_name, _def, ...) \
class _name { \
    typedef _name self_t;/*
*/  TE_TYPEDEFS(__VA_ARGS__)/*
*/public:\
    enum Tag { \
        TE_TAGS(__VA_ARGS__)\
    };/*
*/ private:\
    Tag m_tag; \
    char m_data[MAXS(__VA_ARGS__)];/*
*/ public:\
    _name(): m_tag(_def) {}\
    _name(const _name&) = delete; \
    _name(_name&& x): m_tag(x.m_tag) { x.m_tag = _def; switch(m_tag) {  TE_MOVE_CASES(__VA_ARGS__) } } \
    _name& operator =(_name&& x) { this->~_name(); m_tag = x.m_tag; x.m_tag = _def; switch(m_tag) { TE_MOVE_CASES(__VA_ARGS__) }; return *this; } \
    ~_name() { switch(m_tag) { TE_DEST_CASES(__VA_ARGS__) } } \
    \
    Tag tag() const { return m_tag; }\
    TE_CONSS(__VA_ARGS__) \
/*
*/    static const char *tag_to_str(Tag tag) { \
        switch(tag) {/*
*/          TE_TOSTR_CASES(__VA_ARGS__)/*
*/      } return ""; \
    }/*
*/    static Tag tag_from_str(const ::std::string& str) { \
        if(0); /*
*/      TE_FROMSTR_CASES(__VA_ARGS__)/*
*/      else throw ::std::runtime_error("enum "#_name" No conversion"); \
    }\
}

#define ENUM3(_name, _def, _t1, _v1, _t2, _v2, _t3, _v3)\
    TAGGED_ENUM(_name, _def,\
        (_t1, _v1), \
        (_t2, _v2), \
        (_t3, _v3) \
        )

/*
    ENUM5(Inner, Any,
        Any, bool,
        Tuple, ::std::vector<Pattern>,
        TupleStruct, struct { Path path; ::std::vector<Pattern> sub_patterns; },
        Value, ::std::unique_ptr<ExprNode>,
        Range, struct { ::std::unique_ptr<ExprNode> left; ::std::unique_ptr<ExprNode> right; }
        )   m_contents;
*/

#endif

