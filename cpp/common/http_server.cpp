#include "minicloud/common/http_server.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace minicloud::common {
namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

class HttpServer::Implementation final {
 public:
  Implementation(BindAddress address, HttpHandler handler, const std::size_t max_body_bytes)
      : address_(std::move(address)), handler_(std::move(handler)),
        max_body_bytes_(max_body_bytes), acceptor_(context_) {}

  void run(std::atomic<bool>& running) {
    const auto resolved = tcp::resolver(context_).resolve(address_.host, std::to_string(address_.port));
    if (resolved.empty()) throw std::runtime_error("HTTP bind address did not resolve");
    const tcp::endpoint endpoint = *resolved.begin();
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(asio::socket_base::max_listen_connections);
    running.store(true);
    while (!stopping_.load()) {
      beast::error_code error;
      tcp::socket socket(context_);
      acceptor_.accept(socket, error);
      if (error) {
        if (stopping_.load()) break;
        continue;
      }
      reap_finished_sessions();
      auto done = std::make_shared<std::atomic<bool>>(false);
      std::lock_guard lock(sessions_mutex_);
      if (stopping_.load()) {
        socket.close(error);
        break;
      }
      if (sessions_.size() >= kMaximumConcurrentSessions) {
        socket.close(error);
        continue;
      }
      auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
      sessions_.push_back(Session{
          std::thread([this, done, stream] {
            try {
              serve(*stream);
            } catch (...) {
              // A malformed connection must not terminate the server process.
            }
            done->store(true);
          }),
          std::move(done), std::move(stream)});
    }
    running.store(false);
  }

  void stop() {
    stopping_.store(true);
    beast::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
    // Wake a blocking accept on platforms where close alone is not enough.
    try {
      asio::io_context wake_context;
      tcp::socket wake_socket(wake_context);
      wake_socket.connect(tcp::endpoint(asio::ip::make_address(address_.host), address_.port), ignored);
    } catch (...) {
    }
    std::vector<std::thread> sessions;
    {
      std::lock_guard lock(sessions_mutex_);
      sessions.reserve(sessions_.size());
      for (auto& session : sessions_) {
        session.stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
        sessions.push_back(std::move(session.thread));
      }
      sessions_.clear();
    }
    for (auto& session : sessions) {
      if (session.joinable()) session.join();
    }
  }

 private:
  static constexpr std::size_t kMaximumConcurrentSessions = 256;

  struct Session final {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
    std::shared_ptr<beast::tcp_stream> stream;
  };

  void reap_finished_sessions() {
    std::vector<std::thread> finished;
    {
      std::lock_guard lock(sessions_mutex_);
      auto iterator = sessions_.begin();
      while (iterator != sessions_.end()) {
        if (!iterator->done->load()) {
          ++iterator;
          continue;
        }
        finished.push_back(std::move(iterator->thread));
        iterator = sessions_.erase(iterator);
      }
    }
    for (auto& thread : finished) {
      if (thread.joinable()) thread.join();
    }
  }

  void serve(beast::tcp_stream& stream) {
    beast::flat_buffer buffer;
    beast::error_code error;
    stream.expires_after(std::chrono::seconds(15));
    while (!stopping_.load()) {
      http::request_parser<http::string_body> parser;
      parser.body_limit(max_body_bytes_);
      http::read(stream, buffer, parser, error);
      if (error == http::error::end_of_stream) break;
      if (error) return;
      HttpRequest request = parser.release();
      HttpResponse response;
      try {
        response = handler_(request);
      } catch (...) {
        // Database, cache, and parser exceptions can contain connection strings
        // or implementation details. Keep those details out of HTTP responses.
        response = json_response(http::status::internal_server_error,
                                 "{\"error\":\"internal server error\"}",
                                 request.version(), request.keep_alive());
      }
      response.version(request.version());
      response.keep_alive(request.keep_alive());
      http::write(stream, response, error);
      if (error || !response.keep_alive()) break;
    }
    stream.socket().shutdown(tcp::socket::shutdown_send, error);
  }

  BindAddress address_;
  HttpHandler handler_;
  std::size_t max_body_bytes_;
  asio::io_context context_;
  tcp::acceptor acceptor_;
  std::atomic<bool> stopping_{false};
  std::mutex sessions_mutex_;
  std::vector<Session> sessions_;
};

HttpServer::HttpServer(BindAddress address, HttpHandler handler, const std::size_t max_body_bytes)
    : implementation_(std::make_unique<Implementation>(
          std::move(address), std::move(handler), max_body_bytes)) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
  if (running_.exchange(true)) throw std::logic_error("HTTP server is already running");
  try {
    implementation_->run(running_);
  } catch (...) {
    running_.store(false);
    throw;
  }
}

void HttpServer::stop() {
  if (implementation_) implementation_->stop();
  running_.store(false);
}

HttpResponse text_response(const http::status status, const std::string& body,
                           const std::string& content_type, const unsigned version,
                           const bool keep_alive) {
  HttpResponse response{status, version};
  response.set(http::field::server, "MiniCloud/0.1");
  response.set(http::field::content_type, content_type);
  response.set(http::field::cache_control, "no-store");
  response.keep_alive(keep_alive);
  response.body() = body;
  response.prepare_payload();
  return response;
}

HttpResponse json_response(const http::status status, const std::string& json,
                           const unsigned version, const bool keep_alive) {
  return text_response(status, json, "application/json; charset=utf-8", version, keep_alive);
}

}  // namespace minicloud::common
