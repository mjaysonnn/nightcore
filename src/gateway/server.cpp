#include "gateway/server.h"

#include "ipc/base.h"
#include "ipc/shm_region.h"
#include "common/time.h"
#include "utils/fs.h"
#include "utils/io.h"
#include "utils/docker.h"
#include "worker/worker_lib.h"

#include <absl/flags/flag.h>

ABSL_FLAG(size_t, max_running_external_requests, 0, "");
ABSL_FLAG(bool, disable_monitor, false, "");

#define HLOG(l) LOG(l) << "Server: "
#define HVLOG(l) VLOG(l) << "Server: "

namespace faas {
namespace gateway {

using protocol::FuncCall;
using protocol::FuncCallDebugString;
using protocol::NewFuncCall;
using protocol::NewFuncCallWithMethod;
using protocol::Message;
using protocol::GetFuncCallFromMessage;
using protocol::GetInlineDataFromMessage;
using protocol::IsLauncherHandshakeMessage;
using protocol::IsFuncWorkerHandshakeMessage;
using protocol::IsInvokeFuncMessage;
using protocol::IsFuncCallCompleteMessage;
using protocol::IsFuncCallFailedMessage;
using protocol::NewHandshakeResponseMessage;
using protocol::ComputeMessageDelay;

Server::Server()
    : http_port_(-1), grpc_port_(-1),
      listen_backlog_(kDefaultListenBackLog), num_http_workers_(kDefaultNumHttpWorkers),
      num_ipc_workers_(kDefaultNumIpcWorkers), num_io_workers_(-1),
      next_http_connection_id_(0), next_grpc_connection_id_(0),
      next_http_worker_id_(0), next_ipc_worker_id_(0),
      worker_manager_(new WorkerManager(this)),
      monitor_(absl::GetFlag(FLAGS_disable_monitor) ? nullptr : new Monitor(this)),
      tracer_(new Tracer(this)),
      max_running_external_requests_(absl::GetFlag(FLAGS_max_running_external_requests)),
      next_call_id_(1),
      inflight_external_requests_(0),
      last_external_request_timestamp_(-1),
      incoming_external_requests_stat_(
          stat::Counter::StandardReportCallback("incoming_external_requests")),
      external_requests_instant_rps_stat_(
          stat::StatisticsCollector<float>::StandardReportCallback("external_requests_instant_rps")),
      inflight_external_requests_stat_(
          stat::StatisticsCollector<uint16_t>::StandardReportCallback("inflight_external_requests")),
      pending_external_requests_stat_(
          stat::StatisticsCollector<uint16_t>::StandardReportCallback("pending_external_requests")),
      message_delay_stat_(
          stat::StatisticsCollector<int32_t>::StandardReportCallback("message_delay")),
      input_use_shm_stat_(stat::Counter::StandardReportCallback("input_use_shm")),
      output_use_shm_stat_(stat::Counter::StandardReportCallback("output_use_shm")),
      discarded_func_call_stat_(stat::Counter::StandardReportCallback("discarded_func_call")) {
    if (max_running_external_requests_ > 0) {
        HLOG(INFO) << "max_running_external_requests=" << max_running_external_requests_;
    }
    UV_DCHECK_OK(uv_tcp_init(uv_loop(), &uv_http_handle_));
    uv_http_handle_.data = this;
    UV_DCHECK_OK(uv_tcp_init(uv_loop(), &uv_grpc_handle_));
    uv_grpc_handle_.data = this;
    UV_DCHECK_OK(uv_pipe_init(uv_loop(), &uv_ipc_handle_, 0));
    uv_ipc_handle_.data = this;
}

Server::~Server() {}

void Server::RegisterInternalRequestHandlers() {
    // POST /shutdown
    RegisterSyncRequestHandler([] (std::string_view method, std::string_view path) -> bool {
        return method == "POST" && path == "/shutdown";
    }, [this] (HttpSyncRequestContext* context) {
        context->AppendToResponseBody("Server is shutting down\n");
        ScheduleStop();
    });
    // GET /hello
    RegisterSyncRequestHandler([] (std::string_view method, std::string_view path) -> bool {
        return method == "GET" && path == "/hello";
    }, [] (HttpSyncRequestContext* context) {
        context->AppendToResponseBody("Hello world\n");
    });
    // POST /function/[:name]
    RegisterAsyncRequestHandler([this] (std::string_view method, std::string_view path) -> bool {
        if (method != "POST" || !absl::StartsWith(path, "/function/")) {
            return false;
        }
        std::string_view func_name = absl::StripPrefix(path, "/function/");
        const FuncConfig::Entry* func_entry = func_config_.find_by_func_name(func_name);
        return func_entry != nullptr;
    }, [this] (std::shared_ptr<HttpAsyncRequestContext> context) {
        const FuncConfig::Entry* func_entry = func_config_.find_by_func_name(
            absl::StripPrefix(context->path(), "/function/"));
        DCHECK(func_entry != nullptr);
        OnExternalFuncCall(gsl::narrow_cast<uint16_t>(func_entry->func_id), std::move(context));
    });
}

void Server::StartInternal() {
    RegisterInternalRequestHandlers();
    // Load function config file
    CHECK(!func_config_file_.empty());
    CHECK(fs_utils::ReadContents(func_config_file_, &func_config_json_))
        << "Failed to read from file " << func_config_file_;
    CHECK(func_config_.Load(func_config_json_));
    // Start IO workers
    if (num_io_workers_ == -1) {
        CHECK_GT(num_http_workers_, 0);
        CHECK_GT(num_ipc_workers_, 0);
        HLOG(INFO) << "Start " << num_http_workers_ << " IO workers for HTTP connections";
        for (int i = 0; i < num_http_workers_; i++) {
            http_workers_.push_back(CreateIOWorker(
                absl::StrFormat("Http-%d", i), kHttpConnectionBufferSize));
        }
        HLOG(INFO) << "Start " << num_ipc_workers_ << " IO workers for IPC connections";
        for (int i = 0; i < num_ipc_workers_; i++) {
            ipc_workers_.push_back(CreateIOWorker(
                absl::StrFormat("Ipc-%d", i),
                kMessageConnectionBufferSize, kMessageConnectionBufferSize));
        }
    } else {
        CHECK_GT(num_io_workers_, 0);
        HLOG(INFO) << "Start " << num_io_workers_
                   << " IO workers for both HTTP and IPC connections";
        for (int i = 0; i < num_io_workers_; i++) {
            auto io_worker = CreateIOWorker(absl::StrFormat("IO-%d", i));
            http_workers_.push_back(io_worker);
            ipc_workers_.push_back(io_worker);
        }
    }
    // Listen on address:http_port for HTTP requests
    struct sockaddr_in bind_addr;
    CHECK(!address_.empty());
    CHECK_NE(http_port_, -1);
    UV_CHECK_OK(uv_ip4_addr(address_.c_str(), http_port_, &bind_addr));
    UV_CHECK_OK(uv_tcp_bind(&uv_http_handle_, (const struct sockaddr *)&bind_addr, 0));
    HLOG(INFO) << "Listen on " << address_ << ":" << http_port_ << " for HTTP requests";
    UV_DCHECK_OK(uv_listen(
        UV_AS_STREAM(&uv_http_handle_), listen_backlog_,
        &Server::HttpConnectionCallback));
    // Listen on address:grpc_port for gRPC requests
    if (grpc_port_ != -1) {
        UV_CHECK_OK(uv_ip4_addr(address_.c_str(), grpc_port_, &bind_addr));
        UV_CHECK_OK(uv_tcp_bind(&uv_grpc_handle_, (const struct sockaddr *)&bind_addr, 0));
        HLOG(INFO) << "Listen on " << address_ << ":" << grpc_port_ << " for gRPC requests";
        UV_CHECK_OK(uv_listen(
            UV_AS_STREAM(&uv_grpc_handle_), listen_backlog_,
            &Server::GrpcConnectionCallback));
    }
    // Listen on ipc_path
    std::string ipc_path(ipc::GetGatewayUnixSocketPath());
    if (fs_utils::Exists(ipc_path)) {
        PCHECK(fs_utils::Remove(ipc_path));
    }
    UV_CHECK_OK(uv_pipe_bind(&uv_ipc_handle_, ipc_path.c_str()));
    HLOG(INFO) << "Listen on " << ipc_path << " for IPC connections";
    UV_CHECK_OK(uv_listen(
        UV_AS_STREAM(&uv_ipc_handle_), listen_backlog_,
        &Server::MessageConnectionCallback));
    // Initialize tracer
    tracer_->Init();
}

void Server::StopInternal() {
    uv_close(UV_AS_HANDLE(&uv_http_handle_), nullptr);
    uv_close(UV_AS_HANDLE(&uv_grpc_handle_), nullptr);
    uv_close(UV_AS_HANDLE(&uv_ipc_handle_), nullptr);
}

void Server::RegisterSyncRequestHandler(RequestMatcher matcher, SyncRequestHandler handler) {
    DCHECK(state_.load() == kCreated);
    request_handlers_.emplace_back(new RequestHandler(std::move(matcher), std::move(handler)));
}

void Server::RegisterAsyncRequestHandler(RequestMatcher matcher, AsyncRequestHandler handler) {
    DCHECK(state_.load() == kCreated);
    request_handlers_.emplace_back(new RequestHandler(std::move(matcher), std::move(handler)));
}

bool Server::MatchRequest(std::string_view method, std::string_view path,
                          const RequestHandler** request_handler) const {
    for (const std::unique_ptr<RequestHandler>& entry : request_handlers_) {
        if (entry->matcher_(method, path)) {
            *request_handler = entry.get();
            return true;
        }
    }
    return false;
}

server::IOWorker* Server::PickHttpWorker() {
    server::IOWorker* io_worker = http_workers_[next_http_worker_id_];
    next_http_worker_id_ = (next_http_worker_id_ + 1) % http_workers_.size();
    return io_worker;
}

server::IOWorker* Server::PickIpcWorker() {
    server::IOWorker* io_worker = ipc_workers_[next_ipc_worker_id_];
    next_ipc_worker_id_ = (next_ipc_worker_id_ + 1) % ipc_workers_.size();
    return io_worker;
}

void Server::OnConnectionClose(server::ConnectionBase* connection) {
    DCHECK_IN_EVENT_LOOP_THREAD(uv_loop());
    if (connection->type() == HttpConnection::kTypeId) {
        HttpConnection* http_connection = static_cast<HttpConnection*>(connection);
        DCHECK(http_connections_.contains(http_connection));
        http_connections_.erase(http_connection);
    } else if (connection->type() == GrpcConnection::kTypeId) {
        GrpcConnection* grpc_connection = static_cast<GrpcConnection*>(connection);
        DCHECK(grpc_connections_.contains(grpc_connection));
        grpc_connections_.erase(grpc_connection);
    } else if (connection->type() == MessageConnection::kTypeId) {
        MessageConnection* message_connection = static_cast<MessageConnection*>(connection);
        if (message_connection->handshake_done()) {
            if (message_connection->is_launcher_connection()) {
                worker_manager_->OnLauncherDisconnected(message_connection);
            } else {
                worker_manager_->OnFuncWorkerDisconnected(message_connection);
            }
        }
        message_connections_.erase(message_connection);
        HLOG(INFO) << "A MessageConnection is returned";
    } else {
        HLOG(ERROR) << "Unknown connection type!";
    }
}

bool Server::OnNewHandshake(MessageConnection* connection,
                            const Message& handshake_message, Message* response,
                            std::span<const char>* response_payload) {
    if (!IsLauncherHandshakeMessage(handshake_message)
          && !IsFuncWorkerHandshakeMessage(handshake_message)) {
        HLOG(ERROR) << "Received message is not a handshake message";
        return false;
    }
    HLOG(INFO) << "Receive new handshake message from message connection";
    uint16_t func_id = handshake_message.func_id;
    if (func_config_.find_by_func_id(func_id) == nullptr) {
        HLOG(ERROR) << "Invalid func_id " << func_id << " in handshake message";
        return false;
    }
    bool success;
    if (IsLauncherHandshakeMessage(handshake_message)) {
        std::span<const char> payload = GetInlineDataFromMessage(handshake_message);
        if (payload.size() != docker_utils::kContainerIdLength) {
            HLOG(ERROR) << "Launcher handshake does not have container ID in inline data";
            return false;
        }
        std::string container_id(payload.data(), payload.size());
        if (monitor_ != nullptr && container_id != docker_utils::kInvalidContainerId) {
            monitor_->OnNewFuncContainer(func_id, container_id);
        }
        success = worker_manager_->OnLauncherConnected(connection);
    } else {
        success = worker_manager_->OnFuncWorkerConnected(connection);
        ProcessDiscardedFuncCallIfNecessary();
    }
    if (!success) {
        return false;
    }
    *response = NewHandshakeResponseMessage(func_config_json_.size());
    *response_payload = std::span<const char>(func_config_json_.data(), func_config_json_.size());
    return true;
}

class Server::ExternalFuncCallContext {
public:
    ExternalFuncCallContext(Server* server, FuncCall call,
                            std::shared_ptr<HttpAsyncRequestContext> http_context)
        : server_(server), call_(call), http_context_(http_context), grpc_context_(nullptr),
          input_region_(nullptr), output_region_(nullptr) {
        server_->inflight_external_requests_.fetch_add(1);
    }
    
    ExternalFuncCallContext(Server* server, FuncCall call,
                            std::shared_ptr<GrpcCallContext> grpc_context)
        : server_(server), call_(call), http_context_(nullptr), grpc_context_(grpc_context),
          input_region_(nullptr), output_region_(nullptr) {
        server_->inflight_external_requests_.fetch_add(1);
    }

    ~ExternalFuncCallContext() {
        server_->inflight_external_requests_.fetch_add(-1);
    }

    FuncCall call() const { return call_; }

    std::span<const char> GetInput() const {
        if (http_context_ != nullptr) {
            return http_context_->body();
        } else if (grpc_context_ != nullptr) {
            return grpc_context_->request_body();
        } else {
            LOG(FATAL) << "http_context_ and grpc_context_ are both nullptr";
        }
    }

    bool CreateShmInput() {
        std::span<const char> body = GetInput();
        input_region_ = ipc::ShmCreate(
            ipc::GetFuncCallInputShmName(call_.full_call_id), body.size());
        if (input_region_ == nullptr) {
            LOG(ERROR) << "ShmCreate failed";
            FinishWithError();
            return false;
        }
        input_region_->EnableRemoveOnDestruction();
        if (body.size() > 0) {
            memcpy(input_region_->base(), body.data(), body.size());
        }
        return true;
    }

    void FinishWithShmOutput() {
        output_region_ = ipc::ShmOpen(ipc::GetFuncCallOutputShmName(call_.full_call_id));
        if (output_region_ == nullptr) {
            HLOG(ERROR) << "Failed to open output shm";
            FinishWithError();
            return;
        }
        output_region_->EnableRemoveOnDestruction();
        if (output_region_->size() > 0) {
            if (http_context_ != nullptr) {
                http_context_->AppendToResponseBody(output_region_->to_span());
            } else if (grpc_context_ != nullptr) {
                grpc_context_->AppendToResponseBody(output_region_->to_span());
            } else {
                LOG(FATAL) << "http_context_ and grpc_context_ are both nullptr";
            }
        }
        Finish();
    }

    void FinishWithOutput(std::span<const char> output) {
        if (output.size() > 0) {
            if (http_context_ != nullptr) {
                http_context_->AppendToResponseBody(output);
            } else if (grpc_context_ != nullptr) {
                grpc_context_->AppendToResponseBody(output);
            } else {
                LOG(FATAL) << "http_context_ and grpc_context_ are both nullptr";
            }
        }
        Finish();
    }

    void FinishWithError() {
        if (http_context_ != nullptr) {
            http_context_->AppendToResponseBody("Function call failed\n");
            http_context_->SetStatus(500);
        } else if (grpc_context_ != nullptr) {
            grpc_context_->set_grpc_status(GrpcStatus::UNKNOWN);
        } else {
            LOG(FATAL) << "http_context_ and grpc_context_ are both nullptr";
        }
        Finish();
    }

    void FinishWithDispatchFailure() {
        if (http_context_ != nullptr) {
            http_context_->AppendToResponseBody(
                absl::StrFormat("Dispatch failed for func_id %d\n", call_.func_id));
            http_context_->SetStatus(404);
        } else if (grpc_context_ != nullptr) {
            grpc_context_->set_grpc_status(GrpcStatus::UNIMPLEMENTED);
        } else {
            LOG(FATAL) << "http_context_ and grpc_context_ are both nullptr";
        }
        Finish();
    }

    void Finish() {
        if (http_context_ != nullptr) {
            http_context_->Finish();
        } else if (grpc_context_ != nullptr) {
            grpc_context_->Finish();
        } else {
            LOG(FATAL) << "http_context_ and grpc_context_ are both nullptr";
        }
    }

private:
    Server* server_;
    FuncCall call_;
    std::shared_ptr<HttpAsyncRequestContext> http_context_;
    std::shared_ptr<GrpcCallContext> grpc_context_;
    std::unique_ptr<ipc::ShmRegion> input_region_;
    std::unique_ptr<ipc::ShmRegion> output_region_;
    DISALLOW_COPY_AND_ASSIGN(ExternalFuncCallContext);
};

void Server::OnRecvMessage(MessageConnection* connection, const Message& message) {
    int32_t message_delay = ComputeMessageDelay(message);
    if (IsInvokeFuncMessage(message)) {
        FuncCall func_call = GetFuncCallFromMessage(message);
        FuncCall parent_func_call;
        parent_func_call.full_call_id = message.parent_call_id;
        Dispatcher* dispatcher = nullptr;
        {
            absl::MutexLock lk(&mu_);
            if (message.payload_size < 0) {
                input_use_shm_stat_.Tick();
            }
            if (message_delay >= 0) {
                message_delay_stat_.AddSample(message_delay);
            }
            dispatcher = GetOrCreateDispatcherLocked(func_call.func_id);
        }
        bool success = false;
        if (dispatcher != nullptr) {
            if (message.payload_size < 0) {
                success = dispatcher->OnNewFuncCall(
                    func_call, parent_func_call,
                    /* input_size= */ gsl::narrow_cast<size_t>(-message.payload_size),
                    std::span<const char>(), /* shm_input= */ true);
                
            } else {
                success = dispatcher->OnNewFuncCall(
                    func_call, parent_func_call,
                    /* input_size= */ gsl::narrow_cast<size_t>(message.payload_size),
                    GetInlineDataFromMessage(message), /* shm_input= */ false);
            }
        }
        if (!success) {
            HLOG(ERROR) << "Dispatcher failed for func_id " << func_call.func_id;
        }
    } else if (IsFuncCallCompleteMessage(message) || IsFuncCallFailedMessage(message)) {
        FuncCall func_call = GetFuncCallFromMessage(message);
        Dispatcher* dispatcher = nullptr;
        std::unique_ptr<ExternalFuncCallContext> func_call_context;
        ExternalFuncCallContext* func_call_for_dispatch = nullptr;
        {
            absl::MutexLock lk(&mu_);
            if (message_delay >= 0) {
                message_delay_stat_.AddSample(message_delay);
            }
            if (IsFuncCallCompleteMessage(message)) {
                if ((func_call.client_id == 0 && message.payload_size < 0)
                      || (func_call.client_id > 0
                          && message.payload_size + sizeof(int32_t) > PIPE_BUF)) {
                    output_use_shm_stat_.Tick();
                }
            }
            uint64_t full_call_id = func_call.full_call_id;
            if (func_call.client_id == 0 && running_external_func_calls_.contains(full_call_id)) {
                func_call_context = std::move(running_external_func_calls_[full_call_id]);
                running_external_func_calls_.erase(full_call_id);
                if (!pending_external_func_calls_.empty()
                        && (max_running_external_requests_ == 0 ||
                            running_external_func_calls_.size() < max_running_external_requests_)) {
                    auto func_call_context = std::move(pending_external_func_calls_.front());
                    pending_external_func_calls_.pop();
                    uint64_t full_call_id = func_call_context->call().full_call_id;
                    func_call_for_dispatch = func_call_context.get();
                    running_external_func_calls_[full_call_id] = std::move(func_call_context);
                }
            }
            dispatcher = GetOrCreateDispatcherLocked(func_call.func_id);
        }
        if (dispatcher != nullptr) {
            if (IsFuncCallCompleteMessage(message)) {
                dispatcher->OnFuncCallCompleted(
                    func_call, message.processing_time, message.dispatch_delay,
                    /* output_size= */ gsl::narrow_cast<size_t>(std::abs(message.payload_size)));
            } else {
                dispatcher->OnFuncCallFailed(func_call, message.dispatch_delay);
            }
        }
        if (func_call.client_id == 0) {
            if (func_call_context != nullptr) {
                if (IsFuncCallCompleteMessage(message)) {
                    if (message.payload_size < 0) {
                        VLOG(1) << "External call finished with shm output";
                        func_call_context->FinishWithShmOutput();
                    } else {
                        VLOG(1) << "External call finished with inline output";
                        func_call_context->FinishWithOutput(GetInlineDataFromMessage(message));
                    }
                } else {
                    func_call_context->FinishWithError();
                }
            } else {
                HLOG(ERROR) << "Cannot find external call " << FuncCallDebugString(func_call);
            }
        }
        if (func_call_for_dispatch != nullptr && !DispatchExternalFuncCall(func_call_for_dispatch)) {
            FuncCall func_call = func_call_for_dispatch->call();
            HLOG(ERROR) << fmt::format("Dispatch func_call ({}) failed",
                                       FuncCallDebugString(func_call));
            absl::MutexLock lk(&mu_);
            running_external_func_calls_.erase(func_call.full_call_id);
        }
    } else {
        LOG(ERROR) << "Unknown message type!";
    }
    ProcessDiscardedFuncCallIfNecessary();
}

void Server::OnNewGrpcCall(std::shared_ptr<GrpcCallContext> call_context) {
    const FuncConfig::Entry* func_entry = func_config_.find_by_func_name(
        absl::StrFormat("grpc:%s", call_context->service_name()));
    std::string method_name(call_context->method_name());
    if (func_entry == nullptr
          || func_entry->grpc_method_ids.count(method_name) == 0) {
        call_context->set_grpc_status(GrpcStatus::NOT_FOUND);
        call_context->Finish();
        return;
    }
    NewExternalFuncCall(std::unique_ptr<ExternalFuncCallContext>(
        new ExternalFuncCallContext(
                this, NewFuncCallWithMethod(func_entry->func_id,
                                            func_entry->grpc_method_ids.at(method_name),
                                            /* client_id= */ 0, next_call_id_.fetch_add(1)),
                std::move(call_context))));
}

void Server::OnExternalFuncCall(uint16_t func_id,
                                std::shared_ptr<HttpAsyncRequestContext> http_context) {
    NewExternalFuncCall(std::unique_ptr<ExternalFuncCallContext>(
        new ExternalFuncCallContext(
                this, NewFuncCall(func_id, /* client_id= */ 0, next_call_id_.fetch_add(1)),
                std::move(http_context))));
}

bool Server::DispatchExternalFuncCall(ExternalFuncCallContext* func_call_context) {
    FuncCall func_call = func_call_context->call();
    std::span<const char> input = func_call_context->GetInput();
    if (input.size() > MESSAGE_INLINE_DATA_SIZE) {
        if (!func_call_context->CreateShmInput()) {
            return false;
        }
    }
    Dispatcher* dispatcher = nullptr;
    {
        absl::MutexLock lk(&mu_);
        if (input.size() > MESSAGE_INLINE_DATA_SIZE) {
            input_use_shm_stat_.Tick();
        }
        dispatcher = GetOrCreateDispatcherLocked(func_call.func_id);
    }
    if (dispatcher == nullptr) {
        func_call_context->FinishWithDispatchFailure();
        return false;
    }
    bool success = false;
    if (input.size() <= MESSAGE_INLINE_DATA_SIZE) {
        success = dispatcher->OnNewFuncCall(
            func_call, protocol::kInvalidFuncCall,
            input.size(), /* inline_input= */ input, /* shm_input= */ false);
    } else {
        success = dispatcher->OnNewFuncCall(
            func_call, protocol::kInvalidFuncCall,
            input.size(), /* inline_input= */ std::span<const char>(), /* shm_input= */ true);
    }
    if (!success) {
        func_call_context->FinishWithDispatchFailure();
    }
    return success;
}

void Server::NewExternalFuncCall(std::unique_ptr<ExternalFuncCallContext> func_call_context) {
    ExternalFuncCallContext* func_call_for_dispatch = nullptr;
    {
        absl::MutexLock lk(&mu_);
        incoming_external_requests_stat_.Tick();
        int64_t current_timestamp = GetMonotonicMicroTimestamp();
        if (last_external_request_timestamp_ != -1) {
            external_requests_instant_rps_stat_.AddSample(gsl::narrow_cast<double>(
                1e6 / (current_timestamp - last_external_request_timestamp_)));
        }
        last_external_request_timestamp_ = current_timestamp;
        inflight_external_requests_stat_.AddSample(
            gsl::narrow_cast<uint16_t>(inflight_external_requests_.load()));
        if (max_running_external_requests_ == 0
                || running_external_func_calls_.size() < max_running_external_requests_) {
            func_call_for_dispatch = func_call_context.get();
            uint64_t full_call_id = func_call_context->call().full_call_id;
            running_external_func_calls_[full_call_id] = std::move(func_call_context);
        } else {
            pending_external_func_calls_.push(std::move(func_call_context));
            pending_external_requests_stat_.AddSample(
                gsl::narrow_cast<uint16_t>(pending_external_func_calls_.size()));
        }
    }
    if (func_call_for_dispatch != nullptr && !DispatchExternalFuncCall(func_call_for_dispatch)) {
        FuncCall func_call = func_call_for_dispatch->call();
        HLOG(ERROR) << fmt::format("Dispatch func_call ({}) failed", FuncCallDebugString(func_call));
        absl::MutexLock lk(&mu_);
        running_external_func_calls_.erase(func_call.full_call_id);
    }
}

Dispatcher* Server::GetOrCreateDispatcher(uint16_t func_id) {
    absl::MutexLock lk(&mu_);
    Dispatcher* dispatcher = GetOrCreateDispatcherLocked(func_id);
    return dispatcher;
}

Dispatcher* Server::GetOrCreateDispatcherLocked(uint16_t func_id) {
    if (dispatchers_.contains(func_id)) {
        return dispatchers_[func_id].get();
    }
    if (func_config_.find_by_func_id(func_id) != nullptr) {
        dispatchers_[func_id] = std::make_unique<Dispatcher>(this, func_id);
        return dispatchers_[func_id].get();
    } else {
        return nullptr;
    }
}

void Server::DiscardFuncCall(const protocol::FuncCall& func_call) {
    absl::MutexLock lk(&mu_);
    discarded_func_calls_.push_back(func_call);
    discarded_func_call_stat_.Tick();
}

void Server::ProcessDiscardedFuncCallIfNecessary() {
    std::vector<std::unique_ptr<ExternalFuncCallContext>> discarded_external_func_calls;
    std::vector<FuncCall> discarded_internal_func_calls;
    std::vector<ExternalFuncCallContext*> func_calls_for_dispatch;
    {
        absl::MutexLock lk(&mu_);
        for (const FuncCall& func_call : discarded_func_calls_) {
            if (func_call.client_id == 0) {
                if (running_external_func_calls_.contains(func_call.full_call_id)) {
                    discarded_external_func_calls.push_back(
                        std::move(running_external_func_calls_[func_call.full_call_id]));
                    running_external_func_calls_.erase(func_call.full_call_id);
                }
            } else {
                discarded_internal_func_calls.push_back(func_call);
            }
        }
        discarded_func_calls_.clear();
        while (!pending_external_func_calls_.empty()
                   && (max_running_external_requests_ == 0 ||
                       running_external_func_calls_.size() < max_running_external_requests_)) {
            auto func_call_context = std::move(pending_external_func_calls_.front());
            pending_external_func_calls_.pop();
            uint64_t full_call_id = func_call_context->call().full_call_id;
            func_calls_for_dispatch.push_back(func_call_context.get());
            running_external_func_calls_[full_call_id] = std::move(func_call_context);
        }
    }

    if (!discarded_external_func_calls.empty()) {
        for (auto& external_func_call : discarded_external_func_calls) {
            external_func_call->FinishWithDispatchFailure();
        }
        discarded_external_func_calls.clear();
    }

    if (!discarded_internal_func_calls.empty()) {
        char pipe_buf[PIPE_BUF];
        Message dummy_message;
        for (const FuncCall& func_call : discarded_internal_func_calls) {
            worker_lib::FuncCallFinished(
                func_call, /* success= */ false, /* output= */ std::span<const char>(),
                /* processing_time= */ 0, pipe_buf, &dummy_message);
        }
    }

    for (ExternalFuncCallContext* func_call_for_dispatch : func_calls_for_dispatch) {
        if (!DispatchExternalFuncCall(func_call_for_dispatch)) {
            FuncCall func_call = func_call_for_dispatch->call();
            HLOG(ERROR) << fmt::format("Dispatch func_call ({}) failed",
                                       FuncCallDebugString(func_call));
            absl::MutexLock lk(&mu_);
            running_external_func_calls_.erase(func_call.full_call_id);
        }
    }
}

UV_CONNECTION_CB_FOR_CLASS(Server, HttpConnection) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to open HTTP connection: " << uv_strerror(status);
        return;
    }
    HttpConnection* connection = new HttpConnection(this, next_http_connection_id_++);
    uv_tcp_t* client = reinterpret_cast<uv_tcp_t*>(malloc(sizeof(uv_tcp_t)));
    UV_DCHECK_OK(uv_tcp_init(uv_loop(), client));
    if (uv_accept(UV_AS_STREAM(&uv_http_handle_), UV_AS_STREAM(client)) == 0) {
        RegisterConnection(PickHttpWorker(),
                           std::unique_ptr<server::ConnectionBase>(connection),
                           UV_AS_STREAM(client));
        http_connections_.insert(connection);
    } else {
        LOG(ERROR) << "Failed to accept new HTTP connection";
        free(client);
        delete connection;
    }
}

UV_CONNECTION_CB_FOR_CLASS(Server, GrpcConnection) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to open gRPC connection: " << uv_strerror(status);
        return;
    }
    GrpcConnection* connection = new GrpcConnection(this, next_grpc_connection_id_++);
    uv_tcp_t* client = reinterpret_cast<uv_tcp_t*>(malloc(sizeof(uv_tcp_t)));
    UV_DCHECK_OK(uv_tcp_init(uv_loop(), client));
    if (uv_accept(UV_AS_STREAM(&uv_grpc_handle_), UV_AS_STREAM(client)) == 0) {
        RegisterConnection(PickHttpWorker(),
                           std::unique_ptr<server::ConnectionBase>(connection),
                           UV_AS_STREAM(client));
        grpc_connections_.insert(connection);
    } else {
        LOG(ERROR) << "Failed to accept new gRPC connection";
        free(client);
        delete connection;
    }
}

UV_CONNECTION_CB_FOR_CLASS(Server, MessageConnection) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to open message connection: " << uv_strerror(status);
        return;
    }
    HLOG(INFO) << "New message connection";
    MessageConnection* connection = new MessageConnection(this);
    uv_pipe_t* client = reinterpret_cast<uv_pipe_t*>(malloc(sizeof(uv_pipe_t)));
    UV_DCHECK_OK(uv_pipe_init(uv_loop(), client, 0));
    if (uv_accept(UV_AS_STREAM(&uv_ipc_handle_), UV_AS_STREAM(client)) == 0) {
        RegisterConnection(PickIpcWorker(),
                           std::unique_ptr<server::ConnectionBase>(connection),
                           UV_AS_STREAM(client));
        message_connections_.insert(connection);
    } else {
        LOG(ERROR) << "Failed to accept new message connection";
        free(client);
        delete connection;
    }
}

}  // namespace gateway
}  // namespace faas
