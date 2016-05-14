/*
 */
#include "type.hpp"

namespace HIR {

TypeRef::TypeRef(::HIR::Path path):
    type( TypeRef::Data::make_Path(mv$(path)) )
{
}

}
