/*
 * mrustc - Standalone C backend
 */
#include <string>
#include <iostream>
#include <module_tree.hpp>
#include "codegen.hpp"
#include <algorithm>

struct Opts
{
    ::std::string   infile;
    ::std::string   outfile;

    int parse(int argc, const char* argv[]);
    void show_help(const char* prog) const;
};

int main(int argc, const char* argv[])
{
    Opts    opts;
    if( int rv = opts.parse(argc, argv) )
        return rv;

    ModuleTree	tree;

    // 1. Load the tree
    try
    {
        tree.load_file(opts.infile);
        tree.validate();
    }
    catch(const DebugExceptionTodo& /*e*/)
    {
        ::std::cerr << "Loading: TODO Hit" << ::std::endl;
        return 1;
    }
    catch(const DebugExceptionError& /*e*/)
    {
        ::std::cerr << "Loading: Error encountered" << ::std::endl;
        return 1;
    }


    // 2. Recursively enumerate all types (determining the required emit ordering, and fancy types)
    struct TypeEnumeration {
        struct Compare {
            using is_transparent = void;

            static Ordering get_ord(const HIR::TypeRef& t1, const std::pair<const HIR::TypeRef*, unsigned>& t2) {
                Ordering o;
                o = ord(t1.inner_type, t2.first->inner_type); if(o != OrdEqual) return o;
                o = ord( (uintptr_t)t1.ptr.function_type, (uintptr_t)t2.first->ptr.function_type ); if(o != OrdEqual) return o;

                for(size_t i = 0; i < std::min( t1.wrappers.size(), t2.first->wrappers.size() - t2.second); i ++)
                {
                    o = ord(t1.wrappers.at(i), t2.first->wrappers.at(t2.second + i)); if(o != OrdEqual) return o;
                }
                o = ord(t1.wrappers.size(), t2.first->wrappers.size() - t2.second); if(o != OrdEqual) return o;
                return OrdEqual;
            }

            bool operator()(const HIR::TypeRef& t1, const std::pair<const HIR::TypeRef*, unsigned>& t2) const {
                Ordering rv;
                //TRACE_FUNCTION_R(t1 << ", " << *t2.first << "+" << t2.second, rv);
                rv = get_ord(t1, t2);
                return rv == OrdLess;
            }
            bool operator()(const std::pair<const HIR::TypeRef*, unsigned>& t1, const HIR::TypeRef& t2) const {
                Ordering rv;
                //TRACE_FUNCTION_R(*t1.first << "+" <<t1.second << ", " << t2, rv);
                rv = get_ord(t2, t1);
                return rv == OrdGreater;
            }
            bool operator()(const HIR::TypeRef& t1, const HIR::TypeRef& t2) const {
                return t1 < t2;
            }
        };
        std::set<HIR::TypeRef, Compare>   types_visited;
        std::vector<HIR::TypeRef>   types_to_emit;
        std::vector<RcString>   full_datatypes;

        void visit_type(const HIR::TypeRef& t, unsigned depth=0, bool proto_only=false)
        {
            TRACE_FUNCTION_R(t << " depth=" << depth, "");
            auto it = types_visited.find( std::make_pair(&t, depth) );
            if( it != types_visited.end() )
            {
                // Already handled!
                LOG_DEBUG("Already done");
                return ;
            }

            // Add a clone to the list
            HIR::TypeRef    new_ty;
            new_ty.inner_type = t.inner_type;
            new_ty.ptr.function_type = t.ptr.function_type;
            new_ty.wrappers.insert(new_ty.wrappers.begin(), t.wrappers.begin() + depth, t.wrappers.end());

            types_visited.insert(new_ty);

            // If it's a composite, recurse into it
            // Otherwise, shallow recurse?
            if(t.wrappers.size() == depth)
            {
                switch(t.inner_type)
                {
                case RawType::Unit:
                    break;
                case RawType::Unreachable:
                    break;
                case RawType::U8:
                case RawType::I8:
                case RawType::U16:
                case RawType::I16:
                case RawType::U32:
                case RawType::I32:
                case RawType::U64:
                case RawType::I64:
                case RawType::U128:
                case RawType::I128:
                case RawType::F32 :
                case RawType::F64 :
                case RawType::USize:
                case RawType::ISize:
                case RawType::Bool:
                case RawType::Char:
                case RawType::Str :
                    break;

                case RawType::Composite:
                    if(!proto_only)
                    {
                        // Visit all inner types
                        // Add to deep list
                    }
                    break;
                case RawType::TraitObject:
                    if(!proto_only)
                    {
                    }
                    break;
                case RawType::Function: {
                    const auto& fty = *t.ptr.function_type;
                    // Shallow visit all inner types
                    for(const auto& ity : fty.args)
                        visit_type(ity, 0, true);
                    visit_type(fty.ret, 0, true);
                    } break;
                }
            }
            else
            {
                const auto& w = t.wrappers.at(depth);
                switch(w.type)
                {
                case TypeWrapper::Ty::Array:
                    this->visit_type(t, depth+1, proto_only);
                    break;
                case TypeWrapper::Ty::Slice:
                    this->visit_type(t, depth+1, true);
                    break;
                case TypeWrapper::Ty::Pointer:
                case TypeWrapper::Ty::Borrow:
                    this->visit_type(t, depth+1, true);
                    break;
                }
            }

            types_to_emit.push_back(std::move(new_ty));
        }
    } type_enum;

    tree.iterate_statics([&](RcString name, const Static& s) {
        type_enum.visit_type(s.ty);
        });
    tree.iterate_functions([&](RcString name, const Function& f) {
        type_enum.visit_type(f.ret_ty);
        for(auto& t : f.args)
            type_enum.visit_type(t);
        });

    // 3. Emit C code
    Codegen_C codegen(opts.outfile.c_str());
    //  - Emit types
    //  - Emit function/static prototypes
    tree.iterate_statics([&](RcString name, const Static& s) {
        codegen.emit_static_proto(name, s);
        });
    tree.iterate_functions([&](RcString name, const Function& f) {
        codegen.emit_function_proto(name, f);
        });
    //  - Emit statics
    tree.iterate_statics([&](RcString name, const Static& s) {
        codegen.emit_static(name, s);
        });
    //  - Emit functions
    tree.iterate_functions([&](RcString name, const Function& f) {
        codegen.emit_function(name, tree, f);
        });

    return 0;
}

int Opts::parse(int argc, const char* argv[])
{
    bool all_free = false;

    for(int argidx = 1; argidx < argc; argidx ++)
    {
        const char* arg = argv[argidx]; 
        if( arg[0] != '-' || all_free )
        {
            // Free arguments
            // - First is the input file
            if( this->infile == "" )
            {
                this->infile = arg;
            }
            else
            {
                ::std::cerr << "Unexpected option -" << arg << ::std::endl;
                return 1;
            }
        }
        else if( arg[1] != '-' )
        {
            // Short arguments
            if( arg[2] != '\0' ) {
                // Error?
                ::std::cerr << "Unexpected option " << arg << ::std::endl;
                return 1;
            }
            switch(arg[1])
            {
            case 'h':
                this->show_help(argv[0]);
                exit(0);
            default:
                ::std::cerr << "Unexpected option -" << arg[1] << ::std::endl;
                return 1;
            }
        }
        else if( arg[2] != '\0' )
        {
            // Long
            if( ::std::strcmp(arg, "--help") == 0 ) {
                this->show_help(argv[0]);
                exit(0);
            }
            //else if( ::std::strcmp(arg, "--target") == 0 ) {
            //}
            else {
                ::std::cerr << "Unexpected option " << arg << ::std::endl;
                return 1;
            }
        }
        else
        {
            all_free = true;
        }
    }

    if( this->infile == "" )
    {
        this->show_help(argv[0]);
        return 1;
    }

    if( this->outfile == "" )
    {
        this->outfile = "a.c";
    }

    return 0;
}

void Opts::show_help(const char* prog) const
{
    ::std::cout << "USAGE: " << prog << " <infile> <... args>" << ::std::endl;
}
