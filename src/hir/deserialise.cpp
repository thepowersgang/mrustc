/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise.cpp
 * - HIR (De)Serialisation for crate metadata
 */
#include "hir.hpp"
#include "main_bindings.hpp"
#include <serialiser_texttree.hpp>
#include <mir/mir.hpp>

namespace {
    
    template<typename T>
    struct D
    {
    };
    
    class HirDeserialiser
    {
        
        ::std::istream& m_is;
    public:
        HirDeserialiser(::std::istream& is):
            m_is(is)
        {}
        
        uint8_t read_u8() {
            uint8_t v;
            m_is.read(reinterpret_cast<char*>(&v), 1);
            return v;
        }
        uint16_t read_u16() {
            uint16_t v;
            v  = static_cast<uint16_t>(read_u8());
            v |= static_cast<uint16_t>(read_u8()) << 8;
            return v;
        }
        uint32_t read_u32() {
            uint32_t v;
            v  = static_cast<uint32_t>(read_u8());
            v |= static_cast<uint32_t>(read_u8()) << 8;
            v |= static_cast<uint32_t>(read_u8()) << 16;
            v |= static_cast<uint32_t>(read_u8()) << 24;
            return v;
        }
        uint64_t read_u64() {
            uint64_t v;
            v  = static_cast<uint64_t>(read_u8());
            v |= static_cast<uint64_t>(read_u8()) << 8;
            v |= static_cast<uint64_t>(read_u8()) << 16;
            v |= static_cast<uint64_t>(read_u8()) << 24;
            v |= static_cast<uint64_t>(read_u8()) << 32;
            v |= static_cast<uint64_t>(read_u8()) << 40;
            v |= static_cast<uint64_t>(read_u8()) << 48;
            v |= static_cast<uint64_t>(read_u8()) << 56;
            return v;
        }
        // Variable-length encoded u64 (for array sizes)
        uint64_t read_u64c() {
            auto v = read_u8();
            if( v < (1<<7) ) {
                return static_cast<uint64_t>(v);
            }
            else if( v < 0xC0 ) {
                uint64_t    rv = static_cast<uint64_t>(v & 0x3F) << 16;
                rv |= static_cast<uint64_t>(read_u8()) << 8;
                rv |= static_cast<uint64_t>(read_u8());
                return rv;
            }
            else if( v < 0xFF ) {
                uint64_t    rv = static_cast<uint64_t>(v & 0x3F) << 32;
                rv |= static_cast<uint64_t>(read_u8()) << 24;
                rv |= static_cast<uint64_t>(read_u8()) << 16;
                rv |= static_cast<uint64_t>(read_u8()) << 8;
                rv |= static_cast<uint64_t>(read_u8());
                return rv;
            }
            else {
                return read_u64();
            }
        }
        int64_t read_i64c() {
            uint64_t va = read_u64c();
            bool sign = (va & 0x1) != 0;
            va >>= 1;
            
            if( va == 0 && sign ) {
                return INT64_MIN;
            }
            else if( sign ) {
                return -static_cast<int64_t>(va);
            }
            else {
                return -static_cast<uint64_t>(va);
            }
        }
        double read_double() {
            double v;
            m_is.read(reinterpret_cast<char*>(&v), sizeof v);
            return v;
        }
        unsigned int read_tag() {
            return static_cast<unsigned int>( read_u8() );
        }
        size_t read_count() {
            auto v = read_u8();
            if( v < 0xFE ) {
                return v;
            }
            else if( v == 0xFE ) {
                return read_u16( );
            }
            else /*if( v == 0xFF )*/ {
                return ~0u;
            }
        }
        ::std::string read_string() {
            size_t len = read_u8();
            if( len < 128 ) {
            }
            else {
                len = (len & 0x7F) << 16;
                len |= read_u16();
            }
            ::std::string   rv( '\0', len );
            m_is.read(const_cast<char*>(rv.data()), len);
            return rv;
        }
        bool read_bool() {
            return read_u8() != 0x00;
        }
        
        template<typename V>
        ::std::map< ::std::string,V> deserialise_strmap()
        {
            size_t n = read_count();
            ::std::map< ::std::string, V>   rv;
            //rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
            {
                auto s = read_string();
                rv.insert( ::std::make_pair( mv$(s), D<V>::des(*this) ) );
            }
            return rv;
        }
        template<typename V>
        ::std::unordered_map< ::std::string,V> deserialise_strumap()
        {
            size_t n = read_count();
            ::std::unordered_map< ::std::string, V>   rv;
            //rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
            {
                auto s = read_string();
                rv.insert( ::std::make_pair( mv$(s), D<V>::des(*this) ) );
            }
            return rv;
        }
        
        template<typename T>
        ::std::vector<T> deserialise_vec()
        {
            size_t n = read_count();
            ::std::vector<T>    rv;
            rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
                rv.push_back( D<T>::des(*this) );
            return rv;
        }
        template<typename T>
        ::HIR::VisEnt<T> deserialise_visent()
        {
            return ::HIR::VisEnt<T> { read_bool(), D<T>::des(*this) };
        }
        
        template<typename T>
        ::std::unique_ptr<T> deserialise_ptr() {
            return box$( D<T>::des(*this) );
        }
        
        
        ::HIR::TypeRef deserialise_type();
        ::HIR::SimplePath deserialise_simplepath();
        ::HIR::PathParams deserialise_pathparams();
        ::HIR::GenericPath deserialise_genericpath();
        ::HIR::TraitPath deserialise_traitpath();
        ::HIR::Path deserialise_path();

        ::HIR::GenericParams deserialise_genericparams();
        ::HIR::TypeParamDef deserialise_typaramdef();
        ::HIR::GenericBound deserialise_genericbound();
        
        ::HIR::Crate deserialise_crate();
        ::HIR::Module deserialise_module();
        #if 0
        void serialise_typeimpl(const ::HIR::TypeImpl& impl)
        {
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << impl.m_type);
            serialise_generics(impl.m_params);
            serialise_type(impl.m_type);
            
            write_count(impl.m_methods.size());
            for(const auto& v : impl.m_methods) {
                write_string(v.first);
                write_bool(v.second.is_pub);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            // m_src_module doesn't matter after typeck
        }
        void serialise_traitimpl(const ::HIR::TraitImpl& impl)
        {
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " ?" << impl.m_trait_args << " for " << impl.m_type);
            serialise_generics(impl.m_params);
            serialise_pathparams(impl.m_trait_args);
            serialise_type(impl.m_type);
            
            write_count(impl.m_methods.size());
            for(const auto& v : impl.m_methods) {
                DEBUG("fn " << v.first);
                write_string(v.first);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            write_count(impl.m_constants.size());
            for(const auto& v : impl.m_constants) {
                DEBUG("const " << v.first);
                write_string(v.first);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            write_count(impl.m_types.size());
            for(const auto& v : impl.m_types) {
                DEBUG("type " << v.first);
                write_string(v.first);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            // m_src_module doesn't matter after typeck
        }
        void serialise_markerimpl(const ::HIR::MarkerImpl& impl)
        {
            serialise_generics(impl.m_params);
            serialise_pathparams(impl.m_trait_args);
            serialise_type(impl.m_type);
        }
        
        void serialise(const ::HIR::TypeRef& ty) {
            serialise_type(ty);
        }
        void serialise(const ::HIR::SimplePath& p) {
            serialise_simplepath(p);
        }
        void serialise(const ::HIR::TraitPath& p) {
            serialise_traitpath(p);
        }
        void serialise(const ::std::string& v) {
            write_string(v);
        }

        void serialise(const ::MacroRules& mac)
        {
            //m_exported: IGNORE, should be set
            serialise(mac.m_pattern);
            serialise_vec(mac.m_rules);
        }
        void serialise(const ::MacroRulesPatFrag& pat) {
            serialise_vec(pat.m_pats_ents);
            write_count(pat.m_pattern_end);
            serialise_vec(pat.m_next_frags);
        }
        void serialise(const ::MacroPatEnt& pe) {
            write_string(pe.name);
            write_count(pe.name_index);
            serialise(pe.tok);
            serialise_vec(pe.subpats);
            write_tag( static_cast<int>(pe.type) );
        }
        void serialise(const ::MacroRulesArm& arm) {
            serialise_vec(arm.m_param_names);
            serialise_vec(arm.m_contents);
        }
        void serialise(const ::MacroExpansionEnt& ent) {
            TU_MATCHA( (ent), (e),
            (Token,
                write_tag(0);
                serialise(e);
                ),
            (NamedValue,
                write_tag(1);
                write_u8(e >> 24);
                write_count(e & 0x00FFFFFF);
                ),
            (Loop,
                write_tag(2);
                serialise_vec(e.entries);
                serialise(e.joiner);
                // ::std::set<unsigned int>
                write_count(e.variables.size());
                for(const auto& var : e.variables)
                    write_count(var);
                )
            )
        }
        void serialise(const ::Token& tok) {
            // HACK: Hand off to old serialiser code
            ::std::stringstream tmp;
            {
                Serialiser_TextTree ser(tmp);
                tok.serialise( ser );
            }
            
            write_string(tmp.str());
        }
        
        #endif
        ::HIR::Literal deserialise_literal();
        
        ::HIR::ExprPtr deserialise_exprptr()
        {
            ::HIR::ExprPtr  rv;
            if( read_bool() )
            {
                assert( !"TODO: ");
                rv.m_mir = deserialise_mir();
            }
            return rv;
        }
        ::MIR::FunctionPointer deserialise_mir()
        {
            ::MIR::Function rv;
            rv.named_variables = deserialise_vec< ::HIR::TypeRef>( );
            rv.temporaries = deserialise_vec< ::HIR::TypeRef>( );
            //rv.blocks = deserialise_vec< ::MIR::BasicBlock>( );
            return ::MIR::FunctionPointer( new ::MIR::Function(mv$(rv)) );
        }
        #if 0
        void serialise(const ::MIR::BasicBlock& block)
        {
            serialise_vec( block.statements );
            serialise(block.terminator);
        }
        void serialise(const ::MIR::Statement& stmt)
        {
            TU_MATCHA( (stmt), (e),
            (Assign,
                write_tag(0);
                serialise(e.dst);
                serialise(e.src);
                ),
            (Drop,
                write_tag(1);
                assert(e.kind == ::MIR::eDropKind::DEEP);
                serialise(e.slot);
                )
            )
        }
        void serialise(const ::MIR::Terminator& term)
        {
            write_tag( static_cast<int>(term.tag()) );
            TU_MATCHA( (term), (e),
            (Incomplete,
                // NOTE: loops that diverge (don't break) leave a dangling bb
                //assert(!"Entountered Incomplete MIR block");
                ),
            (Return,
                ),
            (Diverge,
                ),
            (Goto,
                write_count(e);
                ),
            (Panic,
                write_count(e.dst);
                ),
            (If,
                serialise(e.cond);
                write_count(e.bb0);
                write_count(e.bb1);
                ),
            (Switch,
                serialise(e.val);
                write_count(e.targets.size());
                for(auto t : e.targets)
                    write_count(t);
                ),
            (Call,
                write_count(e.ret_block);
                write_count(e.panic_block);
                serialise(e.ret_val);
                serialise(e.fcn_val);
                serialise_vec(e.args);
                )
            )
        }
        void serialise(const ::MIR::LValue& lv)
        {
            write_tag( static_cast<int>(lv.tag()) );
            TU_MATCHA( (lv), (e),
            (Variable,
                write_count(e);
                ),
            (Temporary,
                write_count(e.idx);
                ),
            (Argument,
                write_count(e.idx);
                ),
            (Static,
                serialise_path(e);
                ),
            (Return,
                ),
            (Field,
                serialise(e.val);
                write_count(e.field_index);
                ),
            (Deref,
                serialise(e.val);
                ),
            (Index,
                serialise(e.val);
                serialise(e.idx);
                ),
            (Downcast,
                serialise(e.val);
                write_count(e.variant_index);
                )
            )
        }
        void serialise(const ::MIR::RValue& val)
        {
            write_tag( val.tag() );
            TU_MATCHA( (val), (e),
            (Use,
                serialise(e);
                ),
            (Constant,
                serialise(e);
                ),
            (SizedArray,
                serialise(e.val);
                write_u64c(e.count);
                ),
            (Borrow,
                // TODO: Region?
                write_tag( static_cast<int>(e.type) );
                serialise(e.val);
                ),
            (Cast,
                serialise(e.val);
                serialise(e.type);
                ),
            (BinOp,
                serialise(e.val_l);
                write_tag( static_cast<int>(e.op) );
                serialise(e.val_r);
                ),
            (UniOp,
                serialise(e.val);
                write_tag( static_cast<int>(e.op) );
                ),
            (DstMeta,
                serialise(e.val);
                ),
            (MakeDst,
                serialise(e.ptr_val);
                serialise(e.meta_val);
                ),
            (Tuple,
                serialise_vec(e.vals);
                ),
            (Array,
                serialise_vec(e.vals);
                ),
            (Struct,
                serialise_genericpath(e.path);
                serialise_vec(e.vals);
                )
            )
        }
        void serialise(const ::MIR::Constant& v)
        {
            write_tag(v.tag());
            TU_MATCHA( (v), (e),
            (Int,
                write_i64c(e);
                ),
            (Uint,
                write_u64c(e);
                ),
            (Float,
                write_double(e);
                ),
            (Bool,
                write_bool(e);
                ),
            (Bytes,
                write_count(e.size());
                m_os.write( reinterpret_cast<const char*>(e.data()), e.size() );
                ),
            (StaticString,
                write_string(e);
                ),
            (Const,
                serialise_path(e.p);
                ),
            (ItemAddr,
                serialise_path(e);
                )
            )
        }
        #endif
        
        ::HIR::TypeItem deserialise_typeitem()
        {
            switch( read_tag() )
            {
            case 0:
                return ::HIR::TypeItem( deserialise_simplepath() );
            case 1:
                return ::HIR::TypeItem( deserialise_module() );
            case 2:
                return ::HIR::TypeItem( deserialise_typealias() );
            case 3:
                return ::HIR::TypeItem( deserialise_enum() );
            case 4:
                return ::HIR::TypeItem( deserialise_struct() );
            case 5:
                return ::HIR::TypeItem( deserialise_trait() );
            default:
                throw "";
            }
        }
        ::HIR::ValueItem deserialise_valueitem()
        {
            switch( read_tag() )
            {
            case 0:
                return ::HIR::ValueItem( deserialise_simplepath() );
            case 1:
                return ::HIR::ValueItem( deserialise_constant() );
            case 2:
                return ::HIR::ValueItem( deserialise_static() );
            case 3:
                return ::HIR::ValueItem::make_StructConstant({ deserialise_simplepath() });
            case 4:
                return ::HIR::ValueItem( deserialise_function() );
            case 5:
                return ::HIR::ValueItem::make_StructConstructor({ deserialise_simplepath() });
            default:
                throw "";
            }
        }
        
        // - Value items
        ::HIR::Function deserialise_function()
        {
            return ::HIR::Function {
                static_cast< ::HIR::Function::Receiver>( read_tag() ),
                read_string(),
                read_bool(),
                read_bool(),
                deserialise_genericparams(),
                deserialise_fcnargs(),
                deserialise_type(),
                deserialise_exprptr()
                };
        }
        ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >   deserialise_fcnargs()
        {
            size_t n = read_count();
            ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >    rv;
            rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
                rv.push_back( ::std::make_pair( ::HIR::Pattern{}, deserialise_type() ) );
            return rv;
        }
        ::HIR::Constant deserialise_constant()
        {
            return ::HIR::Constant {
                deserialise_genericparams(),
                deserialise_type(),
                deserialise_exprptr(),
                ::HIR::Literal {}
                //serialise(item.m_value_res);  // - Can be calculated from value?
                };
        }
        ::HIR::Static deserialise_static()
        {
            return ::HIR::Static {
                read_bool(),
                deserialise_type(),
                ::HIR::ExprPtr {}
                };
        }
        
        // - Type items
        ::HIR::TypeAlias deserialise_typealias()
        {
            return ::HIR::TypeAlias {
                deserialise_genericparams(),
                deserialise_type()
                };
        }
        ::HIR::Enum deserialise_enum();
        ::HIR::Enum::Variant deserialise_enumvariant();

        ::HIR::Struct deserialise_struct();
        ::HIR::Trait deserialise_trait();
        
        ::HIR::TraitValueItem deserialise_traitvalueitem()
        {
            switch( read_tag() )
            {
            #define _(x, ...)    case ::HIR::TraitValueItem::TAG_##x: return ::HIR::TraitValueItem::make_##x( __VA_ARGS__ );
            _(Constant, deserialise_constant() )
            _(Static,   deserialise_static() )
            _(Function, deserialise_function() )
            #undef _
            default:
                throw "";
            }
        }
        ::HIR::AssociatedType deserialise_associatedtype()
        {
            return ::HIR::AssociatedType {
                read_bool(),
                "", // TODO: Better lifetime type
                deserialise_vec< ::HIR::TraitPath>(),
                deserialise_type()
                };
        }
    };

    #define DEF_D(ty, ...) \
        struct D< ty > { static ty des(HirDeserialiser& d) { __VA_ARGS__ } };
    
    template<>
    DEF_D( ::std::string,
        return d.read_string(); );
    
    template<typename T>
    DEF_D( ::std::unique_ptr<T>,
        return d.deserialise_ptr<T>(); )
    
    template<typename T, typename U>
    struct D< ::std::pair<T,U> > { static ::std::pair<T,U> des(HirDeserialiser& d) {
        return ::std::make_pair( D<T>::des(d), D<U>::des(d) );
        }};
    
    template<typename T>
    DEF_D( ::HIR::VisEnt<T>,
        return d.deserialise_visent<T>(); )
    
    template<> DEF_D( ::HIR::TypeRef, return d.deserialise_type(); )
    template<> DEF_D( ::HIR::GenericPath, return d.deserialise_genericpath(); )
    template<> DEF_D( ::HIR::TraitPath, return d.deserialise_traitpath(); )
    
    template<> DEF_D( ::HIR::TypeParamDef, return d.deserialise_typaramdef(); )
    template<> DEF_D( ::HIR::GenericBound, return d.deserialise_genericbound(); )
    
    template<> DEF_D( ::HIR::ValueItem, return d.deserialise_valueitem(); )
    template<> DEF_D( ::HIR::TypeItem, return d.deserialise_typeitem(); )
    
    template<> DEF_D( ::HIR::Enum::Variant, return d.deserialise_enumvariant(); )
    template<> DEF_D( ::HIR::Literal, return d.deserialise_literal(); )
    
    template<> DEF_D( ::HIR::AssociatedType, return d.deserialise_associatedtype(); )
    template<> DEF_D( ::HIR::TraitValueItem, return d.deserialise_traitvalueitem(); )
    
    ::HIR::TypeRef HirDeserialiser::deserialise_type()
    {
        switch( read_tag() )
        {
        #define _(x, ...)    case ::HIR::TypeRef::Data::TAG_##x: return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_##x( __VA_ARGS__ ) );
        _(Infer, {})
        _(Diverge, {})
        _(Primitive,
            static_cast< ::HIR::CoreType>( read_tag() )
            )
        _(Path, {
            deserialise_path(),
            {}
            })
        _(Generic, {
            read_string(),
            read_u16()
            })
        _(TraitObject, {
            deserialise_traitpath(),
            deserialise_vec< ::HIR::GenericPath>(),
            ""  // TODO: m_lifetime
            })
        _(Array, {
            deserialise_ptr< ::HIR::TypeRef>(),
            ::HIR::ExprPtr(),
            read_u64c()
            })
        _(Slice, {
            deserialise_ptr< ::HIR::TypeRef>()
            })
        _(Tuple,
            deserialise_vec< ::HIR::TypeRef>()
            )
        _(Borrow, {
            static_cast< ::HIR::BorrowType>( read_tag() ),
            deserialise_ptr< ::HIR::TypeRef>()
            })
        _(Pointer, {
            static_cast< ::HIR::BorrowType>( read_tag() ),
            deserialise_ptr< ::HIR::TypeRef>()
            })
        _(Function, {
            read_bool(),
            read_string(),
            deserialise_ptr< ::HIR::TypeRef>(),
            deserialise_vec< ::HIR::TypeRef>()
            })
        #undef _
        default:
            assert(!"Bad TypeRef tag");
        }
    }
    
    ::HIR::SimplePath HirDeserialiser::deserialise_simplepath()
    {
        return ::HIR::SimplePath {
            read_string(),
            deserialise_vec< ::std::string>()
            };
    }
    ::HIR::PathParams HirDeserialiser::deserialise_pathparams()
    {
        ::HIR::PathParams   rv;
        rv.m_types = deserialise_vec< ::HIR::TypeRef>();
        return rv;
    }
    ::HIR::GenericPath HirDeserialiser::deserialise_genericpath()
    {
        return ::HIR::GenericPath {
            deserialise_simplepath(),
            deserialise_pathparams()
            };
    }
    
    ::HIR::TraitPath HirDeserialiser::deserialise_traitpath()
    {
        return ::HIR::TraitPath {
            deserialise_genericpath(),
            {},
            deserialise_strmap< ::HIR::TypeRef>()
            };
    }
    ::HIR::Path HirDeserialiser::deserialise_path()
    {
        switch( read_tag() )
        {
        case 0:
            return ::HIR::Path( deserialise_genericpath() );
        case 1:
            return ::HIR::Path(
                deserialise_type(),
                read_string(),
                deserialise_pathparams()
                );
        case 2:
            return ::HIR::Path(
                deserialise_type(),
                deserialise_genericpath(),
                read_string(),
                deserialise_pathparams()
                );
        default:
            throw "";
        }
    }
    
    ::HIR::GenericParams HirDeserialiser::deserialise_genericparams()
    {
        ::HIR::GenericParams    params;
        params.m_types = deserialise_vec< ::HIR::TypeParamDef>();
        params.m_lifetimes = deserialise_vec< ::std::string>();
        params.m_bounds = deserialise_vec< ::HIR::GenericBound>();
        DEBUG("params = " << params.fmt_args() << ", " << params.fmt_bounds());
        return params;
    }
    ::HIR::TypeParamDef HirDeserialiser::deserialise_typaramdef()
    {
        return ::HIR::TypeParamDef {
            read_string(),
            deserialise_type(),
            read_bool()
            };
    }
    ::HIR::GenericBound HirDeserialiser::deserialise_genericbound()
    {
        switch( read_tag() )
        {
        case 2:
            return ::HIR::GenericBound::make_TraitBound({
                deserialise_type(),
                deserialise_traitpath()
                });
        case 3:
            return ::HIR::GenericBound::make_TypeEquality({
                deserialise_type(),
                deserialise_type()
                });
        default:
            throw "";
        }
    }

    ::HIR::Enum HirDeserialiser::deserialise_enum()
    {
        return ::HIR::Enum {
            deserialise_genericparams(),
            static_cast< ::HIR::Enum::Repr>(read_tag()),
            deserialise_vec< ::std::pair< ::std::string, ::HIR::Enum::Variant> >()
            };
    }
    ::HIR::Enum::Variant HirDeserialiser::deserialise_enumvariant()
    {
        switch( read_tag() )
        {
        case ::HIR::Enum::Variant::TAG_Unit:
            return ::HIR::Enum::Variant::make_Unit({});
        case ::HIR::Enum::Variant::TAG_Value:
            return ::HIR::Enum::Variant::make_Value({
                ::HIR::ExprPtr {},
                deserialise_literal()
                });
        case ::HIR::Enum::Variant::TAG_Tuple:
            return ::HIR::Enum::Variant( deserialise_vec< ::HIR::VisEnt< ::HIR::TypeRef> >() );
        case ::HIR::Enum::Variant::TAG_Struct:
            return ::HIR::Enum::Variant( deserialise_vec< ::std::pair< ::std::string, ::HIR::VisEnt< ::HIR::TypeRef> > >() );
        default:
            throw "";
        }
    }
    ::HIR::Struct HirDeserialiser::deserialise_struct()
    {
        auto params = deserialise_genericparams();
        auto repr = static_cast< ::HIR::Struct::Repr>( read_tag() );
        
        switch( read_tag() )
        {
        case ::HIR::Struct::Data::TAG_Unit:
            return ::HIR::Struct {
                mv$(params), repr,
                ::HIR::Struct::Data::make_Unit( {} )
                };
        case ::HIR::Struct::Data::TAG_Tuple:
            return ::HIR::Struct {
                mv$(params), repr,
                ::HIR::Struct::Data( deserialise_vec< ::HIR::VisEnt< ::HIR::TypeRef> >() )
                };
        case ::HIR::Struct::Data::TAG_Named:
            return ::HIR::Struct {
                mv$(params), repr,
                ::HIR::Struct::Data( deserialise_vec< ::std::pair< ::std::string, ::HIR::VisEnt< ::HIR::TypeRef> > >() )
                };
        default:
            throw "";
        }
    }
    ::HIR::Trait HirDeserialiser::deserialise_trait()
    {
        ::HIR::Trait rv {
            deserialise_genericparams(),
            "",  // TODO: Better type for lifetime
            deserialise_vec< ::HIR::TraitPath>()
            };
        rv.m_is_marker = read_bool();
        rv.m_types = deserialise_strumap< ::HIR::AssociatedType>();
        rv.m_values = deserialise_strumap< ::HIR::TraitValueItem>();
        return rv;
    }
    
    ::HIR::Literal HirDeserialiser::deserialise_literal()
    {
        switch( read_tag() )
        {
        #define _(x, ...)    case ::HIR::Literal::TAG_##x:   return ::HIR::Literal::make_##x(__VA_ARGS__);
        _(List,   deserialise_vec< ::HIR::Literal>() )
        _(Integer, read_u64() )
        _(Float,   read_double() )
        _(BorrowOf, deserialise_simplepath() )
        _(String,  read_string() )
        #undef _
        default:
            throw "";
        }
    }
    
    ::HIR::Module HirDeserialiser::deserialise_module()
    {
        TRACE_FUNCTION;
        
        ::HIR::Module   rv;
        
        // m_traits doesn't need to be serialised
        rv.m_value_items = deserialise_strumap< ::std::unique_ptr< ::HIR::VisEnt< ::HIR::ValueItem> > >();
        rv.m_mod_items = deserialise_strumap< ::std::unique_ptr< ::HIR::VisEnt< ::HIR::TypeItem> > >();
        
        return rv;
    }
    ::HIR::Crate HirDeserialiser::deserialise_crate()
    {
        ::HIR::Crate    rv;
        
        rv.m_root_module = deserialise_module();
        
        //write_count(crate.m_type_impls.size());
        //for(const auto& impl : crate.m_type_impls) {
        //    serialise_typeimpl(impl);
        //}
        //write_count(crate.m_trait_impls.size());
        //for(const auto& tr_impl : crate.m_trait_impls) {
        //    serialise_simplepath(tr_impl.first);
        //    serialise_traitimpl(tr_impl.second);
        //}
        //write_count(crate.m_marker_impls.size());
        //for(const auto& tr_impl : crate.m_marker_impls) {
        //    serialise_simplepath(tr_impl.first);
        //    serialise_markerimpl(tr_impl.second);
        //}
        //
        //serialise_strmap(crate.m_exported_macros);
        //serialise_strmap(crate.m_lang_items);
        return rv;
    }
}

::HIR::CratePtr HIR_Deserialise(const ::std::string& filename)
{
    ::std::ifstream in(filename);
    HirDeserialiser  s { in };
    
    ::HIR::Crate    rv = s.deserialise_crate();
    
    return ::HIR::CratePtr( mv$(rv) );
}

