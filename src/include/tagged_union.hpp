/*
 * The most evil CPP abuse I have ever written
 *
 * Constructs a tagged union that correctly handles objects.
 *
 * Union is NOT copy-constructable
 */
#ifndef INCLUDED_TAGGED_UNION_H_
#define INCLUDED_TAGGED_UNION_H_

//#include "cpp_unpack.h"
#include <cassert>

#define TU_CASE_ITEM(src, mod, var, name)	mod auto& name = src.as_##var(); (void)&name;
#define TU_CASE_BODY(class,var, ...)	case class::var: { __VA_ARGS__ } break;
#define TU_CASE(mod, class, var,  name,src, ...)	TU_CASE_BODY(mod,class,var, TU_CASE_ITEM(src,mod,var,name) __VA_ARGS__)
#define TU_CASE2(mod, class, var,  n1,s1, n2,s2, ...)	TU_CASE_BODY(mod,class,var, TU_CASE_ITEM(s1,mod,var,n1) TU_CASE_ITEM(s2,mod,var,n2) __VA_ARGS__)

#define TU_FIRST(a, ...)    a

// Argument iteration
#define _DISP0(n)   
#define _DISP1(n, _1)   n _1
#define _DISP2(n, _1, _2)   n _1 n _2
#define _DISP3(n, v, v2, v3)   n v n v2 n v3
#define _DISP4(n, v, v2, v3, v4)   n v n v2 n v3 n v4
#define _DISP5(n, v, ...)   n v _DISP4(n, __VA_ARGS__)
#define _DISP6(n, v, ...)   n v _DISP5(n, __VA_ARGS__)
#define _DISP7(n, v, ...)   n v _DISP6(n, __VA_ARGS__)
#define _DISP8(n, v, ...)   n v _DISP7(n, __VA_ARGS__)
#define _DISP9(n, a1,a2,a3,a4, b1,b2,b3,b4, c1)   _DISP4(n, a1,a2,a3,a4) _DISP3(n, b1,b2,b3)  _DISP2(n, b4,c1)
#define _DISP10(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2)   _DISP4(n, a1,a2,a3,a4) _DISP4(n, b1,b2,b3,b4) _DISP2(n, c1,c2)
#define _DISP11(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3)   _DISP4(n, a1,a2,a3,a4) _DISP4(n, b1,b2,b3,b4) _DISP3(n, c1,c2,c3)
#define _DISP12(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3,c4)   _DISP4(n, a1,a2,a3,a4) _DISP4(n, b1,b2,b3,b4) _DISP4(n, c1,c2,c3,c4)
#define _DISP13(n, a1,a2,a3,a4,a5, b1,b2,b3,b4, c1,c2,c3,c4)   _DISP5(n, a1,a2,a3,a4,a5) _DISP4(n, b1,b2,b3,b4) _DISP4(n, c1,c2,c3,c4)
#define _DISP14(n, a1,a2,a3,a4,a5, b1,b2,b3,b4,b5, c1,c2,c3,c4)   _DISP5(n, a1,a2,a3,a4,a5) _DISP5(n, b1,b2,b3,b4,b5) _DISP4(n, c1,c2,c3,c4)

#define _DISPO0(n)   
#define _DISPO1(n, _1)   n(_1)
#define _DISPO2(n, _1, _2)   n(_1) n(_2)
#define _DISPO3(n, v, v2, v3)   n(v) n(v2) n(v3)
#define _DISPO4(n, v, v2, v3, v4)   n(v) n(v2) n(v3) n(v4)
#define _DISPO5(n, v, ...)   n v _DISPO4(n, __VA_ARGS__)
#define _DISPO6(n, v, ...)   n v _DISPO5(n, __VA_ARGS__)
#define _DISPO7(n, v, ...)   n v _DISPO6(n, __VA_ARGS__)
#define _DISPO8(n, v, ...)   n v _DISPO7(n, __VA_ARGS__)
#define _DISPO9(n, a1,a2,a3,a4, b1,b2,b3,b4, c1)   _DISPO4(n, a1,a2,a3,a4) _DISPO3(n, b1,b2,b3)  _DISPO2(n, b4,c1)
#define _DISPO10(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2)   _DISPO4(n, a1,a2,a3,a4) _DISPO4(n, b1,b2,b3,b4) _DISPO2(n, c1,c2)
#define _DISPO11(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3)   _DISPO4(n, a1,a2,a3,a4) _DISPO4(n, b1,b2,b3,b4) _DISPO3(n, c1,c2,c3)
#define _DISPO12(n, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3,c4)   _DISPO4(n, a1,a2,a3,a4) _DISPO4(n, b1,b2,b3,b4) _DISPO4(n, c1,c2,c3,c4)
#define _DISPO13(n, a1,a2,a3,a4,a5, b1,b2,b3,b4, c1,c2,c3,c4)   _DISPO5(n, a1,a2,a3,a4,a5) _DISPO4(n, b1,b2,b3,b4) _DISPO4(n, c1,c2,c3,c4)
#define _DISPO14(n, a1,a2,a3,a4,a5, b1,b2,b3,b4,b5, c1,c2,c3,c4)   _DISPO5(n, a1,a2,a3,a4,a5) _DISPO5(n, b1,b2,b3,b4,b5) _DISPO4(n, c1,c2,c3,c4)

#define TU_DISPA(n, a)   n a
#define TU_DISPA1(n, a, _1)   TU_DISPA(n, (TU_EXP a, TU_EXP _1))
#define TU_DISPA2(n, a, _1, _2)   TU_DISPA(n, (TU_EXP a, TU_EXP _1))/*
*/    TU_DISPA(n, (TU_EXP a, TU_EXP _2))
#define TU_DISPA3(n, a, _1, _2, _3) \
      TU_DISPA(n, (TU_EXP a, TU_EXP _1))/*
*/    TU_DISPA(n, (TU_EXP a, TU_EXP _2))/*
*/    TU_DISPA(n, (TU_EXP a, TU_EXP _3))
#define TU_DISPA4(n, a, a1,a2, b1,b2)     TU_DISPA2(n,a, a1,a2)    TU_DISPA2(n,a, b1,b2)
#define TU_DISPA5(n, a, a1,a2,a3, b1,b2)    TU_DISPA3(n,a, a1,a2,a3) TU_DISPA2(n,a, b1,b2)
#define TU_DISPA6(n, a, a1,a2,a3, b1,b2,b3) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3)
#define TU_DISPA7(n, a, a1,a2,a3, b1,b2, c1,c2) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA2(n,a, b1,b2) TU_DISPA2(n,a, c1,c2)
#define TU_DISPA8(n, a, a1,a2,a3, b1,b2,b3, c1,c2) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA2(n,a, c1,c2)
#define TU_DISPA9(n, a, a1,a2,a3, b1,b2,b3, c1,c2,c3) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA3(n,a, c1,c2,c3)
#define TU_DISPA10(n, a, a1,a2,a3, b1,b2,b3, c1,c2,c3, d1) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA3(n,a, c1,c2,c3) TU_DISPA(n, (TU_EXP a, TU_EXP d1))
#define TU_DISPA11(n, a, a1,a2,a3, b1,b2,b3, c1,c2,c3, d1,d2) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA3(n,a, c1,c2,c3) TU_DISPA2(n,a, d1,d2)
#define TU_DISPA12(n, a, a1,a2,a3, b1,b2,b3, c1,c2,c3, d1,d2,d3) TU_DISPA3(n,a, a1,a2,a3) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA3(n,a, c1,c2,c3) TU_DISPA3(n,a, d1,d2,d3)
#define TU_DISPA13(n, a, a1,a2,a3,a4, b1,b2,b3, c1,c2,c3, d1,d2,d3) TU_DISPA4(n,a, a1,a2,a3,a4) TU_DISPA3(n,a, b1,b2,b3) TU_DISPA3(n,a, c1,c2,c3) TU_DISPA3(n,a, d1,d2,d3)
#define TU_DISPA14(n, a, a1,a2,a3,a4, b1,b2,b3,b4, c1,c2,c3, d1,d2,d3) TU_DISPA4(n,a, a1,a2,a3,a4) TU_DISPA4(n,a, b1,b2,b3,b4) TU_DISPA3(n,a, c1,c2,c3) TU_DISPA3(n,a, d1,d2,d3)

// Macro to obtain a numbered macro for argument counts
// - Raw variant
#define TU_GM_I(SUF,_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,COUNT,...) SUF##COUNT
#define TU_GM(SUF,...) TU_GM_I(SUF, __VA_ARGS__,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0)
// - _DISP based variant (for iteration)
#define TU_GMX(...) TU_GM(_DISP, __VA_ARGS__)
#define TU_GMA(...) TU_GM(TU_DISPA, __VA_ARGS__)

// Sizes of structures
#define TU_SO(name, ...)   sizeof(TU_DATANAME(name))
#define MAX2(a, b)  (a < b ? b : a)
#define MAXS2(a, b)  (TU_SO a < TU_SO b ? TU_SO b : TU_SO a)
#define MAXS3(a, b, c)   MAX2(MAXS2(a, b), TU_SO c)
#define MAXS4(a, b, c, d)   MAX2(MAXS2(a, b), MAXS2(c, d))
#define MAXS5(a, b, c, d, e)   MAX2(MAXS3(a, b, c), MAXS2(d, e))
#define MAXS6(a, b, c, d, e, f)  MAX2(MAXS3(a, b, c), MAXS3(d, e, f))
#define MAXS7(a, b, c, d, e, f, g)  MAX2(MAXS3(a, b, c), MAXS4(d, e, f, g))
#define MAXS8(a, b, c, d, e, f, g, h)  MAX2(MAXS4(a, b, c, d), MAXS4(e, f, g, h))
#define MAXS9(a, b, c, d, e, f, g, h, i)  MAX2(MAXS4(a, b, c, d), MAXS5(e, f, g, h, i))
#define MAXS10(a, b, c, d, e, f, g, h, i, j)  MAX2(MAXS5(a, b, c, d, e), MAXS5(f, g, h, i, j))
#define MAXS11(a, b, c, d, e, f, g, h, i, j, k)  MAX2(MAXS6(a, b, c, d, e, f), MAXS5(g, h, i, j, k))
#define MAXS12(a, b, c, d, e, f, g, h, i, j, k, l)  MAX2(MAXS6(a, b, c, d, e, f), MAXS6(g, h, i, j, k, l))
#define MAXS13(a1,a2,a3,a4,a5,a6,a7, b1,b2,b3,b4,b5,b6)  MAX2(MAXS7(a1,a2,a3,a4,a5,a6,a7), MAXS6(b1,b2,b3,b4,b5,b6))
#define MAXS14(a1,a2,a3,a4,a5,a6,a7, b1,b2,b3,b4,b5,b6,b7)  MAX2(MAXS7(a1,a2,a3,a4,a5,a6,a7), MAXS7(b1,b2,b3,b4,b5,b6,b7))

// "match"-like statement
// TU_MATCH(Class, m_data, ent, (Variant, CODE), (Variant2, CODE))
#define TU_MATCH(CLASS, VAR, NAME, ...)   switch( (TU_FIRST VAR).tag()) {/*
*/    case CLASS::TAGDEAD: assert(!"ERROR: destructed tagged union used");/*
*/    TU_MATCH_ARMS(CLASS, VAR, NAME, __VA_ARGS__)/*
*/}
#define TU_MATCH_DEF(CLASS, VAR, NAME, DEF, ...)   switch( (TU_FIRST VAR).tag()) {/*
*/    case CLASS::TAGDEAD: assert(!"ERROR: destructed tagged union used");/*
*/    TU_MATCH_ARMS(CLASS, VAR, NAME, __VA_ARGS__)/*
*/    default: {TU_EXP DEF;} break;/*
*/}
#define TU_MATCH_BIND1(TAG, VAR, NAME)  /*MATCH_BIND*/ auto& NAME = (VAR).as_##TAG(); (void)&NAME;
#define TU_MATCH_BIND2_(TAG, v1,v2, n1,n2)   TU_MATCH_BIND1(TAG, v1, n1) TU_MATCH_BIND1(TAG, v2, n2)
#define TU_MATCH_BIND2(...) TU_MATCH_BIND2_(__VA_ARGS__)    // << Exists to cause expansion of the vars
#define TU_MATCH_ARM(CLASS, VAR, NAME, TAG, ...)  case CLASS::TAG_##TAG: {/*
*/    TU_GM(TU_MATCH_BIND, TU_EXP VAR)(TAG, TU_EXP VAR , TU_EXP NAME)/*
*/    __VA_ARGS__/*
*/} break;
#define TU_MATCH_ARMS(CLASS, VAR, NAME, ...)    TU_GMA(__VA_ARGS__)(TU_MATCH_ARM, (CLASS, VAR, NAME), __VA_ARGS__)

#define TU_IFLET(CLASS, VAR, TAG, NAME, ...) if(VAR.tag() == CLASS::TAG_##TAG) { auto& NAME = VAR.as_##TAG(); (void)&NAME; __VA_ARGS__ }


#define TU_DATANAME(name)   Data_##name
// Internals of TU_CONS
#define TU_CONS_I(__name, __tag, __type) \
    __name(__type v): m_tag(TAG_##__tag) { new (&m_data.__tag) __type( ::std::move(v) ); } \
    static self_t make_##__tag(__type v) \
    {\
        return __name( ::std::move(v) );\
    }\
    bool is_##__tag() const { return m_tag == TAG_##__tag; } \
    const __type& as_##__tag() const { assert(m_tag == TAG_##__tag); return m_data.__tag; } \
    __type& as_##__tag() { assert(m_tag == TAG_##__tag); return m_data.__tag; } \
    __type unwrap_##__tag() { return ::std::move(this->as_##__tag()); } \
// Define a tagged union constructor
#define TU_CONS(__name, name, ...) TU_CONS_I(__name, name, TU_DATANAME(name))

// Type definitions
#define TU_EXP(...)  __VA_ARGS__
#define TU_TYPEDEF(name, ...)    typedef __VA_ARGS__ TU_DATANAME(name);/*
*/

#define TU_TAG(name, ...)  TAG_##name,

// Destructor internals
#define TU_DEST_CASE(tag, ...)  case TAG_##tag: m_data.tag.~TU_DATANAME(tag)(); break;/*
*/

// move constructor internals
#define TU_MOVE_CASE(tag, ...)  case TAG_##tag: new(&m_data.tag) TU_DATANAME(tag)( ::std::move(x.m_data.tag) ); break;/*
*/

// "tag_to_str" internals
#define TU_TOSTR_CASE(tag,...)    case TAG_##tag: return #tag;/*
*/
// "tag_from_str" internals
#define TU_FROMSTR_CASE(tag,...)    else if(str == #tag) return TAG_##tag;/*
*/
#define TU_FROMSTR_CASES(...) TU_GMX(__VA_ARGS__)(TU_FROMSTR_CASE,__VA_ARGS__)

#define TU_UNION_FIELD(tag, ...)    TU_DATANAME(tag) tag;/*
*/
#define TU_UNION_FIELDS(...)    TU_GMX(__VA_ARGS__)(TU_UNION_FIELD,__VA_ARGS__)

#define MAXS(...)          TU_GM(MAXS,__VA_ARGS__)(__VA_ARGS__)
#define TU_CONSS(_name, ...)  TU_GMA(__VA_ARGS__)(TU_CONS, (_name), __VA_ARGS__)
#define TU_TYPEDEFS(...)   TU_GMX(__VA_ARGS__)(TU_TYPEDEF  ,__VA_ARGS__)
#define TU_TAGS(...)       TU_GMX(__VA_ARGS__)(TU_TAG      ,__VA_ARGS__)
#define TU_DEST_CASES(...) TU_GMX(__VA_ARGS__)(TU_DEST_CASE,__VA_ARGS__)
#define TU_MOVE_CASES(...) TU_GMX(__VA_ARGS__)(TU_MOVE_CASE,__VA_ARGS__)
#define TU_TOSTR_CASES(...)   TU_GMX(__VA_ARGS__)(TU_TOSTR_CASE  ,__VA_ARGS__)

/**
 * Define a new tagged union
 *
 * ```
 * TAGGED_UNION(Inner, Any,
 *     (Any, (bool flag)),
 *     (Tuple, (::std::vector<Pattern> subpats)),
 *     (TupleStruct, (Path path; ::std::vector<Pattern> sub_patterns;)),
 *     (Value, (::std::unique_ptr<ExprNode> val )),
 *     (Range, (::std::unique_ptr<ExprNode> left; ::std::unique_ptr<ExprNode> right;))
 *     );
 * ```
 */
#define TAGGED_UNION(_name, _def, ...)  TAGGED_UNION_EX(_name, (), _def, (__VA_ARGS__), (), (), ())
#define TAGGED_UNION_EX(_name, _inherit, _def, _variants, _extra_move, _extra_assign, _extra) \
class _name TU_EXP _inherit { \
    typedef _name self_t;/*
*/public:\
    TU_TYPEDEFS _variants/*
*/  enum Tag { \
        TAGDEAD, \
        TU_TAGS _variants\
    };/*
*/ private:\
    Tag m_tag; \
    union DataUnion { TU_UNION_FIELDS _variants DataUnion() {} ~DataUnion() {} } m_data;/*
*/ public:\
    _name(): m_tag(TAG_##_def) { m_data._def = TU_DATANAME(_def)(); }/*
*/  _name(const _name&) = delete;/*
*/  _name(_name&& x) noexcept: m_tag(x.m_tag) TU_EXP _extra_move { switch(m_tag) { case TAGDEAD: break; TU_MOVE_CASES _variants } x.m_tag = TAGDEAD; }/*
*/  _name& operator =(_name&& x) { switch(m_tag) { case TAGDEAD: break; TU_DEST_CASES _variants } m_tag = x.m_tag; TU_EXP _extra_assign switch(m_tag) { case TAGDEAD: break; TU_MOVE_CASES _variants }; return *this; }/*
*/  ~_name() { switch(m_tag) { case TAGDEAD: break; TU_DEST_CASES _variants } m_tag = TAGDEAD; } \
    \
    Tag tag() const { return m_tag; }\
    const char* tag_str() const { return tag_to_str(m_tag); }\
    TU_CONSS(_name, TU_EXP _variants) \
/*
*/    static const char *tag_to_str(Tag tag) { \
        switch(tag) {/*
*/          case TAGDEAD: return "ERR:DEAD";/*
*/          TU_TOSTR_CASES _variants/*
*/      } return ""; \
    }/*
*/    static Tag tag_from_str(const ::std::string& str) { \
        if(0); /*
*/      TU_FROMSTR_CASES _variants/*
*/      else throw ::std::runtime_error("enum "#_name" No conversion"); \
    }\
    TU_EXP _extra\
}

/*
*/

#endif

