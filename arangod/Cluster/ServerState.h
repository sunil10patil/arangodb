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

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "Basics/Common.h"
#include "Basics/ReadWriteLock.h"
#include "Basics/ResultT.h"
#include "Cluster/ClusterTypes.h"
#include "RestServer/arangod.h"
#include "VocBase/voc-types.h"

namespace arangodb {
class AgencyComm;
class Result;

class ServerState {
 public:
  /// @brief an enum describing the roles a server can have
  enum RoleEnum : int {
    ROLE_UNDEFINED = 0,  // initial value
    ROLE_SINGLE,         // is set when cluster feature is off
    ROLE_DBSERVER,
    ROLE_COORDINATOR,
    ROLE_AGENT
  };

  /// @brief an enum describing the possible states a server can have
  enum StateEnum {
    STATE_UNDEFINED = 0,  // initial value
    STATE_STARTUP,        // used by all roles
    STATE_SERVING,        // used by all roles
    STATE_SHUTDOWN        // used by all roles
  };

  enum ReadOnlyMode {
    API_TRUE,  // Set from outside via API
    API_FALSE,
    LICENSE_TRUE,  // Set from license feature
    LICENSE_FALSE
  };

  enum class Mode : uint8_t {
    DEFAULT = 0,
    STARTUP = 1,
    /// reject almost all requests
    MAINTENANCE = 2,
    /// status unclear, client must try again
    TRYAGAIN = 3,
    /// redirect to lead server if possible
    REDIRECT = 4,
    INVALID = 255,  // this mode is used to indicate shutdown
  };

  explicit ServerState(ArangodServer&);

  ~ServerState();

  /// @brief create the (sole) instance
  static ServerState* instance() noexcept;

  /// @brief get the string representation of a role
  static std::string roleToString(RoleEnum);

  static std::string roleToShortString(RoleEnum);

  /// @brief get the key for lists of a role in the agency
  static std::string roleToAgencyListKey(RoleEnum);

  /// @brief get the key for a role in the agency
  static std::string roleToAgencyKey(RoleEnum role);

  /// @brief convert a string to a role
  static RoleEnum stringToRole(std::string_view);

  /// @brief get the string representation of a state
  static std::string stateToString(StateEnum);

  /// @brief convert a string representation to a state
  static StateEnum stringToState(std::string_view);

  /// @brief get the string representation of a mode
  static std::string modeToString(Mode);

  /// @brief convert a string representation to a mode
  static Mode stringToMode(std::string_view);

  /// @brief atomically load current server mode
  static Mode mode();

  /// @brief sets server mode, returns previously held
  /// value (performs atomic read-modify-write operation)
  static Mode setServerMode(Mode mode);

  /// @brief checks maintenance mode
  static bool isStartupOrMaintenance();

  /// @brief should not allow DDL operations / transactions
  static bool readOnly();

  /// @brief should not allow DDL operations / transactions
  static bool readOnlyByLicense();

  /// @brief should not allow DDL operations / transactions
  static bool readOnlyByAPI();

  /// @brief set server read-only
  static bool setReadOnly(ReadOnlyMode);

  static void reset();

  bool isSingleServer() const noexcept { return isSingleServer(loadRole()); }

  static bool isSingleServer(ServerState::RoleEnum role) noexcept {
    TRI_ASSERT(role != ServerState::ROLE_UNDEFINED);
    return (role == ServerState::ROLE_SINGLE);
  }

  /// @brief check whether the server is a coordinator
  bool isCoordinator() const noexcept { return isCoordinator(loadRole()); }

  /// @brief check whether the server is a coordinator
  static bool isCoordinator(ServerState::RoleEnum role) noexcept {
    TRI_ASSERT(role != ServerState::ROLE_UNDEFINED);
    return (role == ServerState::ROLE_COORDINATOR);
  }

  /// @brief check whether the server is a DB server (primary or secondary)
  /// running in cluster mode.
  bool isDBServer() const noexcept { return isDBServer(loadRole()); }

  /// @brief check whether the server is a DB server (primary or secondary)
  /// running in cluster mode.
  static bool isDBServer(ServerState::RoleEnum role) noexcept {
    TRI_ASSERT(role != ServerState::ROLE_UNDEFINED);
    return (role == ServerState::ROLE_DBSERVER);
  }

  /// @brief whether or not the role is a cluster-related role
  static bool isClusterRole(ServerState::RoleEnum role) noexcept {
    return (role == ServerState::ROLE_DBSERVER ||
            role == ServerState::ROLE_COORDINATOR);
  }

  /// @brief whether or not the role is a cluster-related role
  bool isClusterRole() const noexcept { return (isClusterRole(loadRole())); };

  /// @brief check whether the server is an agent
  bool isAgent() const noexcept { return isAgent(loadRole()); }

  /// @brief check whether the server is an agent
  static bool isAgent(ServerState::RoleEnum role) noexcept {
    TRI_ASSERT(role != ServerState::ROLE_UNDEFINED);
    return (role == ServerState::ROLE_AGENT);
  }

  /// @brief check whether the server is running in a cluster
  bool isRunningInCluster() const noexcept { return isClusterRole(loadRole()); }

  /// @brief check whether the server is running in a cluster
  static bool isRunningInCluster(ServerState::RoleEnum role) noexcept {
    return isClusterRole(role);
  }

  /// @brief check whether the server is a single or coordinator
  bool isSingleServerOrCoordinator() const noexcept {
    return isSingleServerOrCoordinator(loadRole());
  }

  static bool isSingleServerOrCoordinator(ServerState::RoleEnum role) noexcept {
    return isCoordinator(role) || isSingleServer(role);
  }

  /// @brief get the server role
  inline RoleEnum getRole() const noexcept { return loadRole(); }

  /// @brief register with agency, create / load server ID
  bool integrateIntoCluster(RoleEnum role, std::string const& myAddr,
                            std::string const& myAdvEndpoint);

  /// @brief unregister this server with the agency, removing it from
  /// the cluster setup.
  /// the timeout can be used as the max wait time for the agency to
  /// acknowledge the unregister action.
  bool unregister(double timeout);

  /// @brief mark this server as shut down in the agency, without removing it
  /// from the cluster setup.
  /// the timeout can be used as the max wait time for the agency to
  /// acknowledge the logoff action.
  bool logoff(double timeout);

  /// @brief set the server role
  void setRole(RoleEnum);

  /// @brief get the server id
  std::string getId() const;

  /// @brief set the server id
  void setId(std::string const&);

  /// @brief get the short id
  uint32_t getShortId() const;

  /// @brief set the server short id
  void setShortId(uint32_t);

  /// @brief set the server short id
  std::string getShortName() const;

  RebootId getRebootId() const;

  void setRebootId(RebootId rebootId);

  /// @brief get the server endpoint
  std::string getEndpoint();

  /// @brief get the server advertised endpoint
  std::string getAdvertisedEndpoint();

  /// @brief find a host identification string
  void findHost(std::string const& fallback);

  /// @brief get a string to identify the host we are running on
  std::string getHost() { return _host; }

  /// @brief get the current state
  StateEnum getState();

  /// @brief set the current state
  void setState(StateEnum);

  bool isFoxxmaster() const;

  std::string getFoxxmaster() const;

  void setFoxxmaster(std::string const&);

  void setFoxxmasterQueueupdate(bool value) noexcept;

  bool getFoxxmasterQueueupdate() const noexcept;

  TRI_voc_tick_t getFoxxmasterSince() const noexcept;

  std::string getPersistedId();
  bool hasPersistedId();
  bool writePersistedId(std::string const&);
  std::string generatePersistedId(RoleEnum const&);

  /// @brief sets server mode and propagates new mode to agency
  Result propagateClusterReadOnly(bool);

  /// file where the server persists its UUID
  std::string getUuidFilename() const;

#ifdef ARANGODB_USE_GOOGLE_TESTS
  [[nodiscard]] bool isGoogleTest() const noexcept;
  void setGoogleTest(bool isGoogleTests) noexcept;
#else
  [[nodiscard]] constexpr bool isGoogleTest() const noexcept { return false; }
#endif

 private:
  /// @brief atomically fetches the server role
  inline RoleEnum loadRole() const noexcept {
    return _role.load(std::memory_order_consume);
  }

  /// @brief validate a state transition for a primary server
  bool checkPrimaryState(StateEnum);

  /// @brief validate a state transition for a coordinator server
  bool checkCoordinatorState(StateEnum);

  /// @brief check equality of engines with other registered servers
  bool checkEngineEquality(AgencyComm&);

  /// @brief check equality of naming conventions settings with other registered
  /// servers
  bool checkNamingConventionsEquality(AgencyComm&);

  /// @brief try to read the rebootID from the Agency
  ResultT<uint64_t> readRebootIdFromAgency(AgencyComm& comm);

  /// @brief check whether the agency has been initalized
  bool checkIfAgencyInitialized(AgencyComm&, RoleEnum const&);

  /// @brief register at agency, might already be done
  bool registerAtAgencyPhase1(AgencyComm&, RoleEnum const&);

  /// @brief write the Current/ServersRegistered entry
  bool registerAtAgencyPhase2(AgencyComm&, bool hadPersistedId);

  void setFoxxmasterSinceNow();

  /// @brief whether or not "value" is a server UUID
  bool isUuid(std::string const& value) const;

 private:
  ArangodServer& _server;

  /// @brief server role
  std::atomic<RoleEnum> _role;

  /// @brief r/w lock for state
  mutable basics::ReadWriteLock _lock;

  /// @brief the server's id, can be set just once, use getId and setId, do not
  /// access directly
  std::string _id;
  /// @brief lock for writing and reading server id
  mutable std::mutex _idLock;

  /// @brief the server's short id, can be set just once
  std::atomic<uint32_t> _shortId;

  /// @brief the server's rebootId.
  ///
  /// A server
  ///   * ~boots~ if it is started on a new database directory without a UUID
  ///   persisted
  ///   * ~reboots~ if it is started on a pre-existing database directory with a
  ///   UUID present
  ///
  /// when integrating into a cluster (via integrateIntoCluster), the server
  /// tries to increment the agency key Current/KnownServers/_id/rebootId; if
  /// this key did not exist it is created with the value 1, so a valid rebootId
  /// is always >= 1, and if the server booted must be 1.
  ///
  /// Changes of rebootIds (i.e. server reboots) are noticed in ClusterInfo and
  /// can be used through a notification architecture from there
  std::atomic<RebootId::value_type> _rebootId = 0;

  /// @brief the server's own endpoint, can be set just once
  std::string _myEndpoint;

  /// @brief the server's own advertised endpoint, can be set just once,
  /// if empty, we advertise _address
  std::string _advertisedEndpoint;

  /// @brief an identification string for the host a server is running on
  std::string _host;

  /// @brief the current state
  StateEnum _state;

  /// @brief lock for all foxxmaster-related members
  mutable basics::ReadWriteLock _foxxmasterLock;

  // protected by _foxxmasterLock
  std::string _foxxmaster;

  // @brief point in time since which this server is the Foxxmaster.
  // protected by _foxxmasterLock
  TRI_voc_tick_t _foxxmasterSince;

  // protected by _foxxmasterLock
  bool _foxxmasterQueueupdate;

#ifdef ARANGODB_USE_GOOGLE_TESTS
  bool _isGoogleTests = false;
#endif
};
}  // namespace arangodb

std::ostream& operator<<(std::ostream&, arangodb::ServerState::RoleEnum);
