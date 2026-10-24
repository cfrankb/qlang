#include <iostream>
#include <fstream>
#include <sstream>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

static std::string readFile(const char* path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error(std::string("cannot open file: ") + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <script.q>\n";
    return 1;
  }

  std::string src = readFile(argv[1]);
  QLangContext ctx;
  try {
    ctx.run(src);
  } catch (const QLangError& e) {
    std::cerr << "Runtime error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
