#pragma once

namespace qlang {
class QLangContext;

// Register all built-in modules (math, etc.) into the given context.
// Call this after constructing QLangContext, before the first run().
void registerBuiltinModules(QLangContext& ctx);

}  // namespace qlang
