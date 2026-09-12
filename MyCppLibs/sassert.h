#ifndef SASSERT_H
#define SASSERT_H

#include <stdexcept>

#define CHECK(condition, message) \
    if (!(condition))\
        throw std::runtime_error(message)

#define CHECK_EX(condition, exception_type, message) \
    if (!(condition))\
        throw exception_type(message)

#endif