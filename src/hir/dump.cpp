/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/dump.cpp
 * - Dump the HIR module tree as pseudo-rust
 */
#include "main_bindings.hpp"
#include "visitor.hpp"
#include "expr.hpp"

#define NODE_IS(valptr, tysuf) ( dynamic_cast<const ::HIR::ExprNode##tysuf*>(&*valptr) != nullptr )

namespace {

    class TreeVisitor:
        public ::HIR::Visitor,
        public ::HIR::ExprVisitor
    {
        ::std::ostream& m_os;
        unsigned int    m_indent_level;

    public:
        TreeVisitor(::std::ostream& os):
            m_os(os),
            m_indent_level(0)
        {
        }

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            if( p.get_name()[0] )
            {
                m_os << indent() << "mod " << p.get_name() << " {\n";
                inc_indent();
            }

            // TODO: Include trait list
            if(true)
            {
                for(const auto& t : mod.m_traits)
                {
                    m_os << indent() << "use " << t << ";\n";
                }
            }
            // TODO: Print publicitiy.
            ::HIR::Visitor::visit_module(p, mod);

            if( p.get_name()[0] )
            {
                dec_indent();
                m_os << indent() << "}\n";
            }
        }

        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            m_os << indent() << "impl" << impl.m_params.fmt_args() << " " << impl.m_type << "\n";
            if( ! impl.m_params.m_bounds.empty() )
            {
                m_os << indent() << " " << impl.m_params.fmt_bounds() << "\n";
            }
            m_os << indent() << "{\n";
            inc_indent();
            ::HIR::Visitor::visit_type_impl(impl);
            dec_indent();
            m_os << indent() << "}\n";
        }
        virtual void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            m_os << indent() << "impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << "\n";
            if( ! impl.m_params.m_bounds.empty() )
            {
                m_os << indent() << " " << impl.m_params.fmt_bounds() << "\n";
            }
            m_os << indent() << "{\n";
            inc_indent();
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            dec_indent();
            m_os << indent() << "}\n";
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            m_os << indent() << "impl" << impl.m_params.fmt_args() << " " << (impl.is_positive ? "" : "!") << trait_path << impl.m_trait_args << " for " << impl.m_type << "\n";
            if( ! impl.m_params.m_bounds.empty() )
            {
                m_os << indent() << " " << impl.m_params.fmt_bounds() << "\n";
            }
            m_os << indent() << "{ }\n";
        }

        // - Type Items
        void visit_type_alias(::HIR::ItemPath p, ::HIR::TypeAlias& item) override
        {
            m_os << indent() << "type " << p.get_name() << item.m_params.fmt_args() << " = " << item.m_type << item.m_params.fmt_bounds() << "\n";
        }
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override
        {
            m_os << indent() << "trait " << p.get_name() << item.m_params.fmt_args() << "\n";
            if( ! item.m_parent_traits.empty() )
            {
                m_os << indent() << "  " << ": ";
                bool is_first = true;
                for(auto& bound : item.m_parent_traits)
                {
                    if( !is_first )
                        m_os << indent() << "  " << "+ ";
                    m_os << bound << "\n";
                    is_first = false;
                }
            }
            if( ! item.m_params.m_bounds.empty() )
            {
                m_os << indent() << " " << item.m_params.fmt_bounds() << "\n";
            }
            m_os << indent() << "{\n";
            inc_indent();

            for(auto& i : item.m_types)
            {
                m_os << indent() << "type " << i.first;
                if( ! i.second.m_trait_bounds.empty() )
                {
                    m_os << ": ";
                    bool is_first = true;
                    for(auto& bound : i.second.m_trait_bounds)
                    {
                        if( !is_first )
                            m_os << " + ";
                        m_os << bound;
                        is_first = false;
                    }
                }
                //this->visit_type(i.second.m_default);
                m_os << ";\n";
            }

            ::HIR::Visitor::visit_trait(p, item);

            dec_indent();
            m_os << indent() << "}\n";
        }
        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override
        {
            m_os << indent() << "struct " << p.get_name() << item.m_params.fmt_args();
            TU_MATCH_HDRA( (item.m_data), {)
            TU_ARMA(Unit, flds) {
                if( item.m_params.m_bounds.empty() )
                {
                    m_os << ";\n";
                }
                else
                {
                    m_os << "\n";
                    m_os << indent() << " " << item.m_params.fmt_bounds() << "\n";
                    m_os << indent() << "    ;\n";
                }
                }
            TU_ARMA(Tuple, flds) {
                m_os << "(";
                for(const auto& fld : flds)
                {
                    m_os << fld.publicity << " " << fld.ent << ", ";
                }
                if( item.m_params.m_bounds.empty() )
                {
                    m_os << ");\n";
                }
                else
                {
                    m_os << ")\n";
                    m_os << indent() << " " << item.m_params.fmt_bounds() << "\n";
                    m_os << indent() << "    ;\n";
                }
                }
            TU_ARMA(Named, flds) {
                m_os << "\n";
                if( ! item.m_params.m_bounds.empty() )
                {
                    m_os << indent() << " " << item.m_params.fmt_bounds() << "\n";
                }
                m_os << indent() << "{\n";
                inc_indent();
                for(const auto& fld : flds)
                {
                    m_os << indent() << fld.second.publicity << " " << fld.first << ": " << fld.second.ent << ",\n";
                }
                dec_indent();
                m_os << indent() << "}\n";
                }
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override
        {
            m_os << indent() << "enum " << p.get_name() << item.m_params.fmt_args() << "\n";
            if( ! item.m_params.m_bounds.empty() )
            {
                m_os << indent() << " " << item.m_params.fmt_bounds() << "\n";
            }
            m_os << indent() << "{\n";
            inc_indent();
            if(const auto* e = item.m_data.opt_Value())
            {
                for(const auto& var : e->variants)
                {
                    m_os << indent() << var.name;
                    m_os << ",\n";
                }
            }
            else
            {
                for(const auto& var : item.m_data.as_Data())
                {
                    m_os << indent() << var.name;
                    if( var.type == ::HIR::TypeRef::new_unit() )
                    {

                    }
                    else
                    {
                        m_os << " " << var.type << (var.is_struct ? "/*struct*/" : "");
                    }
                    m_os << ",\n";
                }
            }
            dec_indent();
            m_os << indent() << "}\n";
        }

        // - Value Items
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override
        {
            m_os << indent();
            if( item.m_const )
                m_os << "const ";
            if( item.m_unsafe )
                m_os << "unsafe ";
            if( item.m_abi != ABI_RUST )
                m_os << "extern \"" << item.m_abi << "\" ";
            m_os << "fn " << p.get_name() << item.m_params.fmt_args() << "(";
            for(const auto& arg : item.m_args)
            {
                m_os << arg.first << ": " << arg.second << ", ";
            }
            m_os << ") -> " << item.m_return << "\n";
            if( ! item.m_params.m_bounds.empty() )
            {
                m_os << indent() << " " << item.m_params.fmt_bounds() << "\n";
            }

            if( item.m_code )
            {
                m_os << indent();
                if( dynamic_cast< ::HIR::ExprNode_Block*>(&*item.m_code) ) {
                    item.m_code->visit( *this );
                }
                else {
                    m_os << "{\n";
                    inc_indent();
                    m_os << indent();

                    item.m_code->visit( *this );

                    m_os << "\n";
                    dec_indent();
                    m_os << indent();
                    m_os << "}";
                }
                m_os << "\n";
            }
            else
            {
                m_os << indent() << "  ;\n";
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            if( item.m_linkage.name != "" )
                m_os << indent() << "#[link_name=\"" << item.m_linkage.name << "\"]\n";
            if( item.m_value )
            {
                m_os << indent() << "static " << p.get_name() << ": " << item.m_type << " = " << item.m_value_res << ";\n";
            }
            else if( item.m_value_generated )
            {
                m_os << indent() << "static " << p.get_name() << ": " << item.m_type << " = /*magic*/ " << item.m_value_res << ";\n";
            }
            else
            {
                m_os << indent() << "extern static " << p.get_name() << ": " << item.m_type << ";\n";
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override
        {
            m_os << indent() << "const " << p.get_name() << ": " << item.m_type << " = " << item.m_value_res;
            if( item.m_value && item.m_value_state != HIR::Constant::ValueState::Known )
            {
                m_os << " /*= ";
                item.m_value->visit(*this);
                m_os << "*/";
            }
            m_os << ";\n";
        }

        // - Misc
        #if 0
        virtual void visit_params(::HIR::GenericParams& params);
        virtual void visit_pattern(::HIR::Pattern& pat);
        virtual void visit_pattern_val(::HIR::Pattern::Value& val);
        virtual void visit_type(::HIR::TypeRef& tr);

        enum class PathContext {
            TYPE,
            TRAIT,

            VALUE,
        };
        virtual void visit_trait_path(::HIR::TraitPath& p);
        virtual void visit_path(::HIR::Path& p, PathContext );
        virtual void visit_path_params(::HIR::PathParams& p);
        virtual void visit_generic_path(::HIR::GenericPath& p, PathContext );

        virtual void visit_expr(::HIR::ExprPtr& exp);
        #endif

        bool node_is_leaf(const ::HIR::ExprNode& node) {
            if( NODE_IS(&node, _PathValue) )
                return true;
            if( NODE_IS(&node, _Variable) )
                return true;
            if( NODE_IS(&node, _Literal) )
                return true;
            if( NODE_IS(&node, _CallPath) )
                return true;
            if( NODE_IS(&node, _Deref) )
                return true;
            return false;
        }

        void visit(::HIR::ExprNode_Block& node) override
        {
            m_os << "{\n";
            inc_indent();
            for(auto& sn : node.m_nodes) {
                m_os << indent();
                this->visit_node_ptr(sn);
                m_os << ";\n";
            }
            if( node.m_value_node )
            {
                m_os << indent();
                this->visit_node_ptr(node.m_value_node);
                m_os << "\n";
            }
            dec_indent();
            m_os << indent() << "}";
        }

        void visit(::HIR::ExprNode_Asm& node) override
        {
            m_os << "llvm_asm!(";
            m_os << ")";
        }
        void visit(::HIR::ExprNode_Asm2& node) override
        {
            m_os << "asm!(";
            m_os << ")";
        }
        void visit(::HIR::ExprNode_Return& node) override
        {
            m_os << "return";
            if( node.m_value ) {
                m_os << " ";
                this->visit_node_ptr(node.m_value);
            }
        }
        void visit(::HIR::ExprNode_Yield& node) override
        {
            m_os << "yield";
            if( node.m_value ) {
                m_os << " ";
                this->visit_node_ptr(node.m_value);
            }
        }
        void visit(::HIR::ExprNode_Let& node) override
        {
            m_os << "let " << node.m_pattern << ": " << node.m_type;
            if( node.m_value ) {
                m_os << " = ";
                this->visit_node_ptr(node.m_value);
            }
            m_os << ";";
        }
        void visit(::HIR::ExprNode_Loop& node) override
        {
            if( node.m_label != "" ) {
                m_os << "'" << node.m_label << ": ";
            }
            m_os << "loop ";
            this->visit_node_ptr(node.m_code);
        }
        void visit(::HIR::ExprNode_LoopControl& node) override
        {
            m_os << (node.m_continue ? "continue" : "break");
            if( node.m_label != "" ) {
                m_os << " '" << node.m_label;
            }
            if( node.m_value ) {
                m_os << " ";
                this->visit_node_ptr(node.m_value);
            }
        }
        void visit(::HIR::ExprNode_Match& node) override
        {
            m_os << "match ";
            this->visit_node_ptr(node.m_value);
            m_os << " {\n";
            for(/*const*/ auto& arm : node.m_arms)
            {
                m_os << indent();
                m_os << arm.m_patterns.front();
                for(unsigned int i = 1; i < arm.m_patterns.size(); i ++ ) {
                    m_os << " | " << arm.m_patterns[i];
                }

                if( arm.m_cond ) {
                    m_os << " if ";
                    this->visit_node_ptr(arm.m_cond);
                }
                m_os << " => ";
                inc_indent();
                this->visit_node_ptr(arm.m_code);
                dec_indent();
                m_os << ",\n";
            }
            m_os << indent() << "}";
        }
        void visit(::HIR::ExprNode_If& node) override
        {
            m_os << "if ";
            this->visit_node_ptr(node.m_cond);
            m_os << " ";
            if( NODE_IS(node.m_true, _Block) ) {
                this->visit_node_ptr(node.m_true);
            }
            else {
                m_os << "{";
                this->visit_node_ptr(node.m_true);
                m_os << "}";
            }
            if( node.m_false )
            {
                m_os << " else ";
                if( NODE_IS(node.m_false, _Block) ) {
                    this->visit_node_ptr(node.m_false);
                }
                else if( NODE_IS(node.m_false, _If) ) {
                    this->visit_node_ptr(node.m_false);
                }
                else {
                    m_os << "{ ";
                    this->visit_node_ptr(node.m_false);
                    m_os << " }";
                }
            }
        }
        void visit(::HIR::ExprNode_Assign& node) override
        {
            this->visit_node_ptr(node.m_slot);
            m_os << " " << ::HIR::ExprNode_Assign::opname(node.m_op) << "= ";
            this->visit_node_ptr(node.m_value);
        }
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            m_os << "(";
            this->visit_node_ptr(node.m_left);
            m_os << ")";
            m_os << " " << ::HIR::ExprNode_BinOp::opname(node.m_op) << " ";
            m_os << "(";
            this->visit_node_ptr(node.m_right);
            m_os << ")";
        }
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Invert: m_os << "!"; break;
            case ::HIR::ExprNode_UniOp::Op::Negate: m_os << "-"; break;
            }
            m_os << "(";
            this->visit_node_ptr(node.m_value);
            m_os << ")";
        }
        void visit(::HIR::ExprNode_Borrow& node) override
        {
            m_os << "&";
            switch(node.m_type)
            {
            case ::HIR::BorrowType::Shared: break;
            case ::HIR::BorrowType::Unique: m_os << "mut "; break;
            case ::HIR::BorrowType::Owned : m_os << "move "; break;
            }

            bool skip_parens = this->node_is_leaf(*node.m_value) || NODE_IS(node.m_value, _Deref);
            if( !skip_parens )  m_os << "(";
            this->visit_node_ptr(node.m_value);
            if( !skip_parens )  m_os << ")";
        }
        void visit(::HIR::ExprNode_RawBorrow& node) override
        {
            m_os << "&raw ";
            switch(node.m_type)
            {
            case ::HIR::BorrowType::Shared: break;
            case ::HIR::BorrowType::Unique: m_os << "mut "; break;
            case ::HIR::BorrowType::Owned : m_os << "move "; break;
            }

            bool skip_parens = this->node_is_leaf(*node.m_value) || NODE_IS(node.m_value, _Deref);
            if( !skip_parens )  m_os << "(";
            this->visit_node_ptr(node.m_value);
            if( !skip_parens )  m_os << ")";
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            this->visit_node_ptr(node.m_value);
            m_os << " as " << node.m_dst_type;
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            this->visit_node_ptr(node.m_value);
            m_os << " : " << node.m_dst_type;
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            // TODO: Avoid parens
            m_os << "(";
            this->visit_node_ptr(node.m_value);
            m_os << ")";
            m_os << "[";
            this->visit_node_ptr(node.m_index);
            m_os << "]";
        }
        void visit(::HIR::ExprNode_Deref& node) override
        {
            m_os << "*";

            bool skip_parens = this->node_is_leaf(*node.m_value);
            if( !skip_parens )  m_os << "(";
            this->visit_node_ptr(node.m_value);
            if( !skip_parens )  m_os << ")";
        }
        void visit(::HIR::ExprNode_Emplace& node) override
        {
            if( node.m_type == ::HIR::ExprNode_Emplace::Type::Noop ) {
                return node.m_value->visit(*this);
            }
            m_os << "(";
            this->visit_node_ptr(node.m_place);
            m_os << " <- ";
            this->visit_node_ptr(node.m_value);
            m_os << ")";
            m_os << "/*" << (node.m_type == ::HIR::ExprNode_Emplace::Type::Boxer ? "box" : "place") << "*/";
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override
        {
            m_os << node.m_path;
            m_os << "(";
            for(/*const*/ auto& arg : node.m_args) {
                this->visit_node_ptr(arg);
                m_os << ", ";
            }
            m_os << ")";
        }
        void visit(::HIR::ExprNode_CallPath& node) override
        {
            m_os << node.m_path;
            m_os << "(";
            for(/*const*/ auto& arg : node.m_args) {
                this->visit_node_ptr(arg);
                m_os << ", ";
            }
            m_os << ")";
            m_os << "/* : " << node.m_res_type << " */";
        }
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            // TODO: Avoid brackets if not needed
            m_os << "(";
            this->visit_node_ptr(node.m_value);
            m_os << ")";
            m_os << "(";
            for(/*const*/ auto& arg : node.m_args) {
                this->visit_node_ptr(arg);
                m_os << ", ";
            }
            m_os << ")";
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            // TODO: Avoid brackets if not needed
            m_os << "(";
            this->visit_node_ptr(node.m_value);
            m_os << ")";
            m_os << "." << node.m_method << node.m_params << "(";
            for(/*const*/ auto& arg : node.m_args) {
                this->visit_node_ptr(arg);
                m_os << ", ";
            }
            m_os << ")";
        }
        void visit(::HIR::ExprNode_Field& node) override
        {
            // TODO: Avoid brackets if not needed
            m_os << "(";
            this->visit_node_ptr(node.m_value);
            m_os << ")";
            m_os << "." << node.m_field;
        }
        void visit(::HIR::ExprNode_Literal& node) override
        {
            TU_MATCHA( (node.m_data), (e),
            (Integer,
                switch(e.m_type)
                {
                case ::HIR::CoreType::U8:   m_os << e.m_value << "_u8" ;    break;
                case ::HIR::CoreType::U16:  m_os << e.m_value << "_u16";    break;
                case ::HIR::CoreType::U32:  m_os << e.m_value << "_u32";    break;
                case ::HIR::CoreType::U64:  m_os << e.m_value << "_u64";    break;
                case ::HIR::CoreType::Usize:m_os << e.m_value << "_usize";  break;
                case ::HIR::CoreType::I8:   m_os << static_cast<int64_t>(e.m_value) << "_i8";   break;
                case ::HIR::CoreType::I16:  m_os << static_cast<int64_t>(e.m_value) << "_i16";  break;
                case ::HIR::CoreType::I32:  m_os << static_cast<int64_t>(e.m_value) << "_i32";  break;
                case ::HIR::CoreType::I64:  m_os << static_cast<int64_t>(e.m_value) << "_i64";  break;
                case ::HIR::CoreType::Isize:m_os << static_cast<int64_t>(e.m_value) << "_isize";break;
                case ::HIR::CoreType::Char:
                    if( e.m_value == '\\' || e.m_value == '\'' )
                        m_os << "'\\" << static_cast<char>(e.m_value) << "'";
                    else if( ' ' <= e.m_value && e.m_value <= 0x7F )
                        m_os << "'" << static_cast<char>(e.m_value) << "'";
                    else
                        m_os << "'\\u{" << ::std::hex << e.m_value << ::std::dec << "}'";
                    break;
                default: m_os << e.m_value << "_unk";    break;
                }
                ),
            (Float,
                switch(e.m_type)
                {
                case ::HIR::CoreType::F32:  m_os << e.m_value << "_f32";    break;
                case ::HIR::CoreType::F64:  m_os << e.m_value << "_f64";    break;
                default: m_os << e.m_value << "_unk";    break;
                }
                ),
            (Boolean,
                m_os << (e ? "true" : "false");
                ),
            (String,
                m_os << "\"" << FmtEscaped(e) << "\"";
                ),
            (ByteString,
                m_os << "b\"";
                for(auto b : e)
                {
                    if( b == '\\' || b == '\"' )
                    {
                        m_os << "\\" << b;
                    }
                    else if( ' ' <= b && b <= 0x7F )
                    {
                        m_os << b;
                    }
                    else
                    {
                        char buf[3];
                        sprintf(buf, "%02x", static_cast<uint8_t>(b));
                        m_os << "\\x" << buf;
                    }
                }
                m_os << "\"";
                )
            )
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override
        {
            m_os << node.m_path;
        }
        void visit(::HIR::ExprNode_PathValue& node) override
        {
            m_os << node.m_path;
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            m_os << node.m_name << "#" << node.m_slot;
        }
        void visit(::HIR::ExprNode_ConstParam& node) override
        {
            m_os << node.m_name << "#" << node.m_binding;
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override
        {
            m_os << node.m_type << " {\n";
            inc_indent();
            for(/*const*/ auto& val : node.m_values) {
                m_os << indent() << val.first << ": ";
                this->visit_node_ptr( val.second );
                m_os << ",\n";
            }
            if( node.m_base_value ) {
                m_os << indent() << ".. ";
                this->visit_node_ptr( node.m_base_value );
                m_os << "\n";
            }
            m_os << indent() << "}";
            dec_indent();
        }
        void visit(::HIR::ExprNode_Tuple& node) override
        {
            m_os << "(";
            for( /*const*/ auto& val : node.m_vals )
            {
                this->visit_node_ptr(val);
                m_os << ", ";
            }
            m_os << ")";
        }
        void visit(::HIR::ExprNode_ArrayList& node) override
        {
            m_os << "[";
            for( /*const*/ auto& val : node.m_vals )
            {
                this->visit_node_ptr(val);
                m_os << ", ";
            }
            m_os << "]";
        }
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            m_os << "[";
            this->visit_node_ptr(node.m_val);
            m_os << "; " << node.m_size;
            m_os << "]";
        }
        void visit(::HIR::ExprNode_Closure& node) override
        {
            if( node.m_code ) {
                if(node.m_is_move)
                    m_os << " move";
                m_os << "|";
                for(const auto& arg : node.m_args)
                    m_os << arg.first << ": " << arg.second << ", ";
                m_os << "| -> " << node.m_return << " ";
                this->visit_node_ptr( node.m_code );
            }
            else {
                m_os << node.m_obj_path << "( ";
                for(/*const*/ auto& cap : node.m_captures) {
                    this->visit_node_ptr(cap);
                    m_os << ", ";
                }
                m_os << ")";
            }
        }
        void visit(::HIR::ExprNode_Generator& node) override
        {
            if( node.m_code ) {
                m_os << "/*gen*/";
                if(node.m_is_pinned)
                    m_os << "static ";
                if(node.m_is_move)
                    m_os << " move";
                m_os << "|";
                //for(const auto& arg : node.m_args)
                //    m_os << arg.first << ": " << arg.second << ", ";
                m_os << "| -> " << node.m_return << " ";
                this->visit_node_ptr( node.m_code );
            }
            else {
                m_os << node.m_obj_path << "( ";
                for(/*const*/ auto& cap : node.m_captures) {
                    this->visit_node_ptr(cap);
                    m_os << ", ";
                }
                m_os << ")";
            }
        }
        void visit(::HIR::ExprNode_GeneratorWrapper& node) override
        {
            m_os << "/*gen body*/";
            m_os << "|";
            //for(const auto& arg : node.m_args)
            //    m_os << arg.first << ": " << arg.second << ", ";
            m_os << "| -> " << node.m_return << " ";
            this->visit_node_ptr( node.m_code );
        }

    private:
        RepeatLitStr indent() const {
            return RepeatLitStr { "    ", static_cast<int>(m_indent_level) };
        }
        void inc_indent() {
            m_indent_level ++;
        }
        void dec_indent() {
            m_indent_level --;
        }
    };
}

void HIR_Dump(::std::ostream& sink, const ::HIR::Crate& crate)
{
    TreeVisitor tv { sink };

    tv.visit_crate( const_cast< ::HIR::Crate&>(crate) );
}
void HIR_DumpExpr(::std::ostream& sink, const ::HIR::ExprPtr& expr)
{
    if(!expr ) {
        sink << "/*NULL*/";
        return;
    }

    TreeVisitor tv { sink };

    const_cast<HIR::ExprPtr&>(expr)->visit(tv);
}

