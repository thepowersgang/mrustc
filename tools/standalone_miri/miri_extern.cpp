/*
* mrustc Standalone MIRI
* - by John Hodge (Mutabah)
*
* miri_extern.cpp
* - Interpreter core - External function shims
*/
#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include "module_tree.hpp"
#include "value.hpp"
#include "string_view.hpp"
#include <algorithm>
#include <iomanip>
#include "debug.hpp"
#include "miri.hpp"
#include <target_version.hpp>
#include <cctype>
// VVV FFI
#include <cstring>  // memrchr
#include <sys/stat.h>
#include <fcntl.h>
#ifdef __APPLE__
# include <AvailabilityMacros.h>
#endif
#ifdef _WIN32
# define NOMINMAX
# include <Windows.h>
#else
# include <unistd.h>
#endif
#undef DEBUG


#if _WIN32 || __APPLE__
const char* memrchr(const void* p, int c, size_t s) {
    const char* p2 = reinterpret_cast<const char*>(p);
    while( s > 0 )
    {
        s -= 1;
        if( p2[s] == c )
            return &p2[s];
    }
    return nullptr;
}
#else
extern "C" {
    long sysconf(int);
    ssize_t write(int, const void*, size_t);
}
#endif
namespace FfiHelpers {
    static const char* read_cstr(const Value& v, size_t ptr_ofs, size_t* out_strlen=nullptr, size_t max_len=SIZE_MAX)
    {
        bool _is_mut;
        size_t  size;
        // Get the base pointer and allocation size (checking for at least one valid byte to start with)
        const char* ptr = reinterpret_cast<const char*>( v.read_pointer_unsafe(ptr_ofs, 1, /*out->*/ size, _is_mut) );
        size_t len = 0;
        // Seek until either out of space, or a NUL is found
        while(size -- && *ptr && max_len --)
        {
            ptr ++;
            len ++;
        }
        if( out_strlen )
        {
            *out_strlen = len;
        }
        return reinterpret_cast<const char*>(v.read_pointer_const(0, max_len == 0 ? len : len + 1));  // Final read will trigger an error if the NUL isn't there
    }
}

// A very simple implementation of `printf`-style formatting, with internal checks
::std::string format_string(const char* fmt, const ::std::vector<Value>& args, size_t cur_arg) {
    ::std::stringstream output;
    for(const char* s = fmt; *s; s++) {
        if( *s == '%' ) {
            struct H {
                static int64_t read_signed(const Value& v) {
                    switch(v.size())
                    {
                    case 1: return v.read_i8(0);
                    case 2: return v.read_i16(0);
                    case 4: return v.read_i32(0);
                    case 8: return v.read_i64(0);
                    default:
                        LOG_ERROR("Unknown printf arg size - " << v.size());
                    }
                }
                static uint64_t read_unsigned(const Value& v) {
                    switch(v.size())
                    {
                    case 1: return v.read_u8(0);
                    case 2: return v.read_u16(0);
                    case 4: return v.read_u32(0);
                    case 8: return v.read_u64(0);
                    default:
                        LOG_ERROR("Unknown printf arg size - " << v.size());
                    }
                }
            };
            s ++;
            if( *s == '%' ) {
                output << '%';
                continue;
            }
            char pad = ' ';
            if( *s == '0' ) {
                pad = '0';
                s ++;
            }
            size_t width = 0;
            if( *s == '*' ) {
                LOG_ASSERT(cur_arg < args.size(), "*printf: Argument " << cur_arg << " >= " << args.size());
                const auto& arg = args.at(cur_arg);
                width = H::read_unsigned(arg);
                cur_arg ++;
                s ++;
            }
            else {
                while(std::isdigit(*s)) {
                    width *= 10;
                    width += *s - '0';
                    s ++;
                }
            }
            LOG_ASSERT(cur_arg < args.size(), "*printf: Argument " << cur_arg << " >= " << args.size());
            const auto& arg = args.at(cur_arg);
            if( *s == 'z' ) {
                // Indicates a size_t argument, but that doesn't matter here.
                s ++;
            }
            LOG_DEBUG("*printf> pad='" << pad << "', width=" << width << ", arg=" << arg);
            switch(*s)
            {
            case 'i':
            case 'd':
                output << std::setfill(pad) << std::setw(width);
                output << std::dec << H::read_signed(arg);
                break;
            case 'u':
                output << std::setfill(pad) << std::setw(width);
                output << std::dec << H::read_unsigned(arg);
                break;
            case 'x':
                output << std::setfill(pad) << std::setw(width);
                output << std::hex << H::read_unsigned(arg);
                break;
            case 's':
                output << std::setfill(pad) << std::setw(width);
                output << FfiHelpers::read_cstr(arg, 0);
                break;
            case 'p':
                LOG_ASSERT(arg.size() == POINTER_SIZE, "*printf `%p` with wrong size integer - " << arg.size() << " != " << POINTER_SIZE);
                output << std::hex << "0x" << arg.read_usize(0);
                break;
            default:
                LOG_FATAL("Malformed *printf string - unexpected character `" << *s << "`");
            }
            cur_arg += 1;
        }
        else {
            output << *s;
        }
    }
    return output.str();
}

bool InterpreterThread::call_extern(Value& rv, const ::std::string& link_name, const ::std::string& abi, ::std::vector<Value> args)
{
    if( link_name == "__rust_allocate" || link_name == "__rust_alloc" || link_name == "__rust_alloc_zeroed" )
    {
        static unsigned s_alloc_count = 0;

        auto alloc_idx = s_alloc_count ++;
        auto alloc_name = FMT_STRING("__rust_alloc#" << alloc_idx);
        auto size = args.at(0).read_usize(0);
        auto align = args.at(1).read_usize(0);
        LOG_DEBUG(link_name << "(size=" << size << ", align=" << align << "): name=" << alloc_name);
        LOG_ASSERT( (align & (align-1)) == 0, "Allocation alignment isn't a power of two - " << align );
        LOG_ASSERT( (size & (align-1)) == 0, "Allocation size isn't a multiple of alignment - s=" << size << ", a=" << align );

        // TODO: Use the alignment when making an allocation?
        // - Could offset the returned pointer by the alignment (to catch misalign errors?)
        auto alloc = Allocation::new_alloc(size, ::std::move(alloc_name));
        LOG_TRACE("- alloc=" << alloc << " (" << alloc->size() << " bytes)");
        auto rty = ::HIR::TypeRef(RawType::Unit).wrap( TypeWrapper::Ty::Pointer, 0 );

        if( link_name == "__rust_alloc_zeroed" )
        {
            alloc->mark_bytes_valid(0, size);
        }

        rv = Value::new_pointer_ofs(rty, 0, RelocationPtr::new_alloc(::std::move(alloc)));
    }
    else if( link_name == "__rust_reallocate" || link_name == "__rust_realloc" )
    {
        auto oldsize = args.at(1).read_usize(0);
        auto ptr = args.at(0).read_pointer_valref_mut(0, oldsize);

        // NOTE: The ordering here depends on the rust version (1.19 has: old, new, align - 1.29 has: old, align, new)
        auto align = args.at(TARGETVER_LEAST_1_29 ? 2 : 3).read_usize(0);
        auto newsize = args.at(TARGETVER_LEAST_1_29 ? 3 : 2).read_usize(0);
        LOG_DEBUG("__rust_reallocate(ptr=" << ptr.m_alloc << ", oldsize=" << oldsize << ", newsize=" << newsize << ", align=" << align << ")");
        LOG_ASSERT(ptr.m_offset == 0, "__rust_reallocate with offset pointer");

        LOG_ASSERT( (align & (align-1)) == 0, "Allocation alignment isn't a power of two - " << align );
        LOG_ASSERT( (newsize & (align-1)) == 0, "Allocation size isn't a multiple of alignment - s=" << newsize << ", a=" << align );

        LOG_ASSERT(ptr.m_alloc, "__rust_reallocate with no backing allocation attached to pointer");
        LOG_ASSERT(ptr.m_alloc.is_alloc(), "__rust_reallocate with no backing allocation attached to pointer");
        auto& alloc = ptr.m_alloc.alloc();
        // TODO: Check old size and alignment against allocation.
        LOG_ASSERT(oldsize == alloc.size(), "__rust_reallocate with different size");

        // TODO: Should this instead make a new allocation to catch use-after-free?
        alloc.resize(newsize);

        rv = ::std::move(args.at(0));
    }
    else if( link_name == "__rust_deallocate" || link_name == "__rust_dealloc" )
    {
        auto ptr = args.at(0).read_pointer_valref_mut(0, 0);
        LOG_ASSERT(ptr.m_offset == 0, "__rust_deallocate with offset pointer");
        LOG_DEBUG("__rust_deallocate(ptr=" << ptr.m_alloc << ")");

        LOG_ASSERT(ptr.m_alloc, "__rust_deallocate with no backing allocation attached to pointer");
        LOG_ASSERT(ptr.m_alloc.is_alloc(), "__rust_deallocate with no backing allocation attached to pointer");
        auto& alloc = ptr.m_alloc.alloc();
        alloc.mark_as_freed();
        // Just let it drop.
        rv = Value();
    }
    else if( link_name == "__rust_maybe_catch_panic" )
    {
        auto fcn_path = args.at(0).read_pointer_fcn(0);
        auto& arg = args.at(1);
        auto data_ptr = args.at(2).read_pointer_valref_mut(0, POINTER_SIZE);
        auto vtable_ptr = args.at(3).read_pointer_valref_mut(0, POINTER_SIZE);

        ::std::vector<Value>    sub_args;
        sub_args.push_back( ::std::move(arg) );

        this->m_stack.push_back(StackFrame::make_wrapper([=](Value& out_rv, Value /*rv*/)->bool{
            out_rv = Value::new_u32(0);
            return true;
            }));

        // TODO: Catch the panic out of this.
        if( this->call_path(rv, fcn_path, ::std::move(sub_args)) )
        {
            bool v = this->pop_stack(rv);
            assert( v == false );
            return true;
        }
        else
        {
            return false;
        }
    }
    else if( link_name == "panic_impl" )
    {
        LOG_TODO("panic_impl");
    }
    else if( link_name == "__rust_start_panic" )
    {
        LOG_TODO("__rust_start_panic");
    }
    else if( link_name == "rust_begin_unwind" )
    {
        LOG_TODO("rust_begin_unwind");
    }
    // libunwind
    else if( link_name == "_Unwind_RaiseException" )
    {
        LOG_DEBUG("_Unwind_RaiseException(" << args.at(0) << ")");
        // Save the first argument in TLS, then return a status that indicates unwinding should commence.
        m_thread.panic_active = true;
        m_thread.panic_count += 1;
        m_thread.panic_value = ::std::move(args.at(0));
    }
    else if( link_name == "_Unwind_DeleteException" )
    {
        LOG_DEBUG("_Unwind_DeleteException(" << args.at(0) << ")");
    }
#ifdef _WIN32
    // WinAPI functions used by libstd
    else if( link_name == "AddVectoredExceptionHandler" )
    {
        LOG_DEBUG("Call `AddVectoredExceptionHandler` - Ignoring and returning non-null");
        rv = Value::new_usize(1);
    }
    else if( link_name == "GetModuleHandleW" )
    {
        const auto& tgt_alloc = args.at(0).get_relocation(0);
        const void* arg0 = (tgt_alloc ? tgt_alloc.alloc().data_ptr() : nullptr);
        //extern void* GetModuleHandleW(const void* s);
        if(arg0) {
            LOG_DEBUG("FFI GetModuleHandleW(" << tgt_alloc.alloc() << ")");
        }
        else {
            LOG_DEBUG("FFI GetModuleHandleW(NULL)");
        }

        auto ret = GetModuleHandleW(static_cast<LPCWSTR>(arg0));
        if(ret)
        {
            rv = Value::new_ffiptr(FFIPointer::new_void("GetModuleHandleW", ret));
        }
        else
        {
            rv = Value(::HIR::TypeRef(RawType::USize));
            rv.create_allocation("GetModuleHandleW");
            rv.write_usize(0,0);
        }
    }
    else if( link_name == "GetProcAddress" )
    {
        const auto& handle_alloc = args.at(0).get_relocation(0);
        const auto& sym_alloc = args.at(1).get_relocation(0);

        // TODO: Ensure that first arg is a FFI pointer with offset+size of zero
        void* handle = handle_alloc.ffi().ptr_value();
        // TODO: Get either a FFI data pointer, or a inner data pointer
        const void* symname = sym_alloc.alloc().data_ptr();
        // TODO: Sanity check that it's a valid c string within its allocation
        LOG_DEBUG("FFI GetProcAddress(" << handle << ", \"" << static_cast<const char*>(symname) << "\")");

        auto ret = GetProcAddress(static_cast<HMODULE>(handle), static_cast<LPCSTR>(symname));

        if( ret )
        {
            char modulename[1024] = {0};
            GetModuleFileNameA(static_cast<HMODULE>(handle), modulename, sizeof(modulename));
            auto* last_slash = std::strrchr(modulename, '\\');
            std::string name = std::string("#FFI{") + (last_slash+1) + "}" + static_cast<const char*>(symname);
            // TODO: Get the functon name (and source library) and store in the result
            // - Maybe return a FFI function pointer (::"#FFI"::DllName+ProcName)
            rv = Value::new_ffiptr(FFIPointer::new_void("GetProcAddress", ret));
        }
        else
        {
            rv = Value(::HIR::TypeRef(RawType::USize));
            rv.create_allocation("GetProcAddress");
            rv.write_usize(0,0);
        }
    }
    // --- Thread-local storage
    else if( link_name == "TlsAlloc" )
    {
        auto key = ThreadState::s_next_tls_key ++;

        rv = Value::new_u32(key);
    }
    else if( link_name == "TlsGetValue" )
    {
        // LPVOID TlsGetValue( DWORD dwTlsIndex );
        auto key = args.at(0).read_u32(0);

        // Get a pointer-sized value from storage
        if( key < m_thread.tls_values.size() )
        {
            const auto& e = m_thread.tls_values[key];
            rv = Value::new_usize(e.first);
            if( e.second )
            {
                rv.set_reloc(0, POINTER_SIZE, e.second);
            }
        }
        else
        {
            // Return zero until populated
            rv = Value::new_usize(0);
        }
    }
    else if( link_name == "TlsSetValue" )
    {
        // BOOL TlsSetValue( DWORD  dwTlsIndex, LPVOID lpTlsValue );
        auto key = args.at(0).read_u32(0);
        auto v = args.at(1).read_usize(0);
        auto v_reloc = args.at(1).get_relocation(0);

        // Store a pointer-sized value in storage
        if( key >= m_thread.tls_values.size() ) {
            m_thread.tls_values.resize(key+1);
        }
        m_thread.tls_values[key] = ::std::make_pair(v, v_reloc);

        rv = Value::new_i32(1);
    }
    // ---
    else if( link_name == "InitializeCriticalSection" )
    {
        // HACK: Just ignore, no locks
    }
    else if( link_name == "EnterCriticalSection" )
    {
        // HACK: Just ignore, no locks
    }
    else if( link_name == "TryEnterCriticalSection" )
    {
        // HACK: Just ignore, no locks
        rv = Value::new_i32(1);
    }
    else if( link_name == "LeaveCriticalSection" )
    {
        // HACK: Just ignore, no locks
    }
    else if( link_name == "DeleteCriticalSection" )
    {
        // HACK: Just ignore, no locks
    }
    // ---
    else if( link_name == "GetStdHandle" )
    {
        // HANDLE WINAPI GetStdHandle( _In_ DWORD nStdHandle );
        auto val = args.at(0).read_u32(0);
        rv = Value::new_ffiptr(FFIPointer::new_void("HANDLE", GetStdHandle(val)));
    }
    else if( link_name == "GetConsoleMode" )
    {
        // BOOL WINAPI GetConsoleMode( _In_  HANDLE  hConsoleHandle, _Out_ LPDWORD lpMode );
        auto hConsoleHandle = args.at(0).read_pointer_tagged_nonnull(0, "HANDLE");
        auto lpMode_vr = args.at(1).read_pointer_valref_mut(0, sizeof(DWORD)).to_write();
        LOG_DEBUG("GetConsoleMode(" << hConsoleHandle << ", " << lpMode_vr);
        auto lpMode = reinterpret_cast<LPDWORD>(lpMode_vr.data_ptr_mut(sizeof(DWORD)));
        auto rv_bool = GetConsoleMode(hConsoleHandle, lpMode);
        if( rv_bool )
        {
            LOG_DEBUG("= TRUE (" << *lpMode << ")");
            lpMode_vr.mark_bytes_valid(0, sizeof(DWORD));
        }
        else
        {
            LOG_DEBUG("= FALSE");
        }
        rv = Value::new_i32(rv_bool ? 1 : 0);
    }
    else if( link_name == "WriteConsoleW" )
    {
        //BOOL WINAPI WriteConsole( _In_ HANDLE  hConsoleOutput, _In_ const VOID    *lpBuffer, _In_ DWORD   nNumberOfCharsToWrite,  _Out_ LPDWORD lpNumberOfCharsWritten, _Reserved_ LPVOID  lpReserved );
        auto hConsoleOutput = args.at(0).read_pointer_tagged_nonnull(0, "HANDLE");
        auto nNumberOfCharsToWrite = args.at(2).read_u32(0);
        auto lpBuffer = args.at(1).read_pointer_const(0, nNumberOfCharsToWrite * 2);
        auto lpNumberOfCharsWritten_vr = args.at(3).read_pointer_valref_mut(0, sizeof(DWORD)).to_write();
        auto lpReserved = args.at(4).read_usize(0);
        LOG_DEBUG("WriteConsoleW(" << hConsoleOutput << ", " << lpBuffer << ", " << nNumberOfCharsToWrite << ", " << lpNumberOfCharsWritten_vr << ")");

        auto lpNumberOfCharsWritten = reinterpret_cast<LPDWORD>(lpNumberOfCharsWritten_vr.data_ptr_mut(sizeof(DWORD)));

        LOG_ASSERT(lpReserved == 0, "");
        auto rv_bool = WriteConsoleW(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, nullptr);
        if( rv_bool )
        {
            LOG_DEBUG("= TRUE (" << *lpNumberOfCharsWritten << ")");
        }
        else
        {
            LOG_DEBUG("= FALSE");
        }
        rv = Value::new_i32(rv_bool ? 1 : 0);
    }
#else
    // POSIX
    else if( link_name == "write" )
    {
        auto fd = args.at(0).read_i32(0);
        auto count = args.at(2).read_isize(0);
        const auto* buf = args.at(1).read_pointer_const(0, count);

        ssize_t val = write(fd, buf, count);

        rv = Value::new_isize(val);
    }
    else if( link_name == "read" )
    {
        auto fd = args.at(0).read_i32(0);
        auto count = args.at(2).read_isize(0);
        auto buf_vr = args.at(1).read_pointer_valref_mut(0, count).to_write();

        LOG_DEBUG("read(" << fd << ", " << buf_vr.data_ptr_mut(count) << ", " << count << ")");
        ssize_t val = read(fd, buf_vr.data_ptr_mut(count), count);
        LOG_DEBUG("= " << val);

        if( val > 0 )
        {
            buf_vr.mark_bytes_valid(0, val);
        }

        rv = Value::new_isize(val);
    }
    else if( link_name == "open" )
    {
        auto path = FfiHelpers::read_cstr(args.at(0), 0);
        auto flags = args.at(1).read_i32(0);
        // TODO: Emulate for windows?
#if 0
        LOG_TODO("open(\"" << path << "\", 0x" << std::hex << flags << ")");
#else
        if( strcmp(path, "/dev/null") == 0 ) {
        }
        else {
            LOG_TODO("open(\"" << path << "\", 0x" << std::hex << flags << ")");
        }
        int fd = open(path, flags);
        if(fd >= 0)
        {
            // TODO: Register FD to avoid double-close
        }
        else
        {
            // TODO: Save errno?
        }
        rv = Value::new_i32(fd);
#endif
    }
    else if( link_name == "close" )
    {
        auto fd = args.at(0).read_i32(0);
        LOG_DEBUG("close(" << fd << ")");
        // TODO: Ensure that this FD is from the set known by the FFI layer
        close(fd);
    }
    else if( link_name == "isatty" )
    {
        auto fd = args.at(0).read_i32(0);
        LOG_DEBUG("isatty(" << fd << ")");
        int rv_i = isatty(fd);
        LOG_DEBUG("= " << rv_i);
        rv = Value::new_i32(rv_i);
    }
    else if( link_name == "fcntl" )
    {
        // `fcntl` has custom handling for the third argument, as some are pointers
        int fd = args.at(0).read_i32(0);
        int command = args.at(1).read_i32(0);

        auto fcntl_noarg = [&](const char* name)->int {
            LOG_DEBUG("fnctl(" << fd << ", " << name << "{" << command << "})");
            return fcntl(fd, command);
            };
        auto fcntl_int = [&](const char* name)->int {
            int arg = args.at(2).read_i32(0);
            LOG_DEBUG("fnctl(" << fd << ", " << name << "{" << command << "}, " << arg << ")");
            return fcntl(fd, command, arg);
            };
        int rv_i;
        switch(command)
        {
        // - No argument
        case F_GETFD: rv_i = fcntl_noarg("F_GETFD");    break;
        // - Integer arguments
        case F_DUPFD        : rv_i = fcntl_int("F_DUPFD"        );  break;
#if !defined(__APPLE__) || (MAC_OS_X_VERSION_MIN_REQUIRED >= 1070 && __DARWIN_C_LEVEL >= 200809L)
        case F_DUPFD_CLOEXEC: rv_i = fcntl_int("F_DUPFD_CLOEXEC");  break;
#endif
        case F_SETFD        : rv_i = fcntl_int("F_SETFD"        ); break;
        default:
            if( args.size() > 2 )
            {
                LOG_TODO("fnctl(..., " << command << ", " << args[2] << ")");
            }
            else
            {
                LOG_TODO("fnctl(..., " << command << ")");
            }
        }

        LOG_DEBUG("= " << rv_i);
        rv = Value(::HIR::TypeRef(RawType::I32));
        rv.write_i32(0, rv_i);
    }
    else if( link_name == "prctl" )
    {
        auto option = args.at(0).read_i32(0);
        int rv_i;
        switch(option)
        {
        case 15: {   // PR_SET_NAME - set thread name
            auto name = FfiHelpers::read_cstr(args.at(1), 0);
            LOG_DEBUG("prctl(PR_SET_NAME, \"" << name << "\"");
            rv_i = 0;
        } break;
        default:
            LOG_TODO("prctl(" << option << ", ...");
        }
        rv = Value::new_i32(rv_i);
    }
    else if( link_name == "sysconf" )
    {
        auto name = args.at(0).read_i32(0);
        LOG_DEBUG("FFI sysconf(" << name << ")");

        long val = sysconf(name);

        rv = Value::new_usize(val);
    }
    else if( link_name == "mmap" )
    {
        // void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
        auto& addr = args.at(0);
        auto length = args.at(1).read_usize(0);
        auto prot = args.at(2).read_i32(0);
        auto flags = args.at(3).read_i32(0);
        auto fd = args.at(4).read_i32(0);
        auto offset = args.at(5).read_usize(0);
        LOG_TODO("TODO: mmap(" << addr << ",0x" << std::hex << length
            << ", prot=0x" << prot
            << ", flags=0x" << flags
            << ", fd=" << std::dec << fd
            << ", offset=0x"<<std::hex<<offset
            );
        rv = std::move(addr);
    }
    else if( link_name == "pipe" )
    {
#if 1   // TODO: `write_i32` doesn't directly work, need to grab allocation and handle
        auto dst = args.at(0).read_pointer_valref_mut(0, 2*4).to_write();
        int pipes[2];
        if( pipe(pipes) != 0 ) {
            // TODO: Save errno
            rv = Value::new_i32(-1);
        }
        else {
            // TODO: Save handles in some state?
            dst.write_i32(0, pipes[0]);
            dst.write_i32(4, pipes[1]);
            rv = Value::new_i32(0);
        }
#else
        LOG_TODO("pipe");
#endif
    }
    // >>> pthread
    else if( link_name == "pthread_self" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_mutex_init"
        || link_name == "pthread_mutex_lock"
        || link_name == "pthread_mutex_trylock"
        || link_name == "pthread_mutex_unlock"
        || link_name == "pthread_mutex_destroy"
        )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_rwlock_rdlock" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_rwlock_unlock" )
    {
        // TODO: Check that this thread holds the lock?
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_mutexattr_init" || link_name == "pthread_mutexattr_settype" || link_name == "pthread_mutexattr_destroy" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_condattr_init" || link_name == "pthread_condattr_destroy" || link_name == "pthread_condattr_setclock" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_attr_init" || link_name == "pthread_attr_destroy" || link_name == "pthread_getattr_np" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_attr_setstacksize" )
    {
        // Lie and return succeess
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_attr_getguardsize" )
    {
        const auto attr_p = args.at(0).read_pointer_const(0, 1);
        auto out_size = args.at(1).deref(0, HIR::TypeRef(RawType::USize));

        (void)attr_p;
        out_size.m_alloc.alloc().write_usize(out_size.m_offset, 0x1000);

        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_attr_getstack" )
    {
        const auto attr_p = args.at(0).read_pointer_const(0, 1);
        auto out_ptr = args.at(2).deref(0, HIR::TypeRef(RawType::USize));
        auto out_size = args.at(2).deref(0, HIR::TypeRef(RawType::USize));

        (void)attr_p;
        out_size.m_alloc.alloc().write_usize(out_size.m_offset, 0x4000);

        rv = Value::new_i32(0);
    }
    //else if( link_name == "pthread_get_stackaddr_np" ) {
    //    rv = Value::new_ffiptr(FFIPointer::new_const_bytes("pthread_get_stackaddr_np", "", 0));
    //}
    //else if( link_name == "pthread_get_stacksize_np" ) {
    //    //rv = Value::new_usize(0x4000);
    //    rv = Value::new_usize(0);
    //}
    else if( link_name == "pthread_create" )
    {
        auto thread_handle_out = args.at(0).read_pointer_valref_mut(0, sizeof(pthread_t));
        auto attrs = args.at(1).read_pointer_const(0, sizeof(pthread_attr_t));
        auto fcn_path = args.at(2).read_pointer_fcn(0);
        auto& arg = args.at(3);
        LOG_NOTICE("TODO: pthread_create(" << thread_handle_out << ", " << attrs << ", " << fcn_path << ", " << arg << ")");
        // TODO: Create a new interpreter context with this thread, use co-operative scheduling
        // HACK: Just run inline
        if( true )
        {
            auto tls = ::std::move(m_thread.tls_values);
            this->m_stack.push_back(StackFrame::make_wrapper([=](Value& out_rv, Value /*rv*/)mutable ->bool {
                out_rv = Value::new_i32(0);
                m_thread.tls_values = ::std::move(tls);
                return true;
                }));

            // TODO: Catch the panic out of this.
            ::std::vector<Value>    args;
            args.push_back(std::move(arg));
            if( this->call_path(rv, fcn_path, std::move(args)) )
            {
                bool v = this->pop_stack(rv);
                assert( v == false );
                return true;
            }
            else
            {
                return false;
            }
        }
        else {
            //this->m_parent.create_thread(fcn_path, arg);
            rv = Value::new_i32(EPERM);
        }
    }
    else if( link_name == "pthread_detach" )
    {
        // "detach" - Prevent the need to explitly join a thread
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_cond_init" || link_name == "pthread_cond_destroy" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_key_create" )
    {
        auto key_ref = args.at(0).read_pointer_valref_mut(0, 4);

        auto key = ThreadState::s_next_tls_key ++;
        key_ref.m_alloc.alloc().write_u32( key_ref.m_offset, key );

        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_getspecific" )
    {
        auto key = args.at(0).read_u32(0);

        // Get a pointer-sized value from storage
        if( key < m_thread.tls_values.size() )
        {
            const auto& e = m_thread.tls_values[key];
            rv = Value::new_usize(e.first);
            if( e.second )
            {
                rv.set_reloc(0, POINTER_SIZE, e.second);
            }
        }
        else
        {
            // Return zero until populated
            rv = Value::new_usize(0);
        }
    }
    else if( link_name == "pthread_setspecific" )
    {
        auto key = args.at(0).read_u32(0);
        auto v = args.at(1).read_u64(0);
        auto v_reloc = args.at(1).get_relocation(0);

        // Store a pointer-sized value in storage
        if( key >= m_thread.tls_values.size() ) {
            m_thread.tls_values.resize(key+1);
        }
        m_thread.tls_values[key] = ::std::make_pair(v, v_reloc);

        rv = Value::new_i32(0);
    }
    else if( link_name == "pthread_key_delete" )
    {
        rv = Value::new_i32(0);
    }
    // - Time
    else if( link_name == "clock_gettime" )
    {
        // int clock_gettime(clockid_t clk_id, struct timespec *tp);
        auto clk_id = (clockid_t) args.at(0).read_u32(0);
        auto tp_vr = args.at(1).read_pointer_valref_mut(0, sizeof(struct timespec)).to_write();

        LOG_DEBUG("clock_gettime(" << clk_id << ", " << tp_vr);
        int rv_i = clock_gettime(clk_id, reinterpret_cast<struct timespec*>(tp_vr.data_ptr_mut(sizeof(struct timespec))));
        if(rv_i == 0)
            tp_vr.mark_bytes_valid(0, sizeof(struct timespec));
        LOG_DEBUG("= " << rv_i << " (" << tp_vr << ")");
        rv = Value::new_i32(rv_i);
    }
    // - Linux extensions
    else if( link_name == "open64" )
    {
        const auto* path = FfiHelpers::read_cstr(args.at(0), 0);
        auto flags = args.at(1).read_i32(0);
        auto mode = (args.size() > 2 ? args.at(2).read_i32(0) : 0);

        LOG_DEBUG("open64(\"" << path << "\", " << flags << ")");
        int rv_i = open(path, flags, mode);
        LOG_DEBUG("= " << rv_i);

        rv = Value(::HIR::TypeRef(RawType::I32));
        rv.write_i32(0, rv_i);
    }
    else if( link_name == "stat64" )
    {
        const auto* path = FfiHelpers::read_cstr(args.at(0), 0);
        auto outbuf_vr = args.at(1).read_pointer_valref_mut(0, sizeof(struct stat)).to_write();

        LOG_DEBUG("stat64(\"" << path << "\", " << outbuf_vr << ")");
        int rv_i = stat(path, reinterpret_cast<struct stat*>(outbuf_vr.data_ptr_mut(sizeof(struct stat))));
        LOG_DEBUG("= " << rv_i);

        if( rv_i == 0 )
        {
            // TODO: Mark the buffer as valid?
            outbuf_vr.mark_bytes_valid(0, sizeof(struct stat));
        }

        rv = Value(::HIR::TypeRef(RawType::I32));
        rv.write_i32(0, rv_i);
    }
    else if( link_name == "__errno_location" || /*OSX*/ link_name == "__error" )
    {
        rv = Value::new_ffiptr(FFIPointer::new_const_bytes("errno", &errno, sizeof(errno)));
    }
    else if( link_name == "syscall" )
    {
        auto num = args.at(0).read_u32(0);

        LOG_DEBUG("syscall(" << num << ", ...) - hack return ENOSYS");
        errno = ENOSYS;
        rv = Value::new_i64(-1);
    }
    else if( link_name == "dlsym" )
    {
        auto handle = args.at(0).read_usize(0);
        const char* name = FfiHelpers::read_cstr(args.at(1), 0);

        LOG_DEBUG("dlsym(0x" << ::std::hex << handle << ", '" << name << "')");
        LOG_NOTICE("dlsym stubbed to zero");
        rv = Value::new_usize(0);
    }
#endif
    // ----
    // C Standard Library
    // ----
    // 
    // <signal.h>
    else if( link_name == "signal" )
    {
        LOG_DEBUG("Call `signal` - Ignoring and returning SIG_IGN");
        rv = Value(::HIR::TypeRef(RawType::USize));
        rv.write_usize(0, 1);
    }
    else if( link_name == "sigaction" )
    {
        rv = Value::new_i32(-1);
    }
    else if( link_name == "sigaltstack" )   // POSIX: Set alternate signal stack
    {
        rv = Value::new_i32(-1);
    }
    //
    // <stdlib.h>
    //
    else if( link_name == "atoi" )
    {
        // extern int atoi(const char *nptr);
        size_t len = 0;
        const char* nptr = FfiHelpers::read_cstr(args.at(0), 0, &len);
        rv = Value::new_i32( atoi(nptr) );
    }
    else if( link_name == "strtoll" )
    {
        // long long strtoll(const char *nptr, char **endptr, int base);
        size_t len = 0;
        const char* nptr = FfiHelpers::read_cstr(args.at(0), 0, &len);
        auto endptr_req = args.at(1).read_usize(0) != 0;
        auto base = args.at(2).read_i32(0);
        char* endptr_real;
        auto retval = strtoll(nptr, endptr_req ? &endptr_real : nullptr, base);
        if(endptr_req) {
            auto ofs = endptr_real - nptr;
            args.at(1).read_pointer_valref_mut(0, ::HIR::TypeRef(RawType::USize).get_size())
                .to_write()
                .write_ptr(0, args.at(0).read_usize(0) + ofs, args.at(0).get_relocation(0));
        }
        rv = Value::new_i64(retval);
    }
    else if( link_name == "strtol" )
    {
        // long long strtoll(const char *nptr, char **endptr, int base);
        size_t len = 0;
        const char* nptr = FfiHelpers::read_cstr(args.at(0), 0, &len);
        auto endptr_req = args.at(1).read_usize(0) != 0;
        auto base = args.at(2).read_i32(0);
        char* endptr_real;
        auto retval = strtol(nptr, endptr_req ? &endptr_real : nullptr, base);
        if(endptr_req) {
            auto ofs = endptr_real - nptr;
            args.at(1).read_pointer_valref_mut(0, ::HIR::TypeRef(RawType::USize).get_size())
                .to_write()
                .write_ptr(0, args.at(0).read_usize(0) + ofs, args.at(0).get_relocation(0));
        }
        rv = Value::new_i64(retval);
    }
    else if( link_name == "malloc" )
    {
        auto size = args.at(0).read_usize(0);

        auto alloc = Allocation::new_alloc(size, "malloc");
        auto rty = ::HIR::TypeRef(RawType::Unit).wrap( TypeWrapper::Ty::Pointer, 0 );

        rv = Value::new_pointer_ofs(rty, 0, RelocationPtr::new_alloc(::std::move(alloc)));
    }
    else if( link_name == "calloc" )
    {
        auto nmemb = args.at(0).read_usize(0);
        auto size = args.at(1).read_usize(0);

        auto alloc = Allocation::new_alloc(size * nmemb, "calloc");
        auto rty = ::HIR::TypeRef(RawType::Unit).wrap( TypeWrapper::Ty::Pointer, 0 );

        alloc->mark_bytes_valid(0, size * nmemb);

        rv = Value::new_pointer_ofs(rty, 0, RelocationPtr::new_alloc(::std::move(alloc)));
    }
    else if( link_name == "realloc" )
    {
        auto size = args.at(1).read_usize(0);
        auto rty = ::HIR::TypeRef(RawType::Unit).wrap( TypeWrapper::Ty::Pointer, 0 );
        auto alloc = Allocation::new_alloc(size, "realloc");
        if( args.at(0).read_usize(0) == 0 ) {
        }
        else {
            auto ptr = args.at(0).read_pointer_valref_mut(0, 0);
            LOG_ASSERT(ptr.m_offset == 0, "`realloc` with pointer not to beginning of block");

            LOG_ASSERT(ptr.m_alloc, "`realloc` with no backing allocation attached to pointer");
            LOG_ASSERT(ptr.m_alloc.is_alloc(), "`realloc` with no backing allocation attached to pointer");
            auto& old_alloc = ptr.m_alloc.alloc();

            auto s = ::std::min(static_cast<size_t>(size), old_alloc.size());
            auto ptr2 = args.at(0).read_pointer_valref_mut(0, s);
            alloc->write_value(0, ptr2.read_value(0, s));
            old_alloc.mark_as_freed();
        }
        rv = Value::new_pointer_ofs(rty, 0, RelocationPtr::new_alloc(::std::move(alloc)));
    }
    else if( link_name == "free" )
    {
        // If `ptr` is NULL, no operation is performed
        if( args.at(0).read_usize(0) != 0 )
        {
            auto ptr = args.at(0).read_pointer_valref_mut(0, 0);
            LOG_ASSERT(ptr.m_offset == 0, "`free` with pointer not to beginning of block");
            LOG_DEBUG("free(ptr=" << ptr.m_alloc << ")");

            LOG_ASSERT(ptr.m_alloc, "`free` with no backing allocation attached to pointer");
            LOG_ASSERT(ptr.m_alloc.is_alloc(), "`free` with no backing allocation attached to pointer");
            auto& alloc = ptr.m_alloc.alloc();
            alloc.mark_as_freed();
        }

        rv = Value();
    }
    //
    // <string.h>
    //
    else if( link_name == "memcmp" )
    {
        auto n = args.at(2).read_usize(0);
        int rv_i;
        if( n > 0 )
        {
            const void* ptr_b = args.at(1).read_pointer_const(0, n);
            const void* ptr_a = args.at(0).read_pointer_const(0, n);

            rv_i = memcmp(ptr_a, ptr_b, n);
        }
        else
        {
            rv_i = 0;
        }
        rv = Value::new_i32(rv_i);
    }
    else if( link_name == "memset" )
    {
        auto b = args.at(1).read_u8(0);
        auto n = args.at(2).read_usize(0);
        if( n > 0 )
        {
            auto vr = args.at(0).read_pointer_valref_mut(0, n).to_write();
            memset(vr.data_ptr_mut(n), b, n);
            vr.mark_bytes_valid(0, n);
        }
        rv = std::move(args.at(0));
    }
    else if( link_name == "memcpy" )
    {
        auto n = args.at(2).read_usize(0);
        if( n > 0 )
        {
            auto vr_dst = args.at(0).read_pointer_valref_mut(0, n).to_write();
            // NOTE: the `mut` part doesn't actually get checked until a write is attempted
            auto vr_src = args.at(1).read_pointer_valref_mut(0, n);
            vr_dst.write_value(0, vr_src.read_value(0, n));
        }
        rv = std::move(args.at(0));
    }
    // - `void *memchr(const void *s, int c, size_t n);`
    else if( link_name == "memchr" )
    {
        auto ptr_alloc = args.at(0).get_relocation(0);
        auto c = args.at(1).read_i32(0);
        auto n = args.at(2).read_usize(0);
        const void* ptr = args.at(0).read_pointer_const(0, n);

        const void* ret = memchr(ptr, c, n);

        rv = Value(::HIR::TypeRef(RawType::USize));
        if( ret )
        {
            auto rv_ofs = args.at(0).read_usize(0) + ( static_cast<const uint8_t*>(ret) - static_cast<const uint8_t*>(ptr) );
            rv.write_ptr(0, rv_ofs, ptr_alloc);
        }
        else
        {
            rv.write_usize(0, 0);
        }
    }
    else if( link_name == "memrchr" )
    {
        auto ptr_alloc = args.at(0).get_relocation(0);
        auto c = args.at(1).read_i32(0);
        auto n = args.at(2).read_usize(0);
        const void* ptr = args.at(0).read_pointer_const(0, n);

        const void* ret = memrchr(ptr, c, n);

        rv = Value(::HIR::TypeRef(RawType::USize));
        if( ret )
        {
            auto rv_ofs = args.at(0).read_usize(0) + ( static_cast<const uint8_t*>(ret) - static_cast<const uint8_t*>(ptr) );
            rv.write_ptr(0, rv_ofs, ptr_alloc);
        }
        else
        {
            rv.write_usize(0, 0);
        }
    }
    else if( link_name == "strcpy" ) {
        // strlen - custom implementation to ensure validity
        size_t len = 0;
        auto src = FfiHelpers::read_cstr(args.at(1), 0, &len);

        auto vr = args.at(0).read_pointer_valref_mut(0, len+1).to_write();
        memcpy(vr.data_ptr_mut(len+1), src, len+1);
        vr.mark_bytes_valid(0, len+1);
        rv = std::move(args.at(0));
    }
    else if( link_name == "strlen" )
    {
        // strlen - custom implementation to ensure validity
        size_t len = 0;
        FfiHelpers::read_cstr(args.at(0), 0, &len);

        //rv = Value::new_usize(len);
        rv = Value(::HIR::TypeRef(RawType::USize));
        rv.write_usize(0, len);
    }
    else if( link_name == "strcmp" )
    {
        size_t len;
        const char* a = FfiHelpers::read_cstr(args.at(0), 0, &len);
        const char* b = FfiHelpers::read_cstr(args.at(1), 0, &len);
        LOG_DEBUG("strcmp(\"" << a <<"\", \"" << b << "\")");

        int rv_i = strcmp(a, b);
        rv = Value::new_i32(rv_i);
    }
    else if( link_name == "strncmp" )
    {
        size_t len;
        const char* a = FfiHelpers::read_cstr(args.at(0), 0, &len);
        const char* b = FfiHelpers::read_cstr(args.at(1), 0, &len);
        size_t max = args.at(2).read_usize(0);
        LOG_DEBUG("strncmp(\"" << a <<"\", \"" << b << "\", " << max <<")");

        int rv_i = strncmp(a, b, max);
        rv = Value::new_i32(rv_i);
    }
    else if( link_name == "strdup" )
    {
        size_t len;
        const char* a = FfiHelpers::read_cstr(args.at(0), 0, &len);

        auto alloc = Allocation::new_alloc(len+1, "strdup");
        auto rty = ::HIR::TypeRef(RawType::Unit).wrap( TypeWrapper::Ty::Pointer, 0 );

        rv = Value::new_pointer_ofs(rty, 0, RelocationPtr::new_alloc(::std::move(alloc)));
        {
            auto vr = rv.read_pointer_valref_mut(0, len+1).to_write();
            memcpy(vr.data_ptr_mut(len+1), a, len+1);
            vr.mark_bytes_valid(0, len+1);
        }
    }
    else if( link_name == "strndup" )
    {
        size_t max = args.at(1).read_usize(0);
        size_t len;
        const char* a = FfiHelpers::read_cstr(args.at(0), 0, &len, max);

        auto alloc = Allocation::new_alloc(len+1, "strndup");
        auto rty = ::HIR::TypeRef(RawType::Unit).wrap( TypeWrapper::Ty::Pointer, 0 );

        rv = Value::new_pointer_ofs(rty, 0, RelocationPtr::new_alloc(::std::move(alloc)));
        {
            auto vr = rv.read_pointer_valref_mut(0, len+1).to_write();
            auto p = vr.data_ptr_mut(len+1);
            memcpy(p, a, len);
            p[len] = 0;
            vr.mark_bytes_valid(0, len+1);
        }
    }
    // --- ?
    else if( link_name == "getenv" )
    {
        const auto* name = FfiHelpers::read_cstr(args.at(0), 0);
        LOG_DEBUG("getenv(\"" << name << "\")");
        const auto* ret_ptr = getenv(name);
        if( ret_ptr )
        {
            LOG_DEBUG("= \"" << ret_ptr << "\"");
            rv = Value::new_ffiptr(FFIPointer::new_const_bytes("getenv", ret_ptr, strlen(ret_ptr)+1));
        }
        else
        {
            LOG_DEBUG("= NULL");
            rv = Value(::HIR::TypeRef(RawType::USize));
            //rv.create_allocation("getenv");
            rv.write_usize(0,0);
        }
    }
    else if( link_name == "setenv" )
    {
        LOG_TODO("Allow `setenv` without incurring thread unsafety");
    }
    else if( link_name == "strerror" )
    {
        auto errnum = args.at(0).read_i32(0);
        auto s = strerror(errnum);
        rv = Value::new_ffiptr(FFIPointer::new_const_bytes("strerror", s, strlen(s)+1));
    }
    else if( link_name == "strerror_r" )
    {
        auto errnum = args.at(0).read_i32(0);
        auto len = args.at(2).read_usize(0);
        auto buf = args.at(1).read_pointer_valref_mut(0, len).to_write();

        auto s = strerror(errnum);
        auto slen = strlen(s);
        if(len > 0)
        {
            auto max_write = (slen < len-1 ? slen : len-1);
            auto* dst = buf.data_ptr_mut(max_write+1);
            memcpy(dst, s, max_write);
            dst[max_write] = 0; // Always include terminating NUL byte, even if truncated
            buf.mark_bytes_valid(0, max_write+1);
        }
        if(true) {
            rv = Value::new_i32(0);
        }
        else {
            // GNU targets only
            rv = std::move(args.at(1));
        }
    }
    else if( link_name == "printf" )
    {
        const auto* fmt = FfiHelpers::read_cstr(args.at(0), 0);
        auto out = format_string(fmt, args, 1);
        ::std::cout << out;
        rv = Value::new_i32(static_cast<int32_t>(out.size()));
    }
    else if( link_name == "snprintf" )
    {
        const auto* fmt = FfiHelpers::read_cstr(args.at(2), 0);
        auto out = format_string(fmt, args, 3);
        LOG_DEBUG("out = " << out);
        size_t len = args.at(1).read_usize(0);
        if( len > 0 )
        {
            auto buf = args.at(0).read_pointer_valref_mut(0, len).to_write();
            buf.write_bytes(0, out.data(), std::min(len-1, out.size()));
            buf.write_u8( ::std::min(len-1, out.size()), 0 );
        }
        rv = Value::new_i32(static_cast<int32_t>(out.size()));
    }
    else if( link_name == "vsnprintf" )
    {
        const auto* fmt = FfiHelpers::read_cstr(args.at(2), 0);
        const auto& va_args = VaArgsState::get_inner( args.at(3) );
        auto out = format_string(fmt, va_args.args, 0);
        LOG_DEBUG("out = " << out);
        size_t len = args.at(1).read_usize(0);
        if( len > 0 )
        {
            auto buf = args.at(0).read_pointer_valref_mut(0, len).to_write();
            buf.write_bytes(0, out.data(), std::min(len-1, out.size()));
            buf.write_u8( ::std::min(len-1, out.size()), 0 );
        }
        rv = Value::new_i32(static_cast<int32_t>(out.size()));
    }
    //
    // <stdio.h>
    //
    else if( link_name == "fopen" )
    {
        const auto* path = FfiHelpers::read_cstr(args.at(0), 0);
        const auto* mode = FfiHelpers::read_cstr(args.at(1), 0);
        LOG_DEBUG("fopen(\"" << path << "\", \"" << mode << "\")");
        FILE* fp = fopen(path, mode);
        if(fp) {
            rv = Value::new_ffiptr(FFIPointer::new_void("FILE", fp));
        }
        else {
            rv = Value::new_usize(0);
        }
    }
    else if( link_name == "fclose" )
    {
        FILE* fp = static_cast<FILE*>(args.at(0).read_pointer_tagged_nonnull(0, "FILE"));
        int retval = fclose(fp);
        args.at(0).get_relocation(0).ffi().release();
        rv = Value::new_i32(retval);
    }
    else if( link_name == "fseek" )
    {
        // int fseek(FILE *stream, long offset, int whence);
        FILE* fp = static_cast<FILE*>(args.at(0).read_pointer_tagged_nonnull(0, "FILE"));
        auto offset = args.at(1).read_i64(0);
        int whence_v = args.at(2).read_i32(0);
        int whence;
        switch(whence_v)
        {
        case -1: whence = SEEK_END;  break;
        case 0: whence = SEEK_CUR;  break;
        case 1: whence = SEEK_SET;  break;
        default:
            rv = Value::new_i32(-1);
            return true;
        }

        rv = Value::new_i32( fseek(fp, static_cast<long>(offset), whence) );
    }
    else if( link_name == "ftell" )
    {
        // long ftell(FILE *stream);
        FILE* fp = static_cast<FILE*>(args.at(0).read_pointer_tagged_nonnull(0, "FILE"));
        rv = Value::new_i64( ftell(fp) );
    }
    else if( link_name == "fread")
    {
        // size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
        FILE* fp = static_cast<FILE*>(args.at(3).read_pointer_tagged_nonnull(0, "FILE"));
        auto nmemb = args.at(2).read_usize(0);
        auto size = args.at(1).read_usize(0);
        auto ptr = args.at(0).read_pointer_valref_mut(0, size*nmemb).to_write();

        size_t retval = fread(ptr.data_ptr_mut(size*nmemb), static_cast<size_t>(size), static_cast<size_t>(nmemb), fp);
        if(retval > 0)
        {
            ptr.mark_bytes_valid(0, retval * size);
        }
        rv = Value::new_i64(retval);
    }
    // --- setjmp.h
    else if( link_name == "setjmp" )
    {
        rv = Value::new_i32(0);
    }
    else if( link_name == "longjmp" )
    {
        LOG_TODO("Call `longjmp`");
    }
    // --- ctype.h
    else if( link_name == "isspace" ) {
        rv = Value::new_i32( isspace(args.at(0).read_i32(0)) );
    }
    else if( link_name == "isalpha" ) {
        rv = Value::new_i32( isalpha(args.at(0).read_i32(0)) );
    }
    else if( link_name == "isalnum" ) {
        rv = Value::new_i32( isalnum(args.at(0).read_i32(0)) );
    }
    else
    {
        LOG_TODO("Call external function " << link_name);
    }
    return true;
}
