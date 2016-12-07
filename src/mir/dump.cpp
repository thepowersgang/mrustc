/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/dump.cpp
 * - Dump MIR for functions (semi-flattened)
 */
#include "main_bindings.hpp"
#include <hir/visitor.hpp>
#include "mir.hpp"

namespace {
    
    class TreeVisitor:
        public ::HIR::Visitor
    {
        ::std::ostream& m_os;
        unsigned int    m_indent_level;
        bool m_short_item_name = false;
        
    public:
        TreeVisitor(::std::ostream& os):
            m_os(os),
            m_indent_level(0)
        {
        }

        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            m_short_item_name = true;
            
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
            
            m_short_item_name = false;
        }
        virtual void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            m_short_item_name = true;
            
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
            
            m_short_item_name = false;
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            m_short_item_name = true;
            
            m_os << indent() << "impl" << impl.m_params.fmt_args() << " " << (impl.is_positive ? "" : "!") << trait_path << impl.m_trait_args << " for " << impl.m_type << "\n";
            if( ! impl.m_params.m_bounds.empty() )
            {
                m_os << indent() << " " << impl.m_params.fmt_bounds() << "\n";
            }
            m_os << indent() << "{ }\n";
            
            m_short_item_name = false;
        }
        
        // - Type Items
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override
        {
            m_short_item_name = true;
            
            m_os << indent() << "trait " << p << item.m_params.fmt_args() << "\n";
            if( ! item.m_params.m_bounds.empty() )
            {
                m_os << indent() << " " << item.m_params.fmt_bounds() << "\n";
            }
            m_os << indent() << "{\n";
            inc_indent();
            ::HIR::Visitor::visit_trait(p, item);
            dec_indent();
            m_os << indent() << "}\n";
            
            m_short_item_name = false;
        }

        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override
        {
            m_os << indent();
            if( item.m_const )
                m_os << "const ";
            if( item.m_unsafe )
                m_os << "unsafe ";
            if( item.m_abi != ABI_RUST )
                m_os << "extern \"" << item.m_abi << "\" ";
            m_os << "fn ";
            if( m_short_item_name )
                m_os << p.get_name();
            else
                m_os << p;
            m_os << item.m_params.fmt_args() << "(";
            for(unsigned int i = 0; i < item.m_args.size(); i ++)
            {
                if( i == 0 && item.m_args[i].first.m_binding.m_name == "self" ) {
                    m_os << "self=";
                }
                m_os << "arg$" << i << ": " << item.m_args[i].second << ", ";
            }
            m_os << ") -> " << item.m_return << "\n";
            if( ! item.m_params.m_bounds.empty() )
            {
                m_os << indent() << " " << item.m_params.fmt_bounds() << "\n";
            }
            
            if( item.m_code )
            {
                m_os << indent() << "{\n";
                inc_indent();
                this->dump_mir(*item.m_code.m_mir);
                dec_indent();
                m_os << indent() << "}\n";
            }
            else
            {
                m_os << indent() << "  ;\n";
            }
        }
        
        
        void dump_mir(const ::MIR::Function& fcn)
        {
            for(unsigned int i = 0; i < fcn.named_variables.size(); i ++)
            {
                m_os << indent() << "let _#" << i << ": " << fcn.named_variables[i] << ";\n";
            }
            for(unsigned int i = 0; i < fcn.temporaries.size(); i ++)
            {
                m_os << indent() << "let tmp$" << i << ": " << fcn.temporaries[i] << ";\n";
            }
            
            #define FMT_M(x)   FMT_CB(os, this->fmt_val(os,x);)
            for(unsigned int i = 0; i < fcn.blocks.size(); i ++)
            {
                const auto& block = fcn.blocks[i];
                DEBUG("BB" << i);
                
                m_os << indent() << "bb" << i << ": {\n";
                inc_indent();
                for(const auto& stmt : block.statements)
                {
                    m_os << indent();
                    
                    TU_MATCHA( (stmt), (e),
                    (Assign,
                        DEBUG("- Assign " << e.dst << " = " << e.src);
                        m_os << FMT_M(e.dst) << " = " << FMT_M(e.src) << ";\n";
                        ),
                    (Drop,
                        DEBUG("- DROP " << e.slot);
                        m_os << "drop(" << FMT_M(e.slot) << ");\n";
                        )
                    )
                }
                
                m_os << indent();
                TU_MATCHA( (block.terminator), (e),
                (Incomplete,
                    m_os << "INVALID;\n";
                    ),
                (Return,
                    m_os << "return;\n";
                    ),
                (Diverge,
                    m_os << "diverge;\n";
                    ),
                (Goto,
                    m_os << "goto bb" << e << ";\n";
                    ),
                (Panic,
                    m_os << "panic bb" << e.dst << ";\n";
                    ),
                (If,
                    m_os << "if " << FMT_M(e.cond) << " { goto bb" << e.bb0 << "; } else { goto bb" << e.bb1 << "; }\n";
                    ),
                (Switch,
                    m_os << "switch " << FMT_M(e.val) << " {";
                    for(unsigned int j = 0; j < e.targets.size(); j ++)
                        m_os << j << " => bb" << e.targets[j] << ", ";
                    m_os << "}\n";
                    ),
                (Call,
                    m_os << FMT_M(e.ret_val) << " = ";
                    TU_MATCHA( (e.fcn), (e2),
                    (Value,
                        m_os << "(" << FMT_M(e2) << ")";
                        ),
                    (Path,
                        m_os << e2;
                        ),
                    (Intrinsic,
                        m_os << "\"" << e2 << "\"";
                        )
                    )
                    m_os << "( ";
                    for(const auto& arg : e.args)
                        m_os << FMT_M(arg) << ", ";
                    m_os << ") goto bb" << e.ret_block << " else bb" << e.panic_block << "\n";
                    )
                )
                dec_indent();
                m_os << indent() << "}\n";
                
                m_os.flush();
            }
            #undef FMT
        }
        void fmt_val(::std::ostream& os, const ::MIR::LValue& lval) {
            TU_MATCHA( (lval), (e),
            (Variable,
                os << "_#" << e;
                ),
            (Temporary,
                os << "tmp$" << e.idx;
                ),
            (Argument,
                os << "arg$" << e.idx;
                ),
            (Static,
                os << e;
                ),
            (Return,
                os << "RETURN";
                ),
            (Field,
                os << "(";
                fmt_val(os, *e.val);
                os << ")." << e.field_index;
                ),
            (Deref,
                os << "*";
                fmt_val(os, *e.val);
                ),
            (Index,
                os << "(";
                fmt_val(os, *e.val);
                os << ")[";
                fmt_val(os, *e.idx);
                os << "]";
                ),
            (Downcast,
                fmt_val(os, *e.val);
                os << " as variant" << e.variant_index;
                )
            )
        }
        void fmt_val(::std::ostream& os, const ::MIR::RValue& rval) {
            TU_MATCHA( (rval), (e),
            (Use,
                fmt_val(os, e);
                ),
            (Constant,
                TU_MATCHA( (e), (ce),
                (Int,
                    os << ce;
                    ),
                (Uint,
                    os << "0x" << ::std::hex << ce << ::std::dec;
                    ),
                (Float,
                    os << ce;
                    ),
                (Bool,
                    os << (ce ? "true" : "false");
                    ),
                (Bytes,
                    os << "b\"" << ce << "\"";
                    ),
                (StaticString,
                    os << "\"" << ce << "\"";
                    ),
                (Const,
                    os << ce.p;
                    ),
                (ItemAddr,
                    os << "addr " << ce;
                    )
                )
                ),
            (SizedArray,
                os << "[";
                fmt_val(os, e.val);
                os << ";" << e.count << "]";
                ),
            (Borrow,
                os << "&";
                //os << e.region;
                switch(e.type) {
                case ::HIR::BorrowType::Shared: break;
                case ::HIR::BorrowType::Unique: os << "mut "; break;
                case ::HIR::BorrowType::Owned: os << "move "; break;
                }
                os << "(";
                fmt_val(os, e.val);
                os << ")";
                ),
            (Cast,
                os << "(";
                fmt_val(os, e.val);
                os << ") as " << e.type;
                ),
            (BinOp,
                switch(e.op)
                {
                case ::MIR::eBinOp::ADD: os << "ADD"; break;
                case ::MIR::eBinOp::SUB: os << "SUB"; break;
                case ::MIR::eBinOp::MUL: os << "MUL"; break;
                case ::MIR::eBinOp::DIV: os << "DIV"; break;
                case ::MIR::eBinOp::MOD: os << "MOD"; break;
                case ::MIR::eBinOp::ADD_OV: os << "ADD_OV"; break;
                case ::MIR::eBinOp::SUB_OV: os << "SUB_OV"; break;
                case ::MIR::eBinOp::MUL_OV: os << "MUL_OV"; break;
                case ::MIR::eBinOp::DIV_OV: os << "DIV_OV"; break;
                //case ::MIR::eBinOp::MOD_OV: os << "MOD_OV"; break;
                
                case ::MIR::eBinOp::BIT_OR : os << "BIT_OR"; break;
                case ::MIR::eBinOp::BIT_AND: os << "BIT_AND"; break;
                case ::MIR::eBinOp::BIT_XOR: os << "BIT_XOR"; break;
                
                case ::MIR::eBinOp::BIT_SHR: os << "BIT_SHR"; break;
                case ::MIR::eBinOp::BIT_SHL: os << "BIT_SHL"; break;
                
                case ::MIR::eBinOp::EQ: os << "EQ"; break;
                case ::MIR::eBinOp::NE: os << "NE"; break;
                case ::MIR::eBinOp::GT: os << "GT"; break;
                case ::MIR::eBinOp::GE: os << "GE"; break;
                case ::MIR::eBinOp::LT: os << "LT"; break;
                case ::MIR::eBinOp::LE: os << "LE"; break;
                }
                os << "(";
                fmt_val(os, e.val_l);
                os << ", ";
                fmt_val(os, e.val_r);
                os << ")";
                ),
            (UniOp,
                switch(e.op)
                {
                case ::MIR::eUniOp::INV: os << "INV"; break;
                case ::MIR::eUniOp::NEG: os << "NEG"; break;
                }
                os << "(";
                fmt_val(os, e.val);
                os << ")";
                ),
            (DstMeta,
                os << "META(";
                fmt_val(os, e.val);
                os << ")";
                ),
            (DstPtr,
                os << "PTR(";
                fmt_val(os, e.val);
                os << ")";
                ),
            (MakeDst,
                os << "DST(";
                fmt_val(os, e.ptr_val);
                os << ", ";
                fmt_val(os, e.meta_val);
                os << ")";
                ),
            (Tuple,
                os << "(";
                for(const auto& v : e.vals) {
                    fmt_val(os, v);
                    os << ", ";
                }
                os << ")";
                ),
            (Array,
                os << "[";
                for(const auto& v : e.vals) {
                    fmt_val(os, v);
                    os << ", ";
                }
                os << "]";
                ),
            (Variant,
                os << e.path << " #" << e.index << " (";
                fmt_val(os, e.val);
                os << ")";
                ),
            (Struct,
                os << e.path << " { ";
                for(const auto& v : e.vals) {
                    fmt_val(os, v);
                    os << ", ";
                }
                os << "}";
                )
            )
        }
    private:
        RepeatLitStr indent() const {
            return RepeatLitStr { "   ", static_cast<int>(m_indent_level) };
        }
        void inc_indent() {
            m_indent_level ++;
        }
        void dec_indent() {
            m_indent_level --;
        }
    };
}

void MIR_Dump(::std::ostream& sink, const ::HIR::Crate& crate)
{
    TreeVisitor tv { sink };
    
    tv.visit_crate( const_cast< ::HIR::Crate&>(crate) );
}

