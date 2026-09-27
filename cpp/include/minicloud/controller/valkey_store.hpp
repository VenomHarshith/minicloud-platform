#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace minicloud::controller {

struct DiscoveredEndpoint {
  std::string allocation_id;
  std::string service_name;
  std::string url;
};

class ValkeyStore final {
 public:
  explicit ValkeyStore(std::string url);
  ~ValkeyStore();
  ValkeyStore(const ValkeyStore&) = delete;
  ValkeyStore& operator=(const ValkeyStore&) = delete;

  void ping();
  void publish_endpoint(const DiscoveredEndpoint& endpoint, int ttl_seconds = 15);
  void remove_endpoint(const std::string& service_name,
                       const std::string& allocation_id);
  [[nodiscard]] std::vector<DiscoveredEndpoint> endpoints(
      const std::string& service_name);
  // Atomically appends a batch exactly once for the lifetime of its dedupe key.
  // Returns false when the batch_id was already committed.
  [[nodiscard]] bool append_logs(const std::string& allocation_id,
                                 const std::string& batch_id,
                                 const std::vector<std::string>& lines,
                                 std::size_t retention_lines = 2000);
  [[nodiscard]] std::vector<std::string> logs(const std::string& allocation_id,
                                              std::size_t tail);

 private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace minicloud::controller
