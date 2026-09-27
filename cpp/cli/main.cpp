#include "minicloud/common/config.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct HttpResult {
  long status{};
  std::string body;
};

std::size_t append_body(char* data, const std::size_t size, const std::size_t count,
                        void* destination) {
  const std::size_t bytes = size * count;
  auto* output = static_cast<std::string*>(destination);
  if (output->size() + bytes > 4 * 1024 * 1024) return 0;
  output->append(data, bytes);
  return bytes;
}

class CurlGlobal final {
 public:
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw std::runtime_error("failed to initialize libcurl");
    }
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

HttpResult request(const std::string& method, const std::string& path,
                   const std::optional<std::string>& body = std::nullopt) {
  const std::string base = minicloud::common::Environment::value(
      "MINICLOUD_API_URL", "http://127.0.0.1:8090");
  const std::string token = minicloud::common::Environment::required("MINICLOUD_API_TOKEN");
  CURL* handle = curl_easy_init();
  if (handle == nullptr) throw std::runtime_error("failed to allocate HTTP client");
  std::string response;
  curl_slist* headers = nullptr;
  const std::string authorization = "Authorization: Bearer " + token;
  headers = curl_slist_append(headers, authorization.c_str());
  headers = curl_slist_append(headers, "Accept: application/json");
  if (body) headers = curl_slist_append(headers, "Content-Type: application/json");
  curl_easy_setopt(handle, CURLOPT_URL, (base + path).c_str());
  curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 15000L);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  if (body) {
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body->data());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body->size()));
  }
  const CURLcode result = curl_easy_perform(handle);
  long status = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(handle);
  if (result != CURLE_OK) throw std::runtime_error(curl_easy_strerror(result));
  return HttpResult{status, std::move(response)};
}

std::string read_json_file(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
    throw std::runtime_error("deployment input must be a regular non-symlink file");
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > 64 * 1024) throw std::runtime_error("deployment input exceeds 64 KiB");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("unable to open deployment input");
  std::ostringstream output;
  output << input.rdbuf();
  const std::string text = output.str();
  const auto document = nlohmann::json::parse(text);
  if (!document.is_object()) {
    throw std::runtime_error("deployment input must contain a JSON object");
  }
  return text;
}

void print(const HttpResult& response) {
  try {
    std::cout << nlohmann::json::parse(response.body).dump(2) << '\n';
  } catch (const nlohmann::json::exception&) {
    std::cout << response.body << '\n';
  }
  if (response.status < 200 || response.status >= 300) {
    throw std::runtime_error("API returned HTTP " + std::to_string(response.status));
  }
}

void usage() {
  std::cout <<
      "MiniCloud control client\n\n"
      "Usage:\n"
      "  minicloudctl status\n"
      "  minicloudctl deploy FILE.json\n"
      "  minicloudctl scale SERVICE REPLICAS\n"
      "  minicloudctl restart SERVICE\n"
      "  minicloudctl delete SERVICE\n"
      "  minicloudctl logs ALLOCATION [TAIL]\n\n"
      "Set MINICLOUD_API_TOKEN to the value generated in deploy/.env.\n";
}

}  // namespace

int main(const int argc, char** argv) {
  try {
    CurlGlobal curl;
    if (argc < 2) {
      usage();
      return 2;
    }
    const std::string command = argv[1];
    if (command == "status" && argc == 2) {
      print(request("GET", "/api/v1/snapshot"));
      return 0;
    }
    if (command == "deploy" && argc == 3) {
      print(request("POST", "/api/v1/services", read_json_file(argv[2])));
      return 0;
    }
    if (command == "scale" && argc == 4) {
      const int replicas = std::stoi(argv[3]);
      if (replicas < 0 || replicas > 50) throw std::invalid_argument("replicas must be 0-50");
      print(request("POST", "/api/v1/services/" + std::string(argv[2]) + "/scale",
                    nlohmann::json{{"replicas", replicas}}.dump()));
      return 0;
    }
    if (command == "restart" && argc == 3) {
      print(request("POST", "/api/v1/services/" + std::string(argv[2]) + "/restart"));
      return 0;
    }
    if (command == "delete" && argc == 3) {
      print(request("DELETE", "/api/v1/services/" + std::string(argv[2])));
      return 0;
    }
    if (command == "logs" && (argc == 3 || argc == 4)) {
      const int tail = argc == 4 ? std::stoi(argv[3]) : 200;
      if (tail < 1 || tail > 1000) throw std::invalid_argument("tail must be 1-1000");
      print(request("GET", "/api/v1/allocations/" + std::string(argv[2]) +
                               "/logs?tail=" + std::to_string(tail)));
      return 0;
    }
    usage();
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "minicloudctl: " << error.what() << '\n';
    return 1;
  }
}
