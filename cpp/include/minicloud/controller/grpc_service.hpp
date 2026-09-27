#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "controller_worker.grpc.pb.h"

namespace minicloud::common { class MetricsRegistry; }

namespace minicloud::controller {

class Repository;
class ValkeyStore;

class ControllerGrpcService final
    : public worker::v1::ControllerWorkerService::Service {
 public:
  ControllerGrpcService(Repository& repository, ValkeyStore& valkey,
                        common::MetricsRegistry& metrics,
                        std::string cluster_token,
                        std::uint64_t controller_epoch);

  grpc::Status RegisterWorker(grpc::ServerContext* context,
                              const worker::v1::RegisterWorkerRequest* request,
                              worker::v1::RegisterWorkerResponse* response) override;
  grpc::Status Heartbeat(grpc::ServerContext* context,
                         const worker::v1::HeartbeatRequest* request,
                         worker::v1::HeartbeatResponse* response) override;
  grpc::Status PollCommands(grpc::ServerContext* context,
                            const worker::v1::PollCommandsRequest* request,
                            worker::v1::PollCommandsResponse* response) override;
  grpc::Status CompleteCommand(grpc::ServerContext* context,
                               const worker::v1::CompleteCommandRequest* request,
                               worker::v1::CompleteCommandResponse* response) override;
  grpc::Status ReportWorkloadStatus(
      grpc::ServerContext* context,
      const worker::v1::ReportWorkloadStatusRequest* request,
      worker::v1::ReportWorkloadStatusResponse* response) override;
  grpc::Status StreamLogs(grpc::ServerContext* context,
                          grpc::ServerReader<worker::v1::LogBatch>* reader,
                          worker::v1::LogIngestSummary* response) override;

 private:
  [[nodiscard]] bool authorized(const grpc::ServerContext& context) const;
  Repository& repository_;
  ValkeyStore& valkey_;
  common::MetricsRegistry& metrics_;
  std::string cluster_token_;
  std::uint64_t controller_epoch_;
};

class GrpcServer final {
 public:
  GrpcServer(std::string address, ControllerGrpcService& service);
  ~GrpcServer();
  void run();
  void stop();

 private:
  std::string address_;
  ControllerGrpcService& service_;
  std::atomic<bool> stop_requested_{false};
  std::mutex server_mutex_;
  grpc::Server* server_{nullptr};
};

}  // namespace minicloud::controller
