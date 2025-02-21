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
/// @author Kaveh Vahedipour
/// @author Copyright 2017, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include <iostream>

#include "gtest/gtest.h"

#include "fakeit.hpp"

#include <velocypack/Parser.h>
#include <velocypack/Slice.h>

#include "Mocks/LogLevels.h"

#include "Agency/AddFollower.h"
#include "Agency/AgentInterface.h"
#include "Agency/Node.h"
#include "Basics/StringUtils.h"
#include "Basics/TimeString.h"
#include "Logger/LogMacros.h"
#include "Random/RandomGenerator.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::consensus;
using namespace fakeit;
using namespace arangodb::velocypack;

namespace arangodb {
namespace tests {
namespace add_follower_test {

[[maybe_unused]] const std::string PREFIX = "arango";
[[maybe_unused]] const std::string DATABASE = "database";
[[maybe_unused]] const std::string COLLECTION = "collection";
[[maybe_unused]] const std::string SHARD = "s99";
[[maybe_unused]] const std::string SHARD_LEADER = "leader";
[[maybe_unused]] const std::string SHARD_FOLLOWER1 = "follower1";
[[maybe_unused]] const std::string SHARD_FOLLOWER2 = "follower2";
[[maybe_unused]] const std::string FREE_SERVER = "free";
[[maybe_unused]] const std::string FREE_SERVER2 = "free2";

bool aborts = false;

const char* agency =
#include "AddFollowerTest.json"
    ;
const char* todo =
#include "AddFollowerTestToDo.json"
    ;

NodePtr createNodeFromBuilder(Builder const& builder) {
  return Node::create(builder.slice());
}

Builder createBuilder(char const* c) {
  Options options;
  options.checkAttributeUniqueness = true;
  VPackParser parser(&options);
  parser.parse(c);

  Builder builder;
  builder.add(parser.steal()->slice());
  return builder;
}

NodePtr createNode(char const* c) {
  return createNodeFromBuilder(createBuilder(c));
}

NodePtr createRootNode() { return createNode(agency); }

typedef std::function<std::unique_ptr<Builder>(Slice const&,
                                               std::string const&)>
    TestStructType;

inline static std::string typeName(Slice const& slice) {
  return std::string(slice.typeName());
}

class AddFollowerTest
    : public ::testing::Test,
      public LogSuppressor<Logger::SUPERVISION, LogLevel::FATAL> {
 protected:
  NodePtr baseStructure;
  Builder builder;
  std::string jobId;
  write_ret_t fakeWriteResult;
  trans_ret_t fakeTransResult;

  AddFollowerTest()
      : baseStructure(createRootNode()),
        jobId("1"),
        fakeWriteResult(true, "", std::vector<apply_ret_t>{APPLIED},
                        std::vector<index_t>{1}),
        fakeTransResult(true, "", 1, 0, std::make_shared<Builder>()) {
    arangodb::RandomGenerator::initialize(
        arangodb::RandomGenerator::RandomType::MERSENNE);
    baseStructure->toBuilder(builder);
  }
};

TEST_F(AddFollowerTest, creating_a_job_should_create_a_job_in_todo) {
  Mock<AgentInterface> mockAgent;

  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        auto expectedJobKey = "/arango/Target/ToDo/" + jobId;
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        EXPECT_EQ(q[0].length(),
                  1);  // we always simply override! no preconditions...
        EXPECT_EQ(typeName(q[0][0]), "object");
        EXPECT_EQ(q[0][0].length(),
                  1);  // should ONLY do an entry in todo
        EXPECT_EQ(typeName(q[0][0].get(expectedJobKey)), "object");

        auto job = q[0][0].get(expectedJobKey);
        EXPECT_EQ(typeName(job.get("creator")), "string");
        EXPECT_EQ(typeName(job.get("type")), "string");
        EXPECT_EQ(job.get("type").copyString(), "addFollower");
        EXPECT_EQ(typeName(job.get("database")), "string");
        EXPECT_EQ(job.get("database").copyString(), DATABASE);
        EXPECT_EQ(typeName(job.get("collection")), "string");
        EXPECT_EQ(job.get("collection").copyString(), COLLECTION);
        EXPECT_EQ(typeName(job.get("shard")), "string");
        EXPECT_EQ(job.get("shard").copyString(), SHARD);
        EXPECT_EQ(typeName(job.get("jobId")), "string");
        EXPECT_EQ(typeName(job.get("timeCreated")), "string");

        return fakeWriteResult;
      });

  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  auto& agent = mockAgent.get();
  auto addFollower = AddFollower(*baseStructure->get(PREFIX), &agent, jobId,
                                 "unittest", DATABASE, COLLECTION, SHARD);

  addFollower.create();
}

TEST_F(AddFollowerTest, collection_still_exists) {
  TestStructType createTestStructure = [&](Slice const& s,
                                           std::string const& path) {
    std::unique_ptr<Builder> builder;
    if (path == "/arango/Plan/Collections/" + DATABASE + "/" + COLLECTION) {
      return builder;
    }
    builder = std::make_unique<Builder>();

    if (s.isObject()) {
      VPackObjectBuilder b(builder.get());
      for (auto it : VPackObjectIterator(s)) {
        auto childBuilder =
            createTestStructure(it.value, path + "/" + it.key.copyString());
        if (childBuilder) {
          builder->add(it.key.copyString(), childBuilder->slice());
        }
      }
      if (path == "/arango/Target/ToDo") {
        builder->add(jobId, createBuilder(todo).slice());
      }
    } else {
      builder->add(s);
    }

    return builder;
  };

  auto builder = createTestStructure(baseStructure->toBuilder().slice(), "");
  ASSERT_TRUE(builder);
  auto agency = createNodeFromBuilder(*builder);

  Mock<AgentInterface> mockAgent;
  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        // we always simply override! no preconditions...
        EXPECT_EQ(q[0].length(), 1);
        EXPECT_EQ(typeName(q[0][0]), "object");

        auto writes = q[0][0];
        EXPECT_EQ(typeName(writes.get("/arango/Target/ToDo/1")), "object");
        EXPECT_TRUE(typeName(writes.get("/arango/Target/ToDo/1").get("op")) ==
                    "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/ToDo/1").get("op").copyString() ==
            "delete");
        EXPECT_EQ(typeName(writes.get("/arango/Target/Finished/1")), "object");
        return fakeWriteResult;
      });

  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  auto& agent = mockAgent.get();
  AddFollower(*agency->get("arango"), &agent, JOB_STATUS::TODO, jobId)
      .start(aborts);
}

TEST_F(AddFollowerTest, collection_has_nonempty_distributeshardslike) {
  TestStructType createTestStructure = [&](Slice const& s,
                                           std::string const& path) {
    std::unique_ptr<Builder> builder = std::make_unique<Builder>();
    if (s.isObject()) {
      VPackObjectBuilder b(builder.get());
      for (auto it : VPackObjectIterator(s)) {
        auto childBuilder =
            createTestStructure(it.value, path + "/" + it.key.copyString());
        if (childBuilder) {
          builder->add(it.key.copyString(), childBuilder->slice());
        }
      }
      if (path == "/arango/Plan/Collections/" + DATABASE + "/" + COLLECTION) {
        builder->add("distributeShardsLike", VPackValue("PENG"));
      }
      if (path == "/arango/Target/ToDo") {
        builder->add(jobId, createBuilder(todo).slice());
      }
    } else {
      builder->add(s);
    }
    return builder;
  };

  auto builder = createTestStructure(baseStructure->toBuilder().slice(), "");
  ASSERT_TRUE(builder);
  auto agency = createNodeFromBuilder(*builder);

  Mock<AgentInterface> mockAgent;
  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        EXPECT_EQ(q[0].length(),
                  1);  // we always simply override! no preconditions...
        EXPECT_EQ(typeName(q[0][0]), "object");

        auto writes = q[0][0];
        EXPECT_EQ(typeName(writes.get("/arango/Target/ToDo/1")), "object");
        EXPECT_TRUE(typeName(writes.get("/arango/Target/ToDo/1").get("op")) ==
                    "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/ToDo/1").get("op").copyString() ==
            "delete");
        EXPECT_EQ(typeName(writes.get("/arango/Target/Failed/1")), "object");
        return fakeWriteResult;
      });
  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  auto& agent = mockAgent.get();
  AddFollower(*agency->get("arango"), &agent, JOB_STATUS::TODO, jobId)
      .start(aborts);
}

TEST_F(AddFollowerTest, condition_still_holds) {
  TestStructType createTestStructure = [&](Slice const& s,
                                           std::string const& path) {
    std::unique_ptr<Builder> builder = std::make_unique<Builder>();

    if (s.isObject()) {
      VPackObjectBuilder b(builder.get());
      for (auto it : VPackObjectIterator(s)) {
        auto childBuilder =
            createTestStructure(it.value, path + "/" + it.key.copyString());
        if (childBuilder) {
          builder->add(it.key.copyString(), childBuilder->slice());
        }
      }
      if (path == "/arango/Target/ToDo") {
        builder->add(jobId, createBuilder(todo).slice());
      }
    } else {
      if (path == "/arango/Plan/Collections/" + DATABASE + "/" + COLLECTION +
                      "/shards/" + SHARD) {
        VPackArrayBuilder a(builder.get());
        for (auto const& serv : VPackArrayIterator(s)) {
          builder->add(serv);
        }
        builder->add(VPackValue(SHARD_FOLLOWER2));
      } else {
        builder->add(s);
      }
    }
    return builder;
  };

  auto builder = createTestStructure(baseStructure->toBuilder().slice(), "");
  ASSERT_TRUE(builder);
  auto agency = createNodeFromBuilder(*builder);

  Mock<AgentInterface> mockAgent;
  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        EXPECT_EQ(q[0].length(),
                  1);  // we always simply override! no preconditions...
        EXPECT_EQ(typeName(q[0][0]), "object");

        auto writes = q[0][0];
        EXPECT_EQ(typeName(writes.get("/arango/Target/ToDo/1")), "object");
        EXPECT_TRUE(typeName(writes.get("/arango/Target/ToDo/1").get("op")) ==
                    "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/ToDo/1").get("op").copyString() ==
            "delete");
        EXPECT_EQ(writes.get("/arango/Target/Finished/1")
                      .get("collection")
                      .copyString(),
                  COLLECTION);
        EXPECT_TRUE(
            writes.get("/arango/Target/Pending/1").get("op").copyString() ==
            "delete");
        EXPECT_EQ(typeName(writes.get("/arango/Target/Failed/1")), "none");
        return fakeWriteResult;
      });
  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  AgentInterface& agent = mockAgent.get();
  AddFollower(*agency->get("arango"), &agent, JOB_STATUS::TODO, jobId)
      .start(aborts);
}

TEST_F(AddFollowerTest, if_no_job_under_shard_leave_job_in_todo) {
  TestStructType createTestStructure = [&](Slice const& s,
                                           std::string const& path) {
    std::unique_ptr<Builder> builder = std::make_unique<Builder>();
    if (s.isObject()) {
      VPackObjectBuilder b(builder.get());
      for (auto it : VPackObjectIterator(s)) {
        auto childBuilder =
            createTestStructure(it.value, path + "/" + it.key.copyString());
        if (childBuilder) {
          builder->add(it.key.copyString(), childBuilder->slice());
        }
      }
      if (path == "/arango/Target/ToDo") {
        builder->add(jobId, createBuilder(todo).slice());
      }
      if (path == "/arango/Supervision/Shards") {
        builder->add(SHARD, VPackValue("2"));
      }
    } else {
      builder->add(s);
    }
    return builder;
  };

  auto builder = createTestStructure(baseStructure->toBuilder().slice(), "");
  ASSERT_TRUE(builder);
  auto agency = createNodeFromBuilder(*builder);

  Mock<AgentInterface> mockAgent;
  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        EXPECT_EQ(q[0].length(),
                  1);  // we always simply override! no preconditions...
        EXPECT_EQ(typeName(q[0][0]), "object");

        auto writes = q[0][0];
        EXPECT_EQ(typeName(writes.get("/arango/Target/ToDo/1")), "object");
        EXPECT_EQ(writes.get("/arango/Target/Finished/1")
                      .get("collection")
                      .copyString(),
                  COLLECTION);
        return fakeWriteResult;
      });
  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  auto& agent = mockAgent.get();
  AddFollower(*agency->get("arango"), &agent, JOB_STATUS::TODO, jobId)
      .start(aborts);
}

TEST_F(AddFollowerTest, we_can_find_one_with_status_good) {
  TestStructType createTestStructure = [&](Slice const& s,
                                           std::string const& path) {
    std::unique_ptr<Builder> builder = std::make_unique<Builder>();
    if (s.isObject()) {
      VPackObjectBuilder b(builder.get());
      for (auto it : VPackObjectIterator(s)) {
        auto childBuilder =
            createTestStructure(it.value, path + "/" + it.key.copyString());
        if (childBuilder) {
          builder->add(it.key.copyString(), childBuilder->slice());
        }
      }
    } else {
      builder->add(s);
    }
    return builder;
  };

  auto builder = createTestStructure(baseStructure->toBuilder().slice(), "");
  ASSERT_TRUE(builder);
  auto agency = createNodeFromBuilder(*builder);
  agency = agency->placeAt("arango/Supervision/Health/free/Status", "FAILED");
  agency =
      agency->placeAt("arango/Supervision/Health/follower2/Status", "FAILED");
  agency = agency->placeAt("/arango/Target/ToDo/" + jobId,
                           createBuilder(todo).slice());

  Mock<AgentInterface> mockAgent;
  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        EXPECT_EQ(q[0].length(),
                  1)
            << q[0].toJson();  // we always simply override! no preconditions...
        EXPECT_EQ(typeName(q[0][0]), "object");

        auto writes = q[0][0];
        EXPECT_EQ(typeName(writes.get("/arango/Target/ToDo/1")), "object");
        EXPECT_EQ(writes.get("/arango/Target/Finished/1")
                      .get("collection")
                      .copyString(),
                  COLLECTION);
        return fakeWriteResult;
      });
  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  AgentInterface& agent = mockAgent.get();
  AddFollower(*agency->get("arango"), &agent, JOB_STATUS::TODO, jobId)
      .start(aborts);
}

TEST_F(AddFollowerTest, job_performed_immediately_in_a_single_transaction) {
  TestStructType createTestStructure = [&](Slice const& s,
                                           std::string const& path) {
    std::unique_ptr<Builder> builder = std::make_unique<Builder>();
    if (s.isObject()) {
      VPackObjectBuilder b(builder.get());
      for (auto it : VPackObjectIterator(s)) {
        auto childBuilder =
            createTestStructure(it.value, path + "/" + it.key.copyString());
        if (childBuilder) {
          builder->add(it.key.copyString(), childBuilder->slice());
        }
      }
      if (path == "/arango/Target/ToDo") {
        builder->add(jobId, createBuilder(todo).slice());
      }
    } else {
      builder->add(s);
    }
    return builder;
  };

  auto builder = createTestStructure(baseStructure->toBuilder().slice(), "");
  ASSERT_TRUE(builder);
  auto agency = createNodeFromBuilder(*builder);

  Mock<AgentInterface> mockAgent;
  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        EXPECT_EQ(q[0].length(),
                  2);  // we always simply override! no preconditions...
        EXPECT_EQ(typeName(q[0][0]), "object");

        auto writes = q[0][0];
        EXPECT_EQ(typeName(writes.get("/arango/Target/ToDo/1")), "object");
        EXPECT_TRUE(typeName(writes.get("/arango/Target/ToDo/1").get("op")) ==
                    "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/ToDo/1").get("op").copyString() ==
            "delete");
        EXPECT_EQ(writes.get("/arango/Target/Finished/1")
                      .get("collection")
                      .copyString(),
                  COLLECTION);
        EXPECT_EQ(typeName(writes.get("/arango/Target/Pending/1")), "none");
        EXPECT_EQ(typeName(writes.get("/arango/Target/Failed/1")), "none");
        return fakeWriteResult;
      });
  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  AgentInterface& agent = mockAgent.get();
  AddFollower(*agency->get("arango"), &agent, JOB_STATUS::TODO, jobId)
      .start(aborts);
}

TEST_F(AddFollowerTest, job_can_still_be_safely_aborted) {
  TestStructType createTestStructure = [&](Slice const& s,
                                           std::string const& path) {
    std::unique_ptr<Builder> builder = std::make_unique<Builder>();
    if (s.isObject()) {
      VPackObjectBuilder b(builder.get());
      for (auto it : VPackObjectIterator(s)) {
        auto childBuilder =
            createTestStructure(it.value, path + "/" + it.key.copyString());
        if (childBuilder) {
          builder->add(it.key.copyString(), childBuilder->slice());
        }
      }
      if (path == "/arango/Target/ToDo") {
        builder->add(jobId, createBuilder(todo).slice());
      }
    } else {
      builder->add(s);
    }
    return builder;
  };

  auto builder = createTestStructure(baseStructure->toBuilder().slice(), "");
  ASSERT_TRUE(builder);
  auto agency = createNodeFromBuilder(*builder);

  Mock<AgentInterface> mockAgent;
  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        EXPECT_EQ(q[0].length(),
                  1);  // we always simply override! no preconditions...
        EXPECT_EQ(typeName(q[0][0]), "object");

        auto writes = q[0][0];
        EXPECT_EQ(typeName(writes.get("/arango/Target/Failed/1")), "object");
        EXPECT_EQ(writes.get("/arango/Target/Failed/1")
                      .get("collection")
                      .copyString(),
                  COLLECTION);
        EXPECT_TRUE(typeName(writes.get("/arango/Target/ToDo/1").get("op")) ==
                    "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/ToDo/1").get("op").copyString() ==
            "delete");
        EXPECT_TRUE(
            typeName(writes.get("/arango/Target/Pending/1").get("op")) ==
            "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/Pending/1").get("op").copyString() ==
            "delete");
        return fakeWriteResult;
      });
  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  AgentInterface& agent = mockAgent.get();
  AddFollower(*agency->get("arango"), &agent, JOB_STATUS::PENDING, jobId)
      .abort("test abort");
}

TEST_F(AddFollowerTest, job_cannot_be_aborted) {
  TestStructType createTestStructure = [&](Slice const& s,
                                           std::string const& path) {
    std::unique_ptr<Builder> builder = std::make_unique<Builder>();
    if (s.isObject()) {
      VPackObjectBuilder b(builder.get());
      for (auto it : VPackObjectIterator(s)) {
        auto childBuilder =
            createTestStructure(it.value, path + "/" + it.key.copyString());
        if (childBuilder) {
          builder->add(it.key.copyString(), childBuilder->slice());
        }
      }
      if (path == "/arango/Target/Pending") {
        builder->add(jobId, createBuilder(todo).slice());
      }
    } else {
      builder->add(s);
    }
    return builder;
  };

  auto builder = createTestStructure(baseStructure->toBuilder().slice(), "");
  ASSERT_TRUE(builder);
  auto agency = createNodeFromBuilder(*builder);

  Mock<AgentInterface> mockAgent;
  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        EXPECT_EQ(q[0].length(),
                  1);  // we always simply override! no preconditions...
        EXPECT_EQ(typeName(q[0][0]), "object");

        auto writes = q[0][0];
        EXPECT_EQ(typeName(writes.get("/arango/Target/Failed/1")), "object");
        EXPECT_EQ(writes.get("/arango/Target/Failed/1")
                      .get("collection")
                      .copyString(),
                  COLLECTION);
        EXPECT_TRUE(typeName(writes.get("/arango/Target/ToDo/1").get("op")) ==
                    "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/ToDo/1").get("op").copyString() ==
            "delete");
        EXPECT_TRUE(
            typeName(writes.get("/arango/Target/Pending/1").get("op")) ==
            "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/Pending/1").get("op").copyString() ==
            "delete");
        return fakeWriteResult;
      });
  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  AgentInterface& agent = mockAgent.get();
  AddFollower(*agency->get("arango"), &agent, JOB_STATUS::TODO, jobId)
      .abort("test abort");
}

TEST_F(AddFollowerTest, job_only_starting_with_delay) {
  TestStructType createTestStructure = [&](Slice const& s,
                                           std::string const& path) {
    std::unique_ptr<Builder> builder = std::make_unique<Builder>();
    if (s.isObject()) {
      VPackObjectBuilder b(builder.get());
      for (auto it : VPackObjectIterator(s)) {
        auto childBuilder =
            createTestStructure(it.value, path + "/" + it.key.copyString());
        if (childBuilder) {
          builder->add(it.key.copyString(), childBuilder->slice());
        }
      }
      if (path == "/arango/Target/ToDo") {
        VPackObjectBuilder guard(builder.get(), jobId, false);
        builder->add("creator", VPackValue("supervision"));
        builder->add("type", VPackValue("addFollower"));
        builder->add("database", VPackValue("database"));
        builder->add("collection", VPackValue("collection"));
        builder->add("shard", VPackValue("s99"));
        builder->add("jobId", VPackValue(jobId));
        auto now = std::chrono::system_clock::now();
        builder->add("timeCreated", VPackValue(timepointToString(now)));
        builder->add(
            "notBefore",
            VPackValue(timepointToString(now + std::chrono::seconds(60))));
      }
    } else {
      builder->add(s);
    }
    return builder;
  };

  auto builder = createTestStructure(baseStructure->toBuilder().slice(), "");
  ASSERT_TRUE(builder);
  auto agency = createNodeFromBuilder(*builder);

  Mock<AgentInterface> mockAgent;
#if 0
  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        EXPECT_EQ(q[0].length(),
                  1);  // we always simply override! no preconditions...
        EXPECT_EQ(typeName(q[0][0]), "object");

        auto writes = q[0][0];
        EXPECT_EQ(typeName(writes.get("/arango/Target/Failed/1")), "object");
        EXPECT_EQ(writes.get("/arango/Target/Failed/1")
                      .get("collection")
                      .copyString(),
                  COLLECTION);
        EXPECT_TRUE(typeName(writes.get("/arango/Target/ToDo/1").get("op")) ==
                    "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/ToDo/1").get("op").copyString() ==
            "delete");
        EXPECT_TRUE(
            typeName(writes.get("/arango/Target/Pending/1").get("op")) ==
            "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/Pending/1").get("op").copyString() ==
            "delete");
        return fakeWriteResult;
      });
#endif
  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  AgentInterface& agent = mockAgent.get();
  bool res =
      AddFollower(*agency->get("arango"), &agent, JOB_STATUS::TODO, jobId)
          .start(aborts);
  EXPECT_FALSE(res);
}

TEST_F(AddFollowerTest, job_only_starting_with_no_delay) {
  TestStructType createTestStructure = [&](Slice const& s,
                                           std::string const& path) {
    std::unique_ptr<Builder> builder = std::make_unique<Builder>();
    if (s.isObject()) {
      VPackObjectBuilder b(builder.get());
      for (auto it : VPackObjectIterator(s)) {
        auto childBuilder =
            createTestStructure(it.value, path + "/" + it.key.copyString());
        if (childBuilder) {
          builder->add(it.key.copyString(), childBuilder->slice());
        }
      }
      if (path == "/arango/Target/ToDo") {
        VPackObjectBuilder guard(builder.get(), jobId, false);
        builder->add("creator", VPackValue("supervision"));
        builder->add("type", VPackValue("addFollower"));
        builder->add("database", VPackValue("database"));
        builder->add("collection", VPackValue("collection"));
        builder->add("shard", VPackValue("s99"));
        builder->add("jobId", VPackValue(jobId));
        auto now = std::chrono::system_clock::now();
        builder->add("timeCreated", VPackValue(timepointToString(now)));
        builder->add("notBefore", VPackValue(timepointToString(now)));
      }
    } else {
      builder->add(s);
    }
    return builder;
  };

  auto builder = createTestStructure(baseStructure->toBuilder().slice(), "");
  ASSERT_TRUE(builder);
  auto agency = createNodeFromBuilder(*builder);

  Mock<AgentInterface> mockAgent;
  When(Method(mockAgent, write))
      .AlwaysDo([&](velocypack::Slice q,
                    consensus::AgentInterface::WriteMode w) -> write_ret_t {
        EXPECT_EQ(typeName(q), "array");
        EXPECT_EQ(q.length(), 1);
        EXPECT_EQ(typeName(q[0]), "array");
        EXPECT_EQ(q[0].length(), 2);
        EXPECT_EQ(typeName(q[0][0]), "object");

        auto writes = q[0][0];
        EXPECT_EQ(typeName(writes.get("/arango/Target/ToDo/1")), "object");
        EXPECT_TRUE(typeName(writes.get("/arango/Target/ToDo/1").get("op")) ==
                    "string");
        EXPECT_TRUE(
            writes.get("/arango/Target/ToDo/1").get("op").copyString() ==
            "delete");
        EXPECT_EQ(writes.get("/arango/Target/Finished/1")
                      .get("collection")
                      .copyString(),
                  COLLECTION);
        return fakeWriteResult;
      });
  When(Method(mockAgent, waitFor))
      .AlwaysReturn(AgentInterface::raft_commit_t::OK);
  AgentInterface& agent = mockAgent.get();
  bool res =
      AddFollower(*agency->get("arango"), &agent, JOB_STATUS::TODO, jobId)
          .start(aborts);
  EXPECT_TRUE(res);
}

}  // namespace add_follower_test
}  // namespace tests
}  // namespace arangodb
