#pragma once
#include "network/NativeUdpTransport.h"
#include "v2/Runner.h"
#ifdef SIM_WITH_ASIO
#include "network/AsioUdpTransport.h"
#endif

namespace rt::v2 {
inline Session::TransportFactory selectTransport(const std::string &name) {
    if (name == "native" || name.empty())
        return [] {
            return std::make_unique<NativeUdpTransport>();
        };
#ifdef SIM_WITH_ASIO
    if (name == "asio-sync" || name == "asio-async")
        return [async = name == "asio-async"] {
            return makeAsioUdpTransport(async);
        };
#else
    if (name == "asio-sync" || name == "asio-async")
        throw std::invalid_argument("Asio not enabled: run scripts/setup-asio-deps.ps1 and "
                                    "configure with -DSIM_WITH_ASIO=ON");
#endif
    throw std::invalid_argument("Unknown V2 transport: " + name);
}
} // namespace rt::v2
