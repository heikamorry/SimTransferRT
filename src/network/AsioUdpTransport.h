#pragma once
#include "network/IUdpTransport.h"

namespace rt {
std::unique_ptr<IUdpTransport> makeAsioUdpTransport(bool async);
}
