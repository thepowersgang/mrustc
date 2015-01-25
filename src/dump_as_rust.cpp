/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 * 
 * dump_as_rust.cpp
 * - Dumps the AST of a crate as rust code (annotated)
 */
#include "ast/expr.hpp"
#include <main_bindings.hpp>

class RustPrinter:
    public AST::NodeVisitor
{
    ::std::ostream& m_os;
    int m_indent_level;
public:
    RustPrinter(::std::ostream& os):
        m_os(os),
        m_indent_level(0)
    {}
    
    void handle_module(const AST::Module& mod);
    void handle_struct(const AST::Struct& s);
    void handle_enum(const AST::Enum& s);
    void handle_trait(const AST::Trait& s);

    void handle_function(const AST::Item<AST::Function>& f);

private:
    void print_params(const AST::TypeParams& params);
    void print_bounds(const AST::TypeParams& params);
    void print_pattern(const AST::Pattern& p);
    
    void inc_indent();
    RepeatLitStr indent();
    void dec_indent();
};

void Dump_Rust(const char *Filename, const AST::Crate& crate)
{
    ::std::ofstream os(Filename);
    RustPrinter printer(os);
    printer.handle_module(crate.root_module());
}

void RustPrinter::handle_module(const AST::Module& mod)
{
    m_os << "\n";
    
    for( const auto& sm : mod.submods() )
    {
        m_os << indent() << (sm.second ? "pub " : "") << "mod " << sm.first.name() << " {\n";
        inc_indent();
        handle_module(sm.first);
        dec_indent();
        m_os << indent() << "}\n";
        m_os << "\n";
    }
    
    
    for( const auto& i : mod.structs() )
    {
        m_os << indent() << (i.is_pub ? "pub " : "") << "struct " << i.name;
        handle_struct(i.data);
    }
    
    for( const auto& i : mod.enums() )
    {
        m_os << indent() << (i.is_pub ? "pub " : "") << "enum " << i.name;
        handle_enum(i.data);
    }
    
    for( const auto& i : mod.traits() )
    {
        m_os << indent() << (i.is_pub ? "pub " : "") << "trait " << i.name;
        handle_trait(i.data);
    }
    
    for( const auto& i : mod.statics() )
    {
        m_os << indent() << (i.is_pub ? "pub " : "");
        switch( i.data.s_class() )
        {
        case AST::Static::CONST:  m_os << "const ";   break;
        case AST::Static::STATIC: m_os << "static ";   break;
        case AST::Static::MUT:    m_os << "static mut ";   break;
        }
        m_os << i.name << ": " << i.data.type() << " = ";
        //handle_expr(i.data.value());
        m_os << ";\n";
    }
    
    for( const auto& i : mod.functions() )
    {
        handle_function(i);
    }
}

void RustPrinter::print_params(const AST::TypeParams& params)
{
    if( params.n_params() > 0 )
    {
        bool is_first = true;
        m_os << "<";
        for( const auto& p : params.params() )
        {
            if( !is_first )
                m_os << ", ";
            m_os << p.name();
            is_first = false;
        }
        m_os << ">";
    }
}

void RustPrinter::print_bounds(const AST::TypeParams& params)
{
    if( params.bounds().size() )
    {
        m_os << indent() << "where\n";
        inc_indent();
        bool is_first = true;
        
        for( const auto& b : params.bounds() )
        {
            if( !is_first )
                m_os << ", ";
            is_first = false;
            
            m_os << indent() << b.name() << ": ";
            m_os << "\n";
        }
    
        dec_indent();
    }
}

void RustPrinter::print_pattern(const AST::Pattern& p)
{
    if( p.binding() != "" && p.type() == AST::Pattern::ANY ) {
        m_os << p.binding();
        return;
    }
    
    if( p.binding() != "" )
        m_os << p.binding() << " @ ";
    switch(p.type())
    {
    case AST::Pattern::ANY:
        m_os << "_";
        break;
    case AST::Pattern::VALUE:
        m_os << p.node();
        break;
    case AST::Pattern::MAYBE_BIND:
        m_os << "/*?*/" << p.path();
        break;
    case AST::Pattern::TUPLE_STRUCT:
        m_os << p.path();
    case AST::Pattern::TUPLE:
        m_os << "(";
        for(const auto& sp : p.sub_patterns())
            print_pattern(sp);
        m_os << ")";
        break;
    }
}

void RustPrinter::handle_struct(const AST::Struct& s)
{
    print_params(s.params());
    
    if( s.fields().size() == 0 )
    {
        m_os << "()\n";
        print_bounds(s.params());
        m_os << indent() << ";\n";
    }
    else if( s.fields().size() == 1 && s.fields()[0].name == "" )
    {
        m_os << "(" << "" <<")\n";
        print_bounds(s.params());
        m_os << indent() << ";\n";
    }
    else
    {
        m_os << "\n";
        print_bounds(s.params());
        
        m_os << indent() << "{\n";
        inc_indent();
        for( const auto& i : s.fields() )
        {
            m_os << indent() << (i.is_pub ? "pub " : "") << i.name << ": " << i.data.print_pretty() << "\n";
        }
        dec_indent();
        m_os << indent() << "}\n";
    }
    m_os << "\n";
}

void RustPrinter::handle_enum(const AST::Enum& s)
{
    print_params(s.params());
    m_os << "\n";
    print_bounds(s.params());

    m_os << indent() << "{\n";
    inc_indent();
    for( const auto& i : s.variants() )
    {
        m_os << indent() << i.name;
        if( i.data.sub_types().size() )
            m_os << i.data.print_pretty();
        m_os << ",\n";
    }
    dec_indent();
    m_os << indent() << "}\n";
    m_os << "\n";
}

void RustPrinter::handle_trait(const AST::Trait& s)
{
    print_params(s.params());
    m_os << "\n";
    print_bounds(s.params());

    m_os << indent() << "{\n";
    inc_indent();
    
    for( const auto& i : s.types() )
    {
        m_os << indent() << "type " << i.name << "\n";
    }
    for( const auto& i : s.functions() )
    {
        handle_function(i);
    }
    
    dec_indent();
    m_os << indent() << "}\n";
    m_os << "\n";
}

void RustPrinter::handle_function(const AST::Item<AST::Function>& f)
{
    m_os << indent() << (f.is_pub ? "pub " : "") << "fn " << f.name;
    print_params(f.data.params());
    m_os << "(";
    bool is_first = true;
    for( const auto& a : f.data.args() )
    {
        if( !is_first )
            m_os << ", ";
        print_pattern( a.first );
        m_os << ": " << a.second.print_pretty();
        is_first = false;
    }
    m_os << ")";
    if( !f.data.rettype().is_unit() )
    {
        m_os << " -> " << f.data.rettype().print_pretty();
    }
    
    if( f.data.code().is_valid() )
    {
        m_os << "\n";
        print_bounds(f.data.params());
        
        m_os << indent() << f.data.code() << "\n";
    }
    else
    {
        print_bounds(f.data.params());
        m_os << ";\n";
    }
}

void RustPrinter::inc_indent()
{
    m_indent_level ++;
}
RepeatLitStr RustPrinter::indent()
{
    return RepeatLitStr { "    ", m_indent_level };
}
void RustPrinter::dec_indent()
{
    m_indent_level --;
}
