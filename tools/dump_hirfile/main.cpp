#include <hir/hir.hpp>
#include <hir/item_path.hpp>
#include <hir/main_bindings.hpp>
#include <macro_rules/macro_rules.hpp>
#include <mir/mir.hpp>
#include <mir/operations.hpp>   // MIR_Dump_Fcn

int g_debug_indent_level = 0;

struct Args
{
    Args(int argc, const char* const argv[]);

    ::std::string   infile;
};

struct Dumper
{
    struct Filters {
        struct Types {
            bool    macros = false;
            bool    functions = false;
            bool    statics = false;
            bool    types = false;
        } types;
        bool public_only = false;
    } filters;

    void dump_crate(const char* name, const ::HIR::Crate& crate) const;
    void dump_module(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Module& mod) const;
    void dump_function(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Function& fcn, int indent=0) const;
};

int main(int argc, const char* argv[])
{
    Args    args(argc, argv);
    Dumper  dumper;

    dumper.filters.types.functions = true;

    auto hir = HIR_Deserialise(args.infile);
    dumper.dump_crate("", *hir);
}
void Dumper::dump_crate(const char* name, const ::HIR::Crate& crate) const
{
    // Dump macros
    for(const auto& mac : crate.m_exported_macros)
    {
        ::std::cout << "macro_rules! " << mac.first << "{" << std::endl;
        for(const auto& arm : mac.second->m_rules)
        {
            ::std::cout << "    (";
            for(const auto& pat : arm.m_pattern)
            {
                TU_MATCH_HDRA( (pat), {)
                TU_ARMA(End, e)
                    ::std::cout << " EOS";
                TU_ARMA(LoopStart, e)
                    ::std::cout << " (";
                TU_ARMA(LoopNext, e)
                    ::std::cout << " ^";
                TU_ARMA(LoopEnd, e)
                    ::std::cout << " )";
                TU_ARMA(Jump, e)
                    ::std::cout << " <" << e.jump_target;
                TU_ARMA(ExpectTok, e)
                    ::std::cout << " =" << e;
                TU_ARMA(ExpectPat, e)
                    ::std::cout << " " << e.idx << "=" << e.type;
                TU_ARMA(If, e) {
                    ::std::cout << " ?" << (e.is_equal ? "" : "!") << "{";
                    for(const auto& ent : e.ents) {
                        if(ent.ty == MacroPatEnt::PAT_TOKEN)
                            ::std::cout << " =" << ent.tok;
                        else
                            ::std::cout << " " << ent.ty;
                    }
                    ::std::cout << "}->" << e.jump_target;
                    }
                }
            }
            ::std::cout << " ) => {\n";
            // TODO: Macro expansion
            ::std::cout << "    }\n";
        }
        ::std::cout << "}\n";
        ::std::cout << ::std::endl;
    }

    this->dump_module(::HIR::ItemPath(name), ::HIR::Publicity::new_global(), crate.m_root_module);

    for(const auto& i : crate.m_trait_impls)
    {
        auto root_ip = ::HIR::ItemPath(i.second.m_type, i.first, i.second.m_trait_args);
        ::std::cout << "impl" << i.second.m_params.fmt_args() << " " << i.first << i.second.m_trait_args << " for " << i.second.m_type << "\n";
        ::std::cout << "  where" << i.second.m_params.fmt_bounds() << "\n";
        ::std::cout << "{" << ::std::endl;
        if( this->filters.types.functions )
        {
            for(const auto& m : i.second.m_methods)
            {
                this->dump_function(root_ip + m.first, ::HIR::Publicity::new_global(), m.second.data, 1);
            }
        }
        ::std::cout << "}" << ::std::endl;
    }

    for(const auto& i : crate.m_type_impls)
    {
        auto root_ip = ::HIR::ItemPath(i.m_type);
        ::std::cout << "impl" << i.m_params.fmt_args() << " " << i.m_type << "\n";
        ::std::cout << "  where" << i.m_params.fmt_bounds() << "\n";
        ::std::cout << "{" << ::std::endl;
        if( this->filters.types.functions )
        {
            for(const auto& m : i.m_methods)
            {
                this->dump_function(root_ip + m.first, ::HIR::Publicity::new_global(), m.second.data, 1);
            }
        }
        ::std::cout << "}" << ::std::endl;
    }
}
void Dumper::dump_module(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Module& mod) const
{
    if( !filters.public_only && !pub.is_global() )
    {
        return ;
    }
    for(const auto& i : mod.m_mod_items)
    {
        auto sub_ip = ip + i.first;
        //::std::cout << "// " << i.second->ent.tagstr() << " " << sub_ip << "\n";
        TU_MATCH_HDRA( (i.second->ent), {)
        TU_ARMA(Module, e) {
            this->dump_module(sub_ip, i.second->publicity, e);
            }
        TU_ARMA(Import, e) {
            //this->dump_mod_import(sub_ip, e);
            }
        TU_ARMA(TypeAlias, e) {
            //this->dump_type_alias(sub_ip, e);
            }
        TU_ARMA(ExternType, e) {
            //this->dump_ext_type(sub_ip, e);
            }
        TU_ARMA(Enum, e) {
            //this->dump_enum(sub_ip, e);
            }
        TU_ARMA(Struct, e) {
            //this->dump_enum(sub_ip, e);
            }
        TU_ARMA(Union, e) {
            //this->dump_enum(sub_ip, e);
            }
        TU_ARMA(Trait, e) {
            //this->dump_enum(sub_ip, e);
            }
        }
    }
    for(const auto& i : mod.m_value_items)
    {
        auto sub_ip = ip + i.first;
        //::std::cout << "// " << i.second->ent.tagstr() << " " << sub_ip << "\n";
        TU_MATCH_HDRA( (i.second->ent), {)
        TU_ARMA(Import, e) {
            //this->dump_val_import(sub_ip, e);
            }
        TU_ARMA(Constant, e) {
            //this->dump_constant(sub_ip, e);
            }
        TU_ARMA(Static, e) {
            //this->dump_constant(sub_ip, e);
            }
        TU_ARMA(StructConstant, e) {
            //this->dump_constant(sub_ip, e);
            }
        TU_ARMA(StructConstructor, e) {
            //this->dump_constant(sub_ip, e);
            }
        TU_ARMA(Function, e) {
            this->dump_function(sub_ip, i.second->publicity, e);
            }
        }
    }
}
void Dumper::dump_function(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Function& fcn, int nindent/*=0*/) const
{
    auto indent = RepeatLitStr { "   ", nindent };
    if( !this->filters.types.functions ) {
        return ;
    }
    if( !filters.public_only && !pub.is_global() ) {
        return ;
    }
    ::std::cout << indent << "fn " << ip << fcn.m_params.fmt_args() << "(";
    ::std::cout << " )";
    if( fcn.m_code.m_mir )
    {
        ::std::cout << "\n";
        ::std::cout << indent << "{\n";
        MIR_Dump_Fcn(::std::cout, *fcn.m_code.m_mir, nindent+1);
        ::std::cout << indent << "}\n";
        ::std::cout << ::std::endl;
    }
    else
    {
        ::std::cout << ";" << ::std::endl;
    }
}

bool debug_enabled()
{
    return false;
}
::std::ostream& debug_output(int indent, const char* function)
{
    return ::std::cout << "- " << RepeatLitStr { " ", indent } << function << ": ";
}

Args::Args(int argc, const char* const argv[])
{
    this->infile = argv[1];
}

// TODO: This is copy-pasted from src/main.cpp, should live somewhere better
::std::ostream& operator<<(::std::ostream& os, const FmtEscaped& x)
{
    os << ::std::hex;
    for(auto s = x.s; *s != '\0'; s ++)
    {
        switch(*s)
        {
        case '\0':  os << "\\0";    break;
        case '\n':  os << "\\n";    break;
        case '\\':  os << "\\\\";   break;
        case '"':   os << "\\\"";   break;
        default:
            uint8_t v = *s;
            if( v < 0x80 )
            {
                if( v < ' ' || v > 0x7F )
                    os << "\\u{" << ::std::hex << (unsigned int)v << "}";
                else
                    os << v;
            }
            else if( v < 0xC0 )
                ;
            else if( v < 0xE0 )
            {
                uint32_t    val = (uint32_t)(v & 0x1F) << 6;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 6;
                os << "\\u{" << ::std::hex << val << "}";
            }
            else if( v < 0xF0 )
            {
                uint32_t    val = (uint32_t)(v & 0x0F) << 12;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 12;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 6;
                os << "\\u{" << ::std::hex << val << "}";
            }
            else if( v < 0xF8 )
            {
                uint32_t    val = (uint32_t)(v & 0x07) << 18;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 18;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 12;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 6;
                os << "\\u{" << ::std::hex << val << "}";
            }
            break;
        }
    }
    os << ::std::dec;
    return os;
}

MIR::EnumCachePtr::~EnumCachePtr()
{
    assert(!this->p);
}
