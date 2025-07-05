#ifndef NETENDIANS_H
#define NETENDIANS_H

#include <stdint.h> // For uint64_t, uint32_t etc.

// On Windows (x86/x64), the system is little-endian.
// Network byte order is big-endian.
// Therefore, we always need to byte-swap for conversions.

#if defined(_MSC_VER)
    // Use Microsoft Visual C++ intrinsics for efficient byte swapping
    #include <intrin.h> // Provides _byteswap_uint64
    #define LOCAL_BSWAP64(x) _byteswap_uint64(x)
#else
    // Fallback for other C compilers on Windows if intrinsics are not available
    // (less likely, as MSVC is dominant on Windows)
    // This is a manual bit manipulation fallback, less optimized than intrinsics.
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

/**
 * @brief Converts a 64-bit unsigned integer from host byte order to network byte order (big-endian).
 * @param val The 64-bit value in host byte order.
 * @return The 64-bit value in network byte order.
 */
static inline uint64_t _htonll(uint64_t val) {
    // On Windows (little-endian), we always need to swap to big-endian (network order).
    return LOCAL_BSWAP64(val);
}

/**
 * @brief Converts a 64-bit unsigned integer from network byte order (big-endian) to host byte order.
 * @param val The 64-bit value in network byte order.
 * @return The 64-bit value in host byte order.
 */
static inline uint64_t _ntohll(uint64_t val) {
    // On Windows (little-endian), we always need to swap from big-endian (network order).
    return LOCAL_BSWAP64(val);
}

#endif // NETENDIANS_H