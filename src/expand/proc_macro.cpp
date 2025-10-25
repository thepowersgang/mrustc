/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/proc_macro.cpp
 * - Support for the `#[proc_macro_derive]` attribute
 */
#include <synext.hpp>
#include "../common.hpp"
#include "cfg.hpp"
#include <ast/ast.hpp>
#include <ast/expr.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <hir/hir.hpp>  // ABI_RUST
#include "proc_macro.hpp"
#include <parse/lex.hpp>
#include <parse/ttstream.hpp>
#include <unordered_set>
#ifdef _WIN32
# define NOMINMAX
# define NOGDI  // Don't include GDI functions (defines some macros that collide with mrustc ones)
# include <Windows.h>
#else
# include <unistd.h>    // read/write/pipe
# include <spawn.h>
# include <sys/wait.h>
#endif

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__APPLE__)
extern char **environ;
#endif

#define NEWNODE(_ty, ...)   ::AST::ExprNodeP(new ::AST::ExprNode##_ty(__VA_ARGS__))

class Decorator_ProcMacroDerive:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& attr, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i) const override
    {
        if( i.is_None() )
            return;

        if( !i.is_Function() )
            TODO(sp, "Error for proc_macro_derive on non-Function");

        ::std::vector<::std::string>    attributes;
        TTStream    lex(sp, ParseState(), attr.data());
        lex.getTokenCheck(TOK_PAREN_OPEN);
        auto trait_name = lex.getTokenCheck(TOK_IDENT).ident().name;
        while(lex.getTokenIf(TOK_COMMA))
        {
            if( lex.lookahead(0) == TOK_PAREN_CLOSE ) {
                break;
            }
            auto k = lex.getTokenCheck(TOK_IDENT).ident().name;
            if( k == "attributes" ) {
                lex.getTokenCheck(TOK_PAREN_OPEN);
                do {
                    if( lex.lookahead(0) == TOK_PAREN_CLOSE ) {
                        break;
                    }
                    attributes.push_back( lex.getTokenCheck(TOK_IDENT).ident().name.c_str() );
                } while(lex.getTokenIf(TOK_COMMA));
                lex.getTokenCheck(TOK_PAREN_CLOSE);
            }
            else {
                ERROR(sp, E0000, "Unexpected `" << k << "` in `#[proc_macro_derive]`");
            }
        }
        lex.getTokenCheck(TOK_PAREN_CLOSE);

        crate.m_proc_macros.push_back(AST::ProcMacroDef { AST::ProcMacroTy::Derive, RcString::new_interned(FMT(trait_name)), path, mv$(attributes) });
    }
};
STATIC_DECORATOR("proc_macro_derive", Decorator_ProcMacroDerive)
class Decorator_ProcMacroAttribute:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& attr, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i) const override
    {
        if( i.is_None() )
            return;

        if( !i.is_Function() )
            TODO(sp, "Error for #[proc_macro_attribute] on non-Function");

        crate.m_proc_macros.push_back(AST::ProcMacroDef { AST::ProcMacroTy::Attribute, path.nodes.back(), path, {} });
    }
};
STATIC_DECORATOR("proc_macro_attribute", Decorator_ProcMacroAttribute)
class Decorator_ProcMacro:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& attr, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i) const override
    {
        if( i.is_None() )
            return;

        if( !i.is_Function() )
            TODO(sp, "Error for #[proc_macro] on non-Function");

        crate.m_proc_macros.push_back(AST::ProcMacroDef { AST::ProcMacroTy::Function, path.nodes.back(), path, {} });
    }
};
STATIC_DECORATOR("proc_macro", Decorator_ProcMacro)



void Expand_ProcMacro(::AST::Crate& crate)
{
    auto pm_crate_name = RcString::new_interned("proc_macro");
    AST::g_implicit_crates.insert( std::make_pair(pm_crate_name, crate.load_extern_crate(Span(), pm_crate_name)) );

    // Create the following module:
    // ```
    // mod `proc_macro#` {
    //   extern crate proc_macro;
    //   fn main() {
    //     self::proc_macro::main(&::`proc_macro#`::MACROS);
    //   }
    //   static TESTS: [proc_macro::MacroDesc; _] = [
    //     proc_macro::MacroDesc { name: "deriving_Foo", handler: ::path::to::foo }
    //     ];
    // }
    // ```

    // ---- main function ----
    auto main_fn = ::AST::Function { Span(), TypeRef(TypeRef::TagUnit(), Span()), {} };
    {
        auto call_node = NEWNODE(_CallPath,
                ::AST::Path(crate.m_ext_cratename_procmacro, { ::AST::PathNode("main") }),
                ::make_vec1(
                    NEWNODE(_UniOp, ::AST::ExprNode_UniOp::REF,
                        NEWNODE(_NamedValue, ::AST::Path("", { ::AST::PathNode("proc_macro#"), ::AST::PathNode("MACROS") }))
                        )
                    )
                );
        main_fn.set_code( mv$(call_node) );
    }


    // ---- test list ----
    ::std::vector< ::AST::ExprNodeP>    test_nodes;

    for(const auto& desc : crate.m_proc_macros)
    {
        const char* type_name = "SingleStream";
        switch(desc.ty)
        {
        case ::AST::ProcMacroTy::Attribute: type_name = "Attribute";    break;
        default:
            break;
        }
        ::AST::ExprNode_StructLiteral::t_values   desc_vals;
        // `name: "foo",`
        desc_vals.push_back({ {}, "name", NEWNODE(_String,  desc.name.c_str()) });
        // `handler`: ::foo
        desc_vals.push_back({ {}, "handler", NEWNODE(_CallPath,
            ::AST::Path(crate.m_ext_cratename_procmacro, { ::AST::PathNode("MacroType"), ::AST::PathNode(type_name) }),
            ::make_vec1(
                NEWNODE(_NamedValue, AST::Path(desc.path))
                )
                )});

        test_nodes.push_back( NEWNODE(_StructLiteral,  ::AST::Path(crate.m_ext_cratename_procmacro, { ::AST::PathNode("MacroDesc")}), nullptr, mv$(desc_vals) ) );
    }
    auto* tests_array = new ::AST::ExprNode_Array(mv$(test_nodes));

    size_t test_count = tests_array->m_values.size();
    auto tests_list = ::AST::Static { ::AST::Static::Class::STATIC,
        TypeRef(TypeRef::TagSizedArray(), Span(),
                TypeRef(Span(), ::AST::Path(crate.m_ext_cratename_procmacro, { ::AST::PathNode("MacroDesc") })),
                ::std::shared_ptr<::AST::ExprNode>( new ::AST::ExprNode_Integer(U128(test_count), CORETYPE_UINT) )
               ),
        ::AST::Expr( mv$(tests_array) )
        };

    // ---- module ----
    auto newmod = ::AST::Module { ::AST::AbsolutePath("", { "proc_macro#" }) };
    // - TODO: These need to be loaded too.
    //  > They don't actually need to exist here, just be loaded (and use absolute paths)
    auto vis_private = AST::Visibility::make_restricted(AST::Visibility::Ty::Private, newmod.path());
    newmod.add_ext_crate(Span(), vis_private, crate.m_ext_cratename_procmacro, "proc_macro", {});

    newmod.add_item(Span(), vis_private, "main", mv$(main_fn), {});
    newmod.add_item(Span(), vis_private, "MACROS", mv$(tests_list), {});

    crate.m_root_module.add_item(Span(), vis_private, "proc_macro#", mv$(newmod), {});
    crate.m_lang_items["mrustc-main"] = ::AST::AbsolutePath("", { "proc_macro#", "main" });
}

enum class TokenClass
{
    EndOfStream = 0,
    Symbol      = 1,
    Ident       = 2,
    Lifetime    = 3,
    String      = 4,
    ByteString  = 5,    // String
    CharLit     = 6,    // v128
    UnsignedInt = 7,
    SignedInt   = 8,
    Float       = 9,
    SpanRef  = 10,
    SpanDef  = 11,
};
enum class FragType
{
    Ident = 0,
    Tt = 1,

    Path = 2,
    Type = 3,

    Expr = 4,
    Statement = 5,
    Block = 6,
    Pattern = 7,
};
struct ProcMacroInv:
    public TokenStream
{
    Span    m_parent_span;
    Span    m_this_span;
    const ::HIR::ProcMacro& m_proc_macro_desc;
    AST::Edition    m_edition;
    ::std::ofstream m_dump_file_out;
    ::std::ofstream m_dump_file_res;

    /// Spans that have had an index assigned
    ::std::unordered_map<const SpanInner*,size_t>  known_spans;
    /// Span indexes that have been sent
    ::std::unordered_set<size_t>  sent_spans;
    size_t  next_span_index = 2;

    struct Handles
    {
        //~Handles();
        Handles() {}
        Handles(Handles&&);
        Handles(const Handles&) = delete;
        Handles& operator=(Handles&&) = delete;
        Handles& operator=(const Handles&) = delete;
#ifdef _WIN32
        HANDLE  child_handle;
        HANDLE  child_stdin;
        HANDLE  child_stdout;
#else
        // POSIX
        pid_t   child_pid;  // Questionably needed
         int    child_stdin;
         int    child_stdout;
        // NOTE: stderr stays as our stderr
#endif
    } handles;
    bool    m_eof_hit = false;

public:
    ProcMacroInv(const Span& sp, AST::Edition edition, const char* executable, const ::HIR::ProcMacro& proc_macro_desc);
    ProcMacroInv(const ProcMacroInv&) = delete;
    ProcMacroInv(ProcMacroInv&&) = default;
    ProcMacroInv& operator=(const ProcMacroInv&) = delete;
    ProcMacroInv& operator=(ProcMacroInv&&) = delete;
    ~ProcMacroInv();

    bool check_good();
    void send_done() {
        this->send_u8(static_cast<uint8_t>(TokenClass::EndOfStream));
        m_dump_file_out.flush();
        DEBUG("Input tokens sent");
    }
    void send_symbol(const char* val) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Symbol));
        this->send_bytes(val, ::std::strlen(val));
    }
    void send_rword(const char* val) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Ident));
        this->send_bytes(val, ::std::strlen(val));
    }
    void send_ident(const char* val) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Ident));
        if( Lex_FindReservedWord(val, m_edition) != TOK_NULL ) {
            auto size = ::std::strlen(val);
            this->send_v128u( 2 + size );
            this->send_bytes_raw("r#", 2);
            this->send_bytes_raw(val, size);
        }
        else {
            this->send_bytes(val, ::std::strlen(val));
        }
    }
    void send_ident(const Ident& val) {
        send_ident(val.name.c_str());
    }
    void send_lifetime(const char* val) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Lifetime));
        this->send_bytes(val, ::std::strlen(val));
    }
    void send_string(const ::std::string& s) {
        this->send_u8(static_cast<uint8_t>(TokenClass::String));
        this->send_bytes(s.data(), s.size());
    }
    void send_bytestring(const ::std::string& s) {
        this->send_u8(static_cast<uint8_t>(TokenClass::ByteString));
        this->send_bytes(s.data(), s.size());
    }
    void send_char(uint32_t ch) {
        this->send_u8(static_cast<uint8_t>(TokenClass::CharLit));
        this->send_v128u(ch);
    }
    void send_int(eCoreType ct, U128 v) {
        uint8_t size;
        switch(ct)
        {
        case CORETYPE_ANY:  size = 0;   if(0)
        case CORETYPE_UINT: size = 1;   if(0)
        case CORETYPE_U8:   size = 8;   if(0)
        case CORETYPE_U16:  size = 16;  if(0)
        case CORETYPE_U32:  size = 32;  if(0)
        case CORETYPE_U64:  size = 64;  if(0)
        case CORETYPE_U128: size = 128; if(0)
            ;
            this->send_u8(static_cast<uint8_t>(TokenClass::UnsignedInt));
            this->send_u8(size);
            break;
        case CORETYPE_INT:  size = 1;   if(0)
        case CORETYPE_I8:   size = 8;   if(0)
        case CORETYPE_I16:  size = 16;  if(0)
        case CORETYPE_I32:  size = 32;  if(0)
        case CORETYPE_I64:  size = 64;  if(0)
        case CORETYPE_I128: size = 128; if(0)
            ;
            this->send_u8(static_cast<uint8_t>(TokenClass::SignedInt));
            this->send_u8(size);
            break;
        default:
            BUG(m_parent_span, "Unknown integer type");
        }
        this->send_v128u(v);
    }
    void send_float(eCoreType ct, double v) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Float));
        switch(ct)
        {
        case CORETYPE_ANY:  this->send_u8(0);  break;
        case CORETYPE_F32:  this->send_u8(32);  break;
        case CORETYPE_F64:  this->send_u8(64);  break;
        default:
            BUG(m_parent_span, "Unknown float type");
        }
        this->send_bytes_raw(&v, sizeof(v));
    }

    void send_span_def(size_t index, const Span& sp) {
        this->known_spans[sp.get()] = index;
        this->sent_spans.insert(index);

        this->send_u8(static_cast<uint8_t>(TokenClass::SpanDef));
        this->send_v128u(index);
        this->send_v128u(0);    // TODO: Parent span
        if( const auto* sp_p = dynamic_cast<const SpanInner_Source*>(sp.get()) ) {
            this->send_bytes(sp_p->filename.c_str(), sp_p->filename.size());
            this->send_u8(1);   // path_is_real
            this->send_v128u( sp_p->start_line );
            this->send_v128u( sp_p->end_line );
            this->send_v128u( sp_p->start_ofs );
            this->send_v128u( sp_p->end_ofs );
        }
        else {
            this->send_bytes("MACRO", 5);   // TODO: better filename?
            this->send_u8(0);   // path_is_real
            this->send_v128u( 0 );
            this->send_v128u( 0 );
            this->send_v128u( 0 );
            this->send_v128u( 0 );
        }
    }

    bool attr_is_used(const RcString& n) const {
        if( n == "repr" )
            return true;
        return ::std::find(m_proc_macro_desc.attributes.begin(), m_proc_macro_desc.attributes.end(), n) != m_proc_macro_desc.attributes.end();
    }

    virtual Position getPosition() const override;
    virtual Token realGetToken() override;
    virtual AST::Edition realGetEdition() const override { return m_edition; }
    virtual Ident::Hygiene realGetHygiene() const override;
private:
    Token realGetToken_();
    void send_u8(uint8_t v);
    void send_bytes(const void* val, size_t size);
    void send_bytes_raw(const void* val, size_t size);
    void send_v128u(uint64_t val);
    void send_v128u(U128 val);

    uint8_t recv_u8();
    ::std::string recv_bytes();
    void recv_bytes_raw(void* out_void, size_t len);
    uint64_t recv_v128u();
    U128 recv_v128u_u128();
};

ProcMacroInv ProcMacro_Invoke_int(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path)
{
    TRACE_FUNCTION_F(mac_path);
    // 1. Locate macro in HIR list
    const auto& crate_name = mac_path.front();
    ASSERT_BUG(sp, crate.m_extern_crates.count(crate_name), "Crate not loaded for macro: [" << mac_path << "]");
    const auto& ext_crate = crate.m_extern_crates.at(crate_name);
    // TODO: Ensure that this macro is in the listed crate.
    const ::HIR::ProcMacro* pmp = nullptr;
    for(const auto& mi : ext_crate.m_hir->m_root_module.m_macro_items)
    {
        if( !mi.second->ent.is_ProcMacro() )
            continue ;
        const auto& pm = mi.second->ent.as_ProcMacro();
        bool good = true;
        for(size_t i = 0; i < ::std::min( mac_path.size()-1, pm.path.components().size() ); i++)
        {
            if( mac_path[1+i] != pm.path.components()[i] )
            {
                good = false;
                break;
            }
        }
        if(good)
        {
            pmp = &pm;
            break;
        }
    }
    if( !pmp )
    {
        ERROR(sp, E0000, "Unable to find referenced proc macro " << mac_path);
    }

    // 2. Get executable and macro name
    // TODO: Windows will have .exe?
    ::std::string   proc_macro_exe_name = ext_crate.m_filename;

    // 3. Create ProcMacroInv
    auto rv = ProcMacroInv(sp, ext_crate.m_hir->m_edition, proc_macro_exe_name.c_str(), *pmp);
    rv.parse_state().crate = &crate;

    return rv;

    // NOTE: 1.39 failure_derive (2015) emits `::failure::foo` but `libcargo` doesn't have `failure` in root (it's a 2018 crate)
    //return ProcMacroInv(sp, ext_crate.m_hir->m_edition, proc_macro_exe_name.c_str(), *pmp);
}


namespace {
    struct Visitor:
        public AST::NodeVisitor
    {
        const Span& sp;
        ProcMacroInv&   m_pmi;
        bool emit_all_attrs;
        Visitor(const Span& sp, ProcMacroInv& pmi):
            sp(sp),
            m_pmi(pmi)
            //,emit_all_attrs(false)
            ,emit_all_attrs(true)
        {
        }

        void visit_token(const ::Token& tok)
        {
            switch(tok.type())
            {
            case TOK_NULL:
                BUG(sp, "Unexpected NUL in token stream");
            case TOK_EOF:
                BUG(sp, "Unexpected EOF in token stream");

            case TOK_NEWLINE:
            case TOK_WHITESPACE:
            case TOK_COMMENT:
                BUG(sp, "Unexpected whitepace in tokenstream");
                break;
            case TOK_INTERPOLATED_TYPE:
                visit_type(const_cast<::Token&>(tok).frag_type());
                break;
            case TOK_INTERPOLATED_PATH:
                TODO(sp, "TOK_INTERPOLATED_PATH");
            case TOK_INTERPOLATED_PATTERN:
                TODO(sp, "TOK_INTERPOLATED_PATTERN");
            case TOK_INTERPOLATED_STMT:
            case TOK_INTERPOLATED_BLOCK:
            case TOK_INTERPOLATED_EXPR:
                visit_node( const_cast<::Token&>(tok).frag_node() );
                break;
            case TOK_INTERPOLATED_META:
            case TOK_INTERPOLATED_ITEM:
            case TOK_INTERPOLATED_VIS:
                TODO(sp, "TOK_INTERPOLATED_...");
            // Value tokens
            case TOK_IDENT:     m_pmi.send_ident(tok.ident().name.c_str());   break;  // TODO: Raw idents
            case TOK_LIFETIME:  m_pmi.send_lifetime(tok.ident().name.c_str());  break;  // TODO: Hygine?
            case TOK_INTEGER:
                if( tok.datatype() == CORETYPE_CHAR ) {
                    m_pmi.send_char(tok.intval().truncate_u64());
                }
                else {
                    m_pmi.send_int(tok.datatype(), tok.intval());
                }
                break;
            case TOK_CHAR:      m_pmi.send_char(tok.intval().truncate_u64());  break;
            case TOK_FLOAT:     m_pmi.send_float(tok.datatype(), tok.floatval());   break;
            case TOK_STRING:        m_pmi.send_string(tok.str());       break;
            case TOK_BYTESTRING:    m_pmi.send_bytestring(tok.str());   break;
            case TOK_CSTRING:
                TODO(sp, "TOK_CSTRING");

            case TOK_HASH:      m_pmi.send_symbol("#"); break;
            case TOK_UNDERSCORE:m_pmi.send_symbol("_"); break;

            // Symbols
            case TOK_PAREN_OPEN:    m_pmi.send_symbol("("); break;
            case TOK_PAREN_CLOSE:   m_pmi.send_symbol(")"); break;
            case TOK_BRACE_OPEN:    m_pmi.send_symbol("{"); break;
            case TOK_BRACE_CLOSE:   m_pmi.send_symbol("}"); break;
            case TOK_LT:    m_pmi.send_symbol("<"); break;
            case TOK_GT:    m_pmi.send_symbol(">"); break;
            case TOK_SQUARE_OPEN:   m_pmi.send_symbol("["); break;
            case TOK_SQUARE_CLOSE:  m_pmi.send_symbol("]"); break;
            case TOK_COMMA:     m_pmi.send_symbol(","); break;
            case TOK_SEMICOLON: m_pmi.send_symbol(";"); break;
            case TOK_COLON:     m_pmi.send_symbol(":"); break;
            case TOK_DOUBLE_COLON:  m_pmi.send_symbol("::"); break;
            case TOK_STAR:  m_pmi.send_symbol("*"); break;
            case TOK_AMP:   m_pmi.send_symbol("&"); break;
            case TOK_PIPE:  m_pmi.send_symbol("|"); break;

            case TOK_FATARROW:  m_pmi.send_symbol("=>"); break;
            case TOK_THINARROW: m_pmi.send_symbol("->"); break;
            case TOK_THINARROW_LEFT: m_pmi.send_symbol("<-"); break;

            case TOK_PLUS:  m_pmi.send_symbol("+"); break;
            case TOK_DASH:  m_pmi.send_symbol("-"); break;
            case TOK_EXCLAM:    m_pmi.send_symbol("!"); break;
            case TOK_PERCENT:   m_pmi.send_symbol("%"); break;
            case TOK_SLASH:     m_pmi.send_symbol("/"); break;

            case TOK_DOT:       m_pmi.send_symbol("."); break;
            case TOK_DOUBLE_DOT:    m_pmi.send_symbol(".."); break;
            case TOK_DOUBLE_DOT_EQUAL:  m_pmi.send_symbol("..="); break;
            case TOK_TRIPLE_DOT:    m_pmi.send_symbol("..."); break;

            case TOK_EQUAL:     m_pmi.send_symbol("="); break;
            case TOK_PLUS_EQUAL:    m_pmi.send_symbol("+="); break;
            case TOK_DASH_EQUAL:    m_pmi.send_symbol("-"); break;
            case TOK_PERCENT_EQUAL: m_pmi.send_symbol("%="); break;
            case TOK_SLASH_EQUAL:   m_pmi.send_symbol("/="); break;
            case TOK_STAR_EQUAL:    m_pmi.send_symbol("*="); break;
            case TOK_AMP_EQUAL:     m_pmi.send_symbol("&="); break;
            case TOK_PIPE_EQUAL:    m_pmi.send_symbol("|="); break;

            case TOK_DOUBLE_EQUAL:  m_pmi.send_symbol("=="); break;
            case TOK_EXCLAM_EQUAL:  m_pmi.send_symbol("!="); break;
            case TOK_GTE:    m_pmi.send_symbol(">="); break;
            case TOK_LTE:    m_pmi.send_symbol("<="); break;

            case TOK_DOUBLE_AMP:    m_pmi.send_symbol("&&"); break;
            case TOK_DOUBLE_PIPE:   m_pmi.send_symbol("||"); break;
            case TOK_DOUBLE_LT:     m_pmi.send_symbol("<<"); break;
            case TOK_DOUBLE_GT:     m_pmi.send_symbol(">>"); break;
            case TOK_DOUBLE_LT_EQUAL:   m_pmi.send_symbol("<="); break;
            case TOK_DOUBLE_GT_EQUAL:   m_pmi.send_symbol(">="); break;

            case TOK_DOLLAR:    m_pmi.send_symbol("$"); break;

            case TOK_QMARK:     m_pmi.send_symbol("?");     break;
            case TOK_AT:        m_pmi.send_symbol("@");     break;
            case TOK_TILDE:     m_pmi.send_symbol("~");     break;
            case TOK_BACKSLASH: m_pmi.send_symbol("\\");    break;
            case TOK_CARET:     m_pmi.send_symbol("^");     break;
            case TOK_CARET_EQUAL:   m_pmi.send_symbol("^="); break;
            case TOK_BACKTICK:  m_pmi.send_symbol("`");     break;

            // Reserved Words
            case TOK_RWORD_PUB:     m_pmi.send_rword("pub");    break;
            case TOK_RWORD_PRIV:    m_pmi.send_rword("priv");   break;
            case TOK_RWORD_MUT:     m_pmi.send_rword("mut");    break;
            case TOK_RWORD_CONST:   m_pmi.send_rword("const");  break;
            case TOK_RWORD_STATIC:  m_pmi.send_rword("static"); break;
            case TOK_RWORD_UNSAFE:  m_pmi.send_rword("unsafe"); break;
            case TOK_RWORD_EXTERN:  m_pmi.send_rword("extern"); break;
            case TOK_RWORD_CRATE:   m_pmi.send_rword("crate");  break;
            case TOK_RWORD_MOD:     m_pmi.send_rword("mod");    break;
            case TOK_RWORD_STRUCT:  m_pmi.send_rword("struct"); break;
            case TOK_RWORD_ENUM:    m_pmi.send_rword("enum");   break;
            case TOK_RWORD_TRAIT:   m_pmi.send_rword("trait");  break;
            case TOK_RWORD_FN:      m_pmi.send_rword("fn");     break;
            case TOK_RWORD_USE:     m_pmi.send_rword("use");    break;
            case TOK_RWORD_IMPL:    m_pmi.send_rword("impl");   break;
            case TOK_RWORD_TYPE:    m_pmi.send_rword("type");   break;
            case TOK_RWORD_WHERE:   m_pmi.send_rword("where");  break;
            case TOK_RWORD_AS:      m_pmi.send_rword("as");     break;
            case TOK_RWORD_LET:     m_pmi.send_rword("let");    break;
            case TOK_RWORD_MATCH:   m_pmi.send_rword("match");  break;
            case TOK_RWORD_IF:      m_pmi.send_rword("if");     break;
            case TOK_RWORD_ELSE:    m_pmi.send_rword("else");   break;
            case TOK_RWORD_LOOP:    m_pmi.send_rword("loop");   break;
            case TOK_RWORD_WHILE:   m_pmi.send_rword("while");  break;
            case TOK_RWORD_FOR:     m_pmi.send_rword("for");    break;
            case TOK_RWORD_IN:      m_pmi.send_rword("in");     break;
            case TOK_RWORD_DO:      m_pmi.send_rword("do");     break;
            case TOK_RWORD_CONTINUE:m_pmi.send_rword("continue"); break;
            case TOK_RWORD_BREAK:   m_pmi.send_rword("break");  break;
            case TOK_RWORD_RETURN:  m_pmi.send_rword("return"); break;
            case TOK_RWORD_YIELD:   m_pmi.send_rword("yeild");  break;
            case TOK_RWORD_BOX:     m_pmi.send_rword("box");    break;
            case TOK_RWORD_REF:     m_pmi.send_rword("ref");    break;
            case TOK_RWORD_FALSE:   m_pmi.send_rword("false"); break;
            case TOK_RWORD_TRUE:    m_pmi.send_rword("true");   break;
            case TOK_RWORD_SELF:    m_pmi.send_rword("self");   break;
            case TOK_RWORD_SUPER:   m_pmi.send_rword("super");  break;
            case TOK_RWORD_MOVE:    m_pmi.send_rword("move");   break;
            case TOK_RWORD_ABSTRACT:m_pmi.send_rword("abstract"); break;
            case TOK_RWORD_FINAL:   m_pmi.send_rword("final");  break;
            case TOK_RWORD_OVERRIDE:m_pmi.send_rword("override"); break;
            case TOK_RWORD_VIRTUAL: m_pmi.send_rword("virtual"); break;
            case TOK_RWORD_TYPEOF:  m_pmi.send_rword("typeof"); break;
            case TOK_RWORD_BECOME:  m_pmi.send_rword("become"); break;
            case TOK_RWORD_UNSIZED: m_pmi.send_rword("unsized"); break;
            case TOK_RWORD_MACRO:   m_pmi.send_rword("macro");  break;

            // 2018
            case TOK_RWORD_ASYNC:   m_pmi.send_rword("async");  break;
            case TOK_RWORD_AWAIT:   m_pmi.send_rword("await");  break;
            case TOK_RWORD_DYN:     m_pmi.send_rword("dyn");    break;
            case TOK_RWORD_TRY:     m_pmi.send_rword("try");    break;
            }
        }
        void visit_tokentree(const ::TokenTree& tt)
        {
            if( tt.is_token() )
            {
                visit_token(tt.tok());
            }
            else
            {
                for(size_t i = 0; i < tt.size(); i ++)
                {
                    visit_tokentree(tt[i]);
                }
            }
        }

        void visit_pattern(const ::AST::Pattern& pat)
        {
            for(const auto& b : pat.bindings() ) {
                if( b.m_mutable ) {
                    m_pmi.send_rword("mut");
                }
                switch(b.m_type)
                {
                case ::AST::PatternBinding::Type::MOVE:
                    break;
                case ::AST::PatternBinding::Type::REF:
                    m_pmi.send_rword("ref");
                    break;
                case ::AST::PatternBinding::Type::MUTREF:
                    m_pmi.send_rword("ref");
                    m_pmi.send_rword("mut");
                    break;
                }
                if( b.m_name == "self" ) {
                    m_pmi.send_rword("self");
                    return;
                }
                else {
                    m_pmi.send_ident(b.m_name);
                }
                m_pmi.send_symbol("@");
            }
            TU_MATCH_HDRA( (pat.data()), { )
            default:
                TODO(sp, "visit_pattern " << pat.data().tag_str() << " - " << pat);
            TU_ARMA(Any, e) {
                m_pmi.send_symbol("_");
                }
            TU_ARMA(MaybeBind, e) {
                if( e.name == "self" ) {
                    m_pmi.send_rword("self");
                }
                else {
                    m_pmi.send_ident(e.name);
                }
                }
            TU_ARMA(Tuple, e) {
                m_pmi.send_symbol("(");
                visit_tuple_pattern(e);
                m_pmi.send_symbol(")");
                }
            TU_ARMA(Struct, e) {
                this->visit_path(e.path);
                m_pmi.send_symbol("{");
                for(const auto& spe : e.sub_patterns) {
                    this->visit_attrs(spe.attrs);
                    m_pmi.send_ident(spe.name);
                    m_pmi.send_symbol(":");
                    this->visit_pattern(spe.pat);
                    m_pmi.send_symbol(",");
                }
                if( !e.is_exhaustive ) {
                    m_pmi.send_symbol("...");
                }
                m_pmi.send_symbol("}");
                }
            }
        }
        void visit_tuple_pattern(const AST::Pattern::TuplePat& v)
        {
            for(const auto& p : v.start) {
                visit_pattern(p);
                m_pmi.send_symbol(",");
            }
            if( v.has_wildcard )
            {
                m_pmi.send_symbol("..");
                m_pmi.send_symbol(",");
                for(const auto& p : v.end) {
                    visit_pattern(p);
                    m_pmi.send_symbol(",");
                }
            }
        }
        void visit_lifetime(const AST::LifetimeRef& x)
        {
            if( x.binding() == AST::LifetimeRef::BINDING_STATIC ) {
                m_pmi.send_lifetime("static");
            }
            else if( x.binding() == AST::LifetimeRef::BINDING_INFER ) {
                m_pmi.send_lifetime("_");
            }
            else if( x.binding() == AST::LifetimeRef::BINDING_UNSPECIFIED ) {
                // Nothing
            }
            else {
                m_pmi.send_lifetime(x.name().name.c_str());
            }
        }
        void visit_type(const ::TypeRef& ty)
        {
            // TODO: Correct handling of visit_type
            TU_MATCHA( (ty.m_data), (te),
            (None,
                BUG(sp, ty);
                ),
            (Any,
                m_pmi.send_symbol("_");
                ),
            (Bang,
                m_pmi.send_symbol("!");
                ),
            (Unit,
                m_pmi.send_symbol("(");
                m_pmi.send_symbol(")");
                ),
            (Macro,
                visit_path(te.inv->path());
                m_pmi.send_symbol("!");
                m_pmi.send_symbol("(");
                visit_tokentree(te.inv->input_tt());
                m_pmi.send_symbol(")");
                ),
            (Primitive,
                TODO(sp, "proc_macro send primitive - " << ty);
                ),
            (Function,
                ::std::stringstream ss;
                ss << ty << " ";
                DEBUG("STRING: " << ss.str());

                parse_string(ss.str());
                ),
            (Tuple,
                m_pmi.send_symbol("(");
                for(const auto& st : te.inner_types)
                {
                    this->visit_type(st);
                    m_pmi.send_symbol(",");
                }
                m_pmi.send_symbol(")");
                ),
            (Borrow,
                m_pmi.send_symbol("&");
                this->visit_lifetime(te.lifetime);
                if( te.is_mut )
                    m_pmi.send_rword("mut");
                this->visit_type(*te.inner);
                ),
            (Pointer,
                m_pmi.send_symbol("*");
                if( te.is_mut )
                    m_pmi.send_rword("mut");
                else
                    m_pmi.send_rword("const");
                this->visit_type(*te.inner);
                ),
            (Array,
                m_pmi.send_symbol("[");
                this->visit_type(*te.inner);
                m_pmi.send_symbol(";");
                if( te.size ) {
                    this->visit_node(*te.size);
                }
                else {
                    m_pmi.send_symbol("_");
                }
                m_pmi.send_symbol("]");
                ),
            (Slice,
                m_pmi.send_symbol("[");
                this->visit_type(*te.inner);
                m_pmi.send_symbol("]");
                ),
            (Generic,
                // TODO: This may already be resolved?... Wait, how?
                m_pmi.send_ident(te.name.c_str());
                ),
            (Path,
                this->visit_path(*te);
                ),
            (TraitObject,
                m_pmi.send_symbol("(");
                m_pmi.send_rword("dyn");
                bool needs_plus = false;
                for(const auto& t : te.traits)
                {
                    if(needs_plus)  m_pmi.send_symbol("+");
                    needs_plus = true;
                    this->visit_hrbs(t.hrbs);
                    this->visit_path(*t.path);
                }
                for(const auto& lft : te.lifetimes) {
                    if( lft != AST::LifetimeRef() ) {
                        if(needs_plus)  m_pmi.send_symbol("+");
                        needs_plus = true;
                        this->visit_lifetime(lft);
                    }
                }
                m_pmi.send_symbol(")");
                ),
            (ErasedType,
                m_pmi.send_rword("impl");
                bool needs_plus = false;
                for(const auto& t : te.traits)
                {
                    if(needs_plus)  m_pmi.send_symbol("+");
                    needs_plus = true;
                    this->visit_hrbs(t.hrbs);
                    this->visit_path(*t.path);
                }
                for(const auto& lft : te.lifetimes) {
                    if(needs_plus)  m_pmi.send_symbol("+");
                    needs_plus = true;
                    m_pmi.send_symbol("+");
                    this->visit_lifetime(lft);
                }
                )
            )
        }
        void visit_hrbs(const AST::HigherRankedBounds& hrbs)
        {
            if( !hrbs.empty() )
            {
                m_pmi.send_rword("for");
                m_pmi.send_symbol("<");
                for(const auto& v : hrbs.m_lifetimes)
                {
                    m_pmi.send_lifetime(v.name().name.c_str());
                    m_pmi.send_symbol(",");
                }
                m_pmi.send_symbol(">");
            }
        }

        void visit_path_node(const AST::PathNode& e, bool is_expr)
        {
            m_pmi.send_ident(e.name().c_str());
            if( ! e.args().is_empty() )
            {
                if( e.args().m_is_paren ) {
                    auto& t = e.args().m_entries.at(0).as_Type();
                    this->visit_type(t);    // Should be a tuple
                    auto& rv = e.args().m_entries.at(1).as_AssociatedTyEqual();
                    m_pmi.send_symbol("->");
                    this->visit_type(rv.second);
                    return ;
                }

                if( is_expr )
                    m_pmi.send_symbol("::");
                m_pmi.send_symbol("<");
                for(const auto& ent : e.args().m_entries)
                {
                    TU_MATCH_HDRA( (ent), {)
                    TU_ARMA(Null, _) {}
                    TU_ARMA(Lifetime, l) {
                        m_pmi.send_lifetime(l.name().name.c_str());
                        m_pmi.send_symbol(",");
                        }
                    TU_ARMA(Type, t) {
                        this->visit_type(t);
                        m_pmi.send_symbol(",");
                        }
                    TU_ARMA(Value, n) {
                        m_pmi.send_symbol("{");
                        this->visit_node(*n);
                        m_pmi.send_symbol("}");
                        m_pmi.send_symbol(",");
                        }
                    TU_ARMA(AssociatedTyEqual, a) {
                        visit_path_node(a.first, false);
                        m_pmi.send_symbol("=");
                        this->visit_type(a.second);
                        m_pmi.send_symbol(",");
                        }
                    TU_ARMA(AssociatedTyBound, a) {
                        visit_path_node(a.first, false);
                        m_pmi.send_symbol(":");
                        for(const auto& p : a.second) {
                            if( &p != a.second.data() )
                                m_pmi.send_symbol("+");
                            this->visit_path(p);
                        }
                        m_pmi.send_symbol(",");
                        }
                    }
                }
                m_pmi.send_symbol(">");
            }
        }

        void visit_path(const AST::Path& path, bool is_expr=false)
        {
            const ::std::vector<AST::PathNode>*  nodes = nullptr;
            TU_MATCH_HDRA( (path.m_class), {)
            TU_ARMA(Invalid, pe) {
                BUG(sp, "Invalid path");
                }
            TU_ARMA(Local, pe) {
                m_pmi.send_ident(pe.name.c_str());
                }
            TU_ARMA(Relative, pe) {
                // TODO: Send hygiene information
                nodes = &pe.nodes;
                }
            TU_ARMA(Self, pe) {
                m_pmi.send_rword("self");
                m_pmi.send_symbol("::");
                nodes = &pe.nodes;
                }
            TU_ARMA(Super, pe) {
                for(unsigned i = 0; i < pe.count; i ++)
                {
                    m_pmi.send_rword("super");
                    m_pmi.send_symbol("::");
                }
                nodes = &pe.nodes;
                }
            TU_ARMA(Absolute, pe) {
                if( pe.crate == "" ) {
                    m_pmi.send_rword("crate");
                }
                else {
                    m_pmi.send_symbol("::");
                    m_pmi.send_string(pe.crate.c_str());
                }
                m_pmi.send_symbol("::");
                nodes = &pe.nodes;
                }
            TU_ARMA(UFCS, pe) {
                m_pmi.send_symbol("<");
                this->visit_type(*pe.type);
                if( pe.trait )
                {
                    m_pmi.send_rword("as");
                    this->visit_path(*pe.trait);
                }
                m_pmi.send_symbol(">");
                m_pmi.send_symbol("::");
                nodes = &pe.nodes;
                }
            }
            bool first = true;
            for(const auto& e : *nodes)
            {
                if(!first)
                    m_pmi.send_symbol("::");
                first = false;
                visit_path_node(e, is_expr);
            }
        }
        void visit_params(const AST::GenericParams& params)
        {
            if( !params.m_params.empty() )
            {
                bool is_first = true;
                m_pmi.send_symbol("<");
                for( const auto& param : params.m_params )
                {
                    if( !is_first )
                        m_pmi.send_symbol(",");
                    TU_MATCH_HDRA( (param), {)
                    TU_ARMA(None, p) {
                        // Uh... oops?
                        BUG(sp, "Enountered GenericParam::None");
                        }
                    TU_ARMA(Lifetime, p) {
                        m_pmi.send_lifetime(p.name().name.c_str());
                        bool first = true;
                        for(size_t i = param.bounds_start; i < param.bounds_end; i++) {
                            if(!params.m_bounds[i].is_None()) {
                                if(first) { m_pmi.send_symbol(":"); first = false; }
                                else { m_pmi.send_symbol("+"); }
                            }
                            TU_MATCH_HDRA((params.m_bounds[i]), {)
                            default:
                                BUG(sp, "");
                            TU_ARMA(None, be) {}
                            TU_ARMA(Lifetime, be) {
                                m_pmi.send_lifetime(be.test.name().name.c_str());
                                }
                            }
                        }
                        }
                    TU_ARMA(Type, p) {
                        this->visit_attrs(p.attrs());
                        m_pmi.send_ident(p.name().c_str());
                        bool first = true;
                        for(size_t i = param.bounds_start; i < param.bounds_end; i++) {
                            if(!params.m_bounds[i].is_None()) {
                                if(first) { m_pmi.send_symbol(":"); first = false; }
                                else { m_pmi.send_symbol("+"); }
                            }
                            TU_MATCH_HDRA((params.m_bounds[i]), {)
                            default:
                                BUG(sp, "Unhandled bound type - " << params.m_bounds[i]);
                            TU_ARMA(None, be) {}
                            TU_ARMA(TypeLifetime, be) {
                                m_pmi.send_lifetime(be.bound.name().name.c_str());
                                }
                            TU_ARMA(IsTrait, be) {
                                assert(be.outer_hrbs.empty());  // Shouldn't be possible in this position
                                if( !be.inner_hrbs.empty() ) {
                                    TODO(sp, "be.inner_hrbs");
                                }
                                visit_path(be.trait);
                                }
                            TU_ARMA(MaybeTrait, be) {
                                m_pmi.send_symbol("?");
                                visit_path(be.trait);
                                }
                            }
                        }
                        if( !p.get_default().is_wildcard() )
                        {
                            m_pmi.send_symbol("=");
                            this->visit_type(p.get_default());
                        }
                        }
                    TU_ARMA(Value, p) {
                        this->visit_attrs(p.attrs());
                        m_pmi.send_rword("const");
                        m_pmi.send_ident(p.name().name.c_str());
                        m_pmi.send_symbol(":");
                        visit_type(p.type());
                        assert(param.bounds_start == param.bounds_end);
                        }
                    }
                    is_first = false;
                }
                m_pmi.send_symbol(">");
            }
        }
        void visit_hrb(const AST::HigherRankedBounds& hrb)
        {
            if( !hrb.empty() ) {
                m_pmi.send_rword("for");
                m_pmi.send_symbol("<");
                for(const auto& lft : hrb.m_lifetimes) {
                    m_pmi.send_lifetime(lft.name().name.c_str());
                    m_pmi.send_symbol(",");
                }
                m_pmi.send_symbol(">");
            }
        }
        void visit_bounds(const AST::GenericParams& params)
        {
            if( !params.m_bounds.empty() )
            {
                bool where_sent = false;

                for(const auto& e : params.m_bounds)
                {
                    size_t i = &e - params.m_bounds.data();
                    bool already_emitted = false;
                    for(const auto& p : params.m_params) {
                        if(p.is_None())
                            continue ;
                        if( p.bounds_start <= i && i < p.bounds_end )
                            already_emitted = true;
                    }
                    if(already_emitted || e.is_None())
                        continue ;

                    if( !where_sent ) {
                        m_pmi.send_rword("where");
                        where_sent = true;
                    }
                    TU_MATCH_HDRA((e), {)
                    TU_ARMA(None, be)   continue;
                    TU_ARMA(Lifetime, be) {
                        m_pmi.send_lifetime(be.bound.name().name.c_str());
                        m_pmi.send_symbol(":");
                        m_pmi.send_lifetime(be.test.name().name.c_str());
                        }
                    TU_ARMA(TypeLifetime, be) {
                        visit_type(be.type);
                        m_pmi.send_symbol(":");
                        m_pmi.send_lifetime(be.bound.name().name.c_str());
                        }
                    TU_ARMA(IsTrait, be) {
                        visit_hrbs(be.outer_hrbs);
                        visit_type(be.type);
                        m_pmi.send_symbol(":");
                        visit_hrbs(be.inner_hrbs);
                        visit_path(be.trait);
                        }
                    TU_ARMA(MaybeTrait, be) {
                        visit_type(be.type);
                        m_pmi.send_symbol(":");
                        m_pmi.send_symbol("?");
                        visit_path(be.trait);
                        }
                    TU_ARMA(NotTrait, be) {
                        visit_type(be.type);
                        m_pmi.send_symbol(":");
                        m_pmi.send_symbol("!");
                        visit_path(be.trait);
                        }
                    TU_ARMA(Equality, be) {
                        visit_type(be.type);
                        m_pmi.send_symbol("=");
                        visit_type(be.replacement);
                        }
                    }
                    m_pmi.send_symbol(",");
                }
            }
        }
        void visit_node(const ::AST::ExprNode& e)
        {
            DEBUG("NODE: " << e);
            // TODO: Dump to a string, then re-parse into a TT and then send that TT
            // - Avoids needing to repeat logic
            ::std::stringstream ss;
            DumpAST_Node(ss, e);
            ss << " ";
            DEBUG("STRING: " << ss.str());

            //const_cast<::AST::ExprNode&>(e).visit(*this);
            parse_string(ss.str());
        }
        void parse_string(const ::std::string& s)
        {
            ::std::istringstream    iss { s };
            Lexer   l { iss, AST::Edition::Rust2021, {} };
            for(;;) {
                auto t = l.getToken();
                if( t == TOK_EOF ) {
                    break;
                }
                // TODO: If this is an ident, then get the comment after it that specifies the hygine info
                visit_token(t);
            }
        }
        void visit_nodes(const ::AST::Expr& e)
        {
            this->visit_node(e.node());
        }

        // === Expressions ====
        void visit(::AST::ExprNode_Block& node) {
            switch(node.m_block_type) {
            case AST::ExprNode_Block::Type::Bare:
                break;
            case AST::ExprNode_Block::Type::Unsafe:
                m_pmi.send_rword("unsafe");
                break;
            case AST::ExprNode_Block::Type::Const:
                m_pmi.send_rword("const");
                break;
            }
            m_pmi.send_symbol("{");
            for(const auto& n : node.m_nodes)
            {
                this->visit_node(*n.node);
                if( n.has_semicolon ) {
                    m_pmi.send_symbol(";");
                }
            }
            m_pmi.send_symbol("}");
        }
        void visit(::AST::ExprNode_AsyncBlock& node) {
            TODO(sp, "ExprNode_Try");
        }
        void visit(::AST::ExprNode_Try& node) {
            TODO(sp, "ExprNode_Try");
        }
        void visit(::AST::ExprNode_Macro& node) {
            TODO(sp, "ExprNode_Macro");
        }
        void visit(::AST::ExprNode_Asm& node) {
            TODO(sp, "ExprNode_Asm");
        }
        void visit(::AST::ExprNode_Asm2& node) {
            TODO(sp, "ExprNode_Asm");
        }
        void visit(::AST::ExprNode_Flow& node) {
            TODO(sp, "ExprNode_Flow");
        }
        void visit(::AST::ExprNode_LetBinding& node) {
            TODO(sp, "ExprNode_LetBinding");
        }
        void visit(::AST::ExprNode_Assign& node) {
            TODO(sp, "ExprNode_Assign");
        }
        void visit(::AST::ExprNode_CallPath& node) {
            TODO(sp, "ExprNode_CallPath");
        }
        void visit(::AST::ExprNode_CallMethod& node) {
            TODO(sp, "ExprNode_CallMethod");
        }
        void visit(::AST::ExprNode_CallObject& node) {
            TODO(sp, "ExprNode_CallObject");
        }
        void visit(::AST::ExprNode_Loop& node) {
            TODO(sp, "ExprNode_Loop");
        }
        void visit(::AST::ExprNode_WhileLet& node) {
            TODO(sp, "ExprNode_Loop");
        }
        void visit(::AST::ExprNode_Match& node) {
            TODO(sp, "ExprNode_Match");
        }
        void visit(::AST::ExprNode_If& node) {
            TODO(sp, "ExprNode_If");
        }
        void visit(::AST::ExprNode_IfLet& node) {
            TODO(sp, "ExprNode_IfLet");
        }

        void visit(::AST::ExprNode_WildcardPattern& node) {
            m_pmi.send_symbol("_");
        }
        void visit(::AST::ExprNode_Integer& node) {
            m_pmi.send_int(node.m_datatype, node.m_value);
        }
        void visit(::AST::ExprNode_Float& node) {
            TODO(sp, "ExprNode_Float");
        }
        void visit(::AST::ExprNode_Bool& node) {
            TODO(sp, "ExprNode_Bool");
        }
        void visit(::AST::ExprNode_String& node) {
            TODO(sp, "ExprNode_String");
        }
        void visit(::AST::ExprNode_ByteString& node) {
            TODO(sp, "ExprNode_ByteString");
        }
        void visit(::AST::ExprNode_CString& node) {
            TODO(sp, "ExprNode_CString");
        }
        void visit(::AST::ExprNode_Closure& node) {
            TODO(sp, "ExprNode_Closure");
        }
        void visit(::AST::ExprNode_StructLiteral& node) {
            TODO(sp, "ExprNode_StructLiteral");
        }
        void visit(::AST::ExprNode_StructLiteralPattern& node) {
            TODO(sp, "ExprNode_StructLiteralPattern");
        }
        void visit(::AST::ExprNode_Array& node) {
            TODO(sp, "ExprNode_Array");
        }
        void visit(::AST::ExprNode_Tuple& node) {
            TODO(sp, "ExprNode_Tuple");
        }
        void visit(::AST::ExprNode_NamedValue& node) {
            this->visit_path(node.m_path, true);
        }

        void visit(::AST::ExprNode_Field& node) {
            TODO(sp, "ExprNode_Field");
        }
        void visit(::AST::ExprNode_Index& node) {
            TODO(sp, "ExprNode_Index");
        }
        void visit(::AST::ExprNode_Deref& node) {
            TODO(sp, "ExprNode_Deref");
        }
        void visit(::AST::ExprNode_Cast& node) {
            TODO(sp, "ExprNode_Cast");
        }
        void visit(::AST::ExprNode_TypeAnnotation& node) {
            TODO(sp, "ExprNode_TypeAnnotation");
        }
        void visit(::AST::ExprNode_BinOp& node) {
            TODO(sp, "ExprNode_BinOp");
        }
        void visit(::AST::ExprNode_UniOp& node) {
            TODO(sp, "ExprNode_UniOp");
        }

        void visit_top_attrs(slice<const ::AST::Attribute>& attrs)
        {
            for(const auto& a : attrs)
            {
                this->visit_attr(a);
            }
        }
        void visit_attrs(const ::AST::AttributeList& attrs)
        {
            for(const auto& a : attrs.m_items)
            {
                this->visit_attr(a);
            }
        }
        void visit_attr(const ::AST::Attribute& a)
        {
            if( a.name() == "cfg_attr" ) {
                auto new_attrs = check_cfg_attr(a);
                for(const auto& na : new_attrs) {
                    this->visit_attr(na);
                }
            }
            if( this->emit_all_attrs || (a.name().is_trivial() && m_pmi.attr_is_used(a.name().as_trivial())) )
            {
                DEBUG("Send " << a);
                m_pmi.send_symbol("#");
                m_pmi.send_symbol("[");
                this->visit_meta_item(a);
                m_pmi.send_symbol("]");
            }
            else {
                DEBUG("Skip " << a << " (" << m_pmi.m_proc_macro_desc.attributes << ")");
            }
        }
        void visit_meta_item(const ::AST::Attribute& i)
        {
            for(const auto& e : i.name().elems)
            {
                if( &e != &i.name().elems.front() )
                    m_pmi.send_symbol("::");
                m_pmi.send_ident(e.c_str());
            }

            visit_tokentree(i.data());
        }
        void visit_vis(const ::AST::Visibility& vis)
        {
            switch(vis.ty())
            {
            case ::AST::Visibility::Ty::Private:
                break;
            case ::AST::Visibility::Ty::Pub:
                m_pmi.send_rword("pub");
                break;
            case ::AST::Visibility::Ty::Crate:
                m_pmi.send_rword("crate");
                break;
            case ::AST::Visibility::Ty::PubCrate:
                m_pmi.send_rword("pub");
                m_pmi.send_symbol("(");
                m_pmi.send_rword("crate");
                m_pmi.send_symbol(")");
                break;
            case ::AST::Visibility::Ty::PubSuper:
                m_pmi.send_rword("pub");
                m_pmi.send_symbol("(");
                m_pmi.send_rword("super");
                m_pmi.send_symbol(")");
                break;
            case ::AST::Visibility::Ty::PubSelf:
                m_pmi.send_rword("pub");
                m_pmi.send_symbol("(");
                m_pmi.send_rword("self");
                m_pmi.send_symbol(")");
                break;
            case ::AST::Visibility::Ty::PubIn:
                m_pmi.send_rword("pub");
                m_pmi.send_symbol("(");
                m_pmi.send_rword("in");
                visit_path(vis.in_path());
                m_pmi.send_symbol(")");
                break;
            }
        }

        void visit_struct(const ::std::string& name, const AST::Visibility& vis, const ::AST::Struct& str)
        {
            this->visit_vis(vis);
            m_pmi.send_rword("struct");
            m_pmi.send_ident(name.c_str());
            this->visit_params(str.params());
            TU_MATCH(AST::StructData, (str.m_data), (se),
            (Unit,
                this->visit_bounds(str.params());
                m_pmi.send_symbol(";");
                ),
            (Tuple,
                m_pmi.send_symbol("(");
                for( const auto& si : se.ents )
                {
                    this->visit_attrs(si.m_attrs);
                    this->visit_vis(si.m_vis);
                    this->visit_type(si.m_type);
                    m_pmi.send_symbol(",");
                }
                m_pmi.send_symbol(")");
                this->visit_bounds(str.params());
                m_pmi.send_symbol(";");
                ),
            (Struct,
                this->visit_bounds(str.params());
                m_pmi.send_symbol("{");

                for( const auto& si : se.ents )
                {
                    this->visit_attrs(si.m_attrs);
                    this->visit_vis(si.m_vis);
                    m_pmi.send_ident(si.m_name.c_str());
                    m_pmi.send_symbol(":");
                    this->visit_type(si.m_type);
                    m_pmi.send_symbol(",");
                }
                m_pmi.send_symbol("}");
                )
            )
        }
        void visit_enum(const ::std::string& name, const AST::Visibility& vis, const ::AST::Enum& enm)
        {
            this->visit_vis(vis);

            m_pmi.send_rword("enum");
            m_pmi.send_ident(name.c_str());
            this->visit_params(enm.params());
            this->visit_bounds(enm.params());
            m_pmi.send_symbol("{");
            for(const auto& v : enm.variants())
            {
                this->visit_attrs(v.m_attrs);
                m_pmi.send_ident(v.m_name.c_str());
                TU_MATCH_HDRA( (v.m_data), { )
                TU_ARMA(Value, e) {
                    if( e.m_value )
                    {
                        m_pmi.send_symbol("=");
                        this->visit_nodes(e.m_value);
                    }
                    }
                TU_ARMA(Tuple, e) {
                    m_pmi.send_symbol("(");
                    for(const auto& f : e.m_items)
                    {
                        this->visit_attrs(f.m_attrs);
                        this->visit_type(f.m_type);
                        m_pmi.send_symbol(",");
                    }
                    m_pmi.send_symbol(")");
                    }
                TU_ARMA(Struct, e) {
                    m_pmi.send_symbol("{");
                    for(const auto& f : e.m_fields)
                    {
                        this->visit_attrs(f.m_attrs);
                        m_pmi.send_ident(f.m_name.c_str());
                        m_pmi.send_symbol(":");
                        this->visit_type(f.m_type);
                        m_pmi.send_symbol(",");
                    }
                    m_pmi.send_symbol("}");
                    }
                }
                m_pmi.send_symbol(",");
            }
            m_pmi.send_symbol("}");
        }
        void visit_union(const ::std::string& name, const AST::Visibility& vis, const ::AST::Union& unn)
        {
            TODO(sp, "visit_union");
        }

        void visit_function(const ::std::string& name, const AST::Visibility& vis, const ::AST::Function& fcn)
        {
            this->visit_vis(vis);

            if( fcn.is_unsafe() ) {
                m_pmi.send_rword("unsafe");
            }
            if( fcn.is_const() ) {
                m_pmi.send_rword("const");
            }
            if( fcn.is_async() ) {
                m_pmi.send_rword("async");
            }
            if( fcn.abi() != ABI_RUST ) {
                m_pmi.send_rword("extern");
                m_pmi.send_string(fcn.abi());
            }
            m_pmi.send_rword("fn");
            m_pmi.send_ident(name.c_str());
            this->visit_params(fcn.params());
            m_pmi.send_symbol("(");
            for( const auto& arg : fcn.args() ) {
                this->visit_attrs(arg.attrs);
                this->visit_pattern(arg.pat);
                m_pmi.send_symbol(":");
                this->visit_type(arg.ty);
                m_pmi.send_symbol(",");
            }
            if( fcn.is_variadic() ) {
                m_pmi.send_symbol("...");
            }
            m_pmi.send_symbol(")");
            //if( fcn.rettype() != TypeRef() ) {
                m_pmi.send_symbol("->");
                this->visit_type(fcn.rettype());
            //}
            this->visit_bounds(fcn.params());
            this->visit_nodes(fcn.code());
        }

        void visit_use(const ::std::string& name, const AST::Visibility& vis, const ::AST::UseItem& item)
        {
            this->visit_vis(vis);
            m_pmi.send_rword("use");

            if( item.entries.size() == 1 ) {
                visit_path(item.entries[0].path);
                if( item.entries[0].name == "" ) {
                    m_pmi.send_symbol("::");
                    m_pmi.send_symbol("*");
                }
                else if( item.entries[0].name != item.entries[0].path.nodes().back().name() ) {
                    m_pmi.send_rword("as");
                    m_pmi.send_ident( item.entries[0].name.c_str() );
                }
                else {
                }
            }
            else {
                TODO(sp, "Multiple items");
            }
            m_pmi.send_symbol(";");
        }

        void visit_item(const ::std::string& name, const AST::Visibility& vis, const ::AST::Item& item)
        {
            TU_MATCH_HDRA((item), {)
            default:
                TODO(sp, "visit_item - " << item.tag_str());
                break;
            TU_ARMA(Use, e) {
                visit_use(name, vis, e);
                }
            // Types
            TU_ARMA(Struct, e) {
                visit_struct(name, vis, e);
                }
            TU_ARMA(Enum, e) {
                visit_enum(name, vis, e);
                }
            TU_ARMA(Union, e) {
                visit_union(name, vis, e);
                }

            // Values
            TU_ARMA(Function, e) {
                visit_function(name, vis, e);
                }
            }
        }
    };
}
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, const TokenTree* attr_input, std::function<void(Visitor& v)> cb)
{
    // 1. Create ProcMacroInv instance
    auto pmi = ProcMacro_Invoke_int(sp, crate, mac_path);
    if( !pmi.check_good() )
        return ::std::unique_ptr<TokenStream>();
    if( attr_input ) {
        // TODO: Assert that this is a `#[proc_macro_attribute]` macro
        if( attr_input->size() != 0 )
        {
            // If the input is non-empty, then it must be a parenthesised token tree
            ASSERT_BUG(sp, attr_input->size() >= 2, "");
            ASSERT_BUG(sp, (*attr_input)[0].tok() == TOK_PAREN_OPEN || (*attr_input)[0].tok() == TOK_SQUARE_OPEN, "");
            Visitor v(sp, pmi);
            // - Strip the parens when sending
            for(size_t i = 1; i < attr_input->size() - 1; i++)
            {
                v.visit_tokentree( (*attr_input)[i] );
            }
        }
        pmi.send_done();
    }
    // 2. Feed item as a token stream.
    Visitor v(sp, pmi);
    cb(v);
    pmi.send_done();
    // 3. Return boxed invocation instance
    return box$(pmi);
}
// --- Derive inputs
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, slice<const AST::Attribute> attrs, const AST::Visibility& vis, const ::std::string& item_name, const ::AST::Struct& i)
{
    return ProcMacro_Invoke(sp, crate, mac_path, nullptr, [&](Visitor& v){
        DEBUG("derive on struct");
        v.visit_top_attrs(attrs);
        v.visit_struct(item_name, vis, i);
        });
}
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, slice<const AST::Attribute> attrs, const AST::Visibility& vis, const ::std::string& item_name, const ::AST::Enum& i)
{
    return ProcMacro_Invoke(sp, crate, mac_path, nullptr, [&](Visitor& v){
        DEBUG("derive on enum");
        v.visit_top_attrs(attrs);
        v.visit_enum(item_name, vis, i);
        });
}
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, slice<const AST::Attribute> attrs, const AST::Visibility& vis, const ::std::string& item_name, const ::AST::Union& i)
{
    return ProcMacro_Invoke(sp, crate, mac_path, nullptr, [&](Visitor& v){
        DEBUG("derive on union");
        v.visit_top_attrs(attrs);
        v.visit_union(item_name, vis, i);
        });
}
// --- attribute
::std::unique_ptr<TokenStream> ProcMacro_Invoke(
    const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, const TokenTree& tt,
    slice<const AST::Attribute> attrs, const AST::Visibility& vis, const ::std::string& item_name, const ::AST::Item& i
    )
{
    return ProcMacro_Invoke(sp, crate, mac_path, &tt, [&](Visitor& v) {
        v.emit_all_attrs = true;
        v.visit_top_attrs(attrs);
        v.visit_item(item_name, vis, i);
        });
}
// -- function-like input
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, const TokenTree& tt)
{
    return ProcMacro_Invoke(sp, crate, mac_path, nullptr, [&](Visitor& v){
        v.visit_tokentree(tt);
        });
}

ProcMacroInv::ProcMacroInv(const Span& sp, AST::Edition edition, const char* executable, const ::HIR::ProcMacro& proc_macro_desc):
    TokenStream(ParseState()),
    m_parent_span(sp),
    m_this_span( Span( m_parent_span, proc_macro_desc.path.crate_name(), proc_macro_desc.name ) ),
    m_proc_macro_desc(proc_macro_desc),
    m_edition(edition)
{
    if( getenv("MRUSTC_DUMP_PROCMACRO") && getenv("MRUSTC_DUMP_PROCMACRO")[0] )
    {
        // TODO: Dump both input and output, AND (optionally) dump each invocation
        static unsigned int dump_count = 0;
        std::string name_prefix;
        name_prefix = FMT(getenv("MRUSTC_DUMP_PROCMACRO") << "-" << dump_count);
        DEBUG("Dumping to " << name_prefix);
        m_dump_file_out.open( FMT(name_prefix << "-out.bin"), ::std::ios::out | ::std::ios::binary );
        m_dump_file_res.open( FMT(name_prefix << "-res.bin"), ::std::ios::out | ::std::ios::binary );
        dump_count ++;
    }
    else
    {
        DEBUG("Set MRUSTC_DUMP_PROCMACRO=procmacro_dump to dump to `procmacro_dump-NNN-{out,res}.bin`");
    }
#ifdef _WIN32
    std::string commandline = std::string{ executable } + " " + proc_macro_desc.name.c_str();
    DEBUG(commandline);

    HANDLE stdin_read = INVALID_HANDLE_VALUE;
    HANDLE stdin_write = INVALID_HANDLE_VALUE;
    HANDLE stdout_read = INVALID_HANDLE_VALUE;
    HANDLE stdout_write = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES saAttr{};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = true;
    saAttr.lpSecurityDescriptor = nullptr;

    // Create a pipe for the child process's STDOUT.
    if( !CreatePipe(&stdout_read, &stdout_write, &saAttr, 0) ) {
        BUG(sp, "stdout CreatePipe failed: " << GetLastError());
    }

    // Ensure the read handle to the pipe for STDOUT is not inherited.
    if( !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) ) {
        BUG(sp, "stdout SetHandleInformation failed: " << GetLastError());
    }

    // Create a pipe for the child process's STDIN.
    if( !CreatePipe(&stdin_read, &stdin_write, &saAttr, 0) ) {
        BUG(sp, "stdin CreatePipe failed: " << GetLastError());
    }

    // Ensure the write handle to the pipe for STDIN is not inherited.
    if( !SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0) ) {
        BUG(sp, "stdin SetHandleInformation failed: " << GetLastError());
    }

    // Create the child process.
    PROCESS_INFORMATION piProcInfo{};
    STARTUPINFO siStartInfo{};
    siStartInfo.cb = sizeof(STARTUPINFO);
    siStartInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    siStartInfo.hStdOutput = stdout_write;
    siStartInfo.hStdInput = stdin_read;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;
    if( !CreateProcessA(executable, const_cast<char*>(commandline.c_str()), NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo) )
    {
        BUG(sp, "Error in CreateProcessW - " << GetLastError() << " - can't start `" << executable << "`");
    }

    this->handles.child_stdin = stdin_write;
    this->handles.child_stdout = stdout_read;
    this->handles.child_handle = piProcInfo.hProcess;

    // Close the handles we don't care about.
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(piProcInfo.hThread);
#else
     int    stdin_pipes[2];
    if( pipe(stdin_pipes) != 0 )
    {
        BUG(sp, "Unable to create stdin pipe pair for proc macro, " << strerror(errno));
    }
    this->handles.child_stdin = stdin_pipes[1]; // Write end
     int    stdout_pipes[2];
    if( pipe(stdout_pipes) != 0)
    {
        BUG(sp, "Unable to create stdout pipe pair for proc macro, " << strerror(errno));
    }
    this->handles.child_stdout = stdout_pipes[0]; // Read end

    posix_spawn_file_actions_t  file_actions;
    posix_spawn_file_actions_init(&file_actions);
    posix_spawn_file_actions_adddup2(&file_actions, stdin_pipes[0], 0);
    posix_spawn_file_actions_adddup2(&file_actions, stdout_pipes[1], 1);
    posix_spawn_file_actions_addclose(&file_actions, stdin_pipes[0]);
    posix_spawn_file_actions_addclose(&file_actions, stdin_pipes[1]);
    posix_spawn_file_actions_addclose(&file_actions, stdout_pipes[0]);
    posix_spawn_file_actions_addclose(&file_actions, stdout_pipes[1]);

    char*   argv[3] = { const_cast<char*>(executable), const_cast<char*>(proc_macro_desc.name.c_str()), nullptr };
    DEBUG(argv[0] << " " << argv[1]);
    //char*   envp[] = { nullptr };
    int rv = posix_spawn(&this->handles.child_pid, executable, &file_actions, nullptr, argv, environ);
    if( rv != 0 )
    {
        BUG(sp, "Error in posix_spawn - " << rv << " - can't start `" << executable << "`");
    }

    posix_spawn_file_actions_destroy(&file_actions);
    // Close the ends we don't care about.
    close(stdin_pipes[0]);
    close(stdout_pipes[1]);
#endif

    // Invocation span is #1 (#0 is always empty/undefined)
    this->send_span_def(1, sp);
}
ProcMacroInv::Handles::Handles(Handles&& x):
#ifdef _WIN32
    child_handle(x.child_handle),
    child_stdin(x.child_stdin),
    child_stdout(x.child_stdout)
#else
    child_pid(x.child_pid),
    child_stdin(x.child_stdin),
    child_stdout(x.child_stdout)
#endif
{
#ifdef _WIN32
    x.child_handle = INVALID_HANDLE_VALUE;
    x.child_stdin = INVALID_HANDLE_VALUE;
    x.child_stdout = INVALID_HANDLE_VALUE;
#else
    x.child_pid = 0;
    x.child_stdin = -1;
    x.child_stdout = -1;
#endif
    DEBUG("");
}
#if 0
ProcMacroInv::Handles& ProcMacroInv::Handles::operator=(Handles&& x)
{
#ifdef _WIN32
    child_handle = x.child_handle;
    child_stdin  = x.child_stdin;
    child_stdout = x.child_stdout;
    x.child_handle = INVALID_HANDLE_VALUE;
    x.child_stdin = INVALID_HANDLE_VALUE;
    x.child_stdout = INVALID_HANDLE_VALUE;
#else
    child_pid = x.child_pid;
    child_stdin = x.child_stdin;
    child_stdout = x.child_stdout;

    x.child_pid = 0;
#endif
    DEBUG("");
    return *this;
}
#endif
ProcMacroInv::~ProcMacroInv()
{
#ifdef _WIN32
    if( this->handles.child_handle != INVALID_HANDLE_VALUE )
    {
        DEBUG("Waiting for child to terminate");
        WaitForSingleObject(this->handles.child_handle, INFINITE);
        CloseHandle(this->handles.child_stdout);
        CloseHandle(this->handles.child_stdin);
        CloseHandle(this->handles.child_handle);
    }
#else
    if( this->handles.child_pid != 0 )
    {
        DEBUG("Waiting for child " << this->handles.child_pid << " to terminate");
        int status;
        waitpid(this->handles.child_pid, &status, 0);
        close(this->handles.child_stdout);
        close(this->handles.child_stdin);
    }
#endif
}
bool ProcMacroInv::check_good()
{
    char    v;
#ifdef _WIN32
    DWORD rv = 0;
    if( !ReadFile(this->handles.child_stdout, &v, 1, &rv, nullptr) )
    {
        DEBUG("Error reading from child, " << GetLastError());
        return false;
    }
#else
    int rv = read(this->handles.child_stdout, &v, 1);
#endif
    if( rv == 0 )
    {
        DEBUG("Unexpected EOF from child");
        return false;
    }
#ifndef _WIN32
    if( rv < 0 )
    {
        DEBUG("Error reading from child, rv=" << rv << " " << strerror(errno));
        return false;
    }
#endif
    DEBUG("Child started, value = " << (int)v);
    if( v != 0 )
        return false;
    return true;
}
void ProcMacroInv::send_u8(uint8_t v)
{
    this->send_bytes_raw(&v, 1);
}
void ProcMacroInv::send_bytes(const void* val, size_t size)
{
    this->send_v128u( static_cast<uint64_t>(size) );
    this->send_bytes_raw(val, size);
}
void ProcMacroInv::send_bytes_raw(const void* val, size_t size)
{
    if( m_dump_file_out.is_open() )
        m_dump_file_out.write( reinterpret_cast<const char*>(val), size);
#ifdef _WIN32
    DWORD bytesWritten = 0;
    if( !WriteFile(this->handles.child_stdin, val, size, &bytesWritten, nullptr) || bytesWritten != size )
        BUG(m_parent_span, "Error writing to child, " << GetLastError());
#else
    if( write(this->handles.child_stdin, val, size) != static_cast<ssize_t>(size) )
        BUG(m_parent_span, "Error writing to child, " << strerror(errno));
#endif
}
void ProcMacroInv::send_v128u(uint64_t val)
{
    while( val >= 128 ) {
        this->send_u8( static_cast<uint8_t>(val & 0x7F) | 0x80 );
        val >>= 7;
    }
    this->send_u8( static_cast<uint8_t>(val & 0x7F) );
}
void ProcMacroInv::send_v128u(U128 val)
{
    while( val >= U128(128) ) {
        this->send_u8( static_cast<uint8_t>(val.truncate_u64() & 0x7F) | 0x80 );
        val >>= 7;
    }
    this->send_u8( static_cast<uint8_t>(val.truncate_u64() & 0x7F) );
}
uint8_t ProcMacroInv::recv_u8()
{
    uint8_t v;
    this->recv_bytes_raw(&v, 1);
    return v;
}
::std::string ProcMacroInv::recv_bytes()
{
    auto len = this->recv_v128u();
    ASSERT_BUG(this->m_parent_span, len < SIZE_MAX, "Oversized string from child process");
    ::std::string   val;
    val.resize(len);

    recv_bytes_raw(&val[0], len);

    return val;
}
void ProcMacroInv::recv_bytes_raw(void* out_void, size_t len)
{
    uint8_t* val = reinterpret_cast<uint8_t*>(out_void);
    size_t  ofs = 0, rem = len;
    while( rem > 0 )
    {
#ifdef _WIN32
        DWORD n;
        ReadFile(this->handles.child_stdout, &val[ofs], rem, &n, nullptr);
#else
        auto n = read(this->handles.child_stdout, &val[ofs], rem);
#endif
        if( n == 0 ) {
            BUG(this->m_this_span, "Unexpected EOF while reading from child process");
        }
        if( n < 0 ) {
            BUG(this->m_parent_span, "Error while reading from child process");
        }
        assert(static_cast<size_t>(n) <= rem);
        ofs += n;
        rem -= n;
    }

    if( m_dump_file_res.is_open() ) {
        m_dump_file_res.write( reinterpret_cast<const char*>(out_void), len );
        m_dump_file_res.flush();
    }
}
uint64_t ProcMacroInv::recv_v128u()
{
    uint64_t    v = 0;
    unsigned    ofs = 0;
    for(;;)
    {
        auto b = recv_u8();
        v |= static_cast<uint64_t>(b & 0x7F) << ofs;
        if( (b & 0x80) == 0 )
            break;
        ofs += 7;
    }
    return v;
}
U128 ProcMacroInv::recv_v128u_u128()
{
    U128    v(0);
    unsigned    ofs = 0;
    for(;;)
    {
        auto b = recv_u8();
        v |= U128(b & 0x7F) << ofs;
        if( (b & 0x80) == 0 )
            break;
        ofs += 7;
    }
    return v;
}

Position ProcMacroInv::getPosition() const {
    DEBUG("" << m_this_span);
    //return Position(m_this_span);
    return Position(m_parent_span);
}
Token ProcMacroInv::realGetToken() {
    auto rv = this->realGetToken_();
    DEBUG(rv);
    return rv;
}
Token ProcMacroInv::realGetToken_() {
    if( m_eof_hit )
        return Token(TOK_EOF);
    uint8_t v = this->recv_u8();

    switch( static_cast<TokenClass>(v) )
    {
    case TokenClass::EndOfStream:
        TODO(this->m_parent_span, "EndOfStream");
    case TokenClass::SpanRef:
        TODO(this->m_parent_span, "SpanDef");
    case TokenClass::SpanDef:
        TODO(this->m_parent_span, "SpanDef");
        break;
    case TokenClass::Symbol: {
        auto val = this->recv_bytes();
        if( val == "" ) {
            m_eof_hit = true;
            return Token(TOK_EOF);
        }
        auto t = Lex_FindOperator(val);
        ASSERT_BUG(this->m_parent_span, t != TOK_NULL, "Unknown symbol from child process - '" << val << "'");
        return t;
        }
    case TokenClass::Ident: {
        auto val = this->recv_bytes();
        auto t = Lex_FindReservedWord(val, m_edition);
        if( t != TOK_NULL )
            return t;
        if(val[0] == 'r' && val[1] == '#' ) {
            return Token(TOK_IDENT, RcString::new_interned(val.c_str() + 2));
        }
        return Token(TOK_IDENT, RcString::new_interned(val));
        }
    case TokenClass::Lifetime: {
        auto val = this->recv_bytes();
        return Token(TOK_LIFETIME, RcString::new_interned(val));
        }
    case TokenClass::String: {
        auto val = this->recv_bytes();
        return Token(TOK_STRING, mv$(val), this->get_hygiene());
        }
    case TokenClass::ByteString: {
        auto val = this->recv_bytes();
        return Token(TOK_BYTESTRING, mv$(val), this->get_hygiene());
        }
    case TokenClass::CharLit: {
        auto val = this->recv_v128u();
        return Token(U128(val), CORETYPE_CHAR);
        }
    case TokenClass::UnsignedInt: {
        ::eCoreType ty;
        switch(this->recv_u8())
        {
        case   0: ty = CORETYPE_ANY;    break;
        case   1: ty = CORETYPE_UINT;   break;
        case   8: ty = CORETYPE_U8;     break;
        case  16: ty = CORETYPE_U16;    break;
        case  32: ty = CORETYPE_U32;    break;
        case  64: ty = CORETYPE_U64;    break;
        case 128: ty = CORETYPE_U128;   break;
        default:    BUG(this->m_parent_span, "Invalid integer size from child process");
        }
        auto val = this->recv_v128u_u128();
        return Token(val, ty);
        }
    case TokenClass::SignedInt: {
        ::eCoreType ty;
        switch(this->recv_u8())
        {
        case   0: ty = CORETYPE_ANY;    break;
        case   1: ty = CORETYPE_INT;    break;
        case   8: ty = CORETYPE_I8;     break;
        case  16: ty = CORETYPE_I16;    break;
        case  32: ty = CORETYPE_I32;    break;
        case  64: ty = CORETYPE_I64;    break;
        case 128: ty = CORETYPE_I128;   break;
        default:    BUG(this->m_parent_span, "Invalid integer size from child process");
        }
        auto val = this->recv_v128u_u128();
        if(val.truncate_u64() & 1) {
            val = ~(val >> 1) + 1;  // Negative (Is this even possible?)
            TODO(this->m_parent_span, "Negative literal from proc macro, what?");
        }
        else {
            val = (val >> 1);
        }
        return Token(val, ty);
        }
    case TokenClass::Float: {
        ::eCoreType ty;
        switch(this->recv_u8())
        {
        case   0: ty = CORETYPE_ANY;    break;
        case  32: ty = CORETYPE_F32;    break;
        case  64: ty = CORETYPE_F64;    break;
        default:    BUG(this->m_parent_span, "Invalid float size from child process");
        }
        double val;
        this->recv_bytes_raw(&val, sizeof(val));
        return Token::make_float(val, ty);
        }
    //case TokenClass::Fragment:
    //    TODO(this->m_parent_span, "Handle ints/floats/fragments from child process");
    }
    BUG(this->m_parent_span, "Invalid token class from child process - " << int(v));

    throw "";
}
Ident::Hygiene ProcMacroInv::realGetHygiene() const {
    return Ident::Hygiene();
}

