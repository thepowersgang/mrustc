#include <hir/hir.hpp>
#include <hir/item_path.hpp>
#include <hir/main_bindings.hpp>
#include <macro_rules/macro_rules.hpp>
#include <mir/mir.hpp>
#include <mir/operations.hpp>   // MIR_Dump_Fcn

TargetVersion gTargetVersion;
//int g_debug_indent_level = 0;

struct Args
{
    Args(int argc, const char* const argv[]);

    ::std::string   infile;
};

struct Dumper
{
    struct Filters {
        struct Types {
            bool    macros = true;

            bool    imports = true;
            bool    functions = true;
            bool    values = true;

            bool    types = true;
            bool    traits = true;
        } types;
        bool public_only = false;
        bool no_body = false;
    } filters;

    void dump_crate(const char* name, const ::HIR::Crate& crate) const;
    void dump_module(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Module& mod, int nindent=0) const;
    void dump_mod_import(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::TypeItem::Data_Import& imp, int nindent=0) const;
    void dump_struct(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Struct& item, int nindent=0) const;
    void dump_trait(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Trait& trait, int nindent=0) const;

    void dump_value_import(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::ValueItem::Data_Import& imp, int nindent=0) const;
    void dump_constant(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Constant& c, int nindent=0) const;
    void dump_function(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Function& fcn, int nindent=0) const;

    void dump_macrorules(const HIR::ItemPath& ip, const MacroRules& rules) const;
};

int main(int argc, const char* argv[])
{
    Args    args(argc, argv);
    Dumper  dumper;

    dumper.filters.types.functions = true;

    auto hir = HIR_Deserialise(args.infile);
    dumper.dump_crate("", *hir);
}
namespace {
    template<typename T, typename Fcn>
    void dump_impl_group(const ::HIR::Crate::ImplGroup<T>& ig, Fcn cb)
    {
        for(const auto& named_il : ig.named)
        {
            for(const auto& impl : named_il.second)
            {
                cb(*impl);
            }
        }
        for(const auto& impl : ig.non_named)
        {
            cb(*impl);
        }
        for(const auto& impl : ig.generic)
        {
            cb(*impl);
        }
    }
}
void Dumper::dump_crate(const char* name, const ::HIR::Crate& crate) const
{
    // Dump macros
#if 0
    for(const auto& mac : crate.m_exported_macros)
    {
        dump_macrorules(mac.first, *mac.second);
    }
#endif

    this->dump_module(::HIR::ItemPath(name), ::HIR::Publicity::new_global(), crate.m_root_module);

    for(const auto& i : crate.m_trait_impls)
    {
        dump_impl_group(i.second, [&](const ::HIR::TraitImpl& ti) {
            auto root_ip = ::HIR::ItemPath(ti.m_type, i.first, ti.m_trait_args);
            ::std::cout << "impl" << ti.m_params.fmt_args() << " " << i.first << ti.m_trait_args << " for " << ti.m_type << "\n";
            ::std::cout << "  where" << ti.m_params.fmt_bounds() << "\n";
            ::std::cout << "{" << ::std::endl;
            if( this->filters.types.functions )
            {
                for(const auto& m : ti.m_methods)
                {
                    this->dump_function(root_ip + m.first, ::HIR::Publicity::new_global(), m.second.data, 1);
                }
            }
            ::std::cout << "}" << ::std::endl;
        });
    }

    dump_impl_group(crate.m_type_impls, [&](const auto& i) {
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
    });
}
void Dumper::dump_module(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Module& mod, int nindent/*=0*/) const
{
    if( filters.public_only && !pub.is_global() )
    {
        return ;
    }
    ::std::cout << "// mod " << ip << ::std::endl;
    for(const auto& i : mod.m_macro_items)
    {
        auto sub_ip = ip + i.first;
        TU_MATCH_HDRA( (i.second->ent), {)
        TU_ARMA(Import, e) {
            ::std::cout << "macro " << sub_ip << " = " << e.path << "\n";
            }
        TU_ARMA(MacroRules, mac) {
            dump_macrorules(sub_ip, *mac);
            }
        TU_ARMA(ProcMacro, mac) {
            // TODO: Attribute list
            ::std::cout << "proc macro " << sub_ip << " = " << mac.path << "\n";
            }
        }
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
            this->dump_mod_import(sub_ip, i.second->publicity, e);
            }
        TU_ARMA(TraitAlias, e) {
            //this->dump_trait_alias(sub_ip, e);
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
            this->dump_struct(sub_ip, i.second->publicity, e);
            }
        TU_ARMA(Union, e) {
            //this->dump_trait(sub_ip, e);
            }
        TU_ARMA(Trait, e) {
            this->dump_trait(sub_ip, i.second->publicity, e);
            }
        }
    }
    for(const auto& i : mod.m_value_items)
    {
        auto sub_ip = ip + i.first;
        //::std::cout << "// " << i.second->ent.tagstr() << " " << sub_ip << "\n";
        TU_MATCH_HDRA( (i.second->ent), {)
        TU_ARMA(Import, e) {
            this->dump_value_import(sub_ip, i.second->publicity, e);
            }
        TU_ARMA(Constant, e) {
            this->dump_constant(sub_ip, i.second->publicity, e);
            }
        TU_ARMA(Static, e) {
            //this->dump_static(sub_ip, e);
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
void Dumper::dump_mod_import(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::TypeItem::Data_Import& imp, int nindent/*=0*/) const
{
    auto indent = RepeatLitStr { "   ", nindent };
    if( !this->filters.types.imports  ) {
        return ;
    }
    if( filters.public_only && !pub.is_global() ) {
        return ;
    }
    ::std::cout << indent << pub << "use[type] " << ip << " = " << imp.path;
    if(imp.is_variant)
    {
        ::std::cout << "#" << imp.idx;
    }
    ::std::cout << "\n";
}
void Dumper::dump_struct(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Struct& item, int nindent/*=0*/) const
{
    auto indent = RepeatLitStr { "   ", nindent };
    if( !this->filters.types.types ) {
        return ;
    }
    if( !filters.public_only && !pub.is_global() ) {
        return ;
    }
    ::std::cout << indent << "struct " << ip << item.m_params.fmt_args();
    TU_MATCH_HDRA( (item.m_data), {)
    TU_ARMA(Unit, se) {
        if( !item.m_params.m_bounds.empty() ) {
            ::std::cout << "\n";
            ::std::cout << indent << item.m_params.fmt_bounds() << "\n";
        }
        ::std::cout << ";\n";
        }
    TU_ARMA(Tuple, se) {
        ::std::cout << "(\n";
        auto indent2 = RepeatLitStr { "   ", nindent+1 };
        for(const auto& f : se)
        {
            ::std::cout << indent2 << f.publicity << " " << f.ent << ",\n";
        }
        ::std::cout << indent2 << ")";
        if( !item.m_params.m_bounds.empty() ) {
            ::std::cout << "\n";
            ::std::cout << indent2 << item.m_params.fmt_bounds() << "\n";
        }
        ::std::cout << ";\n";
        }
    TU_ARMA(Named, se) {
        ::std::cout << "\n";
        if( !item.m_params.m_bounds.empty() ) {
            ::std::cout << indent << item.m_params.fmt_bounds() << "\n";
        }
        ::std::cout << indent << "{\n";
        auto indent2 = RepeatLitStr { "   ", nindent+1 };
        for(const auto& f : se)
        {
            ::std::cout << indent2 << f.second.publicity << " " << f.first << ": " << f.second.ent << ",\n";
        }
        ::std::cout << indent << "}\n";
        }
    }
    ::std::cout << ::std::endl;
}
void Dumper::dump_trait(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Trait& trait, int nindent/*=0*/) const
{
    auto indent = RepeatLitStr { "   ", nindent };
    if( !this->filters.types.traits ) {
        return ;
    }
    if( !filters.public_only && !pub.is_global() ) {
        return ;
    }
    ::std::cout << indent << "trait " << ip << trait.m_params.fmt_args() << "\n";
    ::std::cout << indent << "{\n";
    auto indent2 = RepeatLitStr { "   ", nindent+1 };
    for(const auto& t : trait.m_types)
    {
        ::std::cout << indent2 << "type " << t.first;
        auto prefix = ':';
        if( !t.second.is_sized ) {
            ::std::cout << ": ?Sized";
            prefix = '+';
        }
        for(const auto& t : t.second.m_trait_bounds) {
            ::std::cout << ' ' << prefix << ' ' << t;
            prefix = '+';
        }
        if( t.second.m_default != HIR::TypeRef() ) {
            ::std::cout << " = " << t.second.m_default;
        }
        ::std::cout << ";\n";
    }
    for(const auto& v : trait.m_values)
    {
        TU_MATCH_HDRA( (v.second), {)
        TU_ARMA(Constant, e) {
            //dump_constant(v.first, 
            }
        TU_ARMA(Static, e) {
            //dump_static(v.first, 
            }
        TU_ARMA(Function, e) {
            //dump_function(v.first, 
            }
        }
    }
    ::std::cout << indent << "}\n";
    ::std::cout << ::std::endl;
}

void Dumper::dump_value_import(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::ValueItem::Data_Import& imp, int nindent/*=0*/) const
{
    auto indent = RepeatLitStr { "   ", nindent };
    if( !this->filters.types.imports ) {
        return ;
    }
    if( filters.public_only && !pub.is_global() ) {
        return ;
    }
    ::std::cout << indent << pub << "use[value] " << ip << " = " << imp.path;
    if(imp.is_variant)
    {
        ::std::cout << "#" << imp.idx;
    }
    ::std::cout << "\n";
}
void Dumper::dump_constant(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Constant& c, int nindent/*=0*/) const
{
    auto indent = RepeatLitStr { "   ", nindent };
    if( !this->filters.types.values ) {
        return ;
    }
    if( filters.public_only && !pub.is_global() ) {
        return ;
    }
    ::std::cout << indent << "const " << ip << ": " << c.m_type << " = " << c.m_value_res << "\n";
}
void Dumper::dump_function(::HIR::ItemPath ip, const ::HIR::Publicity& pub, const ::HIR::Function& fcn, int nindent/*=0*/) const
{
    auto indent = RepeatLitStr { "   ", nindent };
    if( !this->filters.types.functions ) {
        return ;
    }
    if( filters.public_only && !pub.is_global() ) {
        return ;
    }
    ::std::cout << indent << "fn " << ip << fcn.m_params.fmt_args() << "(";
    ::std::cout << " )";
    if( fcn.m_code.m_mir )
    {
        ::std::cout << "\n";
        ::std::cout << indent << "{\n";
        if( filters.no_body ) {
            ::std::cout << indent << "...\n";
        }
        else {
            MIR_Dump_Fcn(::std::cout, *fcn.m_code.m_mir, nindent+1);
        }
        ::std::cout << indent << "}\n";
        ::std::cout << ::std::endl;
    }
    else
    {
        ::std::cout << ";" << ::std::endl;
    }
}


namespace {
    void dump_macro_contents(std::ostream& os, const std::vector<MacroExpansionEnt>& ents) {
        for(const auto& ent : ents)
        {
            os << " ";
            TU_MATCH_HDRA( (ent), { )
            TU_ARMA(Loop, e) {
                os << "loop(";
                os << e.controlling_input_loops;
                os << ") {";
                dump_macro_contents(os, e.entries);
                os << "}" << e.joiner;
                }
            TU_ARMA(NamedValue, e) {
                if(e == ~0u)
                    os << "$crate";
                else
                    os << "$" << e;
                }
            TU_ARMA(Token, e) {
                os << e;
                }
            }
        }
    }
}

void Dumper::dump_macrorules(const HIR::ItemPath& ip, const MacroRules& rules) const
{
    ::std::cout << "macro_rules! " << ip << "{" << std::endl;
    for(const auto& arm : rules.m_rules)
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
        ::std::cout << "    ";
        dump_macro_contents(std::cout, arm.m_contents);
        ::std::cout << "\n";
        ::std::cout << "    }\n";
    }
    ::std::cout << "}\n";
    ::std::cout << ::std::endl;
}

/*
bool debug_enabled()
{
    return false;
}

::std::ostream& debug_output(int indent, const char* function)
{
    return ::std::cout << "- " << RepeatLitStr { " ", indent } << function << ": ";
}
*/

Args::Args(int argc, const char* const argv[])
{
    this->infile = argv[1];
}
/*
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
*/

MIR::EnumCachePtr::~EnumCachePtr()
{
    assert(!this->p);
}
