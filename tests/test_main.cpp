#include <iostream>

int test_framing();
int test_framing_raw();
int test_aios_scan();
int test_auth();
int test_object_store();
int test_cluster_map();
int test_object_rpc();
int test_object_rpc_advanced();
int test_store_range();
int test_store_advanced();
int test_http_auth();
int test_http_api();
int test_http_wire();
int test_http_ec();
int test_crc32c();
int test_versions();
int test_object_service_advanced();
int test_fs_clone();
int test_mini_cluster();
int test_ec();
int test_layout();
int test_locks_watches();
int test_pubsub();
int test_stl_client();
int test_append();
int test_admin();
int test_placement();

int main() {
  int failures = 0;
  failures += test_framing();
  failures += test_framing_raw();
  failures += test_aios_scan();
  failures += test_auth();
  failures += test_fs_clone();
  failures += test_object_store();
  failures += test_store_range();
  failures += test_store_advanced();
  failures += test_versions();
  failures += test_crc32c();
  failures += test_cluster_map();
  failures += test_placement();
  failures += test_object_rpc();
  failures += test_object_rpc_advanced();
  failures += test_object_service_advanced();
  failures += test_http_auth();
  failures += test_http_api();
  failures += test_http_wire();
  failures += test_http_ec();
  failures += test_mini_cluster();
  failures += test_ec();
  failures += test_layout();
  failures += test_locks_watches();
  failures += test_pubsub();
  failures += test_stl_client();
  failures += test_append();
  failures += test_admin();
  if (failures == 0) {
    std::cout << "all tests passed\n";
    return 0;
  }
  std::cerr << failures << " failure(s)\n";
  return 1;
}
