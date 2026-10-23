// Les quelques appels au système dont le hub a besoin, isolés ici pour ne pas inclure <windows.h> ailleurs.

#include "Hub.h"

#include <cstdio>
#include <cstdlib>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <sys/mman.h>
 #include <unistd.h>
#endif

namespace osc
{
    void* osAllocate (size_t bytes) noexcept
    {
       #if defined (_WIN32)
        return VirtualAlloc (nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
       #else
        void* p = mmap (nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        return p == MAP_FAILED ? nullptr : p;
       #endif
    }

    unsigned long long currentProcessId() noexcept
    {
       #if defined (_WIN32)
        return (unsigned long long) GetCurrentProcessId();
       #else
        return (unsigned long long) getpid();
       #endif
    }

    bool readProcessVariable (const char* name, char* out, size_t size) noexcept
    {
        if (size == 0)
            return false;
        out[0] = 0;
       #if defined (_WIN32)
        const DWORD got = GetEnvironmentVariableA (name, out, (DWORD) size);
        if (got == 0 || got >= size)
        {
            out[0] = 0;
            return false;
        }
        return true;
       #else
        const char* v = std::getenv (name);
        if (v == nullptr)
            return false;
        std::snprintf (out, size, "%s", v);
        return true;
       #endif
    }

    void writeProcessVariable (const char* name, const char* value) noexcept
    {
       #if defined (_WIN32)
        SetEnvironmentVariableA (name, value);
       #else
        setenv (name, value, 1);
       #endif
    }
}
