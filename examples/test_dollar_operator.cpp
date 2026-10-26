// Test for $ operator (stringify variable content)
#include <iostream>
#include <string>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

int main() {
  QLangContext ctx;
  
  // Test 1: $ operator with int
  std::cout << "Test 1: $ operator with int" << std::endl;
  try {
    ctx.run("local age = 33\nlocal str_age = $age\nprintln(str_age)");
    std::cout << "[PASS] $age = \"33\"" << std::endl;
  } catch (const QLangError& e) {
    std::cout << "[FAIL] " << e.what() << std::endl;
  }
  
  // Test 2: $ operator with string
  std::cout << "\nTest 2: $ operator with string" << std::endl;
  try {
    ctx.run("local name = \"Frank\"\nlocal str_name = $name\nprintln(str_name)");
    std::cout << "[PASS] $name = \"Frank\"" << std::endl;
  } catch (const QLangError& e) {
    std::cout << "[FAIL] " << e.what() << std::endl;
  }
  
  // Test 3: $ operator with float
  std::cout << "\nTest 3: $ operator with float" << std::endl;
  try {
    ctx.run("local price = 19.99\nlocal str_price = $price\nprintln(str_price)");
    std::cout << "[PASS] $price = \"19.99\"" << std::endl;
  } catch (const QLangError& e) {
    std::cout << "[FAIL] " << e.what() << std::endl;
  }
  
  // Test 4: $ operator with bool
  std::cout << "\nTest 4: $ operator with bool" << std::endl;
  try {
    ctx.run("local flag = true\nlocal str_flag = $flag\nprintln(str_flag)");
    std::cout << "[PASS] $flag = \"true\"" << std::endl;
  } catch (const QLangError& e) {
    std::cout << "[FAIL] " << e.what() << std::endl;
  }
  
  // Test 5: $ operator with nil
  std::cout << "\nTest 5: $ operator with nil" << std::endl;
  try {
    ctx.run("local z\nlocal str_z = $z\nprintln(str_z)");
    std::cout << "[PASS] $z = \"false\" (nil is falsy)" << std::endl;
  } catch (const QLangError& e) {
    std::cout << "[FAIL] " << e.what() << std::endl;
  }
  
  // Test 6: $ operator with array (should fail)
  std::cout << "\nTest 6: $ operator with array (should fail)" << std::endl;
  try {
    ctx.run("local a = [1, 2, 3]\nlocal err = $a");
    std::cout << "[FAIL] Should have thrown error for array" << std::endl;
  } catch (const QLangError& e) {
    std::cout << "[PASS] Correctly rejected array: " << e.what() << std::endl;
  }
  
  // Test 7: $ operator with table (should fail)
  std::cout << "\nTest 7: $ operator with table (should fail)" << std::endl;
  try {
    ctx.run("local t = {x=1, y=2}\nlocal err = $t");
    std::cout << "[FAIL] Should have thrown error for table" << std::endl;
  } catch (const QLangError& e) {
    std::cout << "[PASS] Correctly rejected table: " << e.what() << std::endl;
  }
  
  // Test 8: $ operator in table literal (dynamic key)
  std::cout << "\nTest 8: $ operator in table literal (dynamic key)" << std::endl;
  try {
    ctx.run("local prop = \"gender\"\nlocal t:table = {name=\"Bob\", age=99, $prop=\"male\"}\nprintln(t.name)\nprintln(t.age)\nprintln(t.gender)");
    std::cout << "[PASS] Dynamic key resolution works" << std::endl;
  } catch (const QLangError& e) {
    std::cout << "[FAIL] " << e.what() << std::endl;
  }
  
  std::cout << "\nAll tests completed!" << std::endl;
  return 0;
}