#include <iostream>
#include <string>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

int main() {
    QLangContext ctx;
    try {
        ctx.run("local events:table = {\n  onLoad = (name)[] {\n    printf(\"onLOAD -->%s\\n\", name);\n  }\n}\nevents.onLoad(\"test\")");
        std::cout << "[PASS] run ok" << std::endl;
    } catch (const QLangError &e) {
        std::cout << "[FAIL] -> " << e.what() << std::endl;
    }
    return 0;
}
