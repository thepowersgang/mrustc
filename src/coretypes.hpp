#ifndef CORETYPES_HPP_INCLUDED
#define CORETYPES_HPP_INCLUDED

class Serialiser;
class Deserialiser;

enum eCoreType
{
    CORETYPE_INVAL,
    CORETYPE_ANY,
    CORETYPE_BOOL,
    CORETYPE_CHAR,
    CORETYPE_UINT, CORETYPE_INT,
    CORETYPE_U8,  CORETYPE_I8,
    CORETYPE_U16, CORETYPE_I16,
    CORETYPE_U32, CORETYPE_I32,
    CORETYPE_U64, CORETYPE_I64,
    CORETYPE_F32,
    CORETYPE_F64,
};

extern const char* coretype_name(const eCoreType ct);
extern void operator% (::Serialiser& d, eCoreType ct);
extern void operator% (::Deserialiser& d, eCoreType& ct);

#endif // CORETYPES_HPP_INCLUDED
