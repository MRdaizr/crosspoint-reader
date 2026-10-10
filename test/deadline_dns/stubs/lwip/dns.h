#pragma once
#include "ip_addr.h"
using err_t = int8_t;
inline constexpr err_t ERR_OK = 0;
inline constexpr err_t ERR_MEM = -1;
inline constexpr err_t ERR_ARG = -16;
inline constexpr err_t ERR_INPROGRESS = -5;
using dns_found_callback = void (*)(const char*, const ip_addr_t*, void*);
err_t dns_gethostbyname(const char*, ip_addr_t*, dns_found_callback, void*);
