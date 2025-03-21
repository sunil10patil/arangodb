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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <v8.h>

#include "ApplicationFeatures/V8PlatformFeature.h"
#include "Basics/Common.h"
#include "Basics/StringBuffer.h"
#include "Basics/operating-system.h"
#include "V8/JavaScriptSecurityContext.h"

struct TRI_v8_global_t;
struct TRI_vocbase_t;

namespace arangodb {
class V8SecurityFeature;
class HttpEndpointProvider;
class EncryptionFeature;
namespace application_features {
class ApplicationServer;
class CommunicationFeaturePhase;
}  // namespace application_features
}  // namespace arangodb

/// @brief shortcut for fetching the isolate from the thread context
#define ISOLATE v8::Isolate* isolate = v8::Isolate::GetCurrent()

/// @brief macro to initiate a try-catch sequence for V8 callbacks
#define TRI_V8_TRY_CATCH_BEGIN(isolateVar) \
  auto isolateVar = args.GetIsolate();     \
  try {
/// @brief macro to terminate a try-catch sequence for V8 callbacks
#define TRI_V8_TRY_CATCH_END                                       \
  }                                                                \
  catch (arangodb::basics::Exception const& ex) {                  \
    TRI_V8_THROW_EXCEPTION_FULL(ex.code(), ex.what());             \
  }                                                                \
  catch (std::exception const& ex) {                               \
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, ex.what()); \
  }                                                                \
  catch (...) {                                                    \
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);                    \
  }

static inline v8::Local<v8::String> v8OneByteStringFactory(v8::Isolate* isolate,
                                                           void const* ptr,
                                                           int length) {
  return v8::String::NewFromOneByte(isolate, static_cast<uint8_t const*>(ptr),
                                    v8::NewStringType::kNormal, length)
      .ToLocalChecked();
}

static inline v8::Local<v8::String> v8TwoByteStringFactory(v8::Isolate* isolate,
                                                           void const* ptr,
                                                           int length) {
  return v8::String::NewFromTwoByte(isolate, static_cast<uint16_t const*>(ptr),
                                    v8::NewStringType::kNormal, length)
      .ToLocalChecked();
}

static inline v8::Local<v8::String> v8Utf8StringFactory(v8::Isolate* isolate,
                                                        void const* ptr,
                                                        int length) {
  return v8::String::NewFromUtf8(isolate, static_cast<char const*>(ptr),
                                 v8::NewStringType::kNormal, length)
      .ToLocalChecked();
}

static inline v8::Local<v8::String> v8Utf8StringFactory(v8::Isolate* isolate,
                                                        void const* ptr,
                                                        std::size_t length) {
  if (ADB_UNLIKELY(length > (unsigned int)std::numeric_limits<int>::max())) {
    throw std::overflow_error("string length out of range");
  }
  return v8Utf8StringFactory(isolate, ptr, static_cast<int>(length));
}

template<typename T, typename U = std::decay_t<T>,
         std::enable_if_t<std::is_same_v<U, std::string> ||
                              std::is_same_v<U, std::string_view> ||
                              std::is_same_v<U, char const*> ||
                              std::is_same_v<U, arangodb::basics::StringBuffer>,
                          int> = 0>
v8::Local<v8::String> v8Utf8StringFactoryT(v8::Isolate* isolate, T const&);

template<std::size_t n>
v8::Local<v8::String> v8Utf8StringFactoryT(v8::Isolate* isolate,
                                           char const (&arg)[n]) {
  // Note that "n" includes the terminating null byte
  static_assert(n > 0);
  TRI_ASSERT(arg[n - 1] == '\0');
  return v8Utf8StringFactory(isolate, arg, n - 1);
}

/// @brief shortcut for creating a v8 symbol for the specified string
#define TRI_V8_ASCII_STRING(isolate, name) \
  v8OneByteStringFactory(isolate, (name), (int)strlen(name))

#define TRI_V8_ASCII_STD_STRING(isolate, name) \
  v8OneByteStringFactory(isolate, (name).data(), (int)(name).size())

#define TRI_V8_ASCII_PAIR_STRING(isolate, name, length) \
  v8OneByteStringFactory(isolate, (name), (int)(length))

/// @brief shortcut for creating a v8 symbol for the specified string of unknown
/// length
#define TRI_V8_STRING(isolate, name) v8Utf8StringFactoryT(isolate, (name))

/// @brief shortcut for creating a v8 symbol for the specified string
#define TRI_V8_STD_STRING(isolate, name) v8Utf8StringFactoryT(isolate, (name))

/// @brief shortcut for creating a v8 symbol for the specified string of known
/// length
#define TRI_V8_PAIR_STRING(isolate, name, length) \
  v8Utf8StringFactory(isolate, (name), (int)(length))

/// @brief shortcut for creating a v8 symbol for the specified string
#define TRI_V8_STRING_UTF16(isolate, name, length) \
  v8TwoByteStringFactory(isolate, (name), (int)(length))

/// @brief shortcut for current v8 globals and scope
#define TRI_V8_CURRENT_GLOBALS_AND_SCOPE                            \
  TRI_v8_global_t* v8g = static_cast<TRI_v8_global_t*>(             \
      isolate->GetData(arangodb::V8PlatformFeature::V8_DATA_SLOT)); \
  v8::HandleScope scope(isolate);                                   \
  do {                                                              \
  } while (0)

/// @brief shortcut for throwing an exception with an error code
#define TRI_V8_SET_EXCEPTION(code)        \
  do {                                    \
    TRI_CreateErrorObject(isolate, code); \
  } while (0)

#define TRI_V8_THROW_EXCEPTION(code) \
  do {                               \
    TRI_V8_SET_EXCEPTION(code);      \
    return;                          \
  } while (0)

/// @brief shortcut for throwing an exception and returning
#define TRI_V8_SET_EXCEPTION_MESSAGE(code, message)      \
  do {                                                   \
    TRI_CreateErrorObject(isolate, code, message, true); \
  } while (0)

#define TRI_V8_THROW_EXCEPTION_MESSAGE(code, message) \
  do {                                                \
    TRI_V8_SET_EXCEPTION_MESSAGE(code, message);      \
    return;                                           \
  } while (0)

/// @brief shortcut for throwing an exception and returning
#define TRI_V8_THROW_EXCEPTION_FULL(code, message)        \
  do {                                                    \
    TRI_CreateErrorObject(isolate, code, message, false); \
    return;                                               \
  } while (0)

/// @brief shortcut for throwing a usage exception and returning
#define TRI_V8_THROW_EXCEPTION_USAGE(usage)                              \
  do {                                                                   \
    std::string msg = "usage: ";                                         \
    msg += usage;                                                        \
    TRI_CreateErrorObject(isolate, TRI_ERROR_BAD_PARAMETER, msg, false); \
    return;                                                              \
  } while (0)

/// @brief shortcut for throwing an internal exception and returning
#define TRI_V8_THROW_EXCEPTION_INTERNAL(message)                        \
  do {                                                                  \
    TRI_CreateErrorObject(isolate, TRI_ERROR_INTERNAL, message, false); \
    return;                                                             \
  } while (0)

/// @brief shortcut for throwing a parameter exception and returning
#define TRI_V8_THROW_EXCEPTION_PARAMETER(message)                            \
  do {                                                                       \
    TRI_CreateErrorObject(isolate, TRI_ERROR_BAD_PARAMETER, message, false); \
    return;                                                                  \
  } while (0)

/// @brief shortcut for throwing an out-of-memory exception and returning
#define TRI_V8_SET_EXCEPTION_MEMORY()                        \
  do {                                                       \
    TRI_CreateErrorObject(isolate, TRI_ERROR_OUT_OF_MEMORY); \
  } while (0)

#define TRI_V8_THROW_EXCEPTION_MEMORY() \
  do {                                  \
    TRI_V8_SET_EXCEPTION_MEMORY();      \
    return;                             \
  } while (0)

/// @brief shortcut for throwing an exception for an system error
#define TRI_V8_THROW_EXCEPTION_SYS(message)                  \
  do {                                                       \
    TRI_set_errno(TRI_ERROR_SYS_ERROR);                      \
    std::string msg = message;                               \
    msg += ": ";                                             \
    msg += TRI_LAST_ERROR_STR;                               \
    TRI_CreateErrorObject(isolate, TRI_errno(), msg, false); \
    return;                                                  \
  } while (0)

/// @brief shortcut for logging and forward throwing an error
#define TRI_V8_LOG_THROW_EXCEPTION(TRYCATCH) \
  do {                                       \
    TRI_LogV8Exception(isolate, &TRYCATCH);  \
    TRYCATCH.ReThrow();                      \
    return;                                  \
  } while (0)

/// @brief shortcut for throwing an error
#define TRI_V8_SET_ERROR(message)                               \
  do {                                                          \
    isolate->ThrowException(                                    \
        v8::Exception::Error(TRI_V8_STRING(isolate, message))); \
  } while (0)

#define TRI_V8_THROW_ERROR(message) \
  do {                              \
    TRI_V8_SET_ERROR(message);      \
    return;                         \
  } while (0)

/// @brief shortcut for throwing a range error
#define TRI_V8_THROW_RANGE_ERROR(message)                            \
  do {                                                               \
    isolate->ThrowException(                                         \
        v8::Exception::RangeError(TRI_V8_STRING(isolate, message))); \
    return;                                                          \
  } while (0)

/// @brief shortcut for throwing a syntax error
#define TRI_V8_THROW_SYNTAX_ERROR(message)                            \
  do {                                                                \
    isolate->ThrowException(                                          \
        v8::Exception::SyntaxError(TRI_V8_STRING(isolate, message))); \
    return;                                                           \
  } while (0)

/// @brief shortcut for throwing a type error
#define TRI_V8_SET_TYPE_ERROR(message)                              \
  do {                                                              \
    isolate->ThrowException(                                        \
        v8::Exception::TypeError(TRI_V8_STRING(isolate, message))); \
  } while (0)

#define TRI_V8_THROW_TYPE_ERROR(message) \
  do {                                   \
    TRI_V8_SET_TYPE_ERROR(message);      \
    return;                              \
  } while (0)

/// @brief Return undefined (default..)
///   implicitly requires 'args and 'isolate' to be available
#define TRI_V8_RETURN_UNDEFINED()                    \
  args.GetReturnValue().Set(v8::Undefined(isolate)); \
  return

/// @brief Return 'true'
///   implicitly requires 'args and 'isolate' to be available
#define TRI_V8_RETURN_TRUE()                    \
  args.GetReturnValue().Set(v8::True(isolate)); \
  return

/// @brief Return 'false'
///   implicitly requires 'args and 'isolate' to be available
#define TRI_V8_RETURN_FALSE()                    \
  args.GetReturnValue().Set(v8::False(isolate)); \
  return

/// @brief Return a bool
///   implicitly requires 'args and 'isolate' to be available
#define TRI_V8_RETURN_BOOL(WHAT)                   \
  if (WHAT) {                                      \
    args.GetReturnValue().Set(v8::True(isolate));  \
  } else {                                         \
    args.GetReturnValue().Set(v8::False(isolate)); \
  }                                                \
  return

/// @brief Return an integer
///   implicitly requires 'args and 'isolate' to be available
#define TRI_V8_RETURN_INTEGER(WHAT)                           \
  args.GetReturnValue().Set(v8::Integer::New(isolate, WHAT)); \
  return

/// @brief return 'null'
///   implicitly requires 'args and 'isolate' to be available
#define TRI_V8_RETURN_NULL()                    \
  args.GetReturnValue().Set(v8::Null(isolate)); \
  return

/// @brief return any sort of V8-value
///   implicitly requires 'args and 'isolate' to be available
/// @param WHAT the name of the v8::Value/v8::Number/...
#define TRI_V8_RETURN(WHAT)        \
  args.GetReturnValue().Set(WHAT); \
  return

/// @brief return a char*
///   implicitly requires 'args and 'isolate' to be available
/// @param WHAT the name of the char* variable
#define TRI_V8_RETURN_STRING(WHAT)                                       \
  args.GetReturnValue().Set(                                             \
      v8::String::NewFromUtf8(isolate, WHAT, v8::NewStringType::kNormal) \
          .FromMaybe(v8::Local<v8::String>()));                          \
  return

/// @brief return a std::string_view
///   implicitly requires 'args and 'isolate' to be available
/// @param WHAT the name of the std::string_view variable
#define TRI_V8_RETURN_STD_STRING_VIEW(WHAT)                                   \
  args.GetReturnValue().Set(                                                  \
      v8::String::NewFromUtf8(isolate, WHAT.data(),                           \
                              v8::NewStringType::kNormal, (int)WHAT.length()) \
          .FromMaybe(v8::Local<v8::String>()));                               \
  return

/// @brief return a std::string
///   implicitly requires 'args and 'isolate' to be available
/// @param WHAT the name of the std::string variable
#define TRI_V8_RETURN_STD_STRING(WHAT)                                        \
  args.GetReturnValue().Set(                                                  \
      v8::String::NewFromUtf8(isolate, WHAT.c_str(),                          \
                              v8::NewStringType::kNormal, (int)WHAT.length()) \
          .FromMaybe(v8::Local<v8::String>()));                               \
  return

/// @brief return a std::wstring
///   implicitly requires 'args and 'isolate' to be available
/// @param WHAT the name of the std::string variable
#define TRI_V8_RETURN_STD_WSTRING(WHAT)                                  \
  args.GetReturnValue().Set(                                             \
      v8::String::NewFromTwoByte(isolate, (const uint16_t*)WHAT.c_str(), \
                                 v8::NewStringType::kNormal,             \
                                 (int)WHAT.length())                     \
          .FromMaybe(v8::Local<v8::String>()));                          \
  return

#define TRI_IGETC isolate->GetCurrentContext()

#define TRI_GET_INT32(VAL) VAL->Int32Value(TRI_IGETC).FromMaybe(0)

#define TRI_GET_UINT32(VAL) VAL->Uint32Value(TRI_IGETC).FromMaybe(0)

#define TRI_GET_DOUBLE(VAL) VAL->NumberValue(TRI_IGETC).FromMaybe(0.0)

#define TRI_GET_STRING(VAL) \
  VAL->ToString(TRI_IGETC).FromMaybe(v8::Local<v8::String>())

v8::Local<v8::Object> TRI_GetObject(v8::Local<v8::Context>& context,
                                    v8::Handle<v8::Value> val);

bool TRI_HasProperty(v8::Local<v8::Context>& context, v8::Isolate* isolate,
                     v8::Local<v8::Object> obj, std::string_view key);

bool TRI_HasProperty(v8::Local<v8::Context>& context, v8::Isolate* isolate,
                     v8::Local<v8::Object> obj, v8::Local<v8::String> key);

bool TRI_HasRealNamedProperty(v8::Local<v8::Context>& context,
                              v8::Isolate* isolate, v8::Local<v8::Object> obj,
                              v8::Local<v8::String> key);

v8::Local<v8::Value> TRI_GetProperty(v8::Local<v8::Context>& context,
                                     v8::Isolate* isolate,
                                     v8::Local<v8::Object> obj,
                                     std::string_view key);

v8::Local<v8::Value> TRI_GetProperty(v8::Local<v8::Context>& context,
                                     v8::Isolate* isolate,
                                     v8::Local<v8::Object> obj,
                                     v8::Local<v8::String> key);

bool TRI_DeleteProperty(v8::Local<v8::Context>& context, v8::Isolate* isolate,
                        v8::Local<v8::Object>& obj, std::string_view key);

bool TRI_DeleteProperty(v8::Local<v8::Context>& context, v8::Isolate* isolate,
                        v8::Local<v8::Object>& obj, v8::Local<v8::Value> key);

v8::Local<v8::Object> TRI_ToObject(v8::Local<v8::Context>& context,
                                   v8::Handle<v8::Value> val);

v8::Local<v8::String> TRI_ObjectToString(v8::Local<v8::Context>& context,
                                         v8::Handle<v8::Value> val);

std::string TRI_ObjectToString(v8::Local<v8::Context>& context,
                               v8::Isolate* isolate,
                               v8::MaybeLocal<v8::Value> val);

std::string TRI_ObjectToString(v8::Local<v8::Context>& context,
                               v8::Isolate* isolate, v8::Local<v8::String> val);

/// @brief retrieve the instance of the TRI_v8_global_t of the current thread
///   implicitly creates a variable 'v8g' with a pointer to it.
///   implicitly requires 'isolate' to be available
#define TRI_GET_GLOBALS()                               \
  TRI_v8_global_t* v8g = static_cast<TRI_v8_global_t*>( \
      isolate->GetData(arangodb::V8PlatformFeature::V8_DATA_SLOT))

#define TRI_GET_SERVER_GLOBALS(server)                    \
  V8Global<server>* v8g = static_cast<V8Global<server>*>( \
      isolate->GetData(arangodb::V8PlatformFeature::V8_DATA_SLOT))

#define TRI_GET_GLOBALS2(isolate)                       \
  TRI_v8_global_t* v8g = static_cast<TRI_v8_global_t*>( \
      isolate->GetData(arangodb::V8PlatformFeature::V8_DATA_SLOT))

/// @brief fetch a string-member from the global into the local scope of the
/// function
///     will give you a variable of the same name.
///     implicitly requires 'v8g' and 'isolate' to be there.
/// @param WHICH the member string name to get as local variable
#define TRI_GET_GLOBAL_STRING(WHICH) \
  auto WHICH = v8::Local<v8::String>::New(isolate, v8g->WHICH)

/// @brief fetch a member from the global into the local scope of the function
///     will give you a variable of the same name.
///     implicitly requires 'v8g' and 'isolate' to be there.
/// @param WHICH the member name to get as local variable
/// @param TYPE  the type of the member to instantiate
#define TRI_GET_GLOBAL(WHICH, TYPE) \
  auto WHICH = v8::Local<TYPE>::New(isolate, v8g->WHICH)

namespace arangodb {
namespace transaction {
class V8Context;
}
class TransactionState;
}  // namespace arangodb

/// @brief globals stored in the isolate
struct TRI_v8_global_t {
  /// @brief wrapper around a v8::Persistent to hold a shared_ptr and cleanup
  class SharedPtrPersistent {
   public:
    // emplace persistent shared pointer
    static std::pair<SharedPtrPersistent&, bool> emplace(
        v8::Isolate& isolate, std::shared_ptr<void> const& value);

    // constructor used ONLY by SharedPtrPersistent::emplace(...)
    SharedPtrPersistent(v8::Isolate& isolate,
                        std::shared_ptr<void> const& value);
    SharedPtrPersistent(SharedPtrPersistent&&) = delete;
    SharedPtrPersistent(SharedPtrPersistent const&) = delete;
    ~SharedPtrPersistent();
    SharedPtrPersistent& operator=(SharedPtrPersistent&&) = delete;
    SharedPtrPersistent& operator=(SharedPtrPersistent const&) = delete;
    v8::Local<v8::External> get() const { return _persistent.Get(&_isolate); }

   private:
    v8::Isolate& _isolate;
    v8::Persistent<v8::External> _persistent;
    std::shared_ptr<void> _value;
  };

  template<typename Server>
  TRI_v8_global_t(Server& server, v8::Isolate* isolate, size_t id)
      : TRI_v8_global_t{
            server,
            server.template getFeature<arangodb::V8SecurityFeature>(),
            server.template getFeature<arangodb::HttpEndpointProvider>(),
            server.template getFeature<
                arangodb::application_features::CommunicationFeaturePhase>(),
#ifdef USE_ENTERPRISE
            server.template getFeature<arangodb::EncryptionFeature>(),
#endif
            isolate,
            id} {
  }

  ~TRI_v8_global_t();

  /// @brief whether or not the context has active externals
  bool hasActiveExternals() const { return _activeExternals > 0; }

  /// @brief increase the number of active externals
  void increaseActiveExternals() { ++_activeExternals; }

  /// @brief decrease the number of active externals
  void decreaseActiveExternals() { --_activeExternals; }

  /// @brief agency template
  v8::Persistent<v8::ObjectTemplate> AgencyTempl;

  /// @brief local agent template
  v8::Persistent<v8::ObjectTemplate> AgentTempl;

  /// @brief clusterinfo template
  v8::Persistent<v8::ObjectTemplate> ClusterInfoTempl;

  /// @brief server state template
  v8::Persistent<v8::ObjectTemplate> ServerStateTempl;

  /// @brief cluster comm template
  v8::Persistent<v8::ObjectTemplate> ClusterCommTempl;

  /// @brief ArangoError template
  v8::Persistent<v8::ObjectTemplate> ArangoErrorTempl;

  /// @brief collection template
  v8::Persistent<v8::ObjectTemplate> VocbaseColTempl;

  /// @brief view template
  v8::Persistent<v8::ObjectTemplate> VocbaseViewTempl;

  /// @brief replicated log template
  v8::Persistent<v8::ObjectTemplate> VocbaseReplicatedLogTempl;

  /// @brief TRI_vocbase_t template
  v8::Persistent<v8::ObjectTemplate> VocbaseTempl;

  /// @brief TRI_vocbase_t template
  v8::Persistent<v8::ObjectTemplate> EnvTempl;

  /// @brief users template
  v8::Persistent<v8::ObjectTemplate> UsersTempl;

  /// @brief general graphs module template
  v8::Persistent<v8::ObjectTemplate> GeneralGraphModuleTempl;

  /// @brief general graph class template
  v8::Persistent<v8::ObjectTemplate> GeneralGraphTempl;

#ifdef USE_ENTERPRISE
  /// @brief SmartGraph class template
  v8::Persistent<v8::ObjectTemplate> SmartGraphTempl;
  // there is no SmartGraph module because they are
  // identical, just return different graph instances.
#endif

  /// @brief Buffer template
  v8::Persistent<v8::FunctionTemplate> BufferTempl;

  /// @brief stream query cursor templace
  v8::Persistent<v8::FunctionTemplate> StreamQueryCursorTempl;

  v8::Persistent<v8::ObjectTemplate> IResearchAnalyzerInstanceTempl;
  v8::Persistent<v8::ObjectTemplate> IResearchAnalyzerManagerTempl;

  /// @brief "Buffer" constant
  v8::Persistent<v8::String> BufferConstant;

  /// @brief "DELETE" constant
  v8::Persistent<v8::String> DeleteConstant;

  /// @brief "GET" constant
  v8::Persistent<v8::String> GetConstant;

  /// @brief "HEAD" constant
  v8::Persistent<v8::String> HeadConstant;

  /// @brief "OPTIONS" constant
  v8::Persistent<v8::String> OptionsConstant;

  /// @brief "PATCH" constant
  v8::Persistent<v8::String> PatchConstant;

  /// @brief "POST" constant
  v8::Persistent<v8::String> PostConstant;

  /// @brief "PUT" constant
  v8::Persistent<v8::String> PutConstant;

  /// @brief "address" key name
  v8::Persistent<v8::String> AddressKey;

  /// @brief "allowDirtyReads" key name
  v8::Persistent<v8::String> AllowDirtyReadsKey;

  /// @brief "allowUseDatabase" key name
  v8::Persistent<v8::String> AllowUseDatabaseKey;

  /// @brief "authorized" key name
  v8::Persistent<v8::String> AuthorizedKey;

  /// @brief "bodyFromFile" key name
  v8::Persistent<v8::String> BodyFromFileKey;

  /// @brief "body" key name
  v8::Persistent<v8::String> BodyKey;

  /// @brief "client" key name
  v8::Persistent<v8::String> ClientKey;

  /// @brief "code" key name
  v8::Persistent<v8::String> CodeKey;

  /// @brief "contentType" key name
  v8::Persistent<v8::String> ContentTypeKey;

  /// @brief "cookies" key name
  v8::Persistent<v8::String> CookiesKey;

  /// @brief "coordTransactionID" key name
  v8::Persistent<v8::String> CoordTransactionIDKey;

  /// @brief "database" key name
  v8::Persistent<v8::String> DatabaseKey;

  /// @brief "domain" key
  v8::Persistent<v8::String> DomainKey;

  /// @brief "endpoint" key name
  v8::Persistent<v8::String> EndpointKey;

  /// @brief "error" key name
  v8::Persistent<v8::String> ErrorKey;

  /// @brief "errorMessage" key name
  v8::Persistent<v8::String> ErrorMessageKey;

  /// @brief "errorNum" key name
  v8::Persistent<v8::String> ErrorNumKey;

  /// @brief "original" key name
  v8::Persistent<v8::String> OriginalKey;

  /// @brief "headers" key name
  v8::Persistent<v8::String> HeadersKey;

  /// @brief "httpOnly" key
  v8::Persistent<v8::String> HttpOnlyKey;

  /// @brief "id" key name
  v8::Persistent<v8::String> IdKey;

  /// @brief "isAdminUser" key name
  v8::Persistent<v8::String> IsAdminUser;

  /// @brief "initTimeout" key name
  v8::Persistent<v8::String> InitTimeoutKey;

  /// @brief "isRestore" key name
  v8::Persistent<v8::String> IsRestoreKey;

  /// @brief "isSynchronousReplication" key name
  v8::Persistent<v8::String> IsSynchronousReplicationKey;

  /// @brief "isSystem" key name
  v8::Persistent<v8::String> IsSystemKey;

  /// @brief "keepNull" key name
  v8::Persistent<v8::String> KeepNullKey;

  /// @brief "keyOptions" key name
  v8::Persistent<v8::String> KeyOptionsKey;

  /// @brief "length" key
  v8::Persistent<v8::String> LengthKey;

  /// @brief "lifeTime" key
  v8::Persistent<v8::String> LifeTimeKey;

  /// @brief "mergeObjects" key name
  v8::Persistent<v8::String> MergeObjectsKey;

  /// @brief "name" key
  v8::Persistent<v8::String> NameKey;

  /// @brief "operationID" key
  v8::Persistent<v8::String> OperationIDKey;

  /// @brief "overwrite" key
  v8::Persistent<v8::String> OverwriteKey;

  /// @brief "overwriteMode" key
  v8::Persistent<v8::String> OverwriteModeKey;

  /// @brief "overwriteMode" key
  v8::Persistent<v8::String> SkipDocumentValidationKey;

  /// @brief "parameters" key name
  v8::Persistent<v8::String> ParametersKey;

  /// @brief "path" key name
  v8::Persistent<v8::String> PathKey;

  /// @brief "prefix" key name
  v8::Persistent<v8::String> PrefixKey;

  /// @brief "port" key name
  v8::Persistent<v8::String> PortKey;

  /// @brief "portType" key name
  v8::Persistent<v8::String> PortTypeKey;

  /// @brief "protocol" key name
  v8::Persistent<v8::String> ProtocolKey;

  /// @brief "rawRequestBody" key name
  v8::Persistent<v8::String> RawRequestBodyKey;

  /// @brief "rawSuffix" key name
  v8::Persistent<v8::String> RawSuffixKey;

  /// @brief "refillIndexCaches" key name
  v8::Persistent<v8::String> RefillIndexCachesKey;

  /// @brief "requestBody" key name
  v8::Persistent<v8::String> RequestBodyKey;

  /// @brief "requestType" key name
  v8::Persistent<v8::String> RequestTypeKey;

  /// @brief "responseCode" key name
  v8::Persistent<v8::String> ResponseCodeKey;

  /// @brief "returnNew" key name
  v8::Persistent<v8::String> ReturnNewKey;

  /// @brief "returnOld" key name
  v8::Persistent<v8::String> ReturnOldKey;

  /// @brief "secure" key
  v8::Persistent<v8::String> SecureKey;

  /// @brief "server" key
  v8::Persistent<v8::String> ServerKey;

  /// @brief "shardID" key name
  v8::Persistent<v8::String> ShardIDKey;

  /// @brief "silent" key name
  v8::Persistent<v8::String> SilentKey;

  /// @brief "singleRequest" key name
  v8::Persistent<v8::String> SingleRequestKey;

  /// @brief "status" key name
  v8::Persistent<v8::String> StatusKey;

  /// @brief "suffix" key name
  v8::Persistent<v8::String> SuffixKey;

  /// @brief "timeout" key name
  v8::Persistent<v8::String> TimeoutKey;

  /// @brief "toJson" key name
  v8::Persistent<v8::String> ToJsonKey;

  /// @brief "transformations" key name
  v8::Persistent<v8::String> TransformationsKey;

  /// @brief "url" key name
  v8::Persistent<v8::String> UrlKey;

  /// @brief "user" key name
  v8::Persistent<v8::String> UserKey;

  /// @brief "value" key
  v8::Persistent<v8::String> ValueKey;

  /// @brief "version" key
  v8::Persistent<v8::String> VersionKeyHidden;

  /// @brief "waitForSync" key name
  v8::Persistent<v8::String> WaitForSyncKey;

  /// @brief "compact" key name
  v8::Persistent<v8::String> CompactKey;

  /// @brief "_dbCache" key name
  v8::Persistent<v8::String> _DbCacheKey;

  /// @brief "_dbName" key name
  v8::Persistent<v8::String> _DbNameKey;

  /// @brief system attribute names
  v8::Persistent<v8::String> _IdKey;
  v8::Persistent<v8::String> _KeyKey;
  v8::Persistent<v8::String> _RevKey;
  v8::Persistent<v8::String> _FromKey;
  v8::Persistent<v8::String> _ToKey;

  /// @brief currently request object (might be invalid!)
  v8::Handle<v8::Value> _currentRequest;

  /// @brief currently response object (might be invalid!)
  v8::Handle<v8::Value> _currentResponse;

  /// @brief information about the currently running transaction
  arangodb::transaction::V8Context* _transactionContext;

  std::shared_ptr<arangodb::TransactionState> _transactionState;

  /// @brief current AQL expressionContext
  void* _expressionContext;

  /// @brief pointer to the vocbase (TRI_vocbase_t*)
  TRI_vocbase_t* _vocbase;

  /// @brief number of v8 externals used in the context
  int64_t _activeExternals;

  /// @brief cancel has been caught
  bool _canceled;

  /// @brief the current security context
  arangodb::JavaScriptSecurityContext _securityContext;

  /// @brief true if the arango infrastructure is garbage collecting
  bool _inForcedCollect;

  /// @brief the ID that identifies this v8 context
  size_t const _id;

  std::atomic<double> _lastMaxTime;

  std::atomic<size_t> _countOfTimes;

  std::atomic<size_t> _heapMax;

  std::atomic<size_t> _heapLow;

  arangodb::application_features::ApplicationServer& _server;

  arangodb::V8SecurityFeature& _v8security;
  arangodb::HttpEndpointProvider& _endpoints;
#ifdef USE_ENTERPRISE
  arangodb::EncryptionFeature& _encryption;
#endif
  arangodb::application_features::CommunicationFeaturePhase& _comm;

 private:
  TRI_v8_global_t(
      arangodb::application_features::ApplicationServer& server,
      arangodb::V8SecurityFeature& v8security,
      arangodb::HttpEndpointProvider& endpoints,
      arangodb::application_features::CommunicationFeaturePhase& comm,
#ifdef USE_ENTERPRISE
      arangodb::EncryptionFeature& encryption,
#endif
      v8::Isolate* isolate, size_t id);

  /// @brief shared pointer mapping for weak pointers, holds shared pointers
  /// so
  ///        they don't get deallocated while in use by V8
  /// @note used ONLY by the SharedPtrPersistent class
  std::unordered_map<void*, SharedPtrPersistent> JSSharedPtrs;
};

// Intentionally final since we don't have virtual destructor
template<typename Server>
struct V8Global final : TRI_v8_global_t {
  V8Global(Server& server, v8::Isolate* isolate, size_t id)
      : TRI_v8_global_t{server, isolate, id} {}

  Server& server() noexcept {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    auto* p = dynamic_cast<Server*>(&_server);
    TRI_ASSERT(p);
    return *p;
#else
    return static_cast<Server&>(_server);
#endif
  }
};

// Creates a global context
template<typename Server>
V8Global<Server>* CreateV8Globals(Server& server, v8::Isolate* isolate,
                                  size_t id) {
  TRI_GET_GLOBALS();

  TRI_ASSERT(v8g == nullptr);
  v8g = new V8Global<Server>(server, isolate, id);
  isolate->SetData(arangodb::V8PlatformFeature::V8_DATA_SLOT, v8g);

  return static_cast<V8Global<Server>*>(v8g);
}

/// @brief gets the global context
TRI_v8_global_t* TRI_GetV8Globals(v8::Isolate*);

/// @brief adds a method to the prototype of an object
template<typename TARGET>
void TRI_V8_AddProtoMethod(v8::Isolate* isolate, TARGET tpl,
                           v8::Handle<v8::String> name,
                           v8::FunctionCallback callback,
                           bool isHidden = false) {
  if (isHidden) {
    // hidden method
    tpl->PrototypeTemplate()->Set(
        name, v8::FunctionTemplate::New(isolate, callback), v8::DontEnum);
  } else {
    // normal method
    tpl->PrototypeTemplate()->Set(name,
                                  v8::FunctionTemplate::New(isolate, callback));
  }
}

/// @brief adds a method to an object
void TRI_AddMethodVocbase(
    v8::Isolate* isolate, v8::Handle<v8::ObjectTemplate> tpl,
    v8::Handle<v8::String> name,
    void (*func)(v8::FunctionCallbackInfo<v8::Value> const&),
    bool isHidden = false);

/// @brief adds a global function to the given context
bool TRI_AddGlobalFunctionVocbase(
    v8::Isolate* isolate, v8::Handle<v8::String> name,
    void (*func)(v8::FunctionCallbackInfo<v8::Value> const&),
    bool isHidden = false);

/// @brief adds a global function to the given context
bool TRI_AddGlobalFunctionVocbase(v8::Isolate* isolate,
                                  v8::Handle<v8::String> name,
                                  v8::Handle<v8::Function> func,
                                  bool isHidden = false);

/// @brief adds a global read-only variable to the given context
bool TRI_AddGlobalVariableVocbase(v8::Isolate* isolate,
                                  v8::Handle<v8::String> name,
                                  v8::Handle<v8::Value> value);
