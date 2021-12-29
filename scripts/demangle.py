#
# Very basic demangling script
#
from __future__ import print_function

# Exception thrown when a `$` is seen in the input
class Truncated(Exception):
    pass
# Peekable stream
class Peekable(object):
    def __init__(self, inner):
        self.inner = inner
        self.cache = None
    def next(self):
        if self.cache is not None:
            rv = self.cache
            self.cache = None
        else:
            rv = next(self.inner)
        if rv == '$':
            raise Truncated()
        return rv
    def peek(self):
        if self.cache is None:
            self.cache = next(self.inner)
        return self.cache

    def __next__(self):
        return self.next()
    def __iter__(self):
        return self
# Mutable string class
# - Used so the current state is known when `Truncated` is thrown
class Str(object):
    def __init__(self):
        self.inner = ""
    def push(self, s):
        self.inner += s

# Top-level demangling object
def demangle_string(s):
    c_iter = Peekable( iter(s) )
    if next(c_iter) != 'Z':
        return s
    if next(c_iter) != 'R':
        return s
    # Path type
    rv = Str()
    try:
        demangle_int_path(c_iter, rv)
        try:
            c = next(c_iter)
        except StopIteration:
            return rv.inner
    except Truncated:
        rv.push('$')
        for c in c_iter:
            rv.push(c)
        pass
    except Exception:
        tail = (c_iter.cache or '') + ''.join(c_iter.inner)
        print(s)
        print(' '*(len(s) - len(tail)-1), '^', sep='')
        raise
    rv.push("{")
    for c in c_iter:
        rv.push(c)
    rv.push("}")
    return rv.inner

def demangle_int_path(c_iter, rv):
    return {
        'G': demangle_int_pathgeneric,
        'I': demangle_int_pathinherent,
        'Q': demangle_int_pathtrait,
        }[next(c_iter)](c_iter, rv)
def demangle_int_pathsimple(c_iter, rv):
    n = demangle_int_getcount(c_iter)
    assert next(c_iter) == 'c'
    rv.push("::\"")
    rv.push(demangle_int_ident(c_iter))
    rv.push("\"")
    for _ in range(n):
        #print("demangle_int_pathsimple", _, rv)
        rv.push("::")
        rv.push(demangle_int_ident(c_iter))
def demangle_int_pathgeneric(c_iter, rv):
    demangle_int_pathsimple(c_iter, rv)
    demangle_int_params(c_iter, rv)
# UfcsInherent
def demangle_int_pathinherent(c_iter, rv):
    rv.push("<")
    demangle_int_type(c_iter, rv)
    rv.push(">::")
    rv.push(demangle_int_ident(c_iter))
    demangle_int_params(c_iter, rv)
# UfcsKnown
def demangle_int_pathtrait(c_iter, rv):
    rv.push("<")
    demangle_int_type(c_iter, rv)
    rv.push(" as ")
    demangle_int_pathgeneric(c_iter, rv)
    rv.push(">::")
    rv.push(demangle_int_ident(c_iter))
    demangle_int_params(c_iter, rv)

def demangle_int_params(c_iter, rv):
    n = demangle_int_getcount(c_iter)
    if next(c_iter) != 'g':
        raise "error"
    if n == 0:
        return
    rv.push("<")
    for _ in range(n):
        demangle_int_type(c_iter, rv)
        rv.push(",")
    rv.push(">")

def demangle_int_type(c_iter, rv):
    try:
        c = next(c_iter)
        cb = {
            'C': demangle_int_type_primitive,
            'N': demangle_int_path,
            'G': demangle_int_pathgeneric,
            'T': demangle_int_type_tuple,
            'B': demangle_int_type_borrow,
            'P': demangle_int_type_pointer,
            'F': demangle_int_type_function,
            }[c]
    except StopIteration:
        rv.push('?EOS')
        raise
    except KeyError:
        rv.push('?UnkT:'+c)
        raise
    cb(c_iter, rv)
def demangle_int_type_primitive(c_iter, rv):
    try:
        c = next(c_iter)
    except StopIteration:
        rv.push('?EOS')
        return
    try:
        rv.push({
            'a': 'u8',
            'b': 'i8',
            'j': 'i128',
            }[c])
    except IndexError:
        rv.push('?UnkPrim:'+c)
        return
def demangle_int_type_tuple(c_iter, rv):
    n = demangle_int_getcount(c_iter)
    rv.push("(")
    for _ in range(n):
        demangle_int_type(c_iter, rv)
        rv.push(", ")
    rv.push(")")
def demangle_int_type_borrow(c_iter, rv):
    rv.push("&")
    rv.push({ 's': "", 'u': "mut ", 'o': "move "}[next(c_iter)])
    demangle_int_type(c_iter, rv)
def demangle_int_type_pointer(c_iter, rv):
    rv.push("*")
    rv.push({ 's': "const ", 'u': "mut ", 'o': "move "}[next(c_iter)])
    demangle_int_type(c_iter, rv)
def demangle_int_type_function(c_iter, rv):
    if c_iter.peek() == 'u':
        rv.push("unsafe ")
        next(c_iter)
    if c_iter.peek() == 'e':
        next(c_iter)
        abi = demangle_int_ident(c_iter)
        rv.push("extern {:?}".format(abi))
    rv.push("fn(")
    nargs = demangle_int_getcount(c_iter)
    for _ in range(nargs):
        demangle_int_type(c_iter, rv)
        rv.push(", ")
    rv.push(")->")
    demangle_int_type(c_iter, rv)

# ----
# Identifiers: Semi-complex mangling rules (to handle interior `#` and `-`)
# ----
# Top-level identifier demangling
CACHE = []
def demangle_int_ident(c_iter):
    # `'_' <idx>`: back-reference
    if c_iter.peek() == '_':
        c_iter.next()
        idx = demangle_int_getbase26(c_iter)
        return CACHE[idx]
    # `<len:int> <data>` - Non-special string
    if '0' <= c_iter.peek() and c_iter.peek() <= '9':
        rv = demangle_int_suffixed(c_iter)
    else:
        len1 = demangle_int_getbase26(c_iter)
        # `<len:int26> '_' <data>` - Hash prefixed string
        if c_iter.peek() == '_':
            rv = '#' + demangle_int_fixedlen(len1);
            pass
        # `<ofs:int26> <len:idx> <data>` - String with a hash
        else:
            raw = demangle_int_suffixed(c_iter)
            rv = raw[:len1] + "#" + raw[len1:]
    ## `'b' <idx>`: back-reference
    #if c_iter.peek() == 'b':
    #    c_iter.next()
    #    idx = demangle_int_getcount(c_iter)
    #    assert c_iter.next() == '_'
    #    return CACHE[idx]
    #if c_iter.peek() == 'h':
    #    c_iter.next()
    #    rv = demangle_int_suffixed(c_iter)
    #    rv += "#"
    #    rv += demangle_int_suffixed(c_iter)
    #else:
    #    rv = demangle_int_suffixed(c_iter)
    CACHE.append(rv)
    return rv
# Read a base-26 count
def demangle_int_getbase26(c_iter):
    rv = 0
    mul = 1
    while True:
        c = next(c_iter)
        if 'A' <= c and c <= 'Z':
            rv += mul * (ord(c) - ord('A'))
            return rv
        if 'a' <= c and c <= 'z':
            rv += mul * (ord(c) - ord('a'))
            mul *= 26
            continue
        raise Exception("Unexpected character `{}` in base26 value".format(c))
# Read a decimal count
def demangle_int_getcount(c_iter):
    c = next(c_iter)
    if c == '0':
        return 0
    v = str(c)
    while '0' <= c_iter.peek() and c_iter.peek() <= '9':
        v += c_iter.next()
    return int(v)
# Read a length-prefixed string fragment
def demangle_int_suffixed(c_iter):
    l = demangle_int_getcount(c_iter)
    #print("demangle_int_suffixed", l)
    rv = ""
    if l == 80:
        l = 8-1
        rv += '0'
    elif l == 50:
        l = 5-1
        rv += '0'
    return rv + demangle_int_fixedlen(c_iter, l)
def demangle_int_fixedlen(c_iter, l):
    rv = ""
    for _ in range(l):
        rv += next(c_iter)
    return rv

if __name__ == "__main__":
    import sys
    for a in sys.argv[1:]:
        print("{} = {}".format(a, demangle_string(a)))
