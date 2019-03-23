#include <hir/hir.hpp>
#include <hir/main_bindings.hpp>
#include <macro_rules/macro_rules.hpp>

int g_debug_indent_level = 0;

struct Args
{
    Args(int argc, const char* const argv[]);

    ::std::string   infile;
};

int main(int argc, const char* argv[])
{
    Args    args(argc, argv);

    auto hir = HIR_Deserialise(args.infile, "");

    // Dump macros
    for(const auto& mac : hir->m_exported_macros)
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
            ::std::cout << " ) => {" << ::std::endl;
            // TODO...
            ::std::cout << "    }" << ::std::endl;
        }
        ::std::cout << "}" << ::std::endl;
        ::std::cout << ::std::endl;
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
