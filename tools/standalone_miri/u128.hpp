#pragma once

class U128
{
    uint64_t    lo, hi;
public:
    U128(): lo(0), hi(0) {}
};

class I128
{
    U128    v;
public:
    I128() {}
};
