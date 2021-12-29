/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/mangling_v2.cpp
 * - Name mangling (encoding of rust paths into symbols)
 * 
 * Ensures that symbols contain only alpha-numerics and `_`
 * NOTE: `$` is used ONLY for shortening excessively long symbols
 */
#include <debug.hpp>
#include <string_view.hpp>
#include <hir/hir.hpp>  // ABI_RUST
#include <hir/type.hpp>
#include <cctype>
#include <algorithm>	// std::find
#include <cmath>	// ceil/log10

class Mangler
{
    ::std::ostream& m_os;
    std::vector<RcString>    m_name_cache;
public:
    Mangler(::std::ostream& os):
        m_os(os)
    {
    }

    /// Formats an integer in a decodable format (lower case until the final digit)
    // Used to encode values that might be have trailing digits
    void fmt_base26_int(unsigned val)
    {
        // Lower-case: 
        while(val >= 26) {
            m_os << char('a' + (val%26));
            val /= 26;
        }
        assert(val < 26);
        m_os << char('A' + val);
    }

    // Reference-counted item names
    // - These can be repeated quite often, so support back-references
    //   > Back-references are emitted as "`_` <base26>"
    // - Otherwise, emitted as a raw string (see below)
    void fmt_name(const RcString& s)
    {
        // Support back-references to names (if shorter than the literal name)
        auto it = std::find(m_name_cache.begin(), m_name_cache.end(), s);
        if(it != m_name_cache.end())
        {
            auto idx = it - m_name_cache.begin();
            // Only emit this way if shorter than the formatted name would be.
            auto len = 1 + static_cast<unsigned>(std::ceil(std::log10(idx+1) / std::log10(26)));
            if(len < s.size())
            {
                m_os << "_"; fmt_base26_int(idx);
                return ;
            }
        }
        else
        {
            m_name_cache.push_back(s);
        }

        this->fmt_name(s.c_str());
    }
    // Item names:
    // - These can have a single '#' in them (either leading or in the middle)
    // - '-' (crate names)
    // Encoding is either:
    // - "<len:int> <raw_data>" (fully valid identifier)
    // - "<ofs:base26> <len:int> <raw_data1> <raw_data2>" (`#` or `-` present)
    // - "<len:base26> `_` <raw_data2>" (`#` or `-` at the start)
    void fmt_name(const char* const s)
    {
        size_t size = strlen(s);
        const char* hash_pos = nullptr;
        // - Search the string for the '#' or '-' character
        for(const auto* p = s; *p; p++)
        {
            // If looking at the first character, ensure that it's not a digit
            // - Also, `#<digit>` isn't valid (as it'd also badly encode)
            if( p == s ) {
                ASSERT_BUG(Span(), !isdigit(*p), "Leading digit not valid in '" << s << "'");
            }

            if( isalnum(*p) ) {
            }
            else if( *p == '_' ) {
            }
            else if( *p == '#' || *p == '-' ) { // HACK: Treat '-' and '#' as the same in encoding
                // Multiple hash characters? abort/error
                ASSERT_BUG(Span(), hash_pos == nullptr, "Multiple '#' characters in '" << s << "'");
                hash_pos = p;
            }
            else {
                BUG(Span(), "Encounteded invalid character '" << *p << "' in '" << s << "'");
            }
        }

        // If there's a hash, then encode such that it's removed
        if( hash_pos != nullptr )
        {
            auto pre_hash_len = static_cast<int>(hash_pos - s);
#if 1
            if( hash_pos == s && isdigit(hash_pos[1]) ) {
                // <len:base26> '_' <body2>
                // An encoding that allows this pattern
                fmt_base26_int(size-1);
                m_os << '_';
                m_os << hash_pos + 1;
            }
            else {
                // <pos:base26> <len:int> <body1> <body2>
                fmt_base26_int(pre_hash_len);
                m_os << size - 1;
                m_os << ::stdx::string_view(s, s + pre_hash_len);
                m_os << hash_pos + 1;
            }
#else
            // If the suffix is all digits, then print `H` and the literal contents
            if( false && std::isdigit(hash_pos[1]) )
            {
                for(auto c = hash_pos+1; *c; c++)
                    ASSERT_BUG(Span(), std::isdigit(*c), "'" << s << "'");
                m_os << "H";
                m_os << pre_hash_len;
                m_os << ::stdx::string_view(s, s + pre_hash_len);
                m_os << hash_pos + 1;
            }
            else
            {
                // 'h' <len1> <body1> <len2> <body2>
                m_os << "h";
                m_os << pre_hash_len;
                m_os << ::stdx::string_view(s, s + pre_hash_len);
                m_os << size - pre_hash_len - 1;
                m_os << hash_pos + 1;
            }
#endif
        }
        else
        {
            m_os << size;
            m_os << s;
        }
    }
    // SimplePath : <ncomp> 'c' [<RcString> ...]
    void fmt_simple_path(const ::HIR::SimplePath& sp)
    {
        m_os << sp.m_components.size();
        m_os << "c";    // Needed to separate the component count from the crate name
        this->fmt_name(sp.m_crate_name);
        for(const auto& c : sp.m_components)
        {
            this->fmt_name(c);
        }
    }

    // PathParams : <ntys> 'g' [<TypeRef> ...]
    void fmt_path_params(const ::HIR::PathParams& pp)
    {
        // Type Parameter count
        m_os << pp.m_types.size();
        if(pp.m_values.size() > 0) {
            m_os << "v";
            m_os << pp.m_values.size();
        }
        m_os << "g";
        for(const auto& ty : pp.m_types)
        {
            fmt_type(ty);
        }
        for(const auto& v : pp.m_values)
        {
            const auto& ev = *v.as_Evaluated();
            m_os << "V";
            m_os << ev.bytes.size();
            m_os << "_";
            // TODO: Base64 data? (`_` and `$` as the other two?)
            for(size_t i = 0; i < ev.bytes.size(); i ++) {
                m_os << "0123456789abcdef"[ ev.bytes[i] >> 4 ];
                m_os << "0123456789abcdef"[ ev.bytes[i] & 0xF ];
            }
            if( ev.relocations.size() > 0 )
            {
                m_os << "_" << ev.relocations.size() << "R";
                TODO(Span(), "Mangle relocated values");
            }
        }
    }
    // GenericPath : <SimplePath> <PathParams>
    void fmt_generic_path(const ::HIR::GenericPath& gp)
    {
        this->fmt_simple_path(gp.m_path);
        this->fmt_path_params(gp.m_params);
    }

    void fmt_path(const ::HIR::Path& p)
    {
        // Path type
        // - Generic: starts with `G`
        // - Inherent: Starts with `I`
        // - Trait: Starts with `Q` (qualified)
        // - bare type: Starts with `T` (see Trans_MangleType)
        TU_MATCH_HDRA( (p.m_data), {)
        TU_ARMA(Generic, e) {
            m_os << "G";
            this->fmt_generic_path(e);
            }
        TU_ARMA(UfcsInherent, e) {
            m_os << "I";
            this->fmt_type(e.type);
            this->fmt_name(e.item);
            this->fmt_path_params(e.params);
            }
        TU_ARMA(UfcsKnown, e) {
            m_os << "Q";
            this->fmt_type(e.type);
            this->fmt_generic_path(e.trait);
            this->fmt_name(e.item);
            this->fmt_path_params(e.params);
            }
        TU_ARMA(UfcsUnknown, e)
            BUG(Span(), "Non-encodable path " << p);
        }
    }

    // Type
    // - Tuple: 'T' <nelem> [<TypeRef> ...]
    // - Slice: 'S' <TypeRef>
    // - Array: 'A' <size> <TypeRef>
    // - Path: 'N' <Path>
    // - TraitObject: 'D' <data:GenericPath> <nmarker> [markers: <GenericPath> ...] <naty> [<TypeRef> ...]    TODO: Does this need to include the ATY name?
    // - Borrow: 'B' ('s'|'u'|'o') <TypeRef>
    // - RawPointer: 'P' ('s'|'u'|'o') <TypeRef>
    // - Function: 'F' (|'u') (| 'e' <abi:RcString>) <nargs> [args: <TypeRef> ...] <ret:TypeRef>
    // - Primitives::
    //   - u8  : 'C' 'a'
    //   - i8  : 'C' 'b'
    //   - u16 : 'C' 'c'
    //   - i16 : 'C' 'd'
    //   - u32 : 'C' 'e'
    //   - i32 : 'C' 'f'
    //   - u64 : 'C' 'g'
    //   - i64 : 'C' 'h'
    //   - u128: 'C' 'i'
    //   - i128: 'C' 'j'
    //   --
    //   - f32  : 'C' 'n'
    //   - f64  : 'C' 'o'
    //   --
    //   - usize: 'C' 'u'
    //   - isize: 'C' 'v'
    //   - bool : 'C' 'x'
    //   - char : 'C' 'x'
    //   - str  : 'C' 'y'
    // - Diverge: 'C' 'z'
    void fmt_type(const ::HIR::TypeRef& ty)
    {
        TU_MATCH_HDRA( (ty.data()), { )
        case ::HIR::TypeData::TAG_Infer:
        case ::HIR::TypeData::TAG_Generic:
        case ::HIR::TypeData::TAG_ErasedType:
        case ::HIR::TypeData::TAG_Closure:
        case ::HIR::TypeData::TAG_Generator:
            BUG(Span(), "Non-encodable type " << ty);
        TU_ARMA(Tuple, e) {
            m_os << "T" << e.size();
            for(const auto& sty : e)
                this->fmt_type(sty);
            }
        TU_ARMA(Slice, e) {
            m_os << "S";
            this->fmt_type(e.inner);
            }
        TU_ARMA(Array, e) {
            m_os << "A" << e.size.as_Known();
            this->fmt_type(e.inner);
            }
        TU_ARMA(Path, e) {
            m_os << "G";
            ASSERT_BUG(Span(), e.path.m_data.is_Generic(), "Type path not Generic - " << ty);
            this->fmt_generic_path(e.path.m_data.as_Generic());
            }
        TU_ARMA(TraitObject, e) {
            // - TraitObject: 'D' <data:GenericPath> <naty> [<TypeRef> ...] <nmarker> [markers: <GenericPath> ...]
            m_os << "D";
            this->fmt_generic_path(e.m_trait.m_path);
            m_os << e.m_trait.m_type_bounds.size();
            // HACK: Assume all TraitObject types have the same aty set (std::map is deterministic)
            for(const auto& aty : e.m_trait.m_type_bounds)
                this->fmt_type(aty.second.type);
            m_os << e.m_markers.size();
            for(const auto& p : e.m_markers)
                this->fmt_generic_path(p);
            }
        TU_ARMA(Function, e) {
            // - Function: 'F' <abi:RcString> <nargs> [args: <TypeRef> ...] <ret:TypeRef>
            m_os << "F";
            m_os << (e.is_unsafe ? "u" : "");    // Optional allowed, next is a number
            if( e.m_abi != ABI_RUST )
            {
                m_os << "e";
                this->fmt_name(e.m_abi.c_str());
            }
            m_os << e.m_arg_types.size();
            for(const auto& t : e.m_arg_types)
                this->fmt_type(t);
            this->fmt_type(e.m_rettype);
            }
        TU_ARMA(Borrow, e) {
            m_os << "B";
            switch(e.type)
            {
            case ::HIR::BorrowType::Shared: m_os << "s"; break;
            case ::HIR::BorrowType::Unique: m_os << "u"; break;
            case ::HIR::BorrowType::Owned:  m_os << "o"; break;
            }
            this->fmt_type(e.inner);
            }
        TU_ARMA(Pointer, e) {
            m_os << "P";
            switch(e.type)
            {
            case ::HIR::BorrowType::Shared: m_os << "s"; break;
            case ::HIR::BorrowType::Unique: m_os << "u"; break;
            case ::HIR::BorrowType::Owned:  m_os << "o"; break;
            }
            this->fmt_type(e.inner);
            }
        TU_ARMA(Primitive, e) {
            switch(e)
            {
            case ::HIR::CoreType::U8  : m_os << 'C' << 'a'; break;
            case ::HIR::CoreType::I8  : m_os << 'C' << 'b'; break;
            case ::HIR::CoreType::U16 : m_os << 'C' << 'c'; break;
            case ::HIR::CoreType::I16 : m_os << 'C' << 'd'; break;
            case ::HIR::CoreType::U32 : m_os << 'C' << 'e'; break;
            case ::HIR::CoreType::I32 : m_os << 'C' << 'f'; break;
            case ::HIR::CoreType::U64 : m_os << 'C' << 'g'; break;
            case ::HIR::CoreType::I64 : m_os << 'C' << 'h'; break;
            case ::HIR::CoreType::U128: m_os << 'C' << 'i'; break;
            case ::HIR::CoreType::I128: m_os << 'C' << 'j'; break;
            case ::HIR::CoreType::F32 : m_os << 'C' << 'n'; break;
            case ::HIR::CoreType::F64 : m_os << 'C' << 'o'; break;
            case ::HIR::CoreType::Usize: m_os << 'C' << 'u'; break;
            case ::HIR::CoreType::Isize: m_os << 'C' << 'v'; break;
            case ::HIR::CoreType::Bool: m_os << 'C' << 'w'; break;
            case ::HIR::CoreType::Char: m_os << 'C' << 'x'; break;
            case ::HIR::CoreType::Str : m_os << 'C' << 'y'; break;
            }
            }
        TU_ARMA(Diverge, _e) {
            m_os << 'C' << 'z';
            }
        }
    }
};

::FmtLambda Trans_ManglePath(const ::HIR::Path& p)
{
    return FMT_CB(os, os << "ZR"; Mangler(os).fmt_path(p));
}
::FmtLambda Trans_MangleSimplePath(const ::HIR::SimplePath& p)
{
    return FMT_CB(os, os << "ZRG"; Mangler(os).fmt_simple_path(p); Mangler(os).fmt_path_params({}););
}
::FmtLambda Trans_MangleGenericPath(const ::HIR::GenericPath& p)
{
    return FMT_CB(os, os << "ZRG"; Mangler(os).fmt_generic_path(p));
}
::FmtLambda Trans_MangleTypeRef(const ::HIR::TypeRef& p)
{
    return FMT_CB(os, os << "ZRT"; Mangler(os).fmt_type(p));
}

namespace {
    ::FmtLambda max_len(::FmtLambda v) {
        std::stringstream   ss;
        ss << v;
        auto s = ss.str();
        static const size_t MAX_LEN = 128;
        if( s.size() > 128 ) {
            size_t hash = ::std::hash<std::string>()(s);
            ss.str("");
            ss << s.substr(0, MAX_LEN-9) << "$" << ::std::hex << hash;
            DEBUG("Over-long symbol '" << s << "' -> '" << ss.str() << "'");
            s = ss.str();
        }
        else {
        }
        return ::FmtLambda([=](::std::ostream& os){
            os << s;
            });
    }
}
// TODO: If the mangled name exceeds a limit, stop emitting the real name and start hashing the rest.
#define DO_MANGLE(ty) ::FmtLambda Trans_Mangle(const ::HIR::ty& v) { \
    return max_len(Trans_Mangle##ty(v)); \
}
DO_MANGLE(SimplePath)
DO_MANGLE(GenericPath)
DO_MANGLE(Path)
DO_MANGLE(TypeRef)
