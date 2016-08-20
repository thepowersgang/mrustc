#ifndef CORETYPES_HPP_INCLUDED
#define CORETYPES_HPP_INCLUDED

enum eCoreType
{
    CORETYPE_INVAL,
    CORETYPE_ANY,
    CORETYPE_BOOL,
    CORETYPE_CHAR, CORETYPE_STR,
    CORETYPE_UINT, CORETYPE_INT,
    CORETYPE_U8,  CORETYPE_I8,
    CORETYPE_U16, CORETYPE_I16,
    CORETYPE_U32, CORETYPE_I32,
    CORETYPE_U64, CORETYPE_I64,
    CORETYPE_F32,
    CORETYPE_F64,
};

extern enum eCoreType coretype_fromstring(const ::std::string& name);
extern const char* coretype_name(const eCoreType ct);

#endif // CORETYPES_HPP_INCLUDED
