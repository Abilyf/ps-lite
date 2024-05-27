/**
 *  Copyright (c) 2015 by Contributors
 */
#ifndef PS_INTERNAL_UTILS_H_
#define PS_INTERNAL_UTILS_H_
#include "dmlc/logging.h"
#include "ps/internal/env.h"
namespace ps {

#ifdef _MSC_VER
    typedef signed char      int8_t;
    typedef __int16          int16_t;
    typedef __int32          int32_t;
    typedef __int64          int64_t;
    typedef unsigned char    uint8_t;
    typedef unsigned __int16 uint16_t;
    typedef unsigned __int32 uint32_t;
    typedef unsigned __int64 uint64_t;
#else
#include <inttypes.h>
#endif

#ifdef QUIC_DEBUG
#define QLOG(message) std::cout << message << std::endl
#else
#define QLOG(message)
#endif

    /*!
     * \brief Get environment variable as int with default.
     * \param key the name of environment variable.
     * \param default_val the default value of environment variable.
     * \return The value received
     */
    template<typename V>
    inline V GetEnv(const char* key, V default_val) {
        const char* val = Environment::Get()->find(key);
        if (val == nullptr) {
            return default_val;
        }
        else {
            return V(val);
        }
    }

    inline int GetEnv(const char* key, int default_val) {
        const char* val = Environment::Get()->find(key);
        if (val == nullptr) {
            return default_val;
        }
        else {
            return atoi(val);
        }
    }

#define DBG_PRINTF(fmt, ...)                                                                 \
    debug_printf("%s:%u [%s]: " fmt "\n\n",                                                    \
        __FILE__ + MAX(DBG_PRINTF_FILENAME_MAX, sizeof(__FILE__)) - DBG_PRINTF_FILENAME_MAX, \
        __LINE__, __FUNCTION__, ##__VA_ARGS__)


#ifndef DISALLOW_COPY_AND_ASSIGN
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&);               \
  void operator=(const TypeName&)
#endif

#define LL LOG(ERROR)

}  // namespace ps
#endif  // PS_INTERNAL_UTILS_H_
