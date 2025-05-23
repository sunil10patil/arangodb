/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertEqual, assertTrue, assertNotEqual, AQL_EXPLAIN, AQL_EXECUTE */

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for optimizer rules
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const helper = require("@arangodb/aql-helper");
const isEqual = helper.isEqual;

function optimizerRuleTestSuite () {
  const ruleName = "remove-collect-variables";
  // various choices to control the optimizer:
  const paramNone     = { optimizer: { rules: [ "-all" ] } };
  const paramEnabled  = { optimizer: { rules: [ "-all", "+" + ruleName ] } };
  const paramDisabled = { optimizer: { rules: [ "+all", "-" + ruleName ] } };

  return {

////////////////////////////////////////////////////////////////////////////////
/// @brief test that rule has no effect when explicitly disabled
////////////////////////////////////////////////////////////////////////////////

    testRuleDisabled : function () {
      const queries = [
        "FOR i IN 1..10 COLLECT a = i INTO group RETURN a",
        "FOR i IN 1..10 FOR j IN 1..10 COLLECT a = i, b = j INTO group RETURN a",
        "FOR i IN 1..10 FOR j IN 1..10 COLLECT a = i, b = j INTO group RETURN { a: a, b : b }",
        "FOR i IN 1..10 COLLECT WITH COUNT INTO cnt RETURN 1",
        "FOR i IN 1..10 COLLECT AGGREGATE cnt = COUNT() RETURN 1",
        "FOR i IN 1..10 COLLECT AGGREGATE sum = SUM(i) RETURN 1",
        "FOR i IN 1..10 COLLECT AGGREGATE cnt = COUNT(), sum = SUM(i) RETURN 1",
      ];

      queries.forEach(function(query) {
        let result = AQL_EXPLAIN(query, { }, paramNone);
        assertEqual([ ], result.plan.rules);
      });
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test that rule has no effect
////////////////////////////////////////////////////////////////////////////////

    testRuleNoEffect : function () {
      const queries = [
        "FOR i IN 1..10 COLLECT a = i RETURN a",
        "FOR i IN 1..10 FOR j IN 1..10 COLLECT a = i, b = j RETURN a",
        "FOR i IN 1..10 FOR j IN 1..10 COLLECT a = i, b = j INTO group RETURN { a: a, b : b, group: group }",
        "FOR i IN 1..10 COLLECT AGGREGATE cnt = COUNT() RETURN cnt",
        "FOR i IN 1..10 COLLECT AGGREGATE cnt = COUNT() RETURN 1",
        "FOR i IN 1..10 COLLECT AGGREGATE sum = SUM(i) RETURN sum",
        "FOR i IN 1..10 COLLECT AGGREGATE sum = SUM(i) RETURN 1",
        "FOR i IN 1..10 COLLECT AGGREGATE cnt = COUNT(), sum = SUM(i) RETURN [cnt, sum]",
        "FOR i IN 1..10 COLLECT v = i WITH COUNT INTO cnt RETURN cnt",
        "FOR i IN 1..10 COLLECT v = i AGGREGATE cnt = COUNT() RETURN cnt",
        "FOR i IN 1..10 COLLECT v = i AGGREGATE sum = SUM(i) RETURN sum",
      ];

      queries.forEach(function(query) {
        let result = AQL_EXPLAIN(query, { }, paramEnabled);
        assertEqual(-1, result.plan.rules.indexOf(ruleName), query);
      });
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test that rule has an effect
////////////////////////////////////////////////////////////////////////////////

    testRuleHasEffect : function () {
      const queries = [
        "FOR i IN 1..10 COLLECT a = i INTO group RETURN a",
        "FOR i IN 1..10 FOR j IN 1..10 COLLECT a = i, b = j INTO group RETURN a",
        "FOR i IN 1..10 FOR j IN 1..10 COLLECT a = i, b = j INTO group RETURN { a: a, b : b }",
        "FOR i IN 1..10 COLLECT AGGREGATE cnt = COUNT(), sum = SUM(i) RETURN 1",
        "FOR i IN 1..10 COLLECT v = i AGGREGATE cnt = COUNT() RETURN 1",
        "FOR i IN 1..10 COLLECT v = i AGGREGATE sum = SUM(i) RETURN 1",
        "FOR i IN 1..10 COLLECT v = i AGGREGATE cnt = COUNT(), sum = SUM(i) RETURN 1",
        "FOR i IN 1..10 COLLECT v = i WITH COUNT INTO cnt RETURN 1",
      ];

      queries.forEach(function(query) {
        let result = AQL_EXPLAIN(query, { }, paramEnabled);
        assertNotEqual(-1, result.plan.rules.indexOf(ruleName), query);
      });
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test generated plans
////////////////////////////////////////////////////////////////////////////////


    testPlans : function () {
      const plans = [
        [ "FOR i IN 1..10 COLLECT a = i INTO group OPTIONS { method: 'sorted' } RETURN a", [ "SingletonNode", "CalculationNode", "EnumerateListNode", "SortNode", "CollectNode", "ReturnNode" ] ],
        [ "FOR i IN 1..10 COLLECT a = i INTO group OPTIONS { method: 'hash' } RETURN a", [ "SingletonNode", "CalculationNode", "EnumerateListNode", "CollectNode", "SortNode", "ReturnNode" ] ],
        [ "FOR i IN 1..10 FOR j IN 1..10 COLLECT a = i, b = j INTO group OPTIONS { method: 'sorted' } RETURN a", [ "SingletonNode", "CalculationNode", "EnumerateListNode", "CalculationNode", "EnumerateListNode", "SortNode", "CollectNode", "ReturnNode" ] ],
        [ "FOR i IN 1..10 FOR j IN 1..10 COLLECT a = i, b = j INTO group OPTIONS { method: 'hashed' } RETURN a", [ "SingletonNode", "CalculationNode", "EnumerateListNode", "CalculationNode", "EnumerateListNode", "CollectNode", "SortNode", "ReturnNode" ] ],
        [ "FOR i IN 1..10 FOR j IN 1..10 COLLECT a = i, b = j INTO group OPTIONS { method: 'sorted' } RETURN { a: a, b : b }", [ "SingletonNode", "CalculationNode", "EnumerateListNode", "CalculationNode", "EnumerateListNode", "SortNode", "CollectNode", "CalculationNode", "ReturnNode" ] ],
        [ "FOR i IN 1..10 FOR j IN 1..10 COLLECT a = i, b = j INTO group OPTIONS { method: 'hash' } RETURN { a: a, b : b }", [ "SingletonNode", "CalculationNode", "EnumerateListNode", "CalculationNode", "EnumerateListNode", "CollectNode", "SortNode", "CalculationNode", "ReturnNode" ] ],
        [ "FOR i IN 1..10 COLLECT v = i WITH COUNT INTO cnt RETURN 1", [ "SingletonNode", "CalculationNode", "EnumerateListNode", "CollectNode", "SortNode", "CalculationNode", "ReturnNode" ] ],
        [ "FOR i IN 1..10 COLLECT v = i AGGREGATE cnt = COUNT() RETURN 1", [ "SingletonNode", "CalculationNode", "EnumerateListNode", "CollectNode", "SortNode", "CalculationNode", "ReturnNode" ] ],
      ];

      plans.forEach(function(plan) {
        let result = AQL_EXPLAIN(plan[0], { }, paramEnabled);
        assertNotEqual(-1, result.plan.rules.indexOf(ruleName), plan[0]);
        assertEqual(plan[1], helper.getCompactPlan(result).map(function(node) { return node.type; }), plan[0]);
      });
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test results
////////////////////////////////////////////////////////////////////////////////

    testResults : function () {
      const queries = [
        [ "FOR i IN 1..10 COLLECT a = i INTO group RETURN a", [ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 ] ],
        [ "FOR i IN 1..2 FOR j IN 1..2 COLLECT a = i, b = j INTO group RETURN [ a, b ]", [ [ 1, 1 ], [ 1, 2 ], [ 2, 1 ], [ 2, 2 ] ] ],
        [ "FOR i IN [] COLLECT v = i AGGREGATE cnt = COUNT() RETURN 1", [ ] ],
        [ "FOR i IN [] COLLECT v = i WITH COUNT INTO cnt RETURN 1", [ ] ],
        [ "FOR i IN 1..10 COLLECT v = i AGGREGATE cnt = COUNT() RETURN 1", [ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 ] ],
        [ "FOR i IN 1..10 COLLECT v = i WITH COUNT INTO cnt RETURN 1", [ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 ] ],
        [ "FOR i IN 1..10 COLLECT v = i AGGREGATE cnt = COUNT(), sum = SUM(i) RETURN 1", [ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 ] ],
        [ "FOR i IN 1..10 COLLECT v = i AGGREGATE cnt = COUNT(), sum = SUM(i) RETURN cnt", [ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 ] ],
        [ "FOR i IN 1..10 COLLECT v = i AGGREGATE cnt = COUNT(), sum = SUM(i) RETURN sum", [ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 ] ],
      ];

      queries.forEach(function(query) {
        let planDisabled   = AQL_EXPLAIN(query[0], { }, paramDisabled);
        let planEnabled    = AQL_EXPLAIN(query[0], { }, paramEnabled);

        let resultDisabled = AQL_EXECUTE(query[0], { }, paramDisabled).json;
        let resultEnabled  = AQL_EXECUTE(query[0], { }, paramEnabled).json;

        assertTrue(isEqual(resultDisabled, resultEnabled), query[0]);

        assertEqual(-1, planDisabled.plan.rules.indexOf(ruleName), query[0]);
        assertNotEqual(-1, planEnabled.plan.rules.indexOf(ruleName), query[0]);

        assertEqual(resultDisabled, query[1], query);
        assertEqual(resultEnabled, query[1], query);
      });
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test results
////////////////////////////////////////////////////////////////////////////////

    testResultsWhenRuleCannotFire : function () {
      const queries = [
        [ "FOR i IN [] COLLECT AGGREGATE cnt = COUNT() RETURN 1", [ 1 ] ],
        [ "FOR i IN [] COLLECT WITH COUNT INTO cnt RETURN 1", [ 1 ] ],
        [ "FOR i IN 1..10 COLLECT AGGREGATE cnt = COUNT() RETURN 1", [ 1 ] ],
        [ "FOR i IN 1..10 COLLECT WITH COUNT INTO cnt RETURN 1", [ 1 ] ],
        [ "FOR i IN [] COLLECT AGGREGATE cnt = COUNT() RETURN cnt", [ 0 ] ],
        [ "FOR i IN [] COLLECT WITH COUNT INTO cnt RETURN cnt", [ 0 ] ],
        [ "FOR i IN 1..10 COLLECT AGGREGATE cnt = COUNT() RETURN cnt", [ 10 ] ],
        [ "FOR i IN 1..10 COLLECT WITH COUNT INTO cnt RETURN cnt", [ 10 ] ],
      ];

      queries.forEach(function(query) {
        let plan    = AQL_EXPLAIN(query[0], { }, paramEnabled);
        let result  = AQL_EXECUTE(query[0], { }, paramEnabled).json;

        assertEqual(-1, plan.plan.rules.indexOf(ruleName), query[0]);
        assertEqual(result, query[1], query);
      });
    },

    testNestingRuleNotUsed1 : function() {
      const query = `
         LET items = [{_id: 'ID'}]
         FOR item1 IN items
            FOR item2 IN items
              FOR item3 IN items
                COLLECT id1 = item1._id INTO g1
                COLLECT id2 = g1[0].item2._id INTO g2
                RETURN g2[0]
         `;
      const expected = [
          { "g1" : [ { "item1" : { "_id" : "ID" },
                       "item2" : { "_id" : "ID" },
                       "item3" : { "_id" : "ID" },
                     }
                   ],
            "id1" : "ID"
          }
        ];
      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertEqual(-1, explain.plan.rules.indexOf(ruleName));
    },

    testNestingRuleNotUsed2 : function() {
      const query = `
         LET items = [{_id: 'ID'}]
         FOR item1 IN items
            FOR item2 IN items
              FOR item3 IN items
                COLLECT id = item1._id INTO first
                COLLECT id2 = first[0].item2._id INTO other
                let b = other[0]
                RETURN b
         `;
      const expected = [
          { "first" : [ { "item1" : { "_id" : "ID" },
                           "item2" : { "_id" : "ID" },
                           "item3" : { "_id" : "ID" },
                         }
                      ],
            "id" : "ID"
          }
        ];
      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertEqual(-1, explain.plan.rules.indexOf(ruleName));
    },

    testNestingRuleNotUsed3 : function() {
      const query = `
         LET items = [{_id: 'ID'}]
         FOR item1 IN items
            FOR item2 IN items
              FOR item3 IN items
                COLLECT id = item1._id INTO first
                COLLECT id2 = first[0].item2._id INTO other
                RETURN other
         `;
      const expected = [ [
          { "first" : [ { "item1" : { "_id" : "ID" },
                           "item2" : { "_id" : "ID" },
                           "item3" : { "_id" : "ID" },
                         }
                      ],
            "id" : "ID"
          }
       ] ];
      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertEqual(-1, explain.plan.rules.indexOf(ruleName));
    },

    testNestingRuleUsed1 : function() {
      const query = `
         LET items = [{_id: 'ID'}]
         FOR item1 IN items
            FOR item2 IN items
              FOR item3 IN items
                COLLECT id = item1._id INTO first
                COLLECT id2 = first[0].item2._id INTO other
                RETURN other[0].id
         `;
      const expected = [ "ID" ];
      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertNotEqual(-1, explain.plan.rules.indexOf(ruleName));
    },

    testNestingRuleUsed2 : function() {
      const query = `
         LET items = [ { "_id" : 42 }]
         FOR item1 IN items
            FOR item2 IN items
              FOR item3 IN items
                COLLECT id = item1._id INTO first
                COLLECT id2 = first[0].item2._id INTO other
                let b = 1 + other[0].first[0].item1._id
                RETURN b
         `;
      const expected = [ 43 ];
      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertNotEqual(-1, explain.plan.rules.indexOf(ruleName));

    },

    testNestingRuleUsed3 : function() {
      const query = `
         LET items = [ { "_id" : 42 }]
         FOR item1 IN items
            FOR item2 IN items
              FOR item3 IN items
                COLLECT id = item1._id INTO first
                COLLECT id2 = first[0].item2._id INTO other
                let b = [  other[0].first[0], "blub" ]
                RETURN b
         `;
      const expected = [
          [ { "item3" : { "_id" : 42 },
              "item1" : { "_id" : 42 },
              "item2" : { "_id" : 42 } } , "blub" ] ];

      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertNotEqual(-1, explain.plan.rules.indexOf(ruleName));
    },

    testNestingRuleUsed4 : function() {
      const query = `
         LET items = [{_id: 'ID'}]
         FOR item1 IN items
            FOR item2 IN items
              FOR item3 IN items
                COLLECT id = item1._id INTO first
                COLLECT id2 = first[0].id INTO other
                RETURN other[0].id
         `;
      const expected = [ "ID" ];
      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertNotEqual(-1, explain.plan.rules.indexOf(ruleName));
    },

    testNestingRuleUsed5 : function() {
      const query = `
         LET items = [{_id: 'ID'}]
         FOR item1 IN items
            FOR item2 IN items
              FOR item3 IN items
                COLLECT id = item1._id INTO first
                COLLECT id2 = first[0].id INTO other
                RETURN other[0].first
         `;
      const expected = [ [ { "item3" : { "_id" : "ID" },
                             "item1" : { "_id" : "ID" },
                             "item2" : { "_id" : "ID" } } ] ];
      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertNotEqual(-1, explain.plan.rules.indexOf(ruleName));
    },

    testNestingRuleUsed6 : function() {
      const query = `
         LET items = [{_id: 'ID'}]
         FOR item1 IN items
            FOR item2 IN items
              FOR item3 IN items
                COLLECT id = item1._id INTO first
                COLLECT id2 = first[0].id INTO other
                RETURN other[0].first[0]
         `;
      const expected = [ { "item3" : { "_id" : "ID" },
                           "item1" : { "_id" : "ID" },
                           "item2" : { "_id" : "ID" } } ];
      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertNotEqual(-1, explain.plan.rules.indexOf(ruleName));
    },

    testNestingRuleUsed7 : function() {
      const query = `
         LET items = [{_id: 'ID'}]
         FOR item1 IN items
           COLLECT unused1 = item1._id INTO first
           COLLECT unused2 = first[0].item1._id INTO other
           RETURN other[0].first[0].item1._id
         `;
      const expected = [ "ID" ];

      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertNotEqual(-1, explain.plan.rules.indexOf(ruleName));
    },

    // Regression test for https://github.com/arangodb/arangodb/issues/14807.
    // This resulted in an invalid memory access in the removeCollectVariablesRule.
    testIssue14807 : function () {
      const query = `
        for u in union([],[])
        collect test = u.v into g
        return Distinct {
          Test: test,
            Prop1: MIN(g[*].u.someProp),
            Prop2: MAX(g[*].u.someProp)
        }
      `;

      const expectedResults = [];

      const results = AQL_EXECUTE(query);
      assertEqual(expectedResults, results.json);
    },

    testCollectIntoBeforeCollectWithoutInto : function () {
      const query = `
         LET items = [{_id: 'ID'}]
         FOR item1 IN items
           COLLECT unused1 = item1._id INTO first
           COLLECT unused2 = first[0].item1._id
           RETURN unused2
         `;
      const expected = [ "ID" ];

      let resultEnabled = AQL_EXECUTE(query, { }, paramEnabled).json;
      assertEqual(expected, resultEnabled);

      let explain =  AQL_EXPLAIN(query, { }, paramEnabled);
      assertNotEqual(-1, explain.plan.rules.indexOf(ruleName));
    },

  };
}

jsunity.run(optimizerRuleTestSuite);

return jsunity.done();
