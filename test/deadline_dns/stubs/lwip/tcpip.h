#pragma once
#include "dns.h"
using tcpip_callback_fn = void (*)(void*);
err_t tcpip_try_callback(tcpip_callback_fn, void*);
