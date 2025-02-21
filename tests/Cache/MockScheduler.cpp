////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dan Larkin-York
/// @author Copyright 2017, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include <memory>

#include <thread>
#include <vector>

#include <boost/asio/io_service.hpp>

#include "Basics/asio_ns.h"

#include "MockScheduler.h"

using namespace arangodb::cache;

struct MockScheduler::Impl {
  explicit Impl(std::size_t threads)
      : _ioService(new asio_ns::io_context()),
        _serviceGuard(new asio_ns::io_context::work(*_ioService)) {
    for (std::size_t i = 0; i < threads; i++) {
      auto worker = std::bind(static_cast<size_t (asio_ns::io_context::*)()>(
                                  &asio_ns::io_context::run),
                              _ioService.get());
      _group.emplace_back(new std::thread(worker));
    }
  }
  ~Impl() {
    _serviceGuard.reset();
    for (auto g : _group) {
      g->join();
      delete g;
    }
    _ioService->stop();
  }
  typedef std::unique_ptr<asio_ns::io_context::work> asio_worker;
  std::unique_ptr<asio_ns::io_context> _ioService;
  std::unique_ptr<asio_ns::io_context::work> _serviceGuard;
  std::vector<std::thread*> _group;
};

MockScheduler::MockScheduler(std::size_t threads)
    : _impl(std::make_unique<Impl>(threads)) {}

MockScheduler::~MockScheduler() = default;

void MockScheduler::post(std::function<void()> fn) {
  _impl->_ioService->post(fn);
}
