

#include "types.h"
#include "error_handling.h"
#include <cassert>
#include <memory>   // unique_ptr

static bool ENABLE_DEBUG_SYMINFO = false;

TypeRef::t_cache    TypeRef::s_cache;

/*static*/ TypeRef TypeRef::lookup(HANDLE hProcess, ULONG64 mod_base, DWORD type_id)
{
    auto it = s_cache.find(type_id);
    if( it != s_cache.end() )
        return it->second;

    auto get_dword = [=](const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty) {
        DWORD v;
        if( !SymGetTypeInfo(hProcess, mod_base, type_id, info_ty, &v) )
        {
            throw std::runtime_error(FMT_STRING("#" << type_id << " " << name << "=" << WinErrStr(GetLastError())));
        }
        return v;
    };
    auto get_str = [=](const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty)->WideStrLocalFree {
        WCHAR* v;
        if( !SymGetTypeInfo(hProcess, mod_base, type_id, info_ty, &v) )
        {
            throw std::runtime_error(FMT_STRING("#" << type_id << " " << name << "=" << WinErrStr(GetLastError())));
        }
        return WideStrLocalFree(v);
    };


    DWORD   symtag = get_dword("SYMTAG", TI_GET_SYMTAG);

    TypeRef rv;
    switch(symtag)
    {
    case SymTagPointerType:
        rv = TypeRef::lookup(hProcess, mod_base, get_dword("TYPE", TI_GET_TYPE));
        rv.wrappers.push_back(Wrapper::new_pointer());
        break;
    case SymTagArrayType:
        rv = TypeRef::lookup(hProcess, mod_base, get_dword("TYPE", TI_GET_TYPE));
        rv.wrappers.push_back(Wrapper(get_dword("COUNT", TI_GET_COUNT)));
        break;
    case SymTagEnum:
        rv = TypeRef::lookup(hProcess, mod_base, get_dword("TYPE", TI_GET_TYPE));
        break;
    case SymTagBaseType:
        rv.m_class = ClassBasic;
        rv.m_data.basic.bt = static_cast<BasicType>(get_dword("BASETYPE", TI_GET_BASETYPE));
        rv.m_data.basic.size = static_cast<uint8_t>(get_dword("LENGTH", TI_GET_LENGTH));
        break;
    case SymTagUDT:
        rv.m_class = ClassUdt;
        rv.m_data.udt = TypeDefinition::from_syminfo(hProcess, mod_base, type_id);
        assert(rv.m_data.udt);
        break;
    case SymTagFunctionType:
        rv.m_class = ClassMisc;
        rv.m_data.misc.name = _strdup("fn"); //rv.m_data.misc.name = _strdup(FMT_STRING(get_str("SYMNAME", TI_GET_SYMNAME)).c_str());
        rv.m_data.misc.size = 0;//static_cast<uint8_t>(get_dword("LENGTH", TI_GET_LENGTH));
        break;
    case SymTagVTableShape:
        rv.m_class = ClassUdt;
        rv.m_data.udt = TypeDefinition::from_syminfo(hProcess, mod_base, type_id);
        assert(rv.m_data.udt);
        break;
    default:
        throw std::runtime_error(FMT_STRING("#" << type_id << " SYMTAG=" << symtag));
    }
    s_cache.insert(std::make_pair(type_id, rv));
    //::std::cout << "TypeRef::lookup: " << rv << std::endl;
    return rv;
}

TypeRef::TypeNotLoaded::TypeNotLoaded(const std::string& name)
{
    this->name = name;
    this->msg = "Type `" + name + "` not loaded";
}

/*static*/ TypeRef TypeRef::lookup_by_name(const std::string& name)
{
    for(const auto& e : s_cache)
    {
        if( e.second.is_udt(name.c_str()) )
        {
            return e.second;
        }
    }
    throw TypeNotLoaded(name);;
}

bool TypeRef::is_udt(const char* name) const {
    return wrappers.empty() && m_class == ClassUdt && m_data.udt->name == name;
}
bool TypeRef::is_udt_ptr(const char* name) const {
    return wrappers.size() == 1 && wrappers[0].is_pointer() && m_class == ClassUdt && m_data.udt->name == name;
}
bool TypeRef::is_udt_suffix(const char* name) const {
    return wrappers.empty() && m_class == ClassUdt && m_data.udt->name.compare(0, strlen(name), name) == 0;
}

void TypeRef::fmt(std::ostream& os, unsigned recurse_depth, unsigned indent_level/*=0*/) const
{
    for(const auto& w : wrappers)
    {
        if( w.is_pointer() ) {
            os << "*ptr ";
        }
        else {
            os << "[" << w.count << "]";
        }
    }
    switch(m_class)
    {
    case ClassBasic:
        switch(m_data.basic.bt)
        {
        case btChar:    os << "char";   return;
        case btInt:     os << "i" << (m_data.basic.size * 8);    return;
        case btUint:    os << "u" << (m_data.basic.size * 8);   return;
        case btFloat:   os << "f" << (m_data.basic.size * 8);  return;
        case btBool:    os << "bool";   return;
        }
        os << "bt#" << m_data.basic.bt;
        break;
    case ClassUdt:
        m_data.udt->fmt(os, recurse_depth, indent_level);
        break;
    case ClassMisc:
        os << "[m]" << m_data.misc.name;
        break;
    }
}

size_t TypeRef::size() const
{
    size_t  mul = 1;
    for(auto it = wrappers.rbegin(); it != wrappers.rend(); ++it)
    {
        if( it->is_pointer() )
        {
            return mul*8;
        }
        mul *= it->count;
    }

    switch(m_class)
    {
    case ClassBasic:
        return m_data.basic.size;
    case ClassUdt:
        return mul * m_data.udt->size;
    }
    throw std::runtime_error(FMT_STRING("Unknown size for " << *this));
}

TypeRef TypeRef::get_field(std::initializer_list<const char*> fields, size_t* out_ofs/*=nullptr*/) const
{
    //std::cout << "get_field() > " << *this << std::endl;
    auto cur = *this;
    size_t ofs = 0;
    for(const char* fld_name : fields)
    {
        if( fld_name )
        {
            //std::cout << "get_field(): " << fld_name << " in " << *cur << std::endl;
            if(!cur.wrappers.empty())
                throw std::runtime_error(FMT_STRING("Getting field of " << cur));
            if(cur.m_class != ClassUdt)
                throw std::runtime_error(FMT_STRING("Getting field of " << cur));
            bool found = false;
            for(const auto& fld : cur.m_data.udt->fields)
            {
                if( fld.name == fld_name ) {
                    ofs += fld.offset;
                    cur = fld.ty;
                    found = true;
                    break;
                }
            }
            if(!found)
                throw std::runtime_error(FMT_STRING("Cannot find field '" << fld_name << "' in " << cur));
        }
        else
        {
            // Deref
            if(cur.wrappers.empty() || !cur.wrappers.back().is_pointer())
                throw std::runtime_error(FMT_STRING("Deref of " << cur));
            if(out_ofs)
                throw std::runtime_error("Dereference can't get offset");
            cur = cur.deref();
        }
    }
    if(out_ofs)
        *out_ofs = ofs;
    return cur;
}
TypeRef TypeRef::deref() const
{
    if(this->wrappers.empty())
        throw std::runtime_error(FMT_STRING("Dereferencing " << *this));
    auto rv = *this;
    rv.wrappers.pop_back();
    return rv;
}

/*static*/ TypeDefinition* TypeDefinition::from_syminfo(HANDLE hProcess, ULONG64 mod_base, DWORD type_id)
{
    static Indent s_indent = Indent(" ");
#define DO_DEBUG(v) do { if(ENABLE_DEBUG_SYMINFO) { std::cout << s_indent << v << std::endl; } } while(0)
    static std::unordered_map<DWORD, TypeDefinition*>   s_anti_recurse;
    if( s_anti_recurse.find(type_id) != s_anti_recurse.end() ) {
        DO_DEBUG("Recursion on #" << type_id);
        return s_anti_recurse.at(type_id);
    }

    DO_DEBUG("TypeDefinition::from_syminfo(" << type_id << ")");
    auto _ = s_indent.inc();

    auto get_raw = [hProcess,mod_base](DWORD type_id, const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty, void* out) {
        if( !SymGetTypeInfo(hProcess, mod_base, type_id, info_ty, out) )
        {
            throw std::runtime_error(FMT_STRING("#" << type_id << " " << name << "=" << WinErrStr(GetLastError())));
        }
    };
    auto get_dword = [=](DWORD child_type_id, const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty) {
        DWORD v;
        get_raw(child_type_id, name, info_ty, &v);
        return v;
    };
    auto get_str = [=](DWORD child_type_id, const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty)->WideStrLocalFree {
        WCHAR* v;
        get_raw(child_type_id, name, info_ty, &v);
        return WideStrLocalFree(v);
    };

    auto rv = ::std::unique_ptr<TypeDefinition>(new TypeDefinition);

    auto symtag = get_dword(type_id, "SYMTAG", TI_GET_SYMTAG);
    if( symtag == SymTagUDT ) {
        rv->name = FMT_STRING(get_str(type_id, "SYMNAME", TI_GET_SYMNAME));
        rv->size = get_dword(type_id, "LENGTH", TI_GET_LENGTH);
    }
    else if( symtag == SymTagVTableShape ) {
        rv->name = FMT_STRING("VTABLE #" << type_id);
        rv->size = 0;
    }
    else {
        throw std::runtime_error(FMT_STRING("BUG #" << type_id << " SYMTAG=" << get_dword(type_id, "SYMTAG", TI_GET_SYMTAG)));
    }

    s_anti_recurse.insert(std::make_pair(type_id, rv.get()));

    DWORD child_count = get_dword(type_id, "CHILDRENCOUNT", TI_GET_CHILDRENCOUNT);
    DO_DEBUG(rv->name << " child_count=" << child_count);
    if(child_count > 0)
    {
        try
        {
            std::vector<ULONG>  buf( (sizeof(TI_FINDCHILDREN_PARAMS) + (child_count-1) * sizeof(ULONG))/sizeof(ULONG) );
            auto* child_params = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
            child_params->Count = child_count;
            child_params->Start = 0;

            get_raw(type_id, "FINDCHILDREN", TI_FINDCHILDREN, child_params);

            for(DWORD i = 0; i < child_count; i ++)
            {
                auto child_type_id = child_params->ChildId[i];
                auto child_tag = get_dword(child_type_id, "SYMTAG", TI_GET_SYMTAG);
                switch(child_tag)
                {
                case SymTagData: {
                    auto data_kind = get_dword(child_type_id, "DATAKIND", TI_GET_DATAKIND);
                    //DO_DEBUG(i << ":" << child_type_id << " " << child_tag << "/" << data_kind);
                    switch(data_kind)
                    {
                    case 7: { // Field?
                        Field   f;
                        // TODO: Sometimes this fails with "Incorrect function"
                        f.name = FMT_STRING(get_str(child_type_id, "SYMNAME", TI_GET_SYMNAME));
                        f.offset = get_dword(child_type_id, "OFFSET", TI_GET_OFFSET);
                        f.ty = TypeRef::lookup(hProcess, mod_base, get_dword(child_type_id, "TYPE", TI_GET_TYPE));
                        DO_DEBUG("Field @" << f.offset << " " << f.name << ": " << f.ty);
                        rv->fields.push_back(std::move(f));
                    } break;
                    case 8: // Constant?
                            // Seen for `npos` on std::string
                        break;
                    default:
                        throw std::runtime_error(FMT_STRING("#" << type_id << "->" << child_type_id << " DATAKIND=" << data_kind));
                    }
                } break;
                case SymTagFunction:
                    //DO_DEBUG(rv->name << " fn " << get_str(child_type_id, "SYMNAME", TI_GET_SYMNAME));
                    break;
                case SymTagBaseClass:
                    // TODO: Recuse into this type (or look it up?)
                    //DO_DEBUG(rv->name << " => " << child_type_id);
                    DO_DEBUG(rv->name << " => " << get_str(child_type_id, "SYMNAME", TI_GET_SYMNAME));
                    {
                        DO_DEBUG(">> " << child_type_id << " " << get_dword(child_type_id, "TYPE", TI_GET_TYPE));
                        auto p = TypeRef::lookup(hProcess, mod_base, get_dword(child_type_id, "TYPE", TI_GET_TYPE));
                        switch(p.m_class)
                        {
                        case TypeRef::ClassUdt:
                            for(auto& f : p.m_data.udt->fields)
                            {
                                rv->fields.push_back(f);
                            }
                            break;
                        }
                    }
                    break;
                case SymTagVTable:{ // VTable: Add as field
                    Field   f;
                    f.name = "#VTABLE";
                    f.offset = get_dword(child_type_id, "OFFSET", TI_GET_OFFSET);
                    DO_DEBUG(">> VT " << child_type_id << " " << get_dword(child_type_id, "TYPE", TI_GET_TYPE));
                    f.ty = TypeRef::lookup(hProcess, mod_base, get_dword(child_type_id, "TYPE", TI_GET_TYPE));
                    rv->fields.push_back(std::move(f));
                } break;
                case SymTagTypedef:
                    break;
                case SymTagEnum:
                    break;
                    // Why? Inline structure?
                case SymTagUDT:
                    break;
                default:
                    throw std::runtime_error(FMT_STRING("#" << type_id << "->" << child_type_id << " SYMTAG=" << child_tag));
                }
            }
        }
        catch(const std::exception& e)
        {
            ::std::cout << "Exception " << e.what() << " in TypeDefinition::from_syminfo(" << type_id << "): " << rv->name << std::endl;
            //rv->
        }
    }
    DO_DEBUG("TypeDefinition::from_syminfo(" << type_id << "): " << rv->name);

    s_anti_recurse.erase(type_id);
    return rv.release();
#undef DO_DEBUG
}

void TypeDefinition::fmt(std::ostream& os, unsigned recurse_depth, unsigned indent_level/*=0*/) const
{
    os << this->name;
    if(recurse_depth > 0)
    {
        static std::vector<const TypeDefinition*>   s_stack;
        if(std::find(s_stack.begin(), s_stack.end(), this) != s_stack.end()) {
            os << " { ^ }";
            return ;
        }
        s_stack.push_back(this);
        os << " { ";
        auto print_indent = [&]() {
            if(indent_level > 0)
            {
                os << "\n";
                for(unsigned i = 1; i < indent_level; i++)
                    os << "    ";
            }
        };
        if(indent_level > 0)
        {
            indent_level += 1;
        }
        for(const auto& fld : this->fields)
        {
            print_indent();
            os << fld.name << " @ " << fld.offset << "+" << fld.ty.size() << ": ";
            fld.ty.fmt(os, recurse_depth-1, indent_level);
            os << ", ";
        }
        if(indent_level > 0)
        {
            indent_level -= 1;
            print_indent();
        }
        os << "}";
        s_stack.pop_back();
    }
}