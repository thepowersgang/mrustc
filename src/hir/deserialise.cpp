/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise.cpp
 * - HIR (De)Serialisation for crate metadata
 */
#define DISABLE_DEBUG   //  Disable debug for this function - too hot
#include "hir.hpp"
#include "main_bindings.hpp"
#include <serialiser_texttree.hpp>
#include <mir/mir.hpp>
#include <macro_rules/macro_rules.hpp>
#include "serialise_lowlevel.hpp"

namespace {
    
    template<typename T>
    struct D
    {
    };
    
    class HirDeserialiser
    {
        const ::std::string& m_crate_name;
        ::HIR::serialise::Reader&   m_in;
    public:
        HirDeserialiser(const ::std::string& crate_name, ::HIR::serialise::Reader& in):
            m_crate_name( crate_name ),
            m_in(in)
        {}
        
        ::std::string read_string() { return m_in.read_string(); }
        size_t deserialise_count() { return m_in.read_count(); }
        
        template<typename V>
        ::std::map< ::std::string,V> deserialise_strmap()
        {
            size_t n = m_in.read_count();
            ::std::map< ::std::string, V>   rv;
            //rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
            {
                auto s = m_in.read_string();
                rv.insert( ::std::make_pair( mv$(s), D<V>::des(*this) ) );
            }
            return rv;
        }
        template<typename V>
        ::std::unordered_map< ::std::string,V> deserialise_strumap()
        {
            size_t n = m_in.read_count();
            ::std::unordered_map< ::std::string, V>   rv;
            //rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
            {
                auto s = m_in.read_string();
                DEBUG("- " << s);
                rv.insert( ::std::make_pair( mv$(s), D<V>::des(*this) ) );
            }
            return rv;
        }
        template<typename V>
        ::std::unordered_multimap< ::std::string,V> deserialise_strummap()
        {
            size_t n = m_in.read_count();
            ::std::unordered_multimap< ::std::string, V>   rv;
            //rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
            {
                auto s = m_in.read_string();
                DEBUG("- " << s);
                rv.insert( ::std::make_pair( mv$(s), D<V>::des(*this) ) );
            }
            return rv;
        }
        
        template<typename T>
        ::std::vector<T> deserialise_vec()
        {
            size_t n = m_in.read_count();
            ::std::vector<T>    rv;
            rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
                rv.push_back( D<T>::des(*this) );
            return rv;
        }
        template<typename T>
        ::std::vector<T> deserialise_vec_c(::std::function<T()> cb)
        {
            size_t n = m_in.read_count();
            ::std::vector<T>    rv;
            rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
                rv.push_back( cb() );
            return rv;
        }
        template<typename T>
        ::HIR::VisEnt<T> deserialise_visent()
        {
            return ::HIR::VisEnt<T> { m_in.read_bool(), D<T>::des(*this) };
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
        
        ::HIR::TypeImpl deserialise_typeimpl()
        {
            ::HIR::TypeImpl rv;
            TRACE_FUNCTION_FR("", "impl" << rv.m_params.fmt_args() << " " << rv.m_type);
            
            rv.m_params = deserialise_genericparams();
            rv.m_type = deserialise_type();
            
            size_t method_count = m_in.read_count();
            for(size_t i = 0; i < method_count; i ++)
            {
                auto name = m_in.read_string();
                rv.m_methods.insert( ::std::make_pair( mv$(name), ::HIR::TypeImpl::VisImplEnt< ::HIR::Function> {
                    m_in.read_bool(), m_in.read_bool(), deserialise_function()
                    } ) );
            }
            size_t const_count = m_in.read_count();
            for(size_t i = 0; i < const_count; i ++)
            {
                auto name = m_in.read_string();
                rv.m_constants.insert( ::std::make_pair( mv$(name), ::HIR::TypeImpl::VisImplEnt< ::HIR::Constant> {
                    m_in.read_bool(), m_in.read_bool(), deserialise_constant()
                    } ) );
            }
            // m_src_module doesn't matter after typeck
            return rv;
        }
        ::HIR::TraitImpl deserialise_traitimpl()
        {
            ::HIR::TraitImpl    rv;
            TRACE_FUNCTION_FR("", "impl" << rv.m_params.fmt_args() << " ?" << rv.m_trait_args << " for " << rv.m_type);
            
            rv.m_params = deserialise_genericparams();
            rv.m_trait_args = deserialise_pathparams();
            rv.m_type = deserialise_type();
            
            
            size_t method_count = m_in.read_count();
            for(size_t i = 0; i < method_count; i ++)
            {
                auto name = m_in.read_string();
                rv.m_methods.insert( ::std::make_pair( mv$(name), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> {
                    m_in.read_bool(), deserialise_function()
                    } ) );
            }
            size_t const_count = m_in.read_count();
            for(size_t i = 0; i < const_count; i ++)
            {
                auto name = m_in.read_string();
                rv.m_constants.insert( ::std::make_pair( mv$(name), ::HIR::TraitImpl::ImplEnt< ::HIR::Constant> {
                    m_in.read_bool(), deserialise_constant()
                    } ) );
            }
            size_t static_count = m_in.read_count();
            for(size_t i = 0; i < static_count; i ++)
            {
                auto name = m_in.read_string();
                rv.m_statics.insert( ::std::make_pair( mv$(name), ::HIR::TraitImpl::ImplEnt< ::HIR::Static> {
                    m_in.read_bool(), deserialise_static()
                    } ) );
            }
            size_t type_count = m_in.read_count();
            for(size_t i = 0; i < type_count; i ++)
            {
                auto name = m_in.read_string();
                rv.m_types.insert( ::std::make_pair( mv$(name), ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> {
                    m_in.read_bool(), deserialise_type()
                    } ) );
            }
            
            // m_src_module doesn't matter after typeck
            return rv;
        }
        ::HIR::MarkerImpl deserialise_markerimpl()
        {
            return ::HIR::MarkerImpl {
                deserialise_genericparams(),
                deserialise_pathparams(),
                m_in.read_bool(),
                deserialise_type()
                };
        }
        
        ::MacroRulesPtr deserialise_macrorulesptr()
        {
            return ::MacroRulesPtr( new MacroRules(deserialise_macrorules()) );
        }
        ::MacroRules deserialise_macrorules()
        {
            ::MacroRules    rv;
            // NOTE: This is set after loading.
            //rv.m_exported = true;
            rv.m_rules = deserialise_vec_c< ::MacroRulesArm>( [&](){ return deserialise_macrorulesarm(); });
            rv.m_source_crate = m_in.read_string();
            if(rv.m_source_crate == "")
                rv.m_source_crate = m_crate_name;
            return rv;
        }
        ::MacroPatEnt deserialise_macropatent() {
            ::MacroPatEnt   rv {
                m_in.read_string(),
                static_cast<unsigned int>(m_in.read_count()),
                static_cast< ::MacroPatEnt::Type>(m_in.read_tag())
                };
            switch(rv.type)
            {
            case ::MacroPatEnt::PAT_TOKEN:
                rv.tok = deserialise_token();
                break;
            case ::MacroPatEnt::PAT_LOOP:
                rv.tok = deserialise_token();
                rv.subpats = deserialise_vec_c< ::MacroPatEnt>([&](){ return deserialise_macropatent(); });
                break;
            case ::MacroPatEnt::PAT_TT: // :tt
            case ::MacroPatEnt::PAT_PAT:    // :pat
            case ::MacroPatEnt::PAT_IDENT:
            case ::MacroPatEnt::PAT_PATH:
            case ::MacroPatEnt::PAT_TYPE:
            case ::MacroPatEnt::PAT_EXPR:
            case ::MacroPatEnt::PAT_STMT:
            case ::MacroPatEnt::PAT_BLOCK:
            case ::MacroPatEnt::PAT_META:
            case ::MacroPatEnt::PAT_ITEM:
                break;
            default:
                throw "";
            }
            return rv;
        }
        ::MacroRulesArm deserialise_macrorulesarm() {
            ::MacroRulesArm rv;
            rv.m_param_names = deserialise_vec< ::std::string>();
            rv.m_pattern = deserialise_vec_c< ::MacroPatEnt>( [&](){ return deserialise_macropatent(); } );
            rv.m_contents = deserialise_vec_c< ::MacroExpansionEnt>( [&](){ return deserialise_macroexpansionent(); } );
            return rv;
        }
        ::MacroExpansionEnt deserialise_macroexpansionent() {
            switch(m_in.read_tag())
            {
            case 0:
                return ::MacroExpansionEnt( deserialise_token() );
            case 1: {
                unsigned int v = static_cast<unsigned int>(m_in.read_u8()) << 24;
                return ::MacroExpansionEnt( v | m_in.read_count() );
                }
            case 2: {
                auto entries = deserialise_vec_c< ::MacroExpansionEnt>( [&](){ return deserialise_macroexpansionent(); } );
                auto joiner = deserialise_token();
                ::std::map<unsigned int, bool>    variables;
                size_t n = m_in.read_count();
                while(n--) {
                    auto idx = static_cast<unsigned int>(m_in.read_count());
                    bool flag = m_in.read_bool();
                    variables.insert( ::std::make_pair(idx, flag) );
                }
                return ::MacroExpansionEnt::make_Loop({
                    mv$(entries), mv$(joiner), mv$(variables)
                    });
                }
            default:
                throw "";
            }
        }
        
        ::Token deserialise_token() {
            ::Token tok;
            // HACK: Hand off to old serialiser code
            auto s = m_in.read_string();
            ::std::stringstream tmp(s);
            {
                Deserialiser_TextTree ser(tmp);
                tok.deserialise( ser );
            }
            return tok;
        }

        ::HIR::Literal deserialise_literal();
        
        ::HIR::ExprPtr deserialise_exprptr()
        {
            ::HIR::ExprPtr  rv;
            if( m_in.read_bool() )
            {
                rv.m_mir = deserialise_mir();
            }
            rv.m_erased_types = deserialise_vec< ::HIR::TypeRef>();
            return rv;
        }
        ::MIR::FunctionPointer deserialise_mir();
        ::MIR::BasicBlock deserialise_mir_basicblock();
        ::MIR::Statement deserialise_mir_statement();
        ::MIR::Terminator deserialise_mir_terminator();
        ::MIR::CallTarget deserialise_mir_calltarget();
        
        ::MIR::LValue deserialise_mir_lvalue() {
            ::MIR::LValue   rv;
            TRACE_FUNCTION_FR("", rv);
            rv = deserialise_mir_lvalue_();
            return rv;
        }
        ::MIR::LValue deserialise_mir_lvalue_()
        {
            switch(auto tag = m_in.read_tag())
            {
            #define _(x, ...)    case ::MIR::LValue::TAG_##x: return ::MIR::LValue::make_##x( __VA_ARGS__ );
            _(Variable,  static_cast<unsigned int>(m_in.read_count()) )
            _(Temporary, { static_cast<unsigned int>(m_in.read_count()) } )
            _(Argument,  { static_cast<unsigned int>(m_in.read_count()) } )
            _(Static,  deserialise_path() )
            _(Return, {})
            _(Field, {
                box$( deserialise_mir_lvalue() ),
                static_cast<unsigned int>(m_in.read_count())
                } )
            _(Deref, { box$( deserialise_mir_lvalue() ) })
            _(Index, {
                box$( deserialise_mir_lvalue() ),
                box$( deserialise_mir_lvalue() )
                } )
            _(Downcast, {
                box$( deserialise_mir_lvalue() ),
                static_cast<unsigned int>(m_in.read_count())
                } )
            #undef _
            default:
                throw ::std::runtime_error(FMT("Invalid MIR LValue tag - " << tag));
            }
        }
        ::MIR::RValue deserialise_mir_rvalue()
        {
            TRACE_FUNCTION;
            
            switch( m_in.read_tag() )
            {
            #define _(x, ...)    case ::MIR::RValue::TAG_##x: return ::MIR::RValue::make_##x( __VA_ARGS__ );
            _(Use, deserialise_mir_lvalue() )
            _(Constant, deserialise_mir_constant() )
            _(SizedArray, {
                deserialise_mir_lvalue(),
                static_cast<unsigned int>(m_in.read_u64c())
                })
            _(Borrow, {
                0, // TODO: Region?
                static_cast< ::HIR::BorrowType>( m_in.read_tag() ),
                deserialise_mir_lvalue()
                })
            _(Cast, {
                deserialise_mir_lvalue(),
                deserialise_type()
                })
            _(BinOp, {
                deserialise_mir_lvalue(),
                static_cast< ::MIR::eBinOp>( m_in.read_tag() ),
                deserialise_mir_lvalue()
                })
            _(UniOp, {
                deserialise_mir_lvalue(),
                static_cast< ::MIR::eUniOp>( m_in.read_tag() )
                })
            _(DstMeta, {
                deserialise_mir_lvalue()
                })
            _(DstPtr, {
                deserialise_mir_lvalue()
                })
            _(MakeDst, {
                deserialise_mir_lvalue(),
                deserialise_mir_lvalue()
                })
            _(Tuple, {
                deserialise_vec_c< ::MIR::LValue>([&](){ return deserialise_mir_lvalue(); })
                })
            _(Array, {
                deserialise_vec_c< ::MIR::LValue>([&](){ return deserialise_mir_lvalue(); })
                })
            _(Variant, {
                deserialise_genericpath(),
                static_cast<unsigned int>( m_in.read_count() ),
                deserialise_mir_lvalue()
                })
            _(Struct, {
                deserialise_genericpath(),
                static_cast<unsigned int>( m_in.read_count() ),
                deserialise_vec_c< ::MIR::LValue>([&](){ return deserialise_mir_lvalue(); })
                })
            #undef _
            default:
                throw "";
            }
        }
        ::MIR::Constant deserialise_mir_constant()
        {
            TRACE_FUNCTION;
            
            switch( m_in.read_tag() )
            {
            #define _(x, ...)    case ::MIR::Constant::TAG_##x: DEBUG("- " #x); return ::MIR::Constant::make_##x( __VA_ARGS__ );
            _(Int, m_in.read_i64c())
            _(Uint, m_in.read_u64c())
            _(Float, m_in.read_double())
            _(Bool, m_in.read_bool())
            case ::MIR::Constant::TAG_Bytes: {
                ::std::vector<unsigned char>    bytes;
                bytes.resize( m_in.read_count() );
                m_in.read( bytes.data(), bytes.size() );
                return ::MIR::Constant::make_Bytes( mv$(bytes) );
                }
            _(StaticString, m_in.read_string() )
            _(Const,  { deserialise_path() } )
            _(ItemAddr, deserialise_path() )
            #undef _
            default:
                throw "";
            }
        }
        
        ::HIR::TypeItem deserialise_typeitem()
        {
            switch( m_in.read_tag() )
            {
            case 0:
                return ::HIR::TypeItem::make_Import({ deserialise_simplepath(), m_in.read_bool(), static_cast<unsigned int>(m_in.read_count()) });
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
            case 6:
                return ::HIR::TypeItem( deserialise_union() );
            default:
                throw "";
            }
        }
        ::HIR::ValueItem deserialise_valueitem()
        {
            switch( m_in.read_tag() )
            {
            case 0:
                return ::HIR::ValueItem::make_Import({ deserialise_simplepath(), m_in.read_bool(), static_cast<unsigned int>(m_in.read_count()) });
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
            TRACE_FUNCTION;
            
            ::HIR::Function rv {
                static_cast< ::HIR::Function::Receiver>( m_in.read_tag() ),
                m_in.read_string(),
                m_in.read_bool(),
                m_in.read_bool(),
                deserialise_genericparams(),
                deserialise_fcnargs(),
                m_in.read_bool(),
                deserialise_type(),
                deserialise_exprptr()
                };
            return rv;
        }
        ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >   deserialise_fcnargs()
        {
            size_t n = m_in.read_count();
            ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >    rv;
            rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
                rv.push_back( ::std::make_pair( ::HIR::Pattern{}, deserialise_type() ) );
            DEBUG("rv = " << rv);
            return rv;
        }
        ::HIR::Constant deserialise_constant()
        {
            TRACE_FUNCTION;
            
            return ::HIR::Constant {
                deserialise_genericparams(),
                deserialise_type(),
                deserialise_exprptr(),
                deserialise_literal()
                };
        }
        ::HIR::Static deserialise_static()
        {
            TRACE_FUNCTION;
            
            return ::HIR::Static {
                m_in.read_bool(),
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
        ::HIR::TraitMarkings deserialise_markings()
        {
            ::HIR::TraitMarkings    m;
            uint8_t bitflag_1 = m_in.read_u8();
            #define BIT(i,fld)  fld = (bitflag_1 & (1 << (i))) != 0;
            BIT(0, m.can_coerce)
            BIT(1, m.can_unsize)
            BIT(2, m.has_a_deref)
            BIT(3, m.is_always_unsized)
            BIT(4, m.is_always_sized)
            BIT(5, m.is_copy)
            #undef BIT
            // TODO: auto_impls
            return m;
        }
        
        ::HIR::Enum deserialise_enum();
        ::HIR::Enum::Variant deserialise_enumvariant();

        ::HIR::Struct deserialise_struct();
        ::HIR::Union deserialise_union();
        ::HIR::Trait deserialise_trait();
        
        ::HIR::TraitValueItem deserialise_traitvalueitem()
        {
            switch( m_in.read_tag() )
            {
            #define _(x, ...)    case ::HIR::TraitValueItem::TAG_##x: DEBUG("- " #x); return ::HIR::TraitValueItem::make_##x( __VA_ARGS__ ); break;
            _(Constant, deserialise_constant() )
            _(Static,   deserialise_static() )
            _(Function, deserialise_function() )
            #undef _
            default:
                DEBUG("Invalid TraitValueItem tag");
                throw "";
            }
        }
        ::HIR::AssociatedType deserialise_associatedtype()
        {
            return ::HIR::AssociatedType {
                m_in.read_bool(),
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
        auto a = D<T>::des(d);
        return ::std::make_pair( mv$(a), D<U>::des(d) );
        }};
    
    template<typename T>
    DEF_D( ::HIR::VisEnt<T>,
        return d.deserialise_visent<T>(); )
    
    template<> DEF_D( ::HIR::TypeRef, return d.deserialise_type(); )
    template<> DEF_D( ::HIR::SimplePath, return d.deserialise_simplepath(); )
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
    
    template<> DEF_D( ::MIR::LValue, return d.deserialise_mir_lvalue(); )
    template<> DEF_D( ::MIR::Statement, return d.deserialise_mir_statement(); )
    template<> DEF_D( ::MIR::BasicBlock, return d.deserialise_mir_basicblock(); )
    
    template<> DEF_D( ::HIR::TypeImpl, return d.deserialise_typeimpl(); )
    template<> DEF_D( ::MacroRulesPtr, return d.deserialise_macrorulesptr(); )
    template<> DEF_D( unsigned int, return static_cast<unsigned int>(d.deserialise_count()); )
    
    ::HIR::TypeRef HirDeserialiser::deserialise_type()
    {
        TRACE_FUNCTION;
        switch( auto tag = m_in.read_tag() )
        {
        #define _(x, ...)    case ::HIR::TypeRef::Data::TAG_##x: DEBUG("- "#x); return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_##x( __VA_ARGS__ ) );
        _(Infer, {})
        _(Diverge, {})
        _(Primitive,
            static_cast< ::HIR::CoreType>( m_in.read_tag() )
            )
        _(Path, {
            deserialise_path(),
            {}
            })
        _(Generic, {
            m_in.read_string(),
            m_in.read_u16()
            })
        _(TraitObject, {
            deserialise_traitpath(),
            deserialise_vec< ::HIR::GenericPath>(),
            ""  // TODO: m_lifetime
            })
        _(ErasedType, {
            deserialise_path(),
            static_cast<unsigned int>(m_in.read_count()),
            deserialise_vec< ::HIR::TraitPath>(),
            ""  // TODO: m_lifetime
            })
        _(Array, {
            deserialise_ptr< ::HIR::TypeRef>(),
            nullptr,
            m_in.read_u64c()
            })
        _(Slice, {
            deserialise_ptr< ::HIR::TypeRef>()
            })
        _(Tuple,
            deserialise_vec< ::HIR::TypeRef>()
            )
        _(Borrow, {
            static_cast< ::HIR::BorrowType>( m_in.read_tag() ),
            deserialise_ptr< ::HIR::TypeRef>()
            })
        _(Pointer, {
            static_cast< ::HIR::BorrowType>( m_in.read_tag() ),
            deserialise_ptr< ::HIR::TypeRef>()
            })
        _(Function, {
            m_in.read_bool(),
            m_in.read_string(),
            deserialise_ptr< ::HIR::TypeRef>(),
            deserialise_vec< ::HIR::TypeRef>()
            })
        #undef _
        default:
            throw ::std::runtime_error(FMT("Bad TypeRef tag - " << tag));
        }
    }
    
    ::HIR::SimplePath HirDeserialiser::deserialise_simplepath()
    {
        TRACE_FUNCTION;
        // HACK! If the read crate name is empty, replace it with the name we're loaded with
        auto crate_name = m_in.read_string();
        auto components = deserialise_vec< ::std::string>();
        if( crate_name == "" && components.size() > 0)
            crate_name = m_crate_name;
        return ::HIR::SimplePath {
            mv$(crate_name),
            mv$(components)
            };
    }
    ::HIR::PathParams HirDeserialiser::deserialise_pathparams()
    {
        ::HIR::PathParams   rv;
        TRACE_FUNCTION_FR("", rv);
        rv.m_types = deserialise_vec< ::HIR::TypeRef>();
        return rv;
    }
    ::HIR::GenericPath HirDeserialiser::deserialise_genericpath()
    {
        TRACE_FUNCTION;
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
        TRACE_FUNCTION;
        switch( m_in.read_tag() )
        {
        case 0:
            DEBUG("Generic");
            return ::HIR::Path( deserialise_genericpath() );
        case 1:
            DEBUG("Inherent");
            return ::HIR::Path( ::HIR::Path::Data::Data_UfcsInherent {
                box$( deserialise_type() ),
                m_in.read_string(),
                deserialise_pathparams(),
                deserialise_pathparams()
                } );
        case 2:
            DEBUG("Known");
            return ::HIR::Path( ::HIR::Path::Data::Data_UfcsKnown {
                box$( deserialise_type() ),
                deserialise_genericpath(),
                m_in.read_string(),
                deserialise_pathparams()
                } );
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
            m_in.read_string(),
            deserialise_type(),
            m_in.read_bool()
            };
    }
    ::HIR::GenericBound HirDeserialiser::deserialise_genericbound()
    {
        switch( m_in.read_tag() )
        {
        case 0:
        case 1:
            return ::HIR::GenericBound::make_Lifetime({});
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
            DEBUG("Bad GenericBound tag");
            throw "";
        }
    }
    
    ::HIR::Enum HirDeserialiser::deserialise_enum()
    {
        TRACE_FUNCTION;
        return ::HIR::Enum {
            deserialise_genericparams(),
            static_cast< ::HIR::Enum::Repr>(m_in.read_tag()),
            deserialise_vec< ::std::pair< ::std::string, ::HIR::Enum::Variant> >(),
            deserialise_markings()
            };
    }
    ::HIR::Enum::Variant HirDeserialiser::deserialise_enumvariant()
    {
        switch( m_in.read_tag() )
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
    ::HIR::Union HirDeserialiser::deserialise_union()
    {
        TRACE_FUNCTION;
        auto params = deserialise_genericparams();
        auto repr = static_cast< ::HIR::Union::Repr>( m_in.read_tag() );
        auto variants = deserialise_vec< ::std::pair< ::std::string, ::HIR::VisEnt< ::HIR::TypeRef> > >();
        auto markings = deserialise_markings();
        
        return ::HIR::Union {
            mv$(params), repr, mv$(variants), mv$(markings)
            };
    }
    ::HIR::Struct HirDeserialiser::deserialise_struct()
    {
        TRACE_FUNCTION;
        auto params = deserialise_genericparams();
        auto repr = static_cast< ::HIR::Struct::Repr>( m_in.read_tag() );
        DEBUG("params = " << params.fmt_args() << params.fmt_bounds());
        
        switch( m_in.read_tag() )
        {
        case ::HIR::Struct::Data::TAG_Unit:
            DEBUG("Unit");
            return ::HIR::Struct {
                mv$(params), repr,
                ::HIR::Struct::Data::make_Unit( {} ),
                deserialise_markings()
                };
        case ::HIR::Struct::Data::TAG_Tuple:
            DEBUG("Tuple");
            return ::HIR::Struct {
                mv$(params), repr,
                ::HIR::Struct::Data( deserialise_vec< ::HIR::VisEnt< ::HIR::TypeRef> >() ),
                deserialise_markings()
                };
        case ::HIR::Struct::Data::TAG_Named:
            DEBUG("Named");
            return ::HIR::Struct {
                mv$(params), repr,
                ::HIR::Struct::Data( deserialise_vec< ::std::pair< ::std::string, ::HIR::VisEnt< ::HIR::TypeRef> > >() ),
                deserialise_markings()
                };
        default:
            throw "";
        }
    }
    ::HIR::Trait HirDeserialiser::deserialise_trait()
    {
        TRACE_FUNCTION;
        
        ::HIR::Trait rv {
            deserialise_genericparams(),
            "",  // TODO: Better type for lifetime
            deserialise_vec< ::HIR::TraitPath>()
            };
        rv.m_is_marker = m_in.read_bool();
        rv.m_types = deserialise_strumap< ::HIR::AssociatedType>();
        rv.m_values = deserialise_strumap< ::HIR::TraitValueItem>();
        rv.m_value_indexes = deserialise_strummap< ::std::pair<unsigned int, ::HIR::GenericPath> >();
        rv.m_type_indexes = deserialise_strumap< unsigned int>();
        return rv;
    }
    
    ::HIR::Literal HirDeserialiser::deserialise_literal()
    {
        switch( m_in.read_tag() )
        {
        #define _(x, ...)    case ::HIR::Literal::TAG_##x:   return ::HIR::Literal::make_##x(__VA_ARGS__);
        _(Invalid, {})
        _(List,   deserialise_vec< ::HIR::Literal>() )
        _(Variant, {
            static_cast<unsigned int>(m_in.read_count()),
            deserialise_vec< ::HIR::Literal>()
            })
        _(Integer, m_in.read_u64() )
        _(Float,   m_in.read_double() )
        _(BorrowOf, deserialise_path() )
        _(String,  m_in.read_string() )
        #undef _
        default:
            throw "";
        }
    }
    
    ::MIR::FunctionPointer HirDeserialiser::deserialise_mir()
    {
        TRACE_FUNCTION;
        
        ::MIR::Function rv;
        
        rv.named_variables = deserialise_vec< ::HIR::TypeRef>( );
        DEBUG("named_variables = " << rv.named_variables);
        rv.temporaries = deserialise_vec< ::HIR::TypeRef>( );
        DEBUG("temporaries = " << rv.temporaries);
        rv.blocks = deserialise_vec< ::MIR::BasicBlock>( );
        
        return ::MIR::FunctionPointer( new ::MIR::Function(mv$(rv)) );
    }
    ::MIR::BasicBlock HirDeserialiser::deserialise_mir_basicblock()
    {
        TRACE_FUNCTION;
        
        return ::MIR::BasicBlock {
            deserialise_vec< ::MIR::Statement>(),
            deserialise_mir_terminator()
            };
    }
    ::MIR::Statement HirDeserialiser::deserialise_mir_statement()
    {
        TRACE_FUNCTION;
        
        switch( m_in.read_tag() )
        {
        case 0:
            return ::MIR::Statement::make_Assign({
                deserialise_mir_lvalue(),
                deserialise_mir_rvalue()
                });
        case 1:
            return ::MIR::Statement::make_Drop({
                m_in.read_bool() ? ::MIR::eDropKind::DEEP : ::MIR::eDropKind::SHALLOW,
                deserialise_mir_lvalue()
                });
        default:
            ::std::cerr << "Bad tag for a MIR Statement" << ::std::endl;
            throw "";
        }
    }
    ::MIR::Terminator HirDeserialiser::deserialise_mir_terminator()
    {
        TRACE_FUNCTION;
        
        switch( m_in.read_tag() )
        {
        #define _(x, ...)    case ::MIR::Terminator::TAG_##x: return ::MIR::Terminator::make_##x( __VA_ARGS__ );
        _(Incomplete, {})
        _(Return, {})
        _(Diverge, {})
        _(Goto,  static_cast<unsigned int>(m_in.read_count()) )
        _(Panic, { static_cast<unsigned int>(m_in.read_count()) })
        _(If, {
            deserialise_mir_lvalue(),
            static_cast<unsigned int>(m_in.read_count()),
            static_cast<unsigned int>(m_in.read_count())
            })
        _(Switch, {
            deserialise_mir_lvalue(),
            deserialise_vec_c<unsigned int>([&](){ return m_in.read_count(); })
            })
        _(Call, {
            static_cast<unsigned int>(m_in.read_count()),
            static_cast<unsigned int>(m_in.read_count()),
            deserialise_mir_lvalue(),
            deserialise_mir_calltarget(),
            deserialise_vec< ::MIR::LValue>()
            })
        #undef _
        default:
            throw "";
        }
    }
    
    ::MIR::CallTarget HirDeserialiser::deserialise_mir_calltarget()
    {
        switch( m_in.read_tag() )
        {
        #define _(x, ...)    case ::MIR::CallTarget::TAG_##x: return ::MIR::CallTarget::make_##x( __VA_ARGS__ );
        _(Value, deserialise_mir_lvalue() )
        _(Path, deserialise_path() )
        _(Intrinsic, m_in.read_string() )
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
        
        rv.m_type_impls = deserialise_vec< ::HIR::TypeImpl>();
        
        {
            size_t n = m_in.read_count();
            for(size_t i = 0; i < n; i ++)
            {
                auto p = deserialise_simplepath();
                rv.m_trait_impls.insert( ::std::make_pair( mv$(p), deserialise_traitimpl() ) );
            }
        }
        {
            size_t n = m_in.read_count();
            for(size_t i = 0; i < n; i ++)
            {
                auto p = deserialise_simplepath();
                rv.m_marker_impls.insert( ::std::make_pair( mv$(p), deserialise_markerimpl() ) );
            }
        }
        
        rv.m_exported_macros = deserialise_strumap< ::MacroRulesPtr>();
        rv.m_lang_items = deserialise_strumap< ::HIR::SimplePath>();
        
        {
            size_t n = m_in.read_count();
            for(size_t i = 0; i < n; i ++)
            {
                auto ext_crate_name = m_in.read_string();
                rv.m_ext_crates.insert( ::std::make_pair(ext_crate_name, ::HIR::CratePtr{}) );
            }
        }
        
        return rv;
    }
}

::HIR::CratePtr HIR_Deserialise(const ::std::string& filename, const ::std::string& loaded_name)
{
    ::HIR::serialise::Reader    in { filename };
    HirDeserialiser  s { loaded_name, in };
    
    try
    {
        ::HIR::Crate    rv = s.deserialise_crate();
        
        return ::HIR::CratePtr( mv$(rv) );
    }
    catch(int)
    { ::std::abort(); }
    #if 0
    catch(const char*)
    {
        ::std::cerr << "Unable to load crate from " << filename << ": Deserialisation failure" << ::std::endl;
        ::std::abort();
        //return ::HIR::CratePtr();
    }
    #endif
}

