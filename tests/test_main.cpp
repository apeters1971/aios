#include <iostream>

int test_framing();
int test_aios_scan();
int test_auth();
int test_object_store();
int test_cluster_map();
int test_object_rpc();
int test_store_range();
int test_http_auth();
int test_http_api();

int main() {
  int failures = 0;
  failures += test_framing();
  failures += test_aios_scan();
  failures += test_auth();
  failures += test_object_store();
  failures += test_cluster_map();
  failures += test_object_rpc();
  failures += test_store_range();
  failures += test_http_auth();
  failures += test_http_api();
  if (failures == 0) {
    std::cout << "all tests passed\n";
    return 0;
  }
  std::cerr << failures << " failure(s)\n";
  return 1;
}
