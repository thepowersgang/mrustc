/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/proc_macro.cpp
 * - Support for the `#[proc_macro_derive]` attribute
 */
#include <synext.hpp>
#include "../common.hpp"
#include <ast/ast.hpp>
#include <ast/expr.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <hir/hir.hpp>  // ABI_RUST
#include "proc_macro.hpp"
#include <parse/lex.hpp>
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
    void handle(const Span& sp, const AST::Attribute& attr, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        if( i.is_None() )
            return;

        if( !i.is_Function() )
            TODO(sp, "Error for proc_macro_derive on non-Function");
        //auto& fcn = i.as_Function();
        auto trait_name = attr.items().at(0).name();
        ::std::vector<::std::string>    attributes;
        for(size_t i = 1; i < attr.items().size(); i ++)
        {
            if( attr.items()[i].name() == "attributes") {
                for(const auto& si : attr.items()[i].items()) {
                    attributes.push_back( si.name().as_trivial().c_str() );
                }
            }
        }

        // TODO: Store attributes for later use.
        crate.m_proc_macros.push_back(AST::ProcMacroDef { RcString::new_interned(FMT("derive#" << trait_name)), path, mv$(attributes) });
    }
};

STATIC_DECORATOR("proc_macro_derive", Decorator_ProcMacroDerive)



void Expand_ProcMacro(::AST::Crate& crate)
{
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
    auto main_fn = ::AST::Function { Span(), {}, ABI_RUST, false, false, false, TypeRef(TypeRef::TagUnit(), Span()), {} };
    {
        auto call_node = NEWNODE(_CallPath,
                ::AST::Path("proc_macro", { ::AST::PathNode("main") }),
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
        ::AST::ExprNode_StructLiteral::t_values   desc_vals;
        // `name: "foo",`
        desc_vals.push_back({ {}, "name", NEWNODE(_String,  desc.name.c_str()) });
        // `handler`: ::foo
        desc_vals.push_back({ {}, "handler", NEWNODE(_NamedValue, AST::Path(desc.path)) });

        test_nodes.push_back( NEWNODE(_StructLiteral,  ::AST::Path("proc_macro", { ::AST::PathNode("MacroDesc")}), nullptr, mv$(desc_vals) ) );
    }
    auto* tests_array = new ::AST::ExprNode_Array(mv$(test_nodes));

    size_t test_count = tests_array->m_values.size();
    auto tests_list = ::AST::Static { ::AST::Static::Class::STATIC,
        TypeRef(TypeRef::TagSizedArray(), Span(),
                TypeRef(Span(), ::AST::Path("proc_macro", { ::AST::PathNode("MacroDesc") })),
                ::std::shared_ptr<::AST::ExprNode>( new ::AST::ExprNode_Integer(test_count, CORETYPE_UINT) )
               ),
        ::AST::Expr( mv$(tests_array) )
        };

    // ---- module ----
    auto newmod = ::AST::Module { ::AST::Path("", { ::AST::PathNode("proc_macro#") }) };
    // - TODO: These need to be loaded too.
    //  > They don't actually need to exist here, just be loaded (and use absolute paths)
    newmod.add_ext_crate(Span(), false, "proc_macro", "proc_macro", {});

    newmod.add_item(Span(), false, "main", mv$(main_fn), {});
    newmod.add_item(Span(), false, "MACROS", mv$(tests_list), {});

    crate.m_root_module.add_item(Span(), false, "proc_macro#", mv$(newmod), {});
    crate.m_lang_items["mrustc-main"] = ::AST::Path("", { AST::PathNode("proc_macro#"), AST::PathNode("main") });
}

enum class TokenClass
{
    Symbol = 0,
    Ident = 1,
    Lifetime = 2,
    String = 3,
    ByteString = 4, // String
    CharLit = 5,    // v128
    UnsignedInt = 6,
    SignedInt = 7,
    Float = 8,
    Fragment = 9,
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
    const ::HIR::ProcMacro& m_proc_macro_desc;
    ::std::ofstream m_dump_file;

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
    bool    m_eof_hit = false;

public:
    ProcMacroInv(const Span& sp, const char* executable, const ::HIR::ProcMacro& proc_macro_desc);
    ProcMacroInv(const ProcMacroInv&) = delete;
    ProcMacroInv(ProcMacroInv&&);
    ProcMacroInv& operator=(const ProcMacroInv&) = delete;
    ProcMacroInv& operator=(ProcMacroInv&&) = delete;
    virtual ~ProcMacroInv();

    bool check_good();
    void send_done() {
        send_symbol("");
        DEBUG("Input tokens sent");
    }
    void send_symbol(const char* val) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Symbol));
        this->send_bytes(val, ::std::strlen(val));
    }
    void send_ident(const char* val) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Ident));
        this->send_bytes(val, ::std::strlen(val));
    }
    void send_lifetime(const char* val) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Lifetime));
        this->send_bytes(val, ::std::strlen(val));
    }
    void send_string(const ::std::string& s) {
        this->send_u8(static_cast<uint8_t>(TokenClass::String));
        this->send_bytes(s.data(), s.size());
    }
    void send_bytestring(const ::std::string& s);
    void send_char(uint32_t ch);
    void send_int(eCoreType ct, int64_t v);
    void send_float(eCoreType ct, double v);
    //void send_fragment();

    bool attr_is_used(const RcString& n) const {
        return ::std::find(m_proc_macro_desc.attributes.begin(), m_proc_macro_desc.attributes.end(), n) != m_proc_macro_desc.attributes.end();
    }

    virtual Position getPosition() const override;
    virtual Token realGetToken() override;
    virtual Ident::Hygiene realGetHygiene() const override;
private:
    Token realGetToken_();
    void send_u8(uint8_t v);
    void send_bytes(const void* val, size_t size);
    void send_v128u(uint64_t val);

    uint8_t recv_u8();
    ::std::string recv_bytes();
    uint64_t recv_v128u();
};

ProcMacroInv ProcMacro_Invoke_int(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path)
{
    TRACE_FUNCTION_F(mac_path);
    // 1. Locate macro in HIR list
    const auto& crate_name = mac_path.front();
    const auto& ext_crate = crate.m_extern_crates.at(crate_name);
    // TODO: Ensure that this macro is in the listed crate.
    const ::HIR::ProcMacro* pmp = nullptr;
    for(const auto& pm : ext_crate.m_hir->m_proc_macros)
    {
        bool good = true;
        for(size_t i = 0; i < ::std::min( mac_path.size()-1, pm.path.m_components.size() ); i++)
        {
            if( mac_path[1+i] != pm.path.m_components[i] )
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
    return ProcMacroInv(sp, proc_macro_exe_name.c_str(), *pmp);
}


namespace {
    struct Visitor:
        public AST::NodeVisitor
    {
        const Span& sp;
        ProcMacroInv&   m_pmi;
        Visitor(const Span& sp, ProcMacroInv& pmi):
            sp(sp),
            m_pmi(pmi)
        {
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
                TODO(sp, "proc_macro send macro type - " << ty);
                ),
            (Primitive,
                TODO(sp, "proc_macro send primitive - " << ty);
                ),
            (Function,
                TODO(sp, "proc_macro send function - " << ty);
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
                if( te.is_mut )
                    m_pmi.send_ident("mut");
                this->visit_type(*te.inner);
                ),
            (Pointer,
                m_pmi.send_symbol("*");
                if( te.is_mut )
                    m_pmi.send_ident("mut");
                else
                    m_pmi.send_ident("const");
                this->visit_type(*te.inner);
                ),
            (Array,
                m_pmi.send_symbol("[");
                this->visit_type(*te.inner);
                if( te.size )
                {
                    m_pmi.send_symbol(";");
                    this->visit_node(*te.size);
                }
                m_pmi.send_symbol("]");
                ),
            (Generic,
                // TODO: This may already be resolved?... Wait, how?
                m_pmi.send_ident(te.name.c_str());
                ),
            (Path,
                this->visit_path(te.path);
                ),
            (TraitObject,
                m_pmi.send_symbol("(");
                for(const auto& t : te.traits)
                {
                    this->visit_hrbs(t.hrbs);
                    this->visit_path(t.path);
                    m_pmi.send_symbol("+");
                }
                // TODO: Lifetimes
                m_pmi.send_symbol(")");
                ),
            (ErasedType,
                m_pmi.send_ident("impl");
                for(const auto& t : te.traits)
                {
                    this->visit_hrbs(t.hrbs);
                    this->visit_path(t.path);
                    m_pmi.send_symbol("+");
                }
                // TODO: Lifetimes
                )
            )
        }
        void visit_hrbs(const AST::HigherRankedBounds& hrbs)
        {
            if( !hrbs.empty() )
            {
                m_pmi.send_ident("for");
                m_pmi.send_symbol("<");
                for(const auto& v : hrbs.m_lifetimes)
                {
                    m_pmi.send_lifetime(v.name().name.c_str());
                    m_pmi.send_symbol(",");
                }
                m_pmi.send_symbol(">");
            }
        }

        void visit_path(const AST::Path& path, bool is_expr=false)
        {
            const ::std::vector<AST::PathNode>*  nodes = nullptr;
            TU_MATCHA( (path.m_class), (pe),
            (Invalid,
                BUG(sp, "Invalid path");
                ),
            (Local,
                m_pmi.send_ident(pe.name.c_str());
                ),
            (Relative,
                // TODO: Send hygiene information
                nodes = &pe.nodes;
                ),
            (Self,
                m_pmi.send_ident("self");
                m_pmi.send_symbol("::");
                nodes = &pe.nodes;
                ),
            (Super,
                for(unsigned i = 0; i < pe.count; i ++)
                {
                    m_pmi.send_ident("super");
                    m_pmi.send_symbol("::");
                }
                nodes = &pe.nodes;
                ),
            (Absolute,
                m_pmi.send_symbol("::");
                m_pmi.send_string(pe.crate.c_str());
                m_pmi.send_symbol("::");
                nodes = &pe.nodes;
                ),
            (UFCS,
                m_pmi.send_symbol("<");
                this->visit_type(*pe.type);
                if( pe.trait )
                {
                    m_pmi.send_ident("as");
                    this->visit_path(*pe.trait);
                }
                m_pmi.send_symbol(">");
                m_pmi.send_symbol("::");
                nodes = &pe.nodes;
                )
            )
            bool first = true;
            for(const auto& e : *nodes)
            {
                if(!first)
                    m_pmi.send_symbol("::");
                first = false;
                m_pmi.send_ident(e.name().c_str());
                if( ! e.args().is_empty() )
                {
                    if( is_expr )
                        m_pmi.send_symbol("::");
                    m_pmi.send_symbol("<");
                    for(const auto& l : e.args().m_lifetimes)
                    {
                        m_pmi.send_lifetime(l.name().name.c_str());
                        m_pmi.send_symbol(",");
                    }
                    for(const auto& t : e.args().m_types)
                    {
                        this->visit_type(t);
                        m_pmi.send_symbol(",");
                    }
                    for(const auto& a : e.args().m_assoc_equal)
                    {
                        m_pmi.send_ident(a.first.c_str());
                        m_pmi.send_symbol("=");
                        this->visit_type(a.second);
                        m_pmi.send_symbol(",");
                    }
                    for(const auto& a : e.args().m_assoc_bound)
                    {
                        m_pmi.send_ident(a.first.c_str());
                        m_pmi.send_symbol(":");
                        this->visit_path(a.second);
                        m_pmi.send_symbol(",");
                    }
                    m_pmi.send_symbol(">");
                }
            }
        }
        void visit_params(const AST::GenericParams& params)
        {
            if( !params.m_params.empty() )
            {
                bool is_first = true;
                m_pmi.send_symbol("<");
                for( const auto& p : params.m_params )
                {
                    if( !is_first )
                        m_pmi.send_symbol(",");
                    TU_MATCH_HDRA( (p), {)
                    TU_ARMA(None, p) {
                        // Uh... oops?
                        BUG(Span(), "Enountered GenericParam::None");
                        }
                    TU_ARMA(Lifetime, p) {
                        m_pmi.send_lifetime(p.name().name.c_str());
                        }
                    TU_ARMA(Type, p) {
                        this->visit_attrs(p.attrs());
                        m_pmi.send_ident(p.name().c_str());
                        if( !p.get_default().is_wildcard() )
                        {
                            m_pmi.send_symbol("=");
                            this->visit_type(p.get_default());
                        }
                        }
                    TU_ARMA(Value, p) {
                        this->visit_attrs(p.attrs());
                        m_pmi.send_ident("const");
                        m_pmi.send_ident(p.name().name.c_str());
                        m_pmi.send_symbol(":");
                        visit_type(p.type());
                        }
                    }
                    is_first = false;
                }
                m_pmi.send_symbol(">");
            }
        }
        void visit_bounds(const AST::GenericParams& params)
        {
            if( !params.m_bounds.empty() )
            {
                // TODO:
                TODO(Span(), "visit_bounds");
            }
        }
        void visit_node(const ::AST::ExprNode& e)
        {
            const_cast<::AST::ExprNode&>(e).visit(*this);
        }
        void visit_nodes(const ::AST::Expr& e)
        {
            this->visit_node(e.node());
        }

        // === Expressions ====
        void visit(::AST::ExprNode_Block& node) {
            if( node.m_is_unsafe )
                m_pmi.send_ident("unsafe");
            m_pmi.send_symbol("{");
            TODO(sp, "");
            m_pmi.send_symbol("}");
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
        void visit(::AST::ExprNode_Match& node) {
            TODO(sp, "ExprNode_Match");
        }
        void visit(::AST::ExprNode_If& node) {
            TODO(sp, "ExprNode_If");
        }
        void visit(::AST::ExprNode_IfLet& node) {
            TODO(sp, "ExprNode_IfLet");
        }

        void visit(::AST::ExprNode_Integer& node) {
            TODO(sp, "ExprNode_Integer");
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
        void visit(::AST::ExprNode_Closure& node) {
            TODO(sp, "ExprNode_Closure");
        }
        void visit(::AST::ExprNode_StructLiteral& node) {
            TODO(sp, "ExprNode_StructLiteral");
        }
        void visit(::AST::ExprNode_Array& node) {
            TODO(sp, "ExprNode_Array");
        }
        void visit(::AST::ExprNode_Tuple& node) {
            TODO(sp, "ExprNode_Tuple");
        }
        void visit(::AST::ExprNode_NamedValue& node) {
            TODO(sp, "ExprNode_NamedValue");
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
                if( a.name().is_trivial() && m_pmi.attr_is_used(a.name().as_trivial()) )
                {
                    DEBUG("Send " << a);
                    m_pmi.send_symbol("#");
                    m_pmi.send_symbol("[");
                    this->visit_meta_item(a);
                    m_pmi.send_symbol("]");
                }
            }
        }
        void visit_attrs(const ::AST::AttributeList& attrs)
        {
            for(const auto& a : attrs.m_items)
            {
                if( a.name().is_trivial() && m_pmi.attr_is_used(a.name().as_trivial()) )
                {
                    DEBUG("Send " << a);
                    m_pmi.send_symbol("#");
                    m_pmi.send_symbol("[");
                    this->visit_meta_item(a);
                    m_pmi.send_symbol("]");
                }
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

            if( i.has_noarg() ) {
            }
            else if( i.has_string() ) {
                m_pmi.send_symbol("=");
                m_pmi.send_string( i.string().c_str() );
            }
            else {
                assert(i.has_sub_items());
                m_pmi.send_symbol("(");
                bool first = true;
                for(const auto& si : i.items())
                {
                    if(!first)
                        m_pmi.send_symbol(",");
                    this->visit_meta_item(si);
                    first = false;
                }
                m_pmi.send_symbol(")");
            }
        }

        void visit_struct(const ::std::string& name, bool is_pub, const ::AST::Struct& str)
        {
            if( is_pub ) {
                m_pmi.send_ident("pub");
            }

            m_pmi.send_ident("struct");
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
                    if( si.m_is_public )
                        m_pmi.send_ident("pub");
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
                    if( si.m_is_public )
                        m_pmi.send_ident("pub");
                    m_pmi.send_ident(si.m_name.c_str());
                    m_pmi.send_symbol(":");
                    this->visit_type(si.m_type);
                    m_pmi.send_symbol(",");
                }
                m_pmi.send_symbol("}");
                )
            )
        }
        void visit_enum(const ::std::string& name, bool is_pub, const ::AST::Enum& enm)
        {
            if( is_pub ) {
                m_pmi.send_ident("pub");
            }

            m_pmi.send_ident("enum");
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
                    for(const auto& st : e.m_sub_types)
                    {
                        // TODO: Attributes? (None stored in tuple variants)
                        this->visit_type(st);
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
        void visit_union(const ::std::string& name, bool is_pub, const ::AST::Union& unn)
        {
            TODO(sp, "visit_union");
        }
    };
}
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, slice<const AST::Attribute> attrs, const ::std::string& item_name, const ::AST::Struct& i)
{
    // 1. Create ProcMacroInv instance
    auto pmi = ProcMacro_Invoke_int(sp, crate, mac_path);
    if( !pmi.check_good() )
        return ::std::unique_ptr<TokenStream>();
    // 2. Feed item as a token stream.
    // TODO: Get attributes from the caller, filter based on the macro's options then pass to the child.
    Visitor v(sp, pmi);
    v.visit_top_attrs(attrs);
    v.visit_struct(item_name, false, i);
    pmi.send_done();
    // 3. Return boxed invocation instance
    return box$(pmi);
}
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, slice<const AST::Attribute> attrs, const ::std::string& item_name, const ::AST::Enum& i)
{
    // 1. Create ProcMacroInv instance
    auto pmi = ProcMacro_Invoke_int(sp, crate, mac_path);
    if( !pmi.check_good() )
        return ::std::unique_ptr<TokenStream>();
    // 2. Feed item as a token stream.
    Visitor v(sp, pmi);
    v.visit_top_attrs(attrs);
    v.visit_enum(item_name, false, i);
    pmi.send_done();
    // 3. Return boxed invocation instance
    return box$(pmi);
}
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, slice<const AST::Attribute> attrs, const ::std::string& item_name, const ::AST::Union& i)
{
    // 1. Create ProcMacroInv instance
    auto pmi = ProcMacro_Invoke_int(sp, crate, mac_path);
    if( !pmi.check_good() )
        return ::std::unique_ptr<TokenStream>();
    // 2. Feed item as a token stream.
    Visitor v(sp, pmi);
    v.visit_top_attrs(attrs);
    v.visit_union(item_name, false, i);
    pmi.send_done();
    // 3. Return boxed invocation instance
    return box$(pmi);
}

ProcMacroInv::ProcMacroInv(const Span& sp, const char* executable, const ::HIR::ProcMacro& proc_macro_desc):
    TokenStream(ParseState(AST::Edition::Rust2015)), // TODO: Pull edition from the macro
    m_parent_span(sp),
    m_proc_macro_desc(proc_macro_desc)
{
    // TODO: Optionally dump the data sent to the client.
    if( getenv("MRUSTC_DUMP_PROCMACRO") )
    {
        m_dump_file.open( getenv("MRUSTC_DUMP_PROCMACRO"), ::std::ios::out | ::std::ios::binary );
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
    siStartInfo.hStdOutput = stdout_write;
    siStartInfo.hStdInput = stdin_read;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;
    if( !CreateProcessA(executable, const_cast<char*>(commandline.c_str()), NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo) )
    {
        BUG(sp, "Error in CreateProcessW - " << GetLastError() << " - can't start `" << executable << "`");
    }

    this->child_stdin = stdin_write;
    this->child_stdout = stdout_read;
    this->child_handle = piProcInfo.hProcess;

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
    this->child_stdin = stdin_pipes[1]; // Write end
     int    stdout_pipes[2];
    if( pipe(stdout_pipes) != 0)
    {
        BUG(sp, "Unable to create stdout pipe pair for proc macro, " << strerror(errno));
    }
    this->child_stdout = stdout_pipes[0]; // Read end

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
    int rv = posix_spawn(&this->child_pid, executable, &file_actions, nullptr, argv, environ);
    if( rv != 0 )
    {
        BUG(sp, "Error in posix_spawn - " << rv << " - can't start `" << executable << "`");
    }

    posix_spawn_file_actions_destroy(&file_actions);
    // Close the ends we don't care about.
    close(stdin_pipes[0]);
    close(stdout_pipes[1]);

#endif
}
ProcMacroInv::ProcMacroInv(ProcMacroInv&& x):
    TokenStream(x.parse_state()),
    m_parent_span(x.m_parent_span),
    m_proc_macro_desc(x.m_proc_macro_desc),
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
#endif
    DEBUG("");
}
#if 0
ProcMacroInv& ProcMacroInv::operator=(ProcMacroInv&& x)
{
    m_parent_span = x.m_parent_span;
#ifdef _WIN32
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
    if( this->child_handle != INVALID_HANDLE_VALUE )
    {
        DEBUG("Waiting for child to terminate");
        WaitForSingleObject(this->child_handle, INFINITE);
        CloseHandle(this->child_stdout);
        CloseHandle(this->child_stdin);
        CloseHandle(this->child_handle);
    }
#else
    if( this->child_pid != 0 )
    {
        DEBUG("Waiting for child " << this->child_pid << " to terminate");
        int status;
        waitpid(this->child_pid, &status, 0);
        close(this->child_stdout);
        close(this->child_stdin);
    }
#endif
}
bool ProcMacroInv::check_good()
{
    char    v;
#ifdef _WIN32
    DWORD rv = 0;
    if( !ReadFile(this->child_stdout, &v, 1, &rv, nullptr) )
    {
        DEBUG("Error reading from child, " << GetLastError());
        return false;
    }
#else
    int rv = read(this->child_stdout, &v, 1);
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
    if( m_dump_file.is_open() )
        m_dump_file.put(v);
#ifdef _WIN32
    DWORD bytesWritten = 0;
    if( !WriteFile(this->child_stdin, &v, 1, &bytesWritten, nullptr) || bytesWritten != 1 )
        BUG(m_parent_span, "Error writing to child, " << GetLastError());
#else
    if( write(this->child_stdin, &v, 1) != 1 )
        BUG(m_parent_span, "Error writing to child, " << strerror(errno));
#endif
}
void ProcMacroInv::send_bytes(const void* val, size_t size)
{
    this->send_v128u( static_cast<uint64_t>(size) );

    if( m_dump_file.is_open() )
        m_dump_file.write( reinterpret_cast<const char*>(val), size);
#ifdef _WIN32
    DWORD bytesWritten = 0;
    if( !WriteFile(this->child_stdin, val, size, &bytesWritten, nullptr) || bytesWritten != size )
        BUG(m_parent_span, "Error writing to child, " << GetLastError());
#else
    if( write(this->child_stdin, val, size) != static_cast<ssize_t>(size) )
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
uint8_t ProcMacroInv::recv_u8()
{
    uint8_t v;
#ifdef _WIN32
    DWORD n;
    if( !ReadFile(this->child_stdout, &v, 1, &n, nullptr) )
        BUG(this->m_parent_span, "Unexpected EOF while reading from child process");
#else
    if( read(this->child_stdout, &v, 1) != 1 )
        BUG(this->m_parent_span, "Unexpected EOF while reading from child process");
#endif
    return v;
}
::std::string ProcMacroInv::recv_bytes()
{
    auto len = this->recv_v128u();
    ASSERT_BUG(this->m_parent_span, len < SIZE_MAX, "Oversized string from child process");
    ::std::string   val;
    val.resize(len);
    size_t  ofs = 0, rem = len;
    while( rem > 0 )
    {
#ifdef _WIN32
        DWORD n;
        ReadFile(this->child_stdout, &val[ofs], rem, &n, nullptr);
#else
        auto n = read(this->child_stdout, &val[ofs], rem);
#endif
        if( n == 0 ) {
            BUG(this->m_parent_span, "Unexpected EOF while reading from child process");
        }
        if( n < 0 ) {
            BUG(this->m_parent_span, "Error while reading from child process");
        }
        assert(static_cast<size_t>(n) <= rem);
        ofs += n;
        rem -= n;
    }

    return val;
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

Position ProcMacroInv::getPosition() const {
    return Position();
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
    case TokenClass::Symbol: {
        auto val = this->recv_bytes();
        if( val == "" ) {
            m_eof_hit = true;
            return Token(TOK_EOF);
        }
        auto t = Lex_FindOperator(val);
        ASSERT_BUG(this->m_parent_span, t != TOK_NULL, "Unknown symbol from child process - " << val);
        return t;
        }
    case TokenClass::Ident: {
        auto val = this->recv_bytes();
        auto t = Lex_FindReservedWord(val);
        if( t != TOK_NULL )
            return t;
        return Token(TOK_IDENT, RcString::new_interned(val));
        }
    case TokenClass::Lifetime: {
        auto val = this->recv_bytes();
        return Token(TOK_LIFETIME, RcString::new_interned(val));
        }
    case TokenClass::String: {
        auto val = this->recv_bytes();
        return Token(TOK_STRING, mv$(val));
        }
    case TokenClass::ByteString: {
        auto val = this->recv_bytes();
        return Token(TOK_BYTESTRING, mv$(val));
        }
    case TokenClass::CharLit: {
        auto val = this->recv_v128u();
        return Token(static_cast<uint64_t>(val), CORETYPE_CHAR);
        }
    case TokenClass::UnsignedInt: {
        ::eCoreType ty;
        switch(this->recv_u8())
        {
        case   0: ty = CORETYPE_ANY;  break;
        case   1: ty = CORETYPE_UINT; break;
        case   8: ty = CORETYPE_U8;  break;
        case  16: ty = CORETYPE_U16; break;
        case  32: ty = CORETYPE_U32; break;
        case  64: ty = CORETYPE_U64; break;
        case 128: ty = CORETYPE_U128; break;
        default:    BUG(this->m_parent_span, "Invalid integer size from child process");
        }
        auto val = this->recv_v128u();
        return Token(static_cast<uint64_t>(val), ty);
        }
    case TokenClass::SignedInt:
    case TokenClass::Float:
    case TokenClass::Fragment:
        TODO(this->m_parent_span, "Handle ints/floats/fragments from child process");
    }
    BUG(this->m_parent_span, "Invalid token class from child process");

    throw "";
}
Ident::Hygiene ProcMacroInv::realGetHygiene() const {
    return Ident::Hygiene();
}

