#pragma once
#include "network/NativeUdpTransport.h"
#include "v2/Runner.h"
#ifdef SIM_WITH_ASIO
#include "network/AsioUdpTransport.h"
#endif

namespace rt::v2 {
// Supplying the Qt factory is optional: the native test/probe must not link Qt.
inline Session::TransportFactory selectTransport(const std::string& name, Session::TransportFactory qt={}) {
    if(name=="qt" || (name.empty() && qt)) {
        if(!qt) throw std::invalid_argument("The Qt backend is available in SimTransferCli, not SimRtProbe");
        return qt;
    }
    if(name=="native" || name.empty()) return [] {return std::make_unique<NativeUdpTransport>();};
#ifdef SIM_WITH_ASIO
    if(name=="asio-sync" || name=="asio-async")
        return [async=name=="asio-async"] {return makeAsioUdpTransport(async);};
#else
    if(name=="asio-sync" || name=="asio-async")
        throw std::invalid_argument("Asio not enabled: prepare the pinned headers and configure with -DSIM_WITH_ASIO=ON");
#endif
    throw std::invalid_argument("Unknown V2 transport: "+name);
}
}
