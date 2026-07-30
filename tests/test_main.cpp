#include <iostream>

int test_framing();
int test_aios_scan();
int test_auth();
int test_object_store();

int main() {
  int failures = 0;
  failures += test_framing();
  failures += test_aios_scan();
  failures += test_auth();
  failures += test_object_store();
  if (failures == 0) {
    std::cout << "all tests passed\n";
    return 0;
  }
  std::cerr << failures << " failure(s)\n";
  return 1;
}
