#pragma once
#include "core/Types.h"
#include <string>

namespace rt {
    // Derived only when exporting. No receiver or physical timing evidence is inferred.
    std::string acceptanceTableJson(const Snapshot& snapshot);
    std::string acceptanceTableMarkdown(const Snapshot& snapshot);
}
