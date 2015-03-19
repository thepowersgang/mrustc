
#define TE_DATANAME(name)   Data_##name
#define ENUM_CONS(__tag, __type) \
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

#define TE_CONS(name, _) ENUM_CONS(name, TE_DATANAME(name))
#define TE_CONS2(a, b)  TE_CONS a TE_CONS b
#define TE_CONS3(a, ...)  TE_CONS a TE_CONS2(__VA_ARGS__)
#define TE_CONS4(a, ...)  TE_CONS a TE_CONS3(__VA_ARGS__)
#define TE_CONS5(a, ...)  TE_CONS a TE_CONS4(__VA_ARGS__)
#define TE_CONS6(a, ...)  TE_CONS a TE_CONS5(__VA_ARGS__)
#define TE_CONS7(a, ...)  TE_CONS a TE_CONS6(__VA_ARGS__)

// Sizes of structures
#define TE_SO(name, _)   sizeof(TE_DATANAME(name))
#define MAX2(a, b)  (a < b ? b : a)
#define MAXS2(a, b)  (TE_SO a < TE_SO b ? TE_SO b : TE_SO a)
#define MAXS3(a, b, c)   MAX2(MAXS2(a, b), TE_SO c)
#define MAXS4(a, b, c, d)   MAX2(MAXS2(a, b), MAXS2(c, d))
#define MAXS5(a, b, c, d, e)   MAX2(MAXS3(a, b, c), MAXS2(d, e))
#define MAXS6(a, b, c, d, e, f)  MAX2(MAXS3(a, b, c), MAXS3(d, e, f))
#define MAXS7(a, b, c, d, e, f, g)  MAX2(MAXS3(a, b, c), MAXS4(d, e, f, g))

// Type definitions
#define TE_EXP(...)  __VA_ARGS__
#define TE_TYPEDEF(name, content)    struct TE_DATANAME(name) { TE_EXP content };
#define TE_TYPEDEF2(_1, _2) TE_TYPEDEF _1 TE_TYPEDEF _2
#define TE_TYPEDEF3(_1, ...) TE_TYPEDEF _1 TE_TYPEDEF2(__VA_ARGS__)
#define TE_TYPEDEF4(_1, ...) TE_TYPEDEF _1 TE_TYPEDEF3(__VA_ARGS__)
#define TE_TYPEDEF5(_1, ...) TE_TYPEDEF _1 TE_TYPEDEF4(__VA_ARGS__)
#define TE_TYPEDEF6(_1, ...) TE_TYPEDEF _1 TE_TYPEDEF5(__VA_ARGS__)
#define TE_TYPEDEF7(_1, ...) TE_TYPEDEF _1 TE_TYPEDEF6(__VA_ARGS__)

#define TE_TAG(name, _)  name,
#define TE_TAG2(_1,_2)  TE_TAG _1 TE_TAG _2
#define TE_TAG3(_1,...) TE_TAG _1 TE_TAG2(__VA_ARGS__)
#define TE_TAG4(_1,...) TE_TAG _1 TE_TAG3(__VA_ARGS__)
#define TE_TAG5(_1,...) TE_TAG _1 TE_TAG4(__VA_ARGS__)
#define TE_TAG6(_1,...) TE_TAG _1 TE_TAG5(__VA_ARGS__)
#define TE_TAG7(_1,...) TE_TAG _1 TE_TAG6(__VA_ARGS__)

#define TE_DEST_CASE(tag, _)  case tag: as_##tag().~TE_DATANAME(tag)(); break;
#define TE_DEST_CASE2(_1,_2)   TE_DEST_CASE _1 TE_DEST_CASE _2
#define TE_DEST_CASE3(_1, ...) TE_DEST_CASE _1 TE_DEST_CASE2(__VA_ARGS__)
#define TE_DEST_CASE4(_1, ...) TE_DEST_CASE _1 TE_DEST_CASE3(__VA_ARGS__)
#define TE_DEST_CASE5(_1, ...) TE_DEST_CASE _1 TE_DEST_CASE4(__VA_ARGS__)
#define TE_DEST_CASE6(_1, ...) TE_DEST_CASE _1 TE_DEST_CASE5(__VA_ARGS__)
#define TE_DEST_CASE7(_1, ...) TE_DEST_CASE _1 TE_DEST_CASE6(__VA_ARGS__)

#define TE_MOVE_CASE(tag, _)  case tag: new(m_data) TE_DATANAME(tag)(x.unwrap_##tag()); break;
#define TE_MOVE_CASE2(_1,_2)   TE_MOVE_CASE _1 TE_MOVE_CASE _2
#define TE_MOVE_CASE3(_1, ...) TE_MOVE_CASE _1 TE_MOVE_CASE2(__VA_ARGS__)
#define TE_MOVE_CASE4(_1, ...) TE_MOVE_CASE _1 TE_MOVE_CASE3(__VA_ARGS__)
#define TE_MOVE_CASE5(_1, ...) TE_MOVE_CASE _1 TE_MOVE_CASE4(__VA_ARGS__)
#define TE_MOVE_CASE6(_1, ...) TE_MOVE_CASE _1 TE_MOVE_CASE5(__VA_ARGS__)
#define TE_MOVE_CASE7(_1, ...) TE_MOVE_CASE _1 TE_MOVE_CASE6(__VA_ARGS__)

// Macro to obtain a numbered macro for argument counts
#define TE_GM(SUF,_1,_2,_3,_4,_5,_6,_7,COUNT,...) SUF##COUNT

#define MAXS(...)          TE_GM(MAXS        ,__VA_ARGS__,7,6,5,4,3,2)(__VA_ARGS__)
#define TE_TYPEDEFS(...)   TE_GM(TE_TYPEDEF  ,__VA_ARGS__,7,6,5,4,3,2)(__VA_ARGS__)
#define TE_TAGS(...)       TE_GM(TE_TAG      ,__VA_ARGS__,7,6,5,4,3,2)(__VA_ARGS__)
#define TE_DEST_CASES(...) TE_GM(TE_DEST_CASE,__VA_ARGS__,7,6,5,4,3,2)(__VA_ARGS__)
#define TE_MOVE_CASES(...) TE_GM(TE_MOVE_CASE,__VA_ARGS__,7,6,5,4,3,2)(__VA_ARGS__)
#define TE_CONSS(...)      TE_GM(TE_CONS     ,__VA_ARGS__,7,6,5,4,3,2)(__VA_ARGS__)

#define TAGGED_ENUM(_name, _def, ...) \
class _name { \
    typedef _name self_t; \
    TE_TYPEDEFS(__VA_ARGS__) \
    enum Tag { \
        TE_TAGS(__VA_ARGS__)\
    } m_tag; \
    char m_data[MAXS(__VA_ARGS__)]; \
public:\
    _name(): m_tag(_def) {}\
    _name(const _name&) = delete; \
    _name(_name&& x): m_tag(x.m_tag) { x.m_tag = _def; switch(m_tag) {  TE_MOVE_CASES(__VA_ARGS__) } } \
    _name& operator =(_name&& x) { this->~_name(); m_tag = x.m_tag; x.m_tag = _def; switch(m_tag) { TE_MOVE_CASES(__VA_ARGS__) }; return *this; } \
    ~_name() { switch(m_tag) { TE_DEST_CASES(__VA_ARGS__) } } \
    TE_CONSS(__VA_ARGS__) \
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
