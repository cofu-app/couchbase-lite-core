//
// c4Compat.h
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "fleece/Base.h"

#define C4NONNULL NONNULL   // Base.h defines NONNULL

#ifdef _MSC_VER
#  define C4INLINE __forceinline
#elif defined(__GNUC__) && !defined(__clang__)
#  define C4INLINE inline
#else
#  define C4INLINE inline
#endif

// Macros for defining typed enumerations and option flags.
// To define an enumeration whose values won't be combined:
//      typedef C4_ENUM(baseIntType, name) { ... };
// To define an enumeration of option flags that will be ORed together:
//      typedef C4_OPTIONS(baseIntType, name) { ... };
// These aren't just a convenience; they are required for Swift bindings.
#if __APPLE__
    #include <CoreFoundation/CFBase.h>      /* for CF_ENUM and CF_OPTIONS macros */
    #define C4_ENUM CF_ENUM
    #define C4_OPTIONS CF_OPTIONS
#elif DOXYGEN_PARSING
    #define C4_ENUM(_type, _name)     enum _name : _type _name; enum _name : _type
    #define C4_OPTIONS(_type, _name) enum _name : _type _name; enum _name : _type
#else
    #if (__cplusplus && _MSC_VER) || (__cplusplus && __cplusplus >= 201103L && (__has_extension(cxx_strong_enums) || __has_feature(objc_fixed_enum))) || (!__cplusplus && __has_feature(objc_fixed_enum))
        #define C4_ENUM(_type, _name)     enum _name : _type _name; enum _name : _type
        #if (__cplusplus)
            #define C4_OPTIONS(_type, _name) _type _name; enum : _type
        #else
            #define C4_OPTIONS(_type, _name) enum _name : _type _name; enum _name : _type
        #endif
    #else
        #define C4_ENUM(_type, _name) _type _name; enum
        #define C4_OPTIONS(_type, _name) _type _name; enum
    #endif
#endif


// Declaration for API functions; should be just before the ending ";".
#ifdef __cplusplus
    #define C4API noexcept
#else
    #define C4API
#endif

// Deprecating functions & types  (Note: In C++only code, can use standard `[[deprecated]]`)
#ifdef _MSC_VER
#  define C4_DEPRECATED(MSG) __declspec(deprecated(MSG))
#else
#  define C4_DEPRECATED(MSG) __attribute((deprecated(MSG)))
#endif

// Export/import stuff:
#ifdef _MSC_VER
    #ifdef LITECORE_EXPORTS
        #define CBL_CORE_API __declspec(dllexport)
    #else
        #define CBL_CORE_API __declspec(dllimport)
    #endif
#else
    #define CBL_CORE_API
#endif


// Type-checking for printf-style vararg functions:
#ifdef _MSC_VER
    #define __printflike(A, B)
#else
    #ifndef __printflike
        #define __printflike(fmtarg, firstvararg) __attribute__((__format__ (__printf__, fmtarg, firstvararg)))
    #endif
#endif
