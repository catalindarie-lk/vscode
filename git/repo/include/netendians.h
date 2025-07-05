#ifndef NETENDIANS_H
#define NETENDIANS_H

#include <stdint.h> // Guaranteed to be available and provides fixed-width integers

// --- 1. Define standard byte order constants if not already defined ---
// These are common values used by GCC/Clang but are not standard C.
#ifndef __ORDER_LITTLE_ENDIAN__
#define __ORDER_LITTLE_ENDIAN__ 1234
#endif
#ifndef __ORDER_BIG_ENDIAN__
#define __ORDER_BIG_ENDIAN__ 4321
#endif
// No PDP_ENDIAN relevant for modern architectures

// --- 2. Determine host byte order at compile time ---
// We try to rely on compiler-specific predefined macros for robustness.
#ifndef __BYTE_ORDER__
    #if defined(_WIN32)
        // Windows (MSVC on x86/x64/ARM) is universally little-endian.
        #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
    #elif defined(__linux__) || defined(__CYGWIN__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
        // For Unix-like systems, GCC/Clang often define __BYTE_ORDER__ directly.
        // If not, we try to deduce from other common macros.
        #if defined(__LITTLE_ENDIAN__) && !defined(__BIG_ENDIAN__)
            #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
        #elif defined(__BIG_ENDIAN__) && !defined(__LITTLE_ENDIAN__)
            #define __BYTE_ORDER__ __ORDER_BIG_ENDIAN__
        #elif defined(__BYTE_ORDER) && (__BYTE_ORDER == __LITTLE_ENDIAN) // Common for older glibc systems
            #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
        #elif defined(__BYTE_ORDER) && (__BYTE_ORDER == __BIG_ENDIAN)
            #define __BYTE_ORDER__ __ORDER_BIG_ENDIAN__
        #else
            // Fallback for systems where endianness macros are not cleanly defined.
            // Most modern CPUs (x86/x64/ARM) are little-endian.
            #warning "Could not precisely determine byte order for this platform. Assuming little-endian. Please verify for your target."
            #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
        #endif
    #else
        // Generic fallback for any other unknown platform.
        #warning "Could not determine byte order for this platform. Assuming little-endian for _htonll/_ntohll implementation."
        #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
    #endif
#endif

// --- 3. Provide highly optimized 64-bit byte-swapping function (compiler intrinsics) ---
// These functions map directly to efficient CPU instructions (e.g., 'bswap').

#if defined(_MSC_VER)
    // For MSVC, declare the intrinsic. It's normally in <intrin.h>, but we declare it here
    // to avoid including any non-stdint.h headers. The compiler knows its signature.
    extern uint64_t _byteswap_uint64(uint64_t _Val);
    #define LOCAL_BSWAP64(x) _byteswap_uint64(x)
#elif defined(__GNUC__) || defined(__clang__)
    // For GCC/Clang, use the __builtin_bswap64 intrinsic. No extra header needed.
    #define LOCAL_BSWAP64(x) __builtin_bswap64(x)
#else
    // Generic fallback for other C compilers that don't provide intrinsics.
    // This is less optimized but highly portable.
    static inline uint64_t LOCAL_BSWAP64(uint64_t x) {
        return ((x >> 56) & 0x00000000000000FFUL) |
               ((x >> 40) & 0x000000000000FF00UL) |
               ((x >> 24) & 0x0000000000FF0000UL) |
               ((x >>  8) & 0x00000000FF000000UL) |
               ((x <<  8) & 0x000000FF00000000UL) |
               ((x << 24) & 0x0000FF0000000000UL) |
               ((x << 40) & 0x00FF000000000000UL) |
               ((x << 56) & 0xFF00000000000000UL);
    }
#endif

// --- 4. Host-to-network (big-endian) and network-to-host 64-bit conversions ---

/**
 * @brief Converts a 64-bit unsigned integer from host byte order to network byte order (big-endian).
 * @param val The 64-bit value in host byte order.
 * @return The 64-bit value in network byte order.
 */
static inline uint64_t _htonll(uint64_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // On a little-endian host, we need to swap to big-endian (network order).
    return LOCAL_BSWAP64(val);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // On a big-endian host, no swap is needed as host order is already network order.
    return val;
#else
    // This branch should ideally not be reached if __BYTE_ORDER__ is properly defined above.
    // As a fallback, assume little-endian (most common) and perform swap.
    #warning "Undefined __BYTE_ORDER__ during _htonll. Assuming little-endian host."
    return LOCAL_BSWAP64(val);
#endif
}

/**
 * @brief Converts a 64-bit unsigned integer from network byte order (big-endian) to host byte order.
 * @param val The 64-bit value in network byte order.
 * @return The 64-bit value in host byte order.
 */
static inline uint64_t _ntohll(uint64_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // On a little-endian host, we need to swap from big-endian (network order).
    return LOCAL_BSWAP64(val);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // On a big-endian host, no swap is needed as network order is already host order.
    return val;
#else
    // This branch should ideally not be reached.
    #warning "Undefined __BYTE_ORDER__ during _ntohll. Assuming little-endian host."
    return LOCAL_BSWAP64(val);
#endif
}

#endif // NETENDIANS_H