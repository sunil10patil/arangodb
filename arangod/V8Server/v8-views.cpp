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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "v8-views.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/Utf8Helper.h"
#include "Basics/conversions.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "Logger/Logger.h"
#include "Logger/LogMacros.h"
#include "RestServer/DatabaseFeature.h"
#include "Transaction/V8Context.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "Utilities/NameValidator.h"
#include "V8/v8-conv.h"
#include "V8/v8-globals.h"
#include "V8/v8-utils.h"
#include "V8/v8-vpack.h"
#include "V8Server/v8-externals.h"
#include "V8Server/v8-vocbaseprivate.h"
#include "VocBase/LogicalView.h"
#include "VocBase/vocbase.h"

#include <velocypack/Collection.h>

namespace {

using namespace arangodb;

////////////////////////////////////////////////////////////////////////////////
/// @return the specified vocbase is granted 'level' access
////////////////////////////////////////////////////////////////////////////////
bool canUse(auth::Level level, TRI_vocbase_t const& vocbase) {
  return ExecContext::current().canUseDatabase(vocbase.name(), level);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief retrieves a view from a V8 argument
////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<LogicalView> GetViewFromArgument(
    v8::Isolate* isolate, TRI_vocbase_t& vocbase,
    v8::Handle<v8::Value> const val) {
  CollectionNameResolver resolver(vocbase);

  return (val->IsNumber() || val->IsNumberObject())
             ? resolver.getView(
                   DataSourceId{TRI_ObjectToUInt64(isolate, val, true)})
             : resolver.getView(TRI_ObjectToString(isolate, val));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief unwraps a LogicalView wrapped via WrapView(...)
/// @return collection or nullptr on failure
////////////////////////////////////////////////////////////////////////////////
LogicalView* UnwrapView(v8::Isolate* isolate,
                        v8::Local<v8::Object> const& holder) {
  return TRI_UnwrapClass<LogicalView>(holder, WRP_VOCBASE_VIEW_TYPE, TRI_IGETC);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief wraps a LogicalView
////////////////////////////////////////////////////////////////////////////////
v8::Handle<v8::Object> WrapView(v8::Isolate* isolate,
                                std::shared_ptr<LogicalView> const& view) {
  v8::EscapableHandleScope scope(isolate);
  TRI_GET_GLOBALS();
  TRI_GET_GLOBAL(VocbaseViewTempl, v8::ObjectTemplate);
  auto context = TRI_IGETC;
  v8::Handle<v8::Object> result =
      VocbaseViewTempl->NewInstance(TRI_IGETC).FromMaybe(
          v8::Local<v8::Object>());

  if (result.IsEmpty()) {
    return scope.Escape<v8::Object>(result);
  }

  LOG_TOPIC("44ea4", TRACE, arangodb::Logger::V8)
      << "Wrapping View " << view->name() << " with ptr " << (void*)view.get()
      << " to context ID " << v8g->_id;

  auto value = std::shared_ptr<void>(  // persistent value
      view.get(),                      // value
      [view](void*) -> void {  // ensure view shared_ptr is not deallocated
        TRI_ASSERT(!view->vocbase().isDangling());
        view->vocbase()
            .release();  // decrease the reference-counter for the database
      });
  auto itr = TRI_v8_global_t::SharedPtrPersistent::emplace(*isolate, value);
  auto& entry = itr.first;

  TRI_ASSERT(!view->vocbase().isDangling());
  view->vocbase()
      .forceUse();  // increase the reference-counter for the database (will be
                    // decremented by 'value' distructor above, valid for both
                    // new and existing mappings)

  result->SetInternalField(  // required for TRI_UnwrapClass(...)
      SLOT_CLASS_TYPE, v8::Integer::New(isolate, WRP_VOCBASE_VIEW_TYPE)  // args
  );
  result->SetInternalField(SLOT_CLASS, entry.get());

  TRI_GET_GLOBAL_STRING(_IdKey);
  TRI_GET_GLOBAL_STRING(_DbNameKey);
  result
      ->DefineOwnProperty(
          TRI_IGETC, _IdKey,
          TRI_V8UInt64String<DataSourceId::BaseType>(isolate, view->id().id()),
          v8::ReadOnly)
      .FromMaybe(false);  // Ignore result...
  result
      ->Set(context, _DbNameKey,
            TRI_V8_STD_STRING(isolate, view->vocbase().name()))
      .FromMaybe(false);

  return scope.Escape<v8::Object>(result);
}

}  // namespace

using namespace arangodb;
using namespace arangodb::basics;

static void JS_CreateViewVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDangling()) {
    events::CreateView(vocbase.name(), "", TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  // we require exactly 3 arguments
  if (args.Length() != 3) {
    events::CreateView(vocbase.name(), "", TRI_ERROR_FORBIDDEN);
    TRI_V8_THROW_EXCEPTION_USAGE("_createView(<name>, <type>, <properties>)");
  }

  PREVENT_EMBEDDED_TRANSACTION();

  // extract the name
  std::string const name = TRI_ObjectToString(isolate, args[0]);

  // extract the type
  std::string const type = TRI_ObjectToString(isolate, args[1]);

  if (!args[2]->IsObject()) {
    events::CreateView(vocbase.name(), name, TRI_ERROR_BAD_PARAMETER);
    TRI_V8_THROW_TYPE_ERROR("<properties> must be an object");
  }

  v8::Handle<v8::Object> obj =
      args[2]->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Object>());
  VPackBuilder properties;
  try {
    TRI_V8ToVPack(isolate, properties, obj, false);
  } catch (basics::Exception const& ex) {
    events::CreateView(vocbase.name(), name, ex.code());
    throw;
  }

  bool extendedNames =
      vocbase.server().getFeature<DatabaseFeature>().extendedNames();
  if (auto res = ViewNameValidator::validateName(
          /*allowSystem*/ false, extendedNames, name);
      res.fail()) {
    events::CreateView(vocbase.name(), name, res.errorNumber());
    TRI_V8_THROW_EXCEPTION(res);
    return;
  }

  // ...........................................................................
  // end of parameter parsing
  // ...........................................................................

  if (!canUse(auth::Level::RW, vocbase)) {
    events::CreateView(vocbase.name(), name, TRI_ERROR_FORBIDDEN);
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "insufficient rights to create view");
  }

  velocypack::Builder header;

  header.openObject();
  header.add(arangodb::StaticStrings::DataSourceName, VPackValue(name));
  header.add(arangodb::StaticStrings::DataSourceType, VPackValue(type));
  header.close();

  // in velocypack::Collections::merge(...) values from rhs take precedence
  auto builder = velocypack::Collection::merge(
      properties.slice(), header.slice(),
      /*mergeObjects*/ true, /*nullMeansRemove*/ false);

  try {
    // First refresh our analyzers cache to see all latest changes in analyzers
    TRI_GET_SERVER_GLOBALS(ArangodServer);
    auto res = v8g->server()
                   .getFeature<arangodb::iresearch::IResearchAnalyzerFeature>()
                   .loadAvailableAnalyzers(vocbase.name());

    if (res.fail()) {
      TRI_V8_THROW_EXCEPTION(res);
    }

    LogicalView::ptr view;
    res = LogicalView::create(view, vocbase, builder.slice(), true);

    if (res.ok() && !view) {
      res.reset(TRI_ERROR_INTERNAL, "problem creating view");
    }

    if (!res.ok()) {
      TRI_V8_THROW_EXCEPTION(res);
    }

    TRI_ASSERT(view);

    v8::Handle<v8::Value> result = WrapView(isolate, view);

    if (result.IsEmpty()) {
      TRI_V8_THROW_EXCEPTION_MEMORY();
    }

    TRI_V8_RETURN(result);
  } catch (basics::Exception const& ex) {
    events::CreateView(vocbase.name(), name, ex.code());
    TRI_V8_THROW_EXCEPTION_MESSAGE(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    events::CreateView(vocbase.name(), name, TRI_ERROR_INTERNAL);
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    events::CreateView(vocbase.name(), name, TRI_ERROR_INTERNAL);
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "cannot create view");
  }
  TRI_V8_TRY_CATCH_END
}

static void JS_DropViewVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDangling()) {
    events::DropView(vocbase.name(), "", TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  // we require exactly 1 string argument and an optional boolean argument
  if (args.Length() < 1 || args.Length() > 2) {
    events::DropView(vocbase.name(), "", TRI_ERROR_BAD_PARAMETER);
    TRI_V8_THROW_EXCEPTION_USAGE("_dropView(<name> [, allowDropSystem])");
  }

  PREVENT_EMBEDDED_TRANSACTION();

  bool allowDropSystem = false;

  if (args.Length() > 1) {
    // options
    if (args[1]->IsObject()) {
      TRI_GET_GLOBALS();
      v8::Handle<v8::Object> optionsObject = args[1].As<v8::Object>();
      TRI_GET_GLOBAL_STRING(IsSystemKey);

      if (TRI_HasProperty(context, isolate, optionsObject, IsSystemKey)) {
        allowDropSystem = TRI_ObjectToBoolean(
            isolate, optionsObject->Get(context, IsSystemKey)
                         .FromMaybe(v8::Local<v8::Value>()));
      }
    } else {
      allowDropSystem = TRI_ObjectToBoolean(isolate, args[1]);
    }
  }

  // extract the name
  std::string const name = TRI_ObjectToString(isolate, args[0]);

  bool extendedNames =
      vocbase.server().getFeature<DatabaseFeature>().extendedNames();
  if (auto res = ViewNameValidator::validateName(
          /*allowSystem*/ false, extendedNames, name);
      res.fail()) {
    events::DropView(vocbase.name(), name, res.errorNumber());
    TRI_V8_THROW_EXCEPTION(res);
    return;
  }

  // ...........................................................................
  // end of parameter parsing
  // ...........................................................................

  auto view = CollectionNameResolver(vocbase).getView(name);

  if (view) {
    if (!view->canUse(auth::Level::RW)) {  // check auth after ensuring that the
                                           // view exists
      events::DropView(vocbase.name(), view->name(), TRI_ERROR_FORBIDDEN);
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                     "insufficient rights to drop view");
    }

    // prevent dropping of system views
    if (!allowDropSystem && view->system()) {
      events::DropView(vocbase.name(), view->name(), TRI_ERROR_FORBIDDEN);
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                     "insufficient rights to drop system view");
    }

    auto res = view->drop();

    if (!res.ok()) {
      TRI_V8_THROW_EXCEPTION(res);
    }
  } else {
    events::DropView(vocbase.name(), name,
                     TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

/// @brief drops a view
static void JS_DropViewVocbaseObj(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);
  auto* view = UnwrapView(isolate, args.Holder());

  if (!view) {
    events::DropView(vocbase.name(), "", TRI_ERROR_BAD_PARAMETER);
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract view");
  }

  PREVENT_EMBEDDED_TRANSACTION();

  bool allowDropSystem = false;

  if (args.Length() > 0) {
    // options
    if (args[0]->IsObject()) {
      TRI_GET_GLOBALS();
      v8::Handle<v8::Object> optionsObject = args[0].As<v8::Object>();
      TRI_GET_GLOBAL_STRING(IsSystemKey);

      if (TRI_HasProperty(context, isolate, optionsObject, IsSystemKey)) {
        allowDropSystem = TRI_ObjectToBoolean(
            isolate, optionsObject->Get(TRI_IGETC, IsSystemKey)
                         .FromMaybe(v8::Local<v8::Value>()));
      }
    } else {
      allowDropSystem = TRI_ObjectToBoolean(isolate, args[0]);
    }
  }

  // ...........................................................................
  // end of parameter parsing
  // ...........................................................................

  if (!view->canUse(
          auth::Level::RW)) {  // check auth after ensuring that the view exists
    events::DropView(vocbase.name(), view->name(), TRI_ERROR_FORBIDDEN);
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "insufficient rights to drop view");
  }

  // prevent dropping of system views
  if (!allowDropSystem && view->system()) {
    events::DropView(vocbase.name(), view->name(), TRI_ERROR_FORBIDDEN);
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "insufficient rights to drop system view");
  }

  auto res = view->drop();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

static void JS_ViewVocbase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDropped()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  // expecting one argument
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("_view(<name>|<identifier>)");
  }

  v8::Handle<v8::Value> val = args[0];
  auto view = GetViewFromArgument(isolate, vocbase, val);

  if (view == nullptr) {
    TRI_V8_RETURN_NULL();
  }

  // ...........................................................................
  // end of parameter parsing
  // ...........................................................................

  if (!view->canUse(
          auth::Level::RO)) {  // check auth after ensuring that the view exists
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "insufficient rights to get view");
  }

  // skip views for which the full view definition cannot be generated, as per
  // https://github.com/arangodb/backlog/issues/459
  try {
    velocypack::Builder viewBuilder;

    viewBuilder.openObject();

    auto const res = view->properties(
        viewBuilder, LogicalDataSource::Serialization::Properties);

    if (!res.ok()) {
      TRI_V8_THROW_EXCEPTION(res);  // skip view
    }
  } catch (...) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);  // skip view
  }

  v8::Handle<v8::Value> result = WrapView(isolate, view);

  if (result.IsEmpty()) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

/// @brief return a list of all views
static void JS_ViewsVocbase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDropped()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  // ...........................................................................
  // end of parameter parsing
  // ...........................................................................

  if (!canUse(auth::Level::RO, vocbase)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "insufficient rights to get views");
  }

  std::vector<LogicalView::ptr> views;

  LogicalView::enumerate(vocbase,
                         [&views](LogicalView::ptr const& view) -> bool {
                           views.emplace_back(view);

                           return true;
                         });

  bool error = false;
  // already create an array of the correct size
  v8::Handle<v8::Array> result = v8::Array::New(isolate);

  uint32_t entry = 0;
  size_t const n = views.size();

  for (size_t i = 0; i < n; ++i) {
    auto view = views[i];

    if (!view || !view->canUse(auth::Level::RO)) {  // check auth after ensuring
                                                    // that the view exists
      continue;  // skip views that are not authorized to be read
    }

    // skip views for which the full view definition cannot be generated, as per
    // https://github.com/arangodb/backlog/issues/459
    try {
      velocypack::Builder viewBuilder;

      viewBuilder.openObject();

      if (!view->properties(viewBuilder,
                            LogicalDataSource::Serialization::Properties)
               .ok()) {
        continue;  // skip view
      }
    } catch (...) {
      continue;  // skip view
    }

    v8::Handle<v8::Value> c = WrapView(isolate, view);

    if (c.IsEmpty()) {
      error = true;
      break;
    }

    result->Set(context, entry++, c).FromMaybe(false);
  }

  if (error) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

/// @brief returns the name of a view
static void JS_NameViewVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* view = UnwrapView(isolate, args.Holder());

  if (!view) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract view");
  }

  // ...........................................................................
  // end of parameter parsing
  // ...........................................................................

  if (!view->canUse(
          auth::Level::RO)) {  // check auth after ensuring that the view exists
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "insufficient rights to get view");
  }

  std::string const name(view->name());

  if (name.empty()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  v8::Handle<v8::Value> result = TRI_V8_STD_STRING(isolate, name);
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

/// @brief returns the properties of a view
static void JS_PropertiesViewVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto* viewPtr = UnwrapView(isolate, args.Holder());

  if (!viewPtr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract view");
  }

  // In the cluster the view object might contain outdated properties,
  // which will break tests. We need an extra lookup for each operation.
  CollectionNameResolver resolver(viewPtr->vocbase());

  // check if we want to change some parameters
  if (args.Length() > 0 && args[0]->IsObject()) {
    velocypack::Builder builder;
    TRI_V8ToVPack(isolate, builder, args[0], false);

    bool partialUpdate = true;  // partial update by default

    if (args.Length() > 1) {
      if (!args[1]->IsBoolean()) {
        TRI_V8_THROW_EXCEPTION_PARAMETER("<partialUpdate> must be a boolean");
      }

      partialUpdate = TRI_ObjectToBoolean(isolate, args[1]);
    }

    // ...........................................................................
    // end of parameter parsing
    // ...........................................................................

    if (!viewPtr->canUse(auth::Level::RW)) {  // check auth after ensuring that
                                              // the view exists
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                     "insufficient rights to modify view");
    }

    // check ability to read current properties
    {
      velocypack::Builder builderCurrent;

      builderCurrent.openObject();

      auto resCurrent = viewPtr->properties(
          builderCurrent, LogicalDataSource::Serialization::Properties);

      if (!resCurrent.ok()) {
        TRI_V8_THROW_EXCEPTION(resCurrent);
      }
    }

    auto view =
        resolver.getView(viewPtr->id());  // ensure have the latest definition

    if (!view) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    }

    auto& vocbase = GetContextVocBase(isolate);
    TRI_GET_SERVER_GLOBALS(ArangodServer);
    auto res = v8g->server()
                   .getFeature<arangodb::iresearch::IResearchAnalyzerFeature>()
                   .loadAvailableAnalyzers(vocbase.name());

    if (res.fail()) {
      TRI_V8_THROW_EXCEPTION(res);
    }

    res = view->properties(builder.slice(), true, partialUpdate);

    if (!res.ok()) {
      TRI_V8_THROW_EXCEPTION(res);
    }
  }

  auto view = resolver.getView(viewPtr->id());

  if (!view) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  // ...........................................................................
  // end of parameter parsing
  // ...........................................................................

  if (!view->canUse(
          auth::Level::RO)) {  // check auth after ensuring that the view exists
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "insufficient rights to get view");
  }

  velocypack::Builder builder;

  builder.openObject();

  auto const res =
      view->properties(builder, LogicalDataSource::Serialization::Properties);

  builder.close();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  // return the current parameter set
  // Note: no need to check for auth since view is from the v* context (i.e.
  // authed before)
  TRI_V8_RETURN(TRI_VPackToV8(isolate, builder.slice())
                    ->ToObject(TRI_IGETC)
                    .FromMaybe(v8::Local<v8::Object>()));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief rename a view
////////////////////////////////////////////////////////////////////////////////

static void JS_RenameViewVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("rename(<name>)");
  }

  std::string const name = TRI_ObjectToString(isolate, args[0]);

  if (name.empty()) {
    TRI_V8_THROW_EXCEPTION_PARAMETER("<name> must be non-empty");
  }

  auto* view = UnwrapView(isolate, args.Holder());

  if (!view) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract view");
  }

  PREVENT_EMBEDDED_TRANSACTION();

  // ...........................................................................
  // end of parameter parsing
  // ...........................................................................

  if (!view->canUse(
          auth::Level::RW)) {  // check auth after ensuring that the view exists
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "insufficient rights to rename view");
  }

  // skip views for which the full view definition cannot be generated, as per
  // https://github.com/arangodb/backlog/issues/459
  try {
    velocypack::Builder viewBuilder;

    viewBuilder.openObject();

    auto const res = view->properties(
        viewBuilder, LogicalDataSource::Serialization::Properties);

    if (!res.ok()) {
      TRI_V8_THROW_EXCEPTION(res);  // skip view
    }
  } catch (...) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);  // skip view
  }

  auto res = view->rename(std::string(name));

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

/// @brief return the type of a view
static void JS_TypeViewVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto* view = UnwrapView(isolate, args.Holder());

  if (!view) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract view");
  }

  // ...........................................................................
  // end of parameter parsing
  // ...........................................................................

  if (!view->canUse(
          auth::Level::RO)) {  // check auth after ensuring that the view exists
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "insufficient rights to get view");
  }

  auto const type = view->typeName();
  TRI_V8_RETURN(TRI_V8_PAIR_STRING(isolate, type.data(), type.size()));
  TRI_V8_TRY_CATCH_END
}

void TRI_InitV8Views(TRI_v8_global_t& v8g, v8::Isolate* isolate) {
  auto db = v8::Local<v8::ObjectTemplate>::New(isolate, v8g.VocbaseTempl);

  //  v8::Handle<v8::ObjectTemplate> VocbaseTempl = v8g.VocbaseTempl;
  TRI_AddMethodVocbase(isolate, db, TRI_V8_ASCII_STRING(isolate, "_createView"),
                       JS_CreateViewVocbase);
  TRI_AddMethodVocbase(isolate, db, TRI_V8_ASCII_STRING(isolate, "_dropView"),
                       JS_DropViewVocbase);
  TRI_AddMethodVocbase(isolate, db, TRI_V8_ASCII_STRING(isolate, "_view"),
                       JS_ViewVocbase);
  TRI_AddMethodVocbase(isolate, db, TRI_V8_ASCII_STRING(isolate, "_views"),
                       JS_ViewsVocbase);

  v8::Handle<v8::ObjectTemplate> rt;
  v8::Handle<v8::FunctionTemplate> ft;

  ft = v8::FunctionTemplate::New(isolate);
  ft->SetClassName(TRI_V8_ASCII_STRING(isolate, "ArangoView"));

  rt = ft->InstanceTemplate();
  rt->SetInternalFieldCount(2);  // SLOT_CLASS_TYPE + SLOT_CLASS

  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "drop"),
                       JS_DropViewVocbaseObj);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "name"),
                       JS_NameViewVocbase);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "properties"),
                       JS_PropertiesViewVocbase);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "rename"),
                       JS_RenameViewVocbase);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "type"),
                       JS_TypeViewVocbase);

  v8g.VocbaseViewTempl.Reset(isolate, rt);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "ArangoView"),
      ft->GetFunction(TRI_IGETC).FromMaybe(v8::Local<v8::Function>()));
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
