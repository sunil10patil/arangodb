////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "H2CommTask.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StringBuffer.h"
#include "Basics/StringUtils.h"
#include "Basics/asio_ns.h"
#include "Basics/dtrace-wrapper.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "GeneralServer/GeneralServer.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "Logger/LogContext.h"
#include "Logger/LogMacros.h"
#include "Rest/HttpRequest.h"
#include "Rest/HttpResponse.h"
#include "Statistics/ConnectionStatistics.h"
#include "Statistics/RequestStatistics.h"

#include <cstring>

#include <llhttp.h>

// Work-around for nghttp2 non-standard definition ssize_t under windows
// https://github.com/nghttp2/nghttp2/issues/616
#if defined(_WIN32) && defined(_MSC_VER)
#define ssize_t long
#endif
#include <nghttp2/nghttp2.h>

using namespace arangodb::basics;
using std::string_view;

namespace {
constexpr std::string_view switchingProtocols(
    "HTTP/1.1 101 Switching Protocols\r\nConnection: "
    "Upgrade\r\nUpgrade: h2c\r\n\r\n");
}  // namespace

namespace arangodb {
namespace rest {

struct H2Response : public HttpResponse {
  H2Response(ResponseCode code, uint64_t mid)
      : HttpResponse(code, mid, nullptr) {}

  RequestStatistics::Item statistics;
};

template<SocketType T>
/*static*/ int H2CommTask<T>::on_begin_headers(nghttp2_session* session,
                                               const nghttp2_frame* frame,
                                               void* user_data) try {
  H2CommTask<T>* me = static_cast<H2CommTask<T>*>(user_data);

  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return HPE_OK;
  }

  int32_t const sid = frame->hd.stream_id;
  me->acquireStatistics(sid).SET_READ_START(TRI_microtime());
  auto req =
      std::make_unique<HttpRequest>(me->_connectionInfo, /*messageId*/ sid);
  me->createStream(sid, std::move(req));

  LOG_TOPIC("33598", TRACE, Logger::REQUESTS)
      << "<http2> creating new stream " << sid;

  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
/*static*/ int H2CommTask<T>::on_header(nghttp2_session* session,
                                        const nghttp2_frame* frame,
                                        const uint8_t* name, size_t namelen,
                                        const uint8_t* value, size_t valuelen,
                                        uint8_t flags, void* user_data) try {
  H2CommTask<T>* me = static_cast<H2CommTask<T>*>(user_data);
  int32_t const sid = frame->hd.stream_id;

  if (frame->hd.type != NGHTTP2_HEADERS ||
      frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return HPE_OK;
  }

  Stream* strm = me->findStream(sid);
  if (!strm) {
    return HPE_OK;
  }

  // prevent stream headers from becoming too large
  strm->headerBuffSize += namelen + valuelen;
  if (strm->headerBuffSize > 64 * 1024 * 1024) {
    return nghttp2_submit_rst_stream(me->_session, NGHTTP2_FLAG_NONE, sid,
                                     NGHTTP2_INTERNAL_ERROR);
  }

  // handle pseudo headers
  // https://http2.github.io/http2-spec/#rfc.section.8.1.2.3
  std::string_view field(reinterpret_cast<char const*>(name), namelen);
  std::string_view val(reinterpret_cast<char const*>(value), valuelen);

  if (std::string_view(":method") == field) {
    strm->request->setRequestType(GeneralRequest::translateMethod(val));
  } else if (std::string_view(":scheme") == field) {
    // simon: ignore, should contain 'http' or 'https'
  } else if (std::string_view(":path") == field) {
    strm->request->parseUrl(reinterpret_cast<char const*>(value), valuelen);
  } else if (std::string_view(":authority") == field) {
    // simon: ignore, could treat like "Host" header
  } else {  // fall through
    strm->request->setHeader(std::string(field), std::string(val));
  }

  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
/*static*/ int H2CommTask<T>::on_frame_recv(nghttp2_session* session,
                                            const nghttp2_frame* frame,
                                            void* user_data) try {
  H2CommTask<T>* me = static_cast<H2CommTask<T>*>(user_data);

  switch (frame->hd.type) {
    case NGHTTP2_DATA:  // GET or HEAD do not use DATA frames
    case NGHTTP2_HEADERS: {
      if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
        int32_t const sid = frame->hd.stream_id;
        LOG_TOPIC("c75b1", TRACE, Logger::REQUESTS)
            << "<http2> finalized request on stream " << sid << " with ptr "
            << me;

        Stream* strm = me->findStream(sid);
        if (strm) {
          me->processStream(*strm);
        }
      }
      break;
    }
  }

  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
/*static*/ int H2CommTask<T>::on_data_chunk_recv(
    nghttp2_session* session, uint8_t flags, int32_t stream_id,
    const uint8_t* data, size_t len, void* user_data) try {
  LOG_TOPIC("2823c", TRACE, Logger::REQUESTS)
      << "<http2> received data for stream " << stream_id;
  H2CommTask<T>* me = static_cast<H2CommTask<T>*>(user_data);
  Stream* strm = me->findStream(stream_id);
  if (strm) {
    strm->request->appendBody(reinterpret_cast<char const*>(data), len);
  }

  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
/*static*/ int H2CommTask<T>::on_stream_close(nghttp2_session* session,
                                              int32_t stream_id,
                                              uint32_t error_code,
                                              void* user_data) try {
  H2CommTask<T>* me = static_cast<H2CommTask<T>*>(user_data);
  auto it = me->_streams.find(stream_id);
  if (it != me->_streams.end()) {
    Stream& strm = it->second;
    if (strm.response) {
      strm.response->statistics.SET_WRITE_END();
    }
    me->_streams.erase(it);
  }

  if (error_code != NGHTTP2_NO_ERROR) {
    LOG_TOPIC("2824d", DEBUG, Logger::REQUESTS)
        << "<http2> closing stream " << stream_id << " with error '"
        << nghttp2_http2_strerror(error_code) << "' (" << error_code << ")";
  }

  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
/*static*/ int H2CommTask<T>::on_frame_send(nghttp2_session* session,
                                            const nghttp2_frame* frame,
                                            void* user_data) {
  // can be used for push promises
  return HPE_OK;
}

template<SocketType T>
/*static*/ int H2CommTask<T>::on_frame_not_send(nghttp2_session* session,
                                                const nghttp2_frame* frame,
                                                int lib_error_code,
                                                void* user_data) try {
  if (frame->hd.type != NGHTTP2_HEADERS) {
    return HPE_OK;
  }

  int32_t const sid = frame->hd.stream_id;
  LOG_TOPIC("d15e8", DEBUG, Logger::REQUESTS)
      << "sending RST on stream " << sid << " with error '"
      << nghttp2_strerror(lib_error_code) << "' (" << lib_error_code << ")";

  // Issue RST_STREAM so that stream does not hang around.
  nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, sid,
                            NGHTTP2_INTERNAL_ERROR);

  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
H2CommTask<T>::H2CommTask(GeneralServer& server, ConnectionInfo info,
                          std::unique_ptr<AsioSocket<T>> so)
    : GeneralCommTask<T>(server, std::move(info), std::move(so)) {
  this->_connectionStatistics.SET_HTTP();
  this->_generalServerFeature.countHttp2Connection();
  initNgHttp2Session();
}

template<SocketType T>
H2CommTask<T>::~H2CommTask() noexcept {
  nghttp2_session_del(_session);
  _session = nullptr;
  if (!_streams.empty()) {
    LOG_TOPIC("924cf", DEBUG, Logger::REQUESTS)
        << "<http2> got " << _streams.size() << " remaining streams";
  }
  H2Response* res = nullptr;
  while (_responses.pop(res)) {
    delete res;
  }

  LOG_TOPIC("dc6bb", DEBUG, Logger::REQUESTS)
      << "<http2> closing connection \"" << (void*)this << "\"";
}

namespace {
int on_error_callback(nghttp2_session* session, int lib_error_code,
                      const char* msg, size_t len, void*) try {
  // use INFO log level, its still hidden by default
  LOG_TOPIC("bfcd0", INFO, Logger::REQUESTS)
      << "http2 connection error: \"" << std::string_view(msg, len) << "\" ("
      << lib_error_code << ")";
  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

int on_invalid_frame_recv(nghttp2_session* session, const nghttp2_frame* frame,
                          int lib_error_code, void* user_data) try {
  LOG_TOPIC("b5de2", INFO, Logger::REQUESTS)
      << "received illegal data frame on stream " << frame->hd.stream_id
      << ": '" << nghttp2_strerror(lib_error_code) << "' (" << lib_error_code
      << ")";
  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

constexpr uint32_t window_size = (1 << 30) - 1;  // 1 GiB
void submitConnectionPreface(nghttp2_session* session) {
  std::array<nghttp2_settings_entry, 4> iv;
  // 32 streams matches the queue capacity
  iv[0] = {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
           arangodb::H2MaxConcurrentStreams};
  // typically client is just a *sink* and just process data as
  // much as possible.  Use large window size by default.
  iv[1] = {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, window_size};
  iv[2] = {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, (1 << 14)};  // 16k
  iv[3] = {NGHTTP2_SETTINGS_ENABLE_PUSH, 0};

  nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
  // increase connection window size up to window_size
  //  nghttp2_session_set_local_window_size(session, NGHTTP2_FLAG_NONE, 0, 1 <<
  //  30);
}

ssize_t data_source_read_length_callback(nghttp2_session* session,
                                         uint8_t frame_type, int32_t stream_id,
                                         int32_t session_remote_window_size,
                                         int32_t stream_remote_window_size,
                                         uint32_t remote_max_frame_size,
                                         void* user_data) {
  LOG_TOPIC("b6f34", TRACE, Logger::REQUESTS)
      << "session_remote_window_size: " << session_remote_window_size
      << ", stream_remote_window_size: " << stream_remote_window_size
      << ", remote_max_frame_size: " << remote_max_frame_size;
  return (1 << 16);  // 64kB
}
}  // namespace

/// init h2 session
template<SocketType T>
void H2CommTask<T>::initNgHttp2Session() {
  nghttp2_session_callbacks* callbacks;
  int rv = nghttp2_session_callbacks_new(&callbacks);
  if (rv != 0) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  auto cbScope =
      scopeGuard([&]() noexcept { nghttp2_session_callbacks_del(callbacks); });

  nghttp2_session_callbacks_set_on_begin_headers_callback(
      callbacks, H2CommTask<T>::on_begin_headers);
  nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                   H2CommTask<T>::on_header);
  nghttp2_session_callbacks_set_on_frame_recv_callback(
      callbacks, H2CommTask<T>::on_frame_recv);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      callbacks, H2CommTask<T>::on_data_chunk_recv);
  nghttp2_session_callbacks_set_on_stream_close_callback(
      callbacks, H2CommTask<T>::on_stream_close);
  nghttp2_session_callbacks_set_on_frame_send_callback(
      callbacks, H2CommTask<T>::on_frame_send);
  nghttp2_session_callbacks_set_on_frame_not_send_callback(
      callbacks, H2CommTask<T>::on_frame_not_send);
  nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(
      callbacks, on_invalid_frame_recv);
  nghttp2_session_callbacks_set_error_callback2(callbacks, on_error_callback);
  nghttp2_session_callbacks_set_data_source_read_length_callback(
      callbacks, data_source_read_length_callback);

  rv = nghttp2_session_server_new(&_session, callbacks, /*args*/ this);
  if (rv != 0) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }
}

template<SocketType T>
void H2CommTask<T>::upgradeHttp1(std::unique_ptr<HttpRequest> req) {
  bool found;
  std::string const& settings = req->header("http2-settings", found);
  bool const wasHead = req->requestType() == RequestType::HEAD;

  std::string decoded = StringUtils::decodeBase64(settings);
  uint8_t const* src = reinterpret_cast<uint8_t const*>(decoded.data());
  int rv =
      nghttp2_session_upgrade2(_session, src, decoded.size(), wasHead, nullptr);

  if (rv != 0) {
    // The settings_payload is badly formed.
    LOG_TOPIC("0103c", INFO, Logger::REQUESTS)
        << "error during HTTP2 upgrade: \"" << nghttp2_strerror((int)rv)
        << "\" (" << rv << ")";
    this->close();
    return;
  }

  // https://http2.github.io/http2-spec/#discover-http
  auto buffer =
      asio_ns::buffer(::switchingProtocols.data(), ::switchingProtocols.size());
  asio_ns::async_write(
      this->_protocol->socket, buffer,
      withLogContext(
          [self(this->shared_from_this()), request(std::move(req))](
              asio_ns::error_code const& ec, std::size_t nwrite) mutable {
            auto& me = static_cast<H2CommTask<T>&>(*self);
            if (ec) {
              me.close(ec);
              return;
            }

            submitConnectionPreface(me._session);

            // The HTTP/1.1 request that is sent prior to upgrade is assigned
            // a stream identifier of 1 (see Section 5.1.1).
            // Stream 1 is implicitly "half-closed" from the client toward the
            // server

            TRI_ASSERT(request->messageId() == 1);
            auto* strm = me.createStream(1, std::move(request));
            TRI_ASSERT(strm);

            // will start writing later
            me.processStream(*strm);

            // start reading
            me.asyncReadSome();
          }));
}

template<SocketType T>
void H2CommTask<T>::start() {
  LOG_TOPIC("db5ab", DEBUG, Logger::REQUESTS)
      << "<http2> opened connection \"" << (void*)this << "\"";

  asio_ns::post(this->_protocol->context.io_context,
                [self = this->shared_from_this()] {
                  auto& me = static_cast<H2CommTask<T>&>(*self);

                  // queueing the server connection preface,
                  // which always consists of a SETTINGS frame.
                  submitConnectionPreface(me._session);

                  me.doWrite();        // write out preface
                  me.asyncReadSome();  // start reading
                });
}

template<SocketType T>
bool H2CommTask<T>::readCallback(asio_ns::error_code ec) {
  if (ec) {
    this->close(ec);
    return false;  // stop read loop
  }

  size_t parsedBytes = 0;
  for (auto const& buffer : this->_protocol->buffer.data()) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer.data());

    auto rv = nghttp2_session_mem_recv(_session, data, buffer.size());
    if (rv < 0 || static_cast<size_t>(rv) != buffer.size()) {
      LOG_TOPIC("43942", INFO, Logger::REQUESTS)
          << "HTTP2 parsing error: \"" << nghttp2_strerror((int)rv) << "\" ("
          << rv << ")";
      this->close(ec);
      return false;  // stop read loop
    }

    parsedBytes += static_cast<size_t>(rv);
  }

  TRI_ASSERT(parsedBytes < std::numeric_limits<size_t>::max());
  // Remove consumed data from receive buffer.
  this->_protocol->buffer.consume(parsedBytes);

  doWrite();

  if (!this->_writing && shouldStop()) {
    this->close();
    return false;  // stop read loop
  }

  return true;  //  continue read lopp
}

template<SocketType T>
void H2CommTask<T>::setIOTimeout() {
  double secs = this->_generalServerFeature.keepAliveTimeout();
  if (secs <= 0) {
    return;
  }

  const bool wasReading = this->_reading;
  const bool wasWriting = this->_writing;
  TRI_ASSERT(wasReading || wasWriting);
  if (wasWriting) {
    secs = std::max(this->WriteTimeout, secs);
  }

  auto millis = std::chrono::milliseconds(static_cast<int64_t>(secs * 1000));
  this->_protocol->timer.expires_after(millis);  // cancels old waiters
  this->_protocol->timer.async_wait(
      withLogContext([=, this, self = CommTask::weak_from_this()](
                         asio_ns::error_code const& ec) {
        std::shared_ptr<CommTask> s;
        if (ec || !(s = self.lock())) {  // was canceled / deallocated
          return;
        }

        auto& me = static_cast<H2CommTask<T>&>(*s);

        bool idle = wasReading && me._reading && !me._writing;
        bool writeTimeout = wasWriting && me._writing;
        if (idle || writeTimeout) {
          // _numProcessing == 0 also if responses wait for writing
          if (me._numProcessing.load(std::memory_order_relaxed) == 0) {
            LOG_TOPIC("5d6f1", INFO, Logger::REQUESTS)
                << "keep alive timeout, closing stream!";
            static_cast<GeneralCommTask<T>&>(*s).close(ec);
          } else {
            setIOTimeout();
          }
        }
        // In all other cases we do nothing, since we have been posted to the
        // iocontext but the thing we should be timing out has already
        // completed.
      }));
}

#ifdef USE_DTRACE
// Moved out to avoid duplication by templates.
static void __attribute__((noinline)) DTraceH2CommTaskProcessStream(size_t th) {
  DTRACE_PROBE1(arangod, H2CommTaskProcessStream, th);
}
#else
static void DTraceH2CommTaskProcessStream(size_t) {}
#endif

template<SocketType T>
std::string H2CommTask<T>::url(HttpRequest const* req) const {
  if (req != nullptr) {
    return std::string(
               (req->databaseName().empty()
                    ? ""
                    : "/_db/" + StringUtils::urlEncode(req->databaseName()))) +
           (Logger::logRequestParameters() ? req->fullUrl()
                                           : req->requestPath());
  }
  return "";
}

template<SocketType T>
void H2CommTask<T>::processStream(Stream& stream) {
  DTraceH2CommTaskProcessStream((size_t)this);

  if (!stream.request->header("x-omit-www-authenticate").empty()) {
    stream.mustSendAuthHeader = false;
  } else {
    stream.mustSendAuthHeader = true;
  }
  std::unique_ptr<HttpRequest> req = std::move(stream.request);

  auto msgId = req->messageId();
  auto respContentType = req->contentTypeResponse();
  try {
    processRequest(stream, std::move(req));
  } catch (arangodb::basics::Exception const& ex) {
    LOG_TOPIC("a78c5", WARN, Logger::REQUESTS)
        << "request failed with error " << ex.code() << " " << ex.message();
    this->sendErrorResponse(GeneralResponse::responseCode(ex.code()),
                            respContentType, msgId, ex.code(), ex.message());
  } catch (std::exception const& ex) {
    LOG_TOPIC("d1e88", WARN, Logger::REQUESTS)
        << "request failed with error " << ex.what();
    this->sendErrorResponse(ResponseCode::SERVER_ERROR, respContentType, msgId,
                            ErrorCode(TRI_ERROR_FAILED), ex.what());
  }
}

template<SocketType T>
void H2CommTask<T>::processRequest(Stream& stream,
                                   std::unique_ptr<HttpRequest> req) {
  // ensure there is a null byte termination. RestHandlers use
  // C functions like strchr that except a C string as input
  req->appendNullTerminator();

  if (this->stopped()) {
    return;  // we have to ignore this request because the connection has
             // already been closed
  }

  // from here on we will send a response, the connection is not IDLE
  _numProcessing.fetch_add(1, std::memory_order_relaxed);
  {
    LOG_TOPIC("924ce", INFO, Logger::REQUESTS)
        << "\"h2-request-begin\",\"" << (void*)this << "\",\""
        << this->_connectionInfo.clientAddress << "\",\""
        << HttpRequest::translateMethod(req->requestType()) << "\",\""
        << url(req.get()) << "\"";

    std::string_view body = req->rawPayload();
    this->_generalServerFeature.countHttp2Request(body.size());
    if (Logger::isEnabled(LogLevel::TRACE, Logger::REQUESTS) &&
        Logger::logRequestParameters()) {
      // Log HTTP headers:
      this->logRequestHeaders("h2", req->headers());

      if (!body.empty()) {
        this->logRequestBody("h2", req->contentType(), body);
      }
    }
  }

  // store origin header for later use
  stream.origin = req->header(StaticStrings::Origin);
  auto messageId = req->messageId();
  RequestStatistics::Item const& stat = this->statistics(messageId);
  stat.SET_REQUEST_TYPE(req->requestType());
  stat.ADD_RECEIVED_BYTES(stream.headerBuffSize + req->body().size());
  stat.SET_READ_END();
  stat.SET_WRITE_START();

  // OPTIONS requests currently go unauthenticated
  if (req->requestType() == rest::RequestType::OPTIONS) {
    this->processCorsOptions(std::move(req), stream.origin);
    return;
  }

  ServerState::Mode mode = ServerState::mode();

  // scrape the auth headers to determine and authenticate the user
  auto authToken = this->checkAuthHeader(*req, mode);

  // We want to separate superuser token traffic:
  if (req->authenticated() && req->user().empty()) {
    stat.SET_SUPERUSER();
  }

  // first check whether we allow the request to continue
  CommTask::Flow cont = this->prepareExecution(authToken, *req, mode);
  if (cont != CommTask::Flow::Continue) {
    return;  // prepareExecution sends the error message
  }

  // unzip / deflate
  if (!this->handleContentEncoding(*req)) {
    this->sendErrorResponse(rest::ResponseCode::BAD, req->contentTypeResponse(),
                            1, TRI_ERROR_BAD_PARAMETER, "decoding error");
    return;
  }

  // create a handler and execute
  auto resp =
      std::make_unique<H2Response>(rest::ResponseCode::SERVER_ERROR, messageId);
  resp->setContentType(req->contentTypeResponse());
  this->executeRequest(std::move(req), std::move(resp), mode);
}

namespace {
bool expectResponseBody(int status_code) {
  return status_code == 101 ||
         (status_code / 100 != 1 && status_code != 304 && status_code != 204);
}
}  // namespace

#ifdef USE_DTRACE
// Moved out to avoid duplication by templates.
static void __attribute__((noinline)) DTraceH2CommTaskSendResponse(size_t th) {
  DTRACE_PROBE1(arangod, H2CommTaskSendResponse, th);
}
#else
static void DTraceH2CommTaskSendResponse(size_t) {}
#endif

template<SocketType T>
void H2CommTask<T>::sendResponse(std::unique_ptr<GeneralResponse> res,
                                 RequestStatistics::Item stat) {
  DTraceH2CommTaskSendResponse((size_t)this);

  unsigned n = _numProcessing.fetch_sub(1, std::memory_order_relaxed);
  TRI_ASSERT(n > 0);

  if (this->stopped()) {
    return;
  }

  auto* tmp = static_cast<H2Response*>(res.get());

  // handle response code 204 No Content
  if (tmp->responseCode() == rest::ResponseCode::NO_CONTENT) {
    tmp->clearBody();
  }

  if (Logger::isEnabled(LogLevel::TRACE, Logger::REQUESTS) &&
      Logger::logRequestParameters()) {
    auto const& bodyBuf = tmp->body();
    std::string_view body{bodyBuf.data(), bodyBuf.size()};
    if (!body.empty()) {
      this->logRequestBody("h2", res->contentType(), body,
                           true /* isResponse */);
    }
  }

  // and give some request information
  LOG_TOPIC("924cc", DEBUG, Logger::REQUESTS)
      << "\"h2-request-end\",\"" << (void*)this << "\",\""
      << this->_connectionInfo.clientAddress
      << "\",\""
      //      <<
      //      GeneralRequest::translateMethod(::llhttpToRequestType(&_parser))
      << url(nullptr) << "\",\"" << static_cast<int>(res->responseCode())
      << "\"," << Logger::FIXED(stat.ELAPSED_SINCE_READ_START(), 6) << ","
      << Logger::FIXED(stat.ELAPSED_WHILE_QUEUED(), 6);

  tmp->statistics = std::move(stat);

  // this uses a fixed capacity queue, push might fail (unlikely, we limit max
  // streams)
  unsigned retries = 512;
  try {
    while (ADB_UNLIKELY(!_responses.push(tmp) && --retries > 0)) {
      std::this_thread::yield();
    }
  } catch (...) {
    retries = 0;
  }
  if (--retries == 0) {
    LOG_TOPIC("924dc", WARN, Logger::REQUESTS)
        << "was not able to queue response this=" << (void*)this;
    // we are overloaded close stream
    asio_ns::post(this->_protocol->context.io_context,
                  [self(this->shared_from_this()), mid(res->messageId())] {
                    auto& me = static_cast<H2CommTask<T>&>(*self);
                    nghttp2_submit_rst_stream(me._session, NGHTTP2_FLAG_NONE,
                                              static_cast<int32_t>(mid),
                                              NGHTTP2_ENHANCE_YOUR_CALM);
                  });
    return;
  }
  res.release();

  // avoid using asio_ns::post if possible
  bool signaled = _signaledWrite.load();
  if (!signaled && !_signaledWrite.exchange(true)) {
    asio_ns::post(this->_protocol->context.io_context,
                  [self = this->shared_from_this()] {
                    auto& me = static_cast<H2CommTask<T>&>(*self);
                    me._signaledWrite.store(false);
                    me.doWrite();
                  });
  }
}

// queue the response onto the session, call only on IO thread
template<SocketType T>
void H2CommTask<T>::queueHttp2Responses() {
  H2Response* response = nullptr;
  while (_responses.pop(response)) {
    std::unique_ptr<H2Response> guard(response);

    const int32_t streamId = static_cast<int32_t>(response->messageId());
    Stream* strm = findStream(streamId);
    if (strm == nullptr) {  // stream was already closed for some reason
      LOG_TOPIC("e2773", DEBUG, Logger::REQUESTS)
          << "response with message id '" << streamId
          << "' has no H2 stream on server";
      return;
    }
    strm->response = std::move(guard);
    auto& res = *response;

    // will add CORS headers if necessary
    this->finishExecution(res, strm->origin);

    if (Logger::isEnabled(LogLevel::TRACE, Logger::REQUESTS) &&
        Logger::logRequestParameters()) {
      this->logResponseHeaders("h2", res.headers());
    }

    // we need a continuous block of memory for headers
    std::vector<nghttp2_nv> nva;
    nva.reserve(4 + res.headers().size());

    std::string status = std::to_string(static_cast<int>(res.responseCode()));
    nva.push_back({(uint8_t*)":status", (uint8_t*)status.data(), 7,
                   status.size(), NGHTTP2_NV_FLAG_NO_COPY_NAME});

    // if we return HTTP 401, we need to send a www-authenticate header back
    // with the response. in this case we need to check if the header was
    // already set or if we need to set it ourselves. note that clients can
    // suppress sending the www-authenticate header by sending us an
    // x-omit-www-authenticate header.
    bool needWwwAuthenticate =
        (this->_auth->isActive() &&
         res.responseCode() == rest::ResponseCode::UNAUTHORIZED &&
         strm->mustSendAuthHeader);

    bool seenServerHeader = false;
    for (auto const& it : res.headers()) {
      std::string const& key = it.first;
      std::string const& val = it.second;

      // ignore content-length
      if (key == StaticStrings::ContentLength ||
          key == StaticStrings::Connection ||
          key == StaticStrings::TransferEncoding) {
        continue;
      }

      if (key == StaticStrings::Server) {
        seenServerHeader = true;
      } else if (needWwwAuthenticate && key == StaticStrings::WwwAuthenticate) {
        needWwwAuthenticate = false;
      }

      nva.push_back(
          {(uint8_t*)key.data(), (uint8_t*)val.data(), key.size(), val.size(),
           NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});
    }

    // add "Server" response header
    if (!seenServerHeader) {
      nva.push_back(
          {(uint8_t*)"server", (uint8_t*)"ArangoDB", 6, 8,
           NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});
    }

    if (needWwwAuthenticate) {
      TRI_ASSERT(res.responseCode() == rest::ResponseCode::UNAUTHORIZED);
      nva.push_back(
          {(uint8_t*)"www-authenticate", (uint8_t*)"Basic, realm=\"ArangoDB\"",
           16, 23,
           NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});

      nva.push_back(
          {(uint8_t*)"www-authenticate",
           (uint8_t*)"Bearer, token_type=\"JWT\", realm=\"ArangoDB\"", 16, 42,
           NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});
    }

    for (std::string const& cookie : res.cookies()) {
      nva.push_back(
          {(uint8_t*)"set-cookie", (uint8_t*)cookie.data(), 10, cookie.size(),
           NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE});
    }

    std::string type;
    if (res.contentType() != ContentType::CUSTOM) {
      type = rest::contentTypeToString(res.contentType());
      nva.push_back({(uint8_t*)"content-type", (uint8_t*)type.c_str(), 12,
                     type.length(), NGHTTP2_NV_FLAG_NO_COPY_NAME});
    }

    std::string len;
    nghttp2_data_provider *prd_ptr = nullptr, prd;
    if (!res.generateBody() ||
        expectResponseBody(static_cast<int>(res.responseCode()))) {
      len = std::to_string(res.bodySize());
      nva.push_back({(uint8_t*)"content-length", (uint8_t*)len.c_str(), 14,
                     len.size(), NGHTTP2_NV_FLAG_NO_COPY_NAME});
    }

    if ((res.bodySize() > 0) && res.generateBody() &&
        expectResponseBody(static_cast<int>(res.responseCode()))) {
      prd.source.ptr = strm;
      prd.read_callback = [](nghttp2_session* session, int32_t stream_id,
                             uint8_t* buf, size_t length, uint32_t* data_flags,
                             nghttp2_data_source* source,
                             void* user_data) -> ssize_t {
        auto strm = static_cast<H2CommTask<T>::Stream*>(source->ptr);

        basics::StringBuffer& body = strm->response->body();

        // TODO do not copy the body if it is > 16kb
        TRI_ASSERT(body.size() > strm->responseOffset);
        auto nread = std::min(length, body.size() - strm->responseOffset);

        char const* src = body.data() + strm->responseOffset;
        std::copy_n(src, nread, buf);
        strm->responseOffset += nread;

        if (strm->responseOffset == body.size()) {
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }

        // simon: might be needed if NGHTTP2_DATA_FLAG_NO_COPY is used
        //      if (nghttp2_session_get_stream_remote_close(session, stream_id)
        //      == 0) {
        //          nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
        //          stream_id, NGHTTP2_NO_ERROR);
        //      }
        return static_cast<ssize_t>(nread);
      };
      prd_ptr = &prd;
    }

    res.statistics.ADD_SENT_BYTES(res.bodySize());

    int rv = nghttp2_submit_response(this->_session, streamId, nva.data(),
                                     nva.size(), prd_ptr);
    if (rv != 0) {
      LOG_TOPIC("3d794", INFO, arangodb::Logger::REQUESTS)
          << "HTTP2 submit_response error: \"" << nghttp2_strerror((int)rv)
          << "\" (" << rv << ")";
      this->close();
      return;
    }
  }
}

#ifdef USE_DTRACE
// Moved out to avoid duplication by templates.
static void __attribute__((noinline))
DTraceH2CommTaskBeforeAsyncWrite(size_t th) {
  DTRACE_PROBE1(arangod, H2CommTaskBeforeAsyncWrite, th);
}
static void __attribute__((noinline))
DTraceH2CommTaskAfterAsyncWrite(size_t th) {
  DTRACE_PROBE1(arangod, H2CommTaskAfterAsyncWrite, th);
}
#else
static void DTraceH2CommTaskBeforeAsyncWrite(size_t) {}
static void DTraceH2CommTaskAfterAsyncWrite(size_t) {}
#endif

// called on IO context thread
template<SocketType T>
void H2CommTask<T>::doWrite() {
  if (this->_writing) {
    return;
  }
  this->_writing = true;

  queueHttp2Responses();

  static constexpr size_t kMaxOutBufferLen = 128 * 1024;
  _outbuffer.resetTo(0);
  _outbuffer.reserve(16 * 1024);
  TRI_ASSERT(_outbuffer.size() == 0);

  std::array<asio_ns::const_buffer, 2> outBuffers;
  while (true) {
    const uint8_t* data;
    auto rv = nghttp2_session_mem_send(_session, &data);
    if (rv < 0) {  // error
      this->_writing = false;
      LOG_TOPIC("2b6c4", INFO, arangodb::Logger::REQUESTS)
          << "HTTP2 framing error: \"" << nghttp2_strerror((int)rv) << "\" ("
          << rv << ")";
      this->close();
      return;
    }

    if (rv == 0) {  // done
      break;
    }

    const size_t nread = static_cast<size_t>(rv);
    // if the data is long we just pass it to async_write
    if (_outbuffer.size() + nread > kMaxOutBufferLen) {
      outBuffers[1] = asio_ns::buffer(data, nread);
      break;
    }

    _outbuffer.append(data, nread);
  }
  outBuffers[0] = asio_ns::buffer(_outbuffer.data(), _outbuffer.size());

  if (asio_ns::buffer_size(outBuffers) == 0) {
    this->_writing = false;
    if (shouldStop()) {
      this->close();
    }
    return;
  }

  // Reset read timer here, because normally client is sending
  // something, it does not expect timeout while doing it.
  setIOTimeout();

  DTraceH2CommTaskBeforeAsyncWrite((size_t)this);
  asio_ns::async_write(
      this->_protocol->socket, outBuffers,
      withLogContext([self = this->shared_from_this()](
                         const asio_ns::error_code& ec, std::size_t nwrite) {
        auto& me = static_cast<H2CommTask<T>&>(*self);
        me._writing = false;
        if (ec) {
          me.close(ec);
          return;
        }

        DTraceH2CommTaskAfterAsyncWrite((size_t)self.get());

        me.doWrite();
      }));
}

template<SocketType T>
std::unique_ptr<GeneralResponse> H2CommTask<T>::createResponse(
    rest::ResponseCode responseCode, uint64_t mid) {
  return std::make_unique<H2Response>(responseCode, mid);
}

template<SocketType T>
typename H2CommTask<T>::Stream* H2CommTask<T>::createStream(
    int32_t sid, std::unique_ptr<HttpRequest> req) {
  TRI_ASSERT(static_cast<uint64_t>(sid) == req->messageId());
  auto [it, inserted] = _streams.emplace(sid, Stream{std::move(req)});
  TRI_ASSERT(inserted == true);
  return &it->second;
}

template<SocketType T>
typename H2CommTask<T>::Stream* H2CommTask<T>::findStream(int32_t sid) {
  auto const& it = _streams.find(sid);
  if (it != _streams.end()) {
    return &it->second;
  }
  return nullptr;
}

/// should close connection
template<SocketType T>
bool H2CommTask<T>::shouldStop() const {
  return !nghttp2_session_want_read(_session) &&
         !nghttp2_session_want_write(_session);
}

template class arangodb::rest::H2CommTask<SocketType::Tcp>;
template class arangodb::rest::H2CommTask<SocketType::Ssl>;
#ifndef _WIN32
template class arangodb::rest::H2CommTask<SocketType::Unix>;
#endif

}  // namespace rest
}  // namespace arangodb
