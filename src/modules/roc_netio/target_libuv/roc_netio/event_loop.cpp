/*
 * Copyright (c) 2015 Roc authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_netio/event_loop.h"
#include "roc_address/socket_addr_to_str.h"
#include "roc_core/helpers.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_core/shared_ptr.h"

namespace roc {
namespace netio {

EventLoop::EventLoop(packet::PacketPool& packet_pool,
                     core::BufferPool<uint8_t>& buffer_pool,
                     core::IAllocator& allocator)
    : packet_pool_(packet_pool)
    , buffer_pool_(buffer_pool)
    , allocator_(allocator)
    , started_(false)
    , loop_initialized_(false)
    , stop_sem_initialized_(false)
    , task_sem_initialized_(false)
    , task_cond_(mutex_)
    , close_cond_(mutex_)
    , resolver_(*this, loop_) {
    if (int err = uv_loop_init(&loop_)) {
        roc_log(LogError, "event loop: uv_loop_init(): [%s] %s", uv_err_name(err),
                uv_strerror(err));
        return;
    }
    loop_initialized_ = true;

    if (int err = uv_async_init(&loop_, &stop_sem_, stop_sem_cb_)) {
        roc_log(LogError, "event loop: uv_async_init(): [%s] %s", uv_err_name(err),
                uv_strerror(err));
        return;
    }
    stop_sem_.data = this;
    stop_sem_initialized_ = true;

    if (int err = uv_async_init(&loop_, &task_sem_, task_sem_cb_)) {
        roc_log(LogError, "event loop: uv_async_init(): [%s] %s", uv_err_name(err),
                uv_strerror(err));
        return;
    }
    task_sem_.data = this;
    task_sem_initialized_ = true;

    started_ = Thread::start();
}

EventLoop::~EventLoop() {
    if (started_) {
        if (int err = uv_async_send(&stop_sem_)) {
            roc_panic("event loop: uv_async_send(): [%s] %s", uv_err_name(err),
                      uv_strerror(err));
        }
    } else {
        close_sems_();
    }

    if (loop_initialized_) {
        if (started_) {
            Thread::join();
        } else {
            // If the thread was never started we should manually run the loop to
            // wait all opened handles to be closed. Otherwise, uv_loop_close()
            // will fail with EBUSY.
            EventLoop::run(); // non-virtual call from dtor
        }

        if (int err = uv_loop_close(&loop_)) {
            roc_panic("event loop: uv_loop_close(): [%s] %s", uv_err_name(err),
                      uv_strerror(err));
        }
    }

    roc_panic_if(joinable());
    roc_panic_if(open_ports_.size());
    roc_panic_if(closing_ports_.size());
    roc_panic_if(task_sem_initialized_);
    roc_panic_if(stop_sem_initialized_);
}

bool EventLoop::valid() const {
    return started_;
}

size_t EventLoop::num_ports() const {
    core::Mutex::Lock lock(mutex_);

    return open_ports_.size();
}

EventLoop::PortHandle EventLoop::add_udp_receiver(UdpReceiverConfig& config,
                                                  packet::IWriter& writer) {
    if (!valid()) {
        roc_panic("event loop: can't use invalid loop");
    }

    Task task;
    task.func = &EventLoop::add_udp_receiver_;
    task.port_writer = &writer;
    task.receiver_config = &config;

    run_task_(task);

    if (task.state == TaskFailed) {
        if (task.port) {
            wait_port_closed_(*task.port);
        }
        return NULL;
    }

    roc_panic_if(!task.port);
    return (PortHandle)task.port.get();
}

EventLoop::PortHandle EventLoop::add_udp_sender(UdpSenderConfig& config,
                                                packet::IWriter** writer) {
    if (!valid()) {
        roc_panic("event loop: can't use invalid loop");
    }

    Task task;
    task.func = &EventLoop::add_udp_sender_;
    task.sender_config = &config;

    run_task_(task);

    if (task.state == TaskFailed) {
        if (task.port) {
            wait_port_closed_(*task.port);
        }
        return NULL;
    }

    if (writer) {
        *writer = task.port_writer;
    }

    roc_panic_if(!task.port);
    return (PortHandle)task.port.get();
}

void EventLoop::remove_port(PortHandle handle) {
    if (!valid()) {
        roc_panic("event loop: can't use invalid loop");
    }

    if (!handle) {
        roc_panic("event loop: handle is null");
    }

    Task task;
    task.func = &EventLoop::remove_port_;
    task.port = (BasicPort*)handle;

    run_task_(task);

    if (task.state == TaskFailed) {
        roc_panic("event loop: can't remove port %s: unknown port",
                  address::socket_addr_to_str(task.port->address()).c_str());
    }

    wait_port_closed_(*task.port);
}

bool EventLoop::resolve_endpoint_address(const address::EndpointURI& endpoint_uri,
                                         address::SocketAddr& resolved_address) {
    if (!valid()) {
        roc_panic("event loop: can't use invalid loop");
    }

    Task task;
    task.func = &EventLoop::resolve_endpoint_address_;
    task.resolve_req.endpoint_uri = &endpoint_uri;
    task.resolve_req.resolved_address = &resolved_address;

    run_task_(task);

    return (task.state == TaskSucceeded);
}

void EventLoop::handle_closed(BasicPort& port) {
    core::Mutex::Lock lock(mutex_);

    if (!closing_ports_.contains(port)) {
        return;
    }

    roc_log(LogDebug, "event loop: asynchronous close finished: port %s",
            address::socket_addr_to_str(port.address()).c_str());

    closing_ports_.remove(port);
    close_cond_.broadcast();
}

void EventLoop::handle_resolved(ResolverRequest& req) {
    core::Mutex::Lock lock(mutex_);

    Task& task = *ROC_CONTAINER_OF(&req, Task, resolve_req);

    task.state = (req.success ? TaskSucceeded : TaskFailed);
    task_cond_.broadcast();
}

void EventLoop::run() {
    roc_log(LogDebug, "event loop: starting event loop");

    int err = uv_run(&loop_, UV_RUN_DEFAULT);
    if (err != 0) {
        roc_log(LogInfo, "event loop: uv_run() returned non-zero");
    }

    roc_log(LogDebug, "event loop: finishing event loop");
}

void EventLoop::task_sem_cb_(uv_async_t* handle) {
    roc_panic_if_not(handle);

    EventLoop& self = *(EventLoop*)handle->data;
    self.process_tasks_();
}

void EventLoop::stop_sem_cb_(uv_async_t* handle) {
    roc_panic_if_not(handle);

    EventLoop& self = *(EventLoop*)handle->data;
    self.async_close_ports_();
    self.close_sems_();
    self.process_tasks_();
}

void EventLoop::async_close_ports_() {
    core::Mutex::Lock lock(mutex_);

    while (core::SharedPtr<BasicPort> port = open_ports_.front()) {
        open_ports_.remove(*port);
        async_close_port_(*port);
    }
}

void EventLoop::close_sems_() {
    if (task_sem_initialized_) {
        uv_close((uv_handle_t*)&task_sem_, NULL);
        task_sem_initialized_ = false;
    }

    if (stop_sem_initialized_) {
        uv_close((uv_handle_t*)&stop_sem_, NULL);
        stop_sem_initialized_ = false;
    }
}

void EventLoop::run_task_(Task& task) {
    core::Mutex::Lock lock(mutex_);

    tasks_.push_back(task);

    if (int err = uv_async_send(&task_sem_)) {
        roc_panic("event loop: uv_async_send(): [%s] %s", uv_err_name(err),
                  uv_strerror(err));
    }

    while (task.state == TaskPending) {
        task_cond_.wait();
    }
}

void EventLoop::process_tasks_() {
    core::Mutex::Lock lock(mutex_);

    bool notify = false;

    while (Task* task = tasks_.front()) {
        tasks_.remove(*task);

        task->state = (this->*(task->func))(*task);

        if (task->state != TaskPending) {
            notify = true;
        }
    }

    if (notify) {
        task_cond_.broadcast();
    }
}

EventLoop::TaskState EventLoop::add_udp_receiver_(Task& task) {
    core::SharedPtr<BasicPort> rp =
        new (allocator_) UdpReceiverPort(*task.receiver_config, *task.port_writer, *this,
                                         loop_, packet_pool_, buffer_pool_, allocator_);
    if (!rp) {
        roc_log(LogError, "event loop: can't add port %s: can't allocate receiver",
                address::socket_addr_to_str(task.receiver_config->bind_address).c_str());
        return TaskFailed;
    }

    task.port = rp;

    if (!rp->open()) {
        roc_log(LogError, "event loop: can't add port %s: can't start receiver",
                address::socket_addr_to_str(task.receiver_config->bind_address).c_str());
        async_close_port_(*rp);
        return TaskFailed;
    }

    task.receiver_config->bind_address = rp->address();

    open_ports_.push_back(*rp);
    return TaskSucceeded;
}

EventLoop::TaskState EventLoop::add_udp_sender_(Task& task) {
    core::SharedPtr<UdpSenderPort> sp =
        new (allocator_) UdpSenderPort(*task.sender_config, *this, loop_, allocator_);
    if (!sp) {
        roc_log(LogError, "event loop: can't add port %s: can't allocate sender",
                address::socket_addr_to_str(task.sender_config->bind_address).c_str());
        return TaskFailed;
    }

    task.port = sp;

    if (!sp->open()) {
        roc_log(LogError, "event loop: can't add port %s: can't start sender",
                address::socket_addr_to_str(task.sender_config->bind_address).c_str());
        async_close_port_(*sp);
        return TaskFailed;
    }

    task.port_writer = sp.get();
    task.sender_config->bind_address = sp->address();

    open_ports_.push_back(*sp);
    return TaskSucceeded;
}

EventLoop::TaskState EventLoop::remove_port_(Task& task) {
    roc_log(LogDebug, "event loop: removing port %s",
            address::socket_addr_to_str(task.port->address()).c_str());

    open_ports_.remove(*task.port);
    async_close_port_(*task.port);

    return TaskSucceeded;
}

EventLoop::TaskState EventLoop::resolve_endpoint_address_(Task& task) {
    if (!resolver_.async_resolve(task.resolve_req)) {
        return (task.resolve_req.success ? TaskSucceeded : TaskFailed);
    }

    return TaskPending;
}

void EventLoop::async_close_port_(BasicPort& port) {
    if (!port.async_close()) {
        return;
    }

    closing_ports_.push_back(port);
}

void EventLoop::wait_port_closed_(const BasicPort& port) {
    core::Mutex::Lock lock(mutex_);

    while (closing_ports_.contains(port)) {
        close_cond_.wait();
    }
}

} // namespace netio
} // namespace roc
