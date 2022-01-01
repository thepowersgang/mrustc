/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise.cpp
 * - HIR (De)Serialisation for crate metadata
 */
// TODO: Have an environment variable that controls if debug is enabled here.
#define DEBUG_EXTRA_ENABLE && des_debug_enabled()
namespace {
    bool des_debug_enabled();
}
//#define DISABLE_DEBUG   //  Disable debug for this function - too hot
#include "hir.hpp"
#include "main_bindings.hpp"
#include <mir/mir.hpp>
#include <macro_rules/macro_rules.hpp>
#include "serialise_lowlevel.hpp"
#include <typeinfo>

namespace {
    bool des_debug_enabled() {
        static unsigned enabled = 0;
        if( enabled == 0 ) {
            enabled = (getenv("MRUSTC_DEBUG_DESERIALISE") ? 2 : 1);
        }
        return enabled > 1;
    }
}

//namespace {

    template<typename T>
    struct D
    {
    };

    class HirDeserialiser
    {
        RcString m_crate_name;
        ::std::vector<HIR::TypeRef> m_types;
        ::HIR::serialise::Reader&   m_in;
    public:
        HirDeserialiser(::HIR::serialise::Reader& in):
            m_in(in)
        {}

        RcString read_istring() { return m_in.read_istring(); }
        ::std::string read_string() { return m_in.read_string(); }
        bool read_bool() { return m_in.read_bool(); }
        uint8_t read_u8() { return m_in.read_u8(); }
        size_t deserialise_count() { return m_in.read_count(); }

        template<typename V>
        ::std::map< ::std::string,V> deserialise_strmap()
        {
            TRACE_FUNCTION_F("<" << typeid(V).name() << ">");
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
            TRACE_FUNCTION_F("<" << typeid(V).name() << ">");
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
            TRACE_FUNCTION_F("<" << typeid(V).name() << ">");
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

        template<typename V>
        ::std::map< RcString,V> deserialise_istrmap()
        {
            TRACE_FUNCTION_F("<" << typeid(V).name() << ">");
            size_t n = m_in.read_count();
            ::std::map< RcString, V>   rv;
            //rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
            {
                auto s = m_in.read_istring();
                rv.insert( ::std::make_pair( mv$(s), D<V>::des(*this) ) );
            }
            return rv;
        }
        template<typename V>
        ::std::unordered_map< RcString,V> deserialise_istrumap()
        {
            TRACE_FUNCTION_F("<" << typeid(V).name() << ">");
            size_t n = m_in.read_count();
            ::std::unordered_map<RcString, V>   rv;
            //rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
            {
                auto s = m_in.read_istring();
                DEBUG("- " << s);
                rv.insert( ::std::make_pair( mv$(s), D<V>::des(*this) ) );
            }
            return rv;
        }
        template<typename V>
        ::std::unordered_multimap<RcString,V> deserialise_istrummap()
        {
            TRACE_FUNCTION_F("<" << typeid(V).name() << ">");
            size_t n = m_in.read_count();
            ::std::unordered_multimap<RcString, V>   rv;
            //rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
            {
                auto s = m_in.read_istring();
                DEBUG("- " << s);
                rv.insert( ::std::make_pair( mv$(s), D<V>::des(*this) ) );
            }
            return rv;
        }
        template<typename V>
        ::std::map< ::HIR::SimplePath,V> deserialise_pathmap()
        {
            TRACE_FUNCTION_F("<" << typeid(V).name() << ">");
            size_t n = m_in.read_count();
            ::std::map< ::HIR::SimplePath, V>   rv;
            //rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
            {
                auto s = deserialise_simplepath();
                rv.insert( ::std::make_pair( mv$(s), D<V>::des(*this) ) );
            }
            return rv;
        }

        template<typename T>
        ::std::vector<T> deserialise_vec_c(::std::function<T()> cb)
        {
            TRACE_FUNCTION_FR("<" << typeid(T).name() << ">", m_in.get_pos());
            auto _ = m_in.open_object(typeid(::std::vector<T>).name());
            size_t n = m_in.read_count();
            DEBUG("n = " << n);
            ::std::vector<T>    rv;
            rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
                rv.push_back( cb() );
            return rv;
        }
        template<typename T>
        ::std::vector<T> deserialise_vec()
        {
            return deserialise_vec_c<T>([&](){ return D<T>::des(*this); });
        }

        template<typename T>
        ::std::set<T> deserialise_set()
        {
            TRACE_FUNCTION_FR("<" << typeid(T).name() << ">", m_in.get_pos());
            auto _ = m_in.open_object(typeid(::std::set<T>).name());
            size_t n = m_in.read_count();
            DEBUG("n = " << n);
            ::std::set<T>    rv;
            //rv.reserve(n);
            for(size_t i = 0; i < n; i ++)
                rv.insert( D<T>::des(*this) );
            return rv;
        }


        ::HIR::Publicity deserialise_pub()
        {
            return (m_in.read_bool() ? ::HIR::Publicity::new_global() : ::HIR::Publicity::new_none());
        }
        template<typename T>
        ::HIR::VisEnt<T> deserialise_visent()
        {
            return ::HIR::VisEnt<T> { deserialise_pub(), D<T>::des(*this) };
        }

        template<typename T>
        ::std::unique_ptr<T> deserialise_ptr() {
            return box$( D<T>::des(*this) );
        }


        ::HIR::LifetimeDef deserialise_lifetimedef();
        ::HIR::LifetimeRef deserialise_lifetimeref();
        ::HIR::ArraySize deserialise_arraysize();
        ::HIR::GenericRef deserialise_genericref();
        ::HIR::TypeRef deserialise_type();
        ::HIR::SimplePath deserialise_simplepath();
        ::HIR::PathParams deserialise_pathparams();
        ::HIR::GenericPath deserialise_genericpath();
        ::HIR::TraitPath deserialise_traitpath();
        ::HIR::Path deserialise_path();

        ::HIR::GenericParams deserialise_genericparams();
        ::HIR::TypeParamDef deserialise_typaramdef();
        ::HIR::ValueParamDef deserialise_valueparamdef();
        ::HIR::GenericBound deserialise_genericbound();

        ::HIR::Crate deserialise_crate();
        ::HIR::ExternLibrary deserialise_extlib();
        ::HIR::Module deserialise_module();

        ::HIR::ProcMacro deserialise_procmacro()
        {
            ::HIR::ProcMacro    pm;
            TRACE_FUNCTION_FR("", "ProcMacro { " << pm.name << ", " << pm.path << ", [" << pm.attributes << "]}");
            pm.name = m_in.read_istring();
            pm.path = deserialise_simplepath();
            pm.attributes = deserialise_vec< ::std::string>();
            DEBUG("pm = ProcMacro { " << pm.name << ", " << pm.path << ", [" << pm.attributes << "]}");
            return pm;
        }

        ::HIR::TypeImpl deserialise_typeimpl()
        {
            ::HIR::TypeImpl rv;
            TRACE_FUNCTION_FR("", "impl" << rv.m_params.fmt_args() << " " << rv.m_type);

            rv.m_params = deserialise_genericparams();
            rv.m_type = deserialise_type();

            size_t method_count = m_in.read_count();
            for(size_t i = 0; i < method_count; i ++)
            {
                auto name = m_in.read_istring();
                rv.m_methods.insert( ::std::make_pair( mv$(name), ::HIR::TypeImpl::VisImplEnt< ::HIR::Function> {
                    deserialise_pub(), m_in.read_bool(), deserialise_function()
                    } ) );
            }
            size_t const_count = m_in.read_count();
            for(size_t i = 0; i < const_count; i ++)
            {
                auto name = m_in.read_istring();
                rv.m_constants.insert( ::std::make_pair( mv$(name), ::HIR::TypeImpl::VisImplEnt< ::HIR::Constant> {
                    deserialise_pub(), m_in.read_bool(), deserialise_constant()
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
            DEBUG("impl" << rv.m_params.fmt_args() << " ?" << rv.m_trait_args << " for " << rv.m_type);


            size_t method_count = m_in.read_count();
            for(size_t i = 0; i < method_count; i ++)
            {
                auto name = m_in.read_istring();
                auto is_spec = m_in.read_bool();
                DEBUG((is_spec ? "default " : "") << "fn " << name);
                rv.m_methods.insert( ::std::make_pair( mv$(name), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> {
                    is_spec, deserialise_function()
                    } ) );
            }
            size_t const_count = m_in.read_count();
            for(size_t i = 0; i < const_count; i ++)
            {
                auto name = m_in.read_istring();
                auto is_spec = m_in.read_bool();
                DEBUG((is_spec ? "default " : "") << "const " << name);
                rv.m_constants.insert( ::std::make_pair( mv$(name), ::HIR::TraitImpl::ImplEnt< ::HIR::Constant> {
                    is_spec, deserialise_constant()
                    } ) );
            }
            size_t static_count = m_in.read_count();
            for(size_t i = 0; i < static_count; i ++)
            {
                auto name = m_in.read_istring();
                auto is_spec = m_in.read_bool();
                DEBUG((is_spec ? "default " : "") << "static " << name);
                rv.m_statics.insert( ::std::make_pair( mv$(name), ::HIR::TraitImpl::ImplEnt< ::HIR::Static> {
                    is_spec, deserialise_static()
                    } ) );
            }
            size_t type_count = m_in.read_count();
            for(size_t i = 0; i < type_count; i ++)
            {
                auto name = m_in.read_istring();
                auto is_spec = m_in.read_bool();
                DEBUG((is_spec ? "default " : "") << "type " << name);
                rv.m_types.insert( ::std::make_pair( mv$(name), ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> {
                    is_spec, deserialise_type()
                    } ) );
            }

            // m_src_module doesn't matter after typeck
            return rv;
        }
        ::HIR::MarkerImpl deserialise_markerimpl()
        {
            auto generics = deserialise_genericparams();
            auto params = deserialise_pathparams();
            auto is_neg = m_in.read_bool();
            auto ty = deserialise_type();
            return ::HIR::MarkerImpl { mv$(generics), mv$(params), is_neg, mv$(ty) };
        }

        Ident::Hygiene deserialise_hygine()
        {
            auto _ = m_in.open_object(typeid(Ident::Hygiene).name());
            Ident::Hygiene  rv;
            bool has_mod_path = m_in.read_bool();
            if(has_mod_path)
            {
                Ident::ModPath  mp;
                mp.crate = m_in.read_istring();
                mp.ents = deserialise_vec<RcString>();

                if(mp.crate == "")
                {
                    assert(m_crate_name != "");
                    mp.crate = m_crate_name;
                }
                rv.set_mod_path(mv$(mp));
            }
            return rv;
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
            rv.m_source_crate = m_in.read_istring();
            rv.m_hygiene = deserialise_hygine();
            if(rv.m_source_crate == "")
            {
                assert(m_crate_name != "");
                rv.m_source_crate = m_crate_name;
            }
            return rv;
        }
        ::SimplePatIfCheck deserialise_simplepatifcheck() {
            return ::SimplePatIfCheck {
                static_cast<::MacroPatEnt::Type>(m_in.read_tag()),
                deserialise_token()
                };
        }
        ::SimplePatEnt deserialise_simplepatent() {
            auto tag = static_cast< ::SimplePatEnt::Tag>( m_in.read_tag() );
            switch(tag)
            {
            case ::SimplePatEnt::TAG_End:
                return ::SimplePatEnt::make_End({});
            case ::SimplePatEnt::TAG_LoopStart:
                return ::SimplePatEnt::make_LoopStart({ static_cast<unsigned>(m_in.read_count()) });
            case ::SimplePatEnt::TAG_LoopNext:
                return ::SimplePatEnt::make_LoopNext({});
            case ::SimplePatEnt::TAG_LoopEnd:
                return ::SimplePatEnt::make_LoopEnd({});
            case ::SimplePatEnt::TAG_Jump:
                return ::SimplePatEnt::make_Jump({ m_in.read_count() });
            case ::SimplePatEnt::TAG_ExpectTok:
                return SimplePatEnt::make_ExpectTok({
                    deserialise_token()
                    });
            case ::SimplePatEnt::TAG_ExpectPat:
                return SimplePatEnt::make_ExpectPat({
                    static_cast<::MacroPatEnt::Type>(m_in.read_tag()),
                    static_cast<unsigned>(m_in.read_count())
                    });
            case SimplePatEnt::TAG_If:
                return SimplePatEnt::make_If({
                    m_in.read_bool(),
                    m_in.read_count(),
                    deserialise_vec_c< SimplePatIfCheck>([&](){ return deserialise_simplepatifcheck(); })
                    });
            default:
                BUG(Span(), "Bad tag for MacroPatEnt - #" << static_cast<int>(tag));
            }
        }
        ::MacroPatEnt deserialise_macropatent() {
            auto s = m_in.read_istring();
            auto n = static_cast<unsigned int>(m_in.read_count());
            auto type = static_cast< ::MacroPatEnt::Type>(m_in.read_tag());
            ::MacroPatEnt   rv(Span(), mv$(s), mv$(n), mv$(type));
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
            case ::MacroPatEnt::PAT_VIS:
                break;
            default:
                BUG(Span(), "Bad tag for MacroPatEnt - #" << static_cast<int>(rv.type) << " " << rv.type);
            }
            return rv;
        }
        ::MacroRulesArm deserialise_macrorulesarm() {
            ::MacroRulesArm rv;
            rv.m_param_names = deserialise_vec<RcString>();
            rv.m_pattern = deserialise_vec_c< ::SimplePatEnt>( [&](){ return deserialise_simplepatent(); } );
            rv.m_contents = deserialise_vec_c< ::MacroExpansionEnt>( [&](){ return deserialise_macroexpansionent(); } );
            return rv;
        }
        ::MacroExpansionEnt deserialise_macroexpansionent() {
            switch(auto tag = m_in.read_tag())
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
                auto controllers = deserialise_set<unsigned int>();

                return ::MacroExpansionEnt::make_Loop({
                    mv$(entries), mv$(joiner), mv$(controllers)
                    });
                }
            default:
                BUG(Span(), "Bad tag for MacroExpansionEnt - " << tag);
            }
        }

        ::Token deserialise_token() {
            auto ty = static_cast<enum eTokenType>( m_in.read_tag() );
            auto d = deserialise_tokendata();
            return ::Token(ty, ::std::move(d), {});
        }
        ::Token::Data deserialise_tokendata() {
            auto tag = static_cast< ::Token::Data::Tag>( m_in.read_tag() );
            switch(tag)
            {
            case ::Token::Data::TAG_None:
                return ::Token::Data::make_None({});
            case ::Token::Data::TAG_String:
                return ::Token::Data::make_String( m_in.read_string() );
            case ::Token::Data::TAG_Ident: {
                auto hygine = deserialise_hygine();
                auto name = m_in.read_istring();
                return ::Token::Data::make_Ident( Ident(std::move(hygine), std::move(name)) );
                }
            case ::Token::Data::TAG_Integer: {
                auto dty = static_cast<eCoreType>(m_in.read_tag());
                return ::Token::Data::make_Integer({ dty, m_in.read_u64c() });
                }
            case ::Token::Data::TAG_Float: {
                auto dty = static_cast<eCoreType>(m_in.read_tag());
                return ::Token::Data::make_Float({ dty, m_in.read_double() });
                }
            default:
                BUG(Span(), "Bad tag for Token::Data - " << static_cast<int>(tag));
            }
        }

        ::HIR::ConstGeneric deserialise_constgeneric();
        EncodedLiteral deserialise_encodedliteral();

        ::HIR::ExprPtr deserialise_exprptr()
        {
            ::HIR::ExprPtr  rv;
            auto _ = m_in.open_object("HIR::ExprPtr");
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
        AsmCommon::Options deserialise_asm_options();
        AsmCommon::LineFragment deserialise_asm_line_frag();
        AsmCommon::Line deserialise_asm_line();
        AsmCommon::RegisterSpec deserialise_asm_spec();
        ::MIR::AsmParam deserialise_asm_param();
        ::MIR::Terminator deserialise_mir_terminator();
        ::MIR::Terminator deserialise_mir_terminator_();
        ::MIR::SwitchValues deserialise_mir_switchvalues();
        ::MIR::CallTarget deserialise_mir_calltarget();

        ::MIR::Param deserialise_mir_param()
        {
            switch(auto tag = m_in.read_tag())
            {
            case ::MIR::Param::TAG_LValue:  return deserialise_mir_lvalue();
            case ::MIR::Param::TAG_Borrow:
                return ::MIR::Param::make_Borrow({
                    static_cast< ::HIR::BorrowType>( m_in.read_tag() ),
                    deserialise_mir_lvalue()
                    });
            case ::MIR::Param::TAG_Constant: return deserialise_mir_constant();
            default:
                BUG(Span(), "Bad tag for MIR::Param - " << tag);
            }
        }
        ::MIR::LValue deserialise_mir_lvalue() {
            ::MIR::LValue   rv;
            TRACE_FUNCTION_FR("", rv);
            rv = deserialise_mir_lvalue_();
            return rv;
        }
        ::MIR::LValue::Wrapper deserialise_mir_lvalue_wrapper()
        {
            return ::MIR::LValue::Wrapper::from_inner(m_in.read_count());
        }
        ::MIR::LValue deserialise_mir_lvalue_()
        {
            auto root_v = m_in.read_count();
            auto root = (root_v == 3 ? ::MIR::LValue::Storage::new_Static(deserialise_path()) : ::MIR::LValue::Storage::from_inner(root_v));
            return ::MIR::LValue( mv$(root), deserialise_vec<::MIR::LValue::Wrapper>() );
        }
        ::MIR::RValue deserialise_mir_rvalue()
        {
            TRACE_FUNCTION;

            switch(auto tag = m_in.read_tag())
            {
            #define _(x, ...)    case ::MIR::RValue::TAG_##x: return ::MIR::RValue::make_##x( __VA_ARGS__ );
            _(Use, deserialise_mir_lvalue() )
            _(Constant, deserialise_mir_constant() )
            _(SizedArray, {
                deserialise_mir_param(),
                deserialise_arraysize()
                })
            _(Borrow, {
                static_cast< ::HIR::BorrowType>( m_in.read_tag() ),
                deserialise_mir_lvalue()
                })
            _(Cast, {
                deserialise_mir_lvalue(),
                deserialise_type()
                })
            _(BinOp, {
                deserialise_mir_param(),
                static_cast< ::MIR::eBinOp>( m_in.read_tag() ),
                deserialise_mir_param()
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
                deserialise_mir_param(),
                m_in.read_bool() ? deserialise_mir_param() : MIR::Constant::make_ItemAddr({})
                })
            _(Tuple, {
                deserialise_vec< ::MIR::Param>()
                })
            _(Array, {
                deserialise_vec< ::MIR::Param>()
                })
            _(UnionVariant, {
                deserialise_genericpath(),
                static_cast<unsigned int>( m_in.read_count() ),
                deserialise_mir_param()
                })
            _(EnumVariant, {
                deserialise_genericpath(),
                static_cast<unsigned int>( m_in.read_count() ),
                deserialise_vec< ::MIR::Param>()
                })
            _(Struct, {
                deserialise_genericpath(),
                deserialise_vec< ::MIR::Param>()
                })
            #undef _
            default:
                BUG(Span(), "Bad tag for MIR::RValue - " << tag);
            }
        }
        ::MIR::Constant deserialise_mir_constant()
        {
            TRACE_FUNCTION;

            switch(auto tag = m_in.read_tag())
            {
            #define _(x, ...)    case ::MIR::Constant::TAG_##x: DEBUG("- " #x); return ::MIR::Constant::make_##x( __VA_ARGS__ );
            _(Int, {
                m_in.read_i64c(),
                static_cast< ::HIR::CoreType>(m_in.read_tag())
                })
            _(Uint, {
                m_in.read_u64c(),
                static_cast< ::HIR::CoreType>(m_in.read_tag())
                })
            _(Float, {
                m_in.read_double(),
                static_cast< ::HIR::CoreType>(m_in.read_tag())
                })
            _(Bool, {
                m_in.read_bool()
                })
            case ::MIR::Constant::TAG_Bytes: {
                ::std::vector<unsigned char>    bytes;
                bytes.resize( m_in.read_count() );
                m_in.read( bytes.data(), bytes.size() );
                return ::MIR::Constant::make_Bytes( mv$(bytes) );
                }
            _(StaticString, m_in.read_string() )
            _(Const,  { box$(deserialise_path()) } )
            _(Generic,  deserialise_genericref())
            _(ItemAddr, box$(deserialise_path()) )
            #undef _
            default:
                BUG(Span(), "Bad tag for MIR::Const - " << tag);
            }
        }

        ::HIR::ExternType deserialise_externtype()
        {
            return ::HIR::ExternType {
                deserialise_markings()
                };
        }

        ::HIR::TraitAlias deserialise_traitalias()
        {
            return ::HIR::TraitAlias {
                deserialise_genericparams(),
                deserialise_vec<HIR::TraitPath>()
                };
        }

        ::HIR::TypeItem deserialise_typeitem()
        {
            switch(auto tag = m_in.read_tag())
            {
            case 0: {
                auto spath = deserialise_simplepath();
                auto is_variant = m_in.read_bool();
                return ::HIR::TypeItem::make_Import({ mv$(spath), is_variant, static_cast<unsigned int>(m_in.read_count()) });
                }
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
            case 7:
                return ::HIR::TypeItem( deserialise_externtype() );
            case 8:
                return ::HIR::TypeItem( deserialise_traitalias() );
            default:
                BUG(Span(), "Bad tag for HIR::TypeItem - " << tag);
            }
        }
        ::HIR::ValueItem deserialise_valueitem()
        {
            switch(auto tag = m_in.read_tag())
            {
            case 0: {
                auto spath = deserialise_simplepath();
                auto is_variant = m_in.read_bool();
                return ::HIR::ValueItem::make_Import({ mv$(spath), is_variant, static_cast<unsigned int>(m_in.read_count()) });
                }
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
                BUG(Span(), "Bad tag for HIR::ValueItem - " << tag);
            }
        }

        ::HIR::MacroItem deserialise_macroitem()
        {
            auto _ = m_in.open_object("HIR::MacroItem");
            auto tag = m_in.read_tag();
            switch(tag)
            {
            case HIR::MacroItem::TAG_Import:
                return HIR::MacroItem::Data_Import { 
                    deserialise_simplepath()
                    };
            case HIR::MacroItem::TAG_MacroRules:
                return deserialise_macrorulesptr();
            case HIR::MacroItem::TAG_ProcMacro:
                return deserialise_procmacro();
            }

            TODO(Span(), "Bad tag for MacroItem - " << tag);
        }

        ::HIR::Linkage deserialise_linkage()
        {
            ::HIR::Linkage  l;
            l.type = ::HIR::Linkage::Type::Auto;
            l.name = m_in.read_string();
            return l;
        }

        // - Value items
        ::HIR::Function deserialise_function()
        {
            TRACE_FUNCTION;
            auto _ = m_in.open_object("HIR::Function");

            ::HIR::Function rv;
            rv.m_save_code = false;
            rv.m_linkage = deserialise_linkage();
            rv.m_receiver = static_cast< ::HIR::Function::Receiver>( m_in.read_tag() );
            rv.m_receiver_type = deserialise_type();
            rv.m_abi = m_in.read_string();
            rv.m_unsafe = m_in.read_bool();
            rv.m_const = m_in.read_bool();
            rv.m_markings = deserialise_function_markings();
            rv.m_params = deserialise_genericparams();
            rv.m_args = deserialise_fcnargs();
            rv.m_variadic = m_in.read_bool();
            rv.m_return = deserialise_type();
            rv.m_code = deserialise_exprptr();
            return rv;
        }
        ::HIR::Function::Markings deserialise_function_markings()
        {
            auto _ = m_in.open_object("HIR::Function::Markings");
            ::HIR::Function::Markings rv;
            rv.rustc_legacy_const_generics = deserialise_vec<unsigned>();
            rv.track_caller = m_in.read_bool();
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

            ::HIR::Constant rv;
            rv.m_params = deserialise_genericparams();
            rv.m_type = deserialise_type();
            rv.m_value = deserialise_exprptr();
            if(m_in.read_bool())
            {
                rv.m_value_res = deserialise_encodedliteral();
                rv.m_value_state = ::HIR::Constant::ValueState::Known;
            }
            else
            {
                rv.m_value_state = ::HIR::Constant::ValueState::Generic;
            }
            return rv;
        }
        ::HIR::Static deserialise_static()
        {
            TRACE_FUNCTION;

            auto linkage = deserialise_linkage();
            uint8_t bitflag_1 = m_in.read_u8();
            #define BIT(i,fld)  fld = (bitflag_1 & (1 << (i))) != 0;
            bool is_mut;
            bool save_literal;
            BIT(0, is_mut);
            BIT(1, save_literal);
            #undef BIT
            auto ty = deserialise_type();
            auto rv = ::HIR::Static(mv$(linkage), is_mut, mv$(ty), {});
            if(save_literal)
            {
                rv.m_value_res = deserialise_encodedliteral();
                rv.m_value_generated = true;
                rv.m_no_emit_value = true;
            }
            return rv;
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
            BIT(0, m.has_a_deref)
            BIT(1, m.is_copy)
            BIT(2, m.has_drop_impl)
            #undef BIT
            // TODO: auto_impls
            return m;
        }
        ::HIR::StructMarkings deserialise_str_markings()
        {
            ::HIR::StructMarkings    m;
            uint8_t bitflag_1 = m_in.read_u8();
            #define BIT(i,fld)  fld = (bitflag_1 & (1 << (i))) != 0;
            BIT(0, m.can_unsize)
            BIT(1, m.is_nonzero)
            BIT(2, m.bounded_max)
            #undef BIT
            m.dst_type = static_cast< ::HIR::StructMarkings::DstType>( m_in.read_tag() );
            m.coerce_unsized = static_cast<::HIR::StructMarkings::Coerce>( m_in.read_tag() );
            m.coerce_unsized_index = m_in.read_count( );
            m.coerce_param = m_in.read_count( );
            m.unsized_field = m_in.read_count( );
            m.unsized_param = m_in.read_count();
            if(m.bounded_max)
                m.bounded_max_value = m_in.read_u64();
            // TODO: auto_impls
            return m;
        }


        ::HIR::Enum deserialise_enum();
        ::HIR::Enum::DataVariant deserialise_enumdatavariant();
        ::HIR::Enum::ValueVariant deserialise_enumvaluevariant();

        ::HIR::Struct deserialise_struct();
        ::HIR::Union deserialise_union();
        ::HIR::Trait deserialise_trait();

        ::HIR::TraitValueItem deserialise_traitvalueitem()
        {
            switch(auto tag = m_in.read_tag())
            {
            #define _(x, ...)    case ::HIR::TraitValueItem::TAG_##x: DEBUG("- " #x); return ::HIR::TraitValueItem::make_##x( __VA_ARGS__ ); break;
            _(Constant, deserialise_constant() )
            _(Static,   deserialise_static() )
            _(Function, deserialise_function() )
            #undef _
            default:
                BUG(Span(), "Bad tag for HIR::TraitValueItem - " << tag);
            }
        }
        ::HIR::AssociatedType deserialise_associatedtype()
        {
            return ::HIR::AssociatedType {
                m_in.read_bool(),
                deserialise_lifetimeref(),
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
    template<>
    DEF_D( RcString,
        return d.read_istring(); );
    template<>
    DEF_D( bool,
        return d.read_bool(); );
    template<> DEF_D( uint8_t, return d.read_u8(); );

    template<typename T>
    DEF_D( ::std::unique_ptr<T>,
        return d.deserialise_ptr<T>(); )

    template<typename T>
    DEF_D( ::std::vector<T>,
        return d.deserialise_vec<T>(); )
    template<typename T, typename U>
    struct D< ::std::pair<T,U> > { static ::std::pair<T,U> des(HirDeserialiser& d) {
        auto a = D<T>::des(d);
        return ::std::make_pair( mv$(a), D<U>::des(d) );
        }};

    template<typename T>
    DEF_D( ::HIR::VisEnt<T>,
        return d.deserialise_visent<T>(); )

    template<> DEF_D( ::HIR::LifetimeDef, return d.deserialise_lifetimedef(); )
    template<> DEF_D( ::HIR::LifetimeRef, return d.deserialise_lifetimeref(); )
    template<> DEF_D( ::HIR::TypeRef, return d.deserialise_type(); )
    template<> DEF_D( ::HIR::SimplePath, return d.deserialise_simplepath(); )
    template<> DEF_D( ::HIR::GenericPath, return d.deserialise_genericpath(); )
    template<> DEF_D( ::HIR::TraitPath, return d.deserialise_traitpath(); )

    template<> DEF_D( ::HIR::TypeParamDef, return d.deserialise_typaramdef(); )
    template<> DEF_D( ::HIR::ValueParamDef, return d.deserialise_valueparamdef(); )
    template<> DEF_D( ::HIR::GenericBound, return d.deserialise_genericbound(); )

    template<> DEF_D( ::HIR::ValueItem, return d.deserialise_valueitem(); )
    template<> DEF_D( ::HIR::TypeItem, return d.deserialise_typeitem(); )
    template<> DEF_D( ::HIR::MacroItem, return d.deserialise_macroitem(); )

    template<> DEF_D( ::HIR::Enum::ValueVariant, return d.deserialise_enumvaluevariant(); )
    template<> DEF_D( ::HIR::Enum::DataVariant, return d.deserialise_enumdatavariant(); )
    //template<> DEF_D( ::HIR::Literal, return d.deserialise_literal(); )
    template<> DEF_D( ::HIR::ConstGeneric, return d.deserialise_constgeneric(); )

    template<> DEF_D( ::HIR::AssociatedType, return d.deserialise_associatedtype(); )
    template<> DEF_D( ::HIR::TraitValueItem, return d.deserialise_traitvalueitem(); )

    template<> DEF_D( ::MIR::Param, return d.deserialise_mir_param(); )
    template<> DEF_D( ::MIR::LValue::Wrapper, return d.deserialise_mir_lvalue_wrapper(); )
    template<> DEF_D( ::MIR::LValue, return d.deserialise_mir_lvalue(); )
    template<> DEF_D( AsmCommon::LineFragment, return d.deserialise_asm_line_frag(); )
    template<> DEF_D( AsmCommon::Line, return d.deserialise_asm_line(); )
    template<> DEF_D( ::MIR::AsmParam, return d.deserialise_asm_param(); )
    template<> DEF_D( ::MIR::Statement, return d.deserialise_mir_statement(); )
    template<> DEF_D( ::MIR::BasicBlock, return d.deserialise_mir_basicblock(); )

    template<> DEF_D( ::HIR::TraitPath::AtyEqual,
        auto src = d.deserialise_genericpath();
        return ::HIR::TraitPath::AtyEqual {
            mv$(src),
            d.deserialise_type()
        };
    )
    template<> DEF_D( ::HIR::TraitPath::AtyBound,
        auto src = d.deserialise_genericpath();
        return ::HIR::TraitPath::AtyBound {
            mv$(src),
            d.deserialise_vec<HIR::TraitPath>()
        };
    );

    template<> DEF_D( ::HIR::ProcMacro, return d.deserialise_procmacro(); )
    template<> DEF_D( ::HIR::TypeImpl, return d.deserialise_typeimpl(); )
    template<> DEF_D( ::HIR::TraitImpl, return d.deserialise_traitimpl(); )
    template<> DEF_D( ::HIR::MarkerImpl, return d.deserialise_markerimpl(); )
    template<> DEF_D( ::MacroRulesPtr, return d.deserialise_macrorulesptr(); )
    template<> DEF_D( unsigned int, return static_cast<unsigned int>(d.deserialise_count()); )

    template<typename T>
    DEF_D( ::HIR::Crate::ImplGroup<std::unique_ptr<T>>,
        ::HIR::Crate::ImplGroup<std::unique_ptr<T>>  rv;
        rv.named = d.deserialise_pathmap< ::std::vector<::std::unique_ptr<T> > >();
        rv.non_named = d.deserialise_vec< ::std::unique_ptr<T> >();
        rv.generic = d.deserialise_vec< ::std::unique_ptr<T> >();
        return rv;
        )
    template<> DEF_D( ::HIR::ExternLibrary, return d.deserialise_extlib(); )

    ::HIR::LifetimeDef HirDeserialiser::deserialise_lifetimedef()
    {
        ::HIR::LifetimeDef  rv;
        rv.m_name = m_in.read_istring();
        return rv;
    }
    ::HIR::LifetimeRef HirDeserialiser::deserialise_lifetimeref()
    {
        ::HIR::LifetimeRef  rv;
        rv.binding = static_cast<uint32_t>(m_in.read_count());
        return rv;
    }

    ::HIR::GenericRef HirDeserialiser::deserialise_genericref()
    {
        return HIR::GenericRef {
            m_in.read_istring(),
            m_in.read_u16()
        };
    }

    ::HIR::ArraySize HirDeserialiser::deserialise_arraysize()
    {
        switch(auto tag = m_in.read_tag())
        {
        #define _(x, ...)   case ::HIR::ArraySize::TAG_##x: DEBUG("- "#x); return HIR::ArraySize::make_##x(__VA_ARGS__);
        _(Known, m_in.read_u64c())
        _(Unevaluated,
            deserialise_constgeneric()
            )
        default:
            BUG(Span(), "Bad tag for HIR::ArraySize - " << tag);
        #undef _
        }
    }

    ::HIR::TypeRef HirDeserialiser::deserialise_type()
    {
        ::HIR::TypeRef  rv;
        TRACE_FUNCTION_FR("", rv);

        auto idx = m_in.read_count();
        if( idx != ~0u ) {
            DEBUG("#" << idx << "");
            rv = m_types.at(idx).clone();
            return rv;
        }
        else {
            DEBUG("Fresh (=" << m_types.size() << ")");
        }
        auto _ = m_in.open_object("HIR::TypeData");

        switch( auto tag = m_in.read_tag() )
        {
        #define _(x, ...)    case ::HIR::TypeData::TAG_##x: DEBUG("- "#x); rv = ::HIR::TypeRef( ::HIR::TypeData::make_##x( __VA_ARGS__ ) ); break;
        _(Infer, { ~0u, HIR::InferClass::None })
        _(Diverge, {})
        _(Primitive,
            static_cast< ::HIR::CoreType>( m_in.read_tag() )
            )
        _(Path, {
            deserialise_path(),
            {}
            })
        _(Generic, deserialise_genericref())
        _(TraitObject, {
            deserialise_traitpath(),
            deserialise_vec< ::HIR::GenericPath>(),
            deserialise_lifetimeref()
            })
        _(ErasedType, {
            deserialise_path(),
            static_cast<unsigned int>(m_in.read_count()),
            m_in.read_bool(),
            deserialise_vec< ::HIR::TraitPath>(),
            deserialise_lifetimeref()
            })
        _(Array, {
            deserialise_type(),
            deserialise_arraysize()
            })
        _(Slice, {
            deserialise_type()
            })
        _(Tuple,
            deserialise_vec< ::HIR::TypeRef>()
            )
        _(Borrow, {
            deserialise_lifetimeref(),
            static_cast< ::HIR::BorrowType>( m_in.read_tag() ),
            deserialise_type()
            })
        _(Pointer, {
            static_cast< ::HIR::BorrowType>( m_in.read_tag() ),
            deserialise_type()
            })
        _(Function, {
            m_in.read_bool(),
            m_in.read_string(),
            deserialise_type(),
            deserialise_vec< ::HIR::TypeRef>()
            })
        #undef _
        default:
            BUG(Span(), "Bad tag for HIR::TypeRef - " << tag);
        }
        m_types.push_back(rv.clone());
        return rv;
    }

    ::HIR::SimplePath HirDeserialiser::deserialise_simplepath()
    {
        TRACE_FUNCTION;
        // HACK! If the read crate name is empty, replace it with the name we're loaded with
        auto crate_name = m_in.read_istring();
        auto components = deserialise_vec< RcString>();
        if( crate_name == "" && components.size() > 0)
        {
            assert(m_crate_name != "");
            crate_name = m_crate_name;
        }
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
        rv.m_values = deserialise_vec< ::HIR::ConstGeneric>();
        return rv;
    }
    ::HIR::GenericPath HirDeserialiser::deserialise_genericpath()
    {
        TRACE_FUNCTION;
        auto spath = deserialise_simplepath();
        auto params = deserialise_pathparams();
        return ::HIR::GenericPath { mv$(spath), mv$(params) };
    }

    ::HIR::TraitPath HirDeserialiser::deserialise_traitpath()
    {
        auto _ = m_in.open_object("HIR::TraitPath");
        auto gpath = deserialise_genericpath();
        auto tys = deserialise_istrmap< ::HIR::TraitPath::AtyEqual>();
        auto bounds = deserialise_istrmap< ::HIR::TraitPath::AtyBound>();
        return ::HIR::TraitPath { mv$(gpath), {}, mv$(tys), mv$(bounds) };
    }
    ::HIR::Path HirDeserialiser::deserialise_path()
    {
        TRACE_FUNCTION;
        switch(auto tag = m_in.read_tag())
        {
        case 0:
            DEBUG("Generic");
            return ::HIR::Path( deserialise_genericpath() );
        case 1:
            DEBUG("Inherent");
            return ::HIR::Path( ::HIR::Path::Data::Data_UfcsInherent {
                deserialise_type(),
                m_in.read_istring(),
                deserialise_pathparams(),
                deserialise_pathparams()
                } );
        case 2:
            DEBUG("Known");
            return ::HIR::Path( ::HIR::Path::Data::Data_UfcsKnown {
                deserialise_type(),
                deserialise_genericpath(),
                m_in.read_istring(),
                deserialise_pathparams()
                } );
        default:
            BUG(Span(), "Bad tag for HIR::Path - " << tag);
        }
    }

    ::HIR::GenericParams HirDeserialiser::deserialise_genericparams()
    {
        TRACE_FUNCTION;
        ::HIR::GenericParams    params;
        params.m_types = deserialise_vec< ::HIR::TypeParamDef>();
        params.m_values = deserialise_vec< ::HIR::ValueParamDef>();
        params.m_lifetimes = deserialise_vec< ::HIR::LifetimeDef>();
        params.m_bounds = deserialise_vec< ::HIR::GenericBound>();
        DEBUG("params = " << params.fmt_args() << ", " << params.fmt_bounds());
        return params;
    }
    ::HIR::TypeParamDef HirDeserialiser::deserialise_typaramdef()
    {
        auto rv = ::HIR::TypeParamDef {
            m_in.read_istring(),
            deserialise_type(),
            m_in.read_bool()
            };
        DEBUG("::HIR::TypeParamDef { " << rv.m_name << ", " << rv.m_default << ", " << rv.m_is_sized << "}");
        return rv;
    }
    ::HIR::ValueParamDef HirDeserialiser::deserialise_valueparamdef()
    {
        auto rv = ::HIR::ValueParamDef {
            m_in.read_istring(),
            deserialise_type()
            };
        DEBUG("::HIR::ValueParamDef { " << rv.m_name << ", " << rv.m_type << "}");
        return rv;
    }
    ::HIR::GenericBound HirDeserialiser::deserialise_genericbound()
    {
        switch(auto tag = m_in.read_tag())
        {
        case 0:
            return ::HIR::GenericBound::make_Lifetime({});
        case 1:
            return ::HIR::GenericBound::make_TypeLifetime({});
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
            BUG(Span(), "Bad tag for HIR::GenericBound - " << tag);
        }
    }

    ::HIR::Enum HirDeserialiser::deserialise_enum()
    {
        TRACE_FUNCTION;
        auto _ = m_in.open_object("HIR::Enum");
        struct H {
            static ::HIR::Enum::Class deserialise_enumclass(HirDeserialiser& des) {
                switch( auto tag = des.m_in.read_tag() )
                {
                case ::HIR::Enum::Class::TAG_Data:
                    return ::HIR::Enum::Class::make_Data( des.deserialise_vec<::HIR::Enum::DataVariant>() );
                case ::HIR::Enum::Class::TAG_Value:
                    return ::HIR::Enum::Class::make_Value({
                        des.deserialise_vec<::HIR::Enum::ValueVariant>(),
                        true
                        });
                default:
                    BUG(Span(), "Bad tag for HIR::Enum::Class - " << tag);
                }
            }
        };
        return ::HIR::Enum {
            deserialise_genericparams(),
            m_in.read_bool(),
            static_cast< ::HIR::Enum::Repr>(m_in.read_tag()),
            H::deserialise_enumclass(*this),
            deserialise_markings()
            };
    }
    ::HIR::Enum::DataVariant HirDeserialiser::deserialise_enumdatavariant()
    {
        auto name = m_in.read_istring();
        DEBUG("Enum::DataVariant " << name);
        return ::HIR::Enum::DataVariant {
            mv$(name),
            m_in.read_bool(),
            deserialise_type()
            };
    }
    ::HIR::Enum::ValueVariant HirDeserialiser::deserialise_enumvaluevariant()
    {
        auto name = m_in.read_istring();
        DEBUG("Enum::ValueVariant " << name);
        return ::HIR::Enum::ValueVariant {
            mv$(name),
            ::HIR::ExprPtr {},
            m_in.read_u64()
            };
    }
    ::HIR::Union HirDeserialiser::deserialise_union()
    {
        TRACE_FUNCTION;
        auto params = deserialise_genericparams();
        auto repr = static_cast< ::HIR::Union::Repr>( m_in.read_tag() );
        auto variants = deserialise_vec< ::std::pair< RcString, ::HIR::VisEnt< ::HIR::TypeRef> > >();
        auto markings = deserialise_markings();

        return ::HIR::Union {
            mv$(params), repr, mv$(variants), mv$(markings)
            };
    }
    ::HIR::Struct HirDeserialiser::deserialise_struct()
    {
        TRACE_FUNCTION_FR("", m_in.get_pos());
        auto _ = m_in.open_object("HIR::Struct");
        auto params = deserialise_genericparams();
        DEBUG("params = " << params.fmt_args() << params.fmt_bounds());
        auto repr = static_cast< ::HIR::Struct::Repr>( m_in.read_tag() );

        ::HIR::Struct::Data data;
        switch( auto tag = m_in.read_tag() )
        {
        case ::HIR::Struct::Data::TAG_Unit:
            DEBUG("Unit");
            data = ::HIR::Struct::Data::make_Unit( {} );
            break;
        case ::HIR::Struct::Data::TAG_Tuple:
            DEBUG("Tuple");
            data = ::HIR::Struct::Data( deserialise_vec< ::HIR::VisEnt< ::HIR::TypeRef> >() );
            break;
        case ::HIR::Struct::Data::TAG_Named:
            DEBUG("Named");
            data = ::HIR::Struct::Data( deserialise_vec< ::std::pair< RcString, ::HIR::VisEnt< ::HIR::TypeRef> > >() );
            break;
        default:
            BUG(Span(), "Bad tag for HIR::Struct::Data - " << tag);
        }
        unsigned forced_alignment = m_in.read_count();
        unsigned max_field_alignment = m_in.read_count();
        DEBUG("align = " << forced_alignment);
        auto markings = deserialise_markings();
        auto str_markings = deserialise_str_markings();

        auto rv = ::HIR::Struct {
            mv$(params), repr, mv$(data), forced_alignment, mv$(markings), mv$(str_markings)
            };
        rv.m_max_field_alignment = max_field_alignment;
        return rv;
    }
    ::HIR::Trait HirDeserialiser::deserialise_trait()
    {
        TRACE_FUNCTION;
        auto _ = m_in.open_object("HIR::Trait");

        ::HIR::Trait rv {
            deserialise_genericparams(),
            ::HIR::LifetimeRef(),  // TODO: Better type for lifetime
            {}
            };
        rv.m_is_marker = m_in.read_bool();
        rv.m_types = deserialise_istrumap< ::HIR::AssociatedType>();
        rv.m_values = deserialise_istrumap< ::HIR::TraitValueItem>();
        rv.m_value_indexes = deserialise_istrummap< ::std::pair<unsigned int, ::HIR::GenericPath> >();
        rv.m_type_indexes = deserialise_istrumap< unsigned int>();
        rv.m_all_parent_traits = deserialise_vec< ::HIR::TraitPath>();
        rv.m_vtable_path = deserialise_simplepath();
        return rv;
    }
    
    ::HIR::ConstGeneric HirDeserialiser::deserialise_constgeneric()
    {
        switch( auto tag = m_in.read_tag() )
        {
        #define _(x, ...)    case ::HIR::ConstGeneric::TAG_##x:   return ::HIR::ConstGeneric::make_##x(__VA_ARGS__);
        _(Infer, {})
        _(Unevaluated, std::make_shared<HIR::ExprPtr>(deserialise_exprptr()))
        _(Generic,
            deserialise_genericref()
            )
        _(Evaluated,
            HIR::EncodedLiteralPtr(deserialise_encodedliteral())
            )
        #undef _
        default:
            BUG(Span(), "Unknown HIR::ConstGeneric tag when deserialising - " << tag);
        }
    }
    EncodedLiteral HirDeserialiser::deserialise_encodedliteral()
    {
        EncodedLiteral  rv;
        rv.bytes = deserialise_vec<uint8_t>();

        auto nreloc = m_in.read_count();
        rv.relocations.reserve(nreloc);
        for(size_t i = 0; i < nreloc; i ++)
        {
            auto ofs = m_in.read_count();
            auto len = m_in.read_count();
            switch(m_in.read_tag())
            {
            case 0: rv.relocations.push_back( Reloc::new_named(ofs, len, deserialise_path()) ); break;
            case 1: rv.relocations.push_back( Reloc::new_bytes(ofs, len, m_in.read_string()) ); break;
            default:    abort();
            }
        }
        return rv;
    }

    ::MIR::FunctionPointer HirDeserialiser::deserialise_mir()
    {
        TRACE_FUNCTION;

        ::MIR::Function rv;

        rv.locals = deserialise_vec< ::HIR::TypeRef>( );
        //rv.local_names = deserialise_vec< ::std::string>( );
        rv.drop_flags = deserialise_vec<bool>();
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
    AsmCommon::Options HirDeserialiser::deserialise_asm_options()
    {
        AsmCommon::Options  o;
        const uint16_t bitflag_1 = m_in.read_u16();
        #define BIT(i,fld)  if(bitflag_1 & (1 << (i))) fld = true
        BIT(0, o.pure);
        BIT(1, o.nomem);
        BIT(2, o.readonly);
        BIT(3, o.preserves_flags);
        BIT(4, o.noreturn);
        BIT(5, o.nostack);
        BIT(6, o.att_syntax);
        #undef BIT
        return o;
    }
    AsmCommon::LineFragment HirDeserialiser::deserialise_asm_line_frag()
    {
        AsmCommon::LineFragment lf;
        lf.before = m_in.read_string();
        lf.index = m_in.read_count();
        lf.modifier = static_cast<char>(m_in.read_i64c());
        return lf;
    }
    AsmCommon::Line HirDeserialiser::deserialise_asm_line()
    {
        AsmCommon::Line l;
        l.frags = deserialise_vec<AsmCommon::LineFragment>();
        l.trailing = m_in.read_string();
        return l;
    }
    AsmCommon::RegisterSpec HirDeserialiser::deserialise_asm_spec()
    {
        switch(auto tag = m_in.read_tag())
        {
        case AsmCommon::RegisterSpec::TAG_Class:
            return static_cast<AsmCommon::RegisterClass>(m_in.read_tag());
        case AsmCommon::RegisterSpec::TAG_Explicit:
            return m_in.read_string();
        default:
            BUG(Span(), "Bad tag for AsmCommon::RegisterSpec - " << tag);
        }
    }
    ::MIR::AsmParam HirDeserialiser::deserialise_asm_param()
    {
        switch(auto tag = m_in.read_tag())
        {
        case ::MIR::AsmParam::TAG_Sym:
            return ::MIR::AsmParam::make_Sym(deserialise_path());
        case ::MIR::AsmParam::TAG_Const:
            return ::MIR::AsmParam::make_Const(deserialise_mir_constant());
        case ::MIR::AsmParam::TAG_Reg:
            return ::MIR::AsmParam::make_Reg({
                static_cast<AsmCommon::Direction>(m_in.read_tag()),
                deserialise_asm_spec(),
                m_in.read_bool() ? ::std::make_unique<MIR::Param>(deserialise_mir_param()) : std::unique_ptr<MIR::Param>(),
                m_in.read_bool() ? ::std::make_unique<MIR::LValue>(deserialise_mir_lvalue()) : std::unique_ptr<MIR::LValue>()
                });
        default:
            BUG(Span(), "Bad tag for MIR::AsmParam - " << tag);
        }
    }
    ::MIR::Statement HirDeserialiser::deserialise_mir_statement()
    {
        MIR::Statement  rv;
        TRACE_FUNCTION_FR("", rv);
        auto _ = m_in.open_object("MIR::Statement");

        switch( auto tag = m_in.read_tag() )
        {
        case 0:
            rv = ::MIR::Statement::make_Assign({
                deserialise_mir_lvalue(),
                deserialise_mir_rvalue()
                });
            break;
        case 1:
            rv = ::MIR::Statement::make_Drop({
                m_in.read_bool() ? ::MIR::eDropKind::DEEP : ::MIR::eDropKind::SHALLOW,
                deserialise_mir_lvalue(),
                static_cast<unsigned int>(m_in.read_count())
                });
            break;
        case 2:
            rv = ::MIR::Statement::make_Asm({
                m_in.read_string(),
                deserialise_vec< ::std::pair< ::std::string, ::MIR::LValue> >(),
                deserialise_vec< ::std::pair< ::std::string, ::MIR::LValue> >(),
                deserialise_vec< ::std::string>(),
                deserialise_vec< ::std::string>()
                });
            break;
        case 3: {
            ::MIR::Statement::Data_SetDropFlag  sdf;
            sdf.idx = static_cast<unsigned int>(m_in.read_count());
            sdf.new_val = m_in.read_bool();
            sdf.other = static_cast<unsigned int>(m_in.read_count());
            rv = ::MIR::Statement::make_SetDropFlag(sdf);
            }
            break;
        case 4:
            rv = ::MIR::Statement::make_ScopeEnd({
                deserialise_vec<unsigned int>()
                });
            break;
        case 5:
            rv = ::MIR::Statement::make_Asm2({
                deserialise_asm_options(),
                deserialise_vec< AsmCommon::Line>(),
                deserialise_vec< MIR::AsmParam>()
                });
            break;
        default:
            BUG(Span(), "Bad tag for MIR::Statement - " << tag);
        }
        return rv;
    }
    ::MIR::Terminator HirDeserialiser::deserialise_mir_terminator()
    {
        ::MIR::Terminator   rv;
        TRACE_FUNCTION_FR("", rv);
        rv = this->deserialise_mir_terminator_();
        return rv;
    }
    ::MIR::Terminator HirDeserialiser::deserialise_mir_terminator_()
    {
        switch( auto tag = m_in.read_tag() )
        {
        #define _(x, ...)    case ::MIR::Terminator::TAG_##x: return ::MIR::Terminator::make_##x( __VA_ARGS__ );
        case MIR::Terminator::TAGDEAD:
            BUG(Span(), "MIR::Terminator::TAGDEAD found");
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
            deserialise_vec_c<unsigned int>([&](){ return static_cast<unsigned int>(m_in.read_count()); })
            })
        _(SwitchValue, {
            deserialise_mir_lvalue(),
            static_cast<unsigned int>(m_in.read_count()),
            deserialise_vec_c<unsigned int>([&](){ return static_cast<unsigned int>(m_in.read_count()); }),
            deserialise_mir_switchvalues()
            })
        _(Call, {
            static_cast<unsigned int>(m_in.read_count()),
            static_cast<unsigned int>(m_in.read_count()),
            deserialise_mir_lvalue(),
            deserialise_mir_calltarget(),
            deserialise_vec< ::MIR::Param>()
            })
        #undef _
        default:
            BUG(Span(), "Bad tag for MIR::Terminator - " << tag);
        }
    }
    ::MIR::SwitchValues HirDeserialiser::deserialise_mir_switchvalues()
    {
        TRACE_FUNCTION;
        switch(auto tag = m_in.read_tag())
        {
        #define _(x, ...)    case ::MIR::SwitchValues::TAG_##x: return ::MIR::SwitchValues::make_##x( __VA_ARGS__ );
        _(Unsigned, deserialise_vec_c<uint64_t>([&](){ return m_in.read_u64c(); }))
        _(Signed  , deserialise_vec_c< int64_t>([&](){ return m_in.read_i64c(); }))
        _(String  , deserialise_vec<::std::string>())
        _(ByteString, deserialise_vec<::std::vector<uint8_t>>())
        #undef _
        default:
            BUG(Span(), "Bad tag for MIR::SwitchValues - " << tag);
        }
    }

    ::MIR::CallTarget HirDeserialiser::deserialise_mir_calltarget()
    {
        switch(auto tag = m_in.read_tag())
        {
        #define _(x, ...)    case ::MIR::CallTarget::TAG_##x: return ::MIR::CallTarget::make_##x( __VA_ARGS__ );
        _(Value, deserialise_mir_lvalue() )
        _(Path, deserialise_path() )
        _(Intrinsic, {
            m_in.read_istring(),
            deserialise_pathparams()
            })
        #undef _
        default:
            BUG(Span(), "Bad tag for MIR::CallTarget - " << tag);
        }
    }

    ::HIR::Module HirDeserialiser::deserialise_module()
    {
        TRACE_FUNCTION;
        auto _ = m_in.open_object("HIR::Module");

        ::HIR::Module   rv;

        // m_traits doesn't need to be serialised
        rv.m_value_items = deserialise_istrumap< ::std::unique_ptr< ::HIR::VisEnt< ::HIR::ValueItem> > >();
        rv.m_mod_items = deserialise_istrumap< ::std::unique_ptr< ::HIR::VisEnt< ::HIR::TypeItem> > >();
        rv.m_macro_items = deserialise_istrumap< ::std::unique_ptr< ::HIR::VisEnt< ::HIR::MacroItem> > >();

        return rv;
    }
    ::HIR::ExternLibrary HirDeserialiser::deserialise_extlib()
    {
        return ::HIR::ExternLibrary {
            m_in.read_string()
            };
    }
    ::HIR::Crate HirDeserialiser::deserialise_crate()
    {
        ::HIR::Crate    rv;

        // NOTE: This MUST be the first item
        this->m_crate_name = m_in.read_istring();
        assert(this->m_crate_name != "" && "Empty crate name loaded from metadata");
        rv.m_crate_name = this->m_crate_name;
        rv.m_edition = static_cast<AST::Edition>(m_in.read_tag());
        rv.m_root_module = deserialise_module();

        rv.m_type_impls = D< ::HIR::Crate::ImplGroup<std::unique_ptr<::HIR::TypeImpl>> >::des(*this);
        rv.m_trait_impls = deserialise_pathmap< ::HIR::Crate::ImplGroup<std::unique_ptr<::HIR::TraitImpl>>>();
        rv.m_marker_impls = deserialise_pathmap< ::HIR::Crate::ImplGroup<std::unique_ptr<::HIR::MarkerImpl>>>();

        rv.m_exported_macro_names = deserialise_vec< ::RcString>();
        //rv.m_exported_macros = deserialise_istrumap< ::MacroRulesPtr>();
        //rv.m_proc_macro_reexports = deserialise_istrumap< ::HIR::Crate::MacroImport>();
        rv.m_lang_items = deserialise_strumap< ::HIR::SimplePath>();

        {
            size_t n = m_in.read_count();
            for(size_t i = 0; i < n; i ++)
            {
                auto ext_crate_name = m_in.read_istring();
                auto ext_crate_file = m_in.read_string();
                auto ext_crate = ::HIR::ExternCrate {};
                ext_crate.m_basename = ext_crate_file;
                ext_crate.m_path = ext_crate_file;
                rv.m_ext_crates.insert( ::std::make_pair( mv$(ext_crate_name), mv$(ext_crate) ) );
            }
        }

        rv.m_ext_libs = deserialise_vec< ::HIR::ExternLibrary>();
        rv.m_link_paths = deserialise_vec< ::std::string>();

        //rv.m_proc_macros = deserialise_vec< ::HIR::ProcMacro>();

        return rv;
    }
//}

::HIR::CratePtr HIR_Deserialise(const ::std::string& filename)
{
    try
    {
        ::HIR::serialise::Reader    in{ filename + ".hir" };    // HACK!
        HirDeserialiser  s { in };

        ::HIR::Crate    rv = s.deserialise_crate();

        return ::HIR::CratePtr( mv$(rv) );
    }
    catch(int)
    { ::std::abort(); }
    catch(const ::std::runtime_error& e)
    {
        ::std::cerr << "Unable to deserialise crate metadata from " << filename << ": " << e.what() << ::std::endl;
        ::std::abort();
    }
    #if 0
    catch(const char*)
    {
        ::std::cerr << "Unable to load crate from " << filename << ": Deserialisation failure" << ::std::endl;
        ::std::abort();
        //return ::HIR::CratePtr();
    }
    #endif
}

RcString HIR_Deserialise_JustName(const ::std::string& filename)
{
    try
    {
        ::HIR::serialise::Reader    in{ filename + ".hir" };    // HACK!

        // NOTE: This is the first item loaded by deserialise_crate
        auto crate_name = in.read_istring();
        assert(crate_name != "" && "Empty crate name loaded from metadata");
        return crate_name;
    }
    catch(int)
    { ::std::abort(); }
    catch(const ::std::runtime_error& e)
    {
        ::std::cerr << "Unable to deserialise crate metadata from " << filename << ": " << e.what() << ::std::endl;
        ::std::abort();
    }
    #if 0
    catch(const char*)
    {
        ::std::cerr << "Unable to load crate from " << filename << ": Deserialisation failure" << ::std::endl;
        ::std::abort();
        //return ::HIR::CratePtr();
    }
    #endif
}

