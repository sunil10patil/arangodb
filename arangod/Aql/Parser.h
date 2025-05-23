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

#include "Aql/Ast.h"
#include "Basics/Common.h"

namespace arangodb {
namespace aql {
struct AstNode;
class QueryContext;
struct QueryResult;
class QueryString;

/// @brief the parser
class Parser {
  Parser(Parser const&) = delete;

 public:
  /// @brief create the parser
  explicit Parser(QueryContext&, Ast&, QueryString&);

  /// @brief destroy the parser
  ~Parser();

 public:
  /// @brief return the ast during parsing
  Ast* ast() { return &_ast; }

  /// @brief return the query during parsing
  QueryContext& query() { return _query; }

  /// @brief return the scanner
  void* scanner() const { return _scanner; }

  /// @brief a pointer to the start of the query string
  char const* queryStringStart() const { return _queryStringStart; }

  /// @brief return the remaining length of the query string to process
  size_t remainingLength() const { return _remainingLength; }

  /// @brief return the current marker position
  char const* marker() const { return _marker; }

  /// @brief set the current marker position
  void marker(char const* marker) { _marker = marker; }

  /// @brief return the current parse position
  size_t offset() const { return _offset; }

  /// @brief adjust the current parse position
  void increaseOffset(int offset) { _offset += static_cast<size_t>(offset); }

  /// @brief adjust the current parse position
  void increaseOffset(size_t offset) { _offset += offset; }

  void decreaseOffset(int offset) { _offset -= static_cast<size_t>(offset); }

  /// @brief adjust the current parse position
  void decreaseOffset(size_t offset) { _offset -= offset; }

  /// @brief fill the output buffer with a fragment of the query
  void fillBuffer(char* result, size_t length) {
    memcpy(result, _buffer, length);
    _buffer += length;
    _remainingLength -= length;
  }

  /// @brief set data for write queries
  bool configureWriteQuery(AstNode const*, AstNode* optionNode);

  /// @brief parse the query
  void parse();

  /// @brief parse the query and return parse details
  QueryResult parseWithDetails();

  /// @brief register a parse error, position is specified as line / column
  void registerParseError(ErrorCode errorCode, char const* format,
                          std::string_view data, int line, int column);

  /// @brief register a parse error, position is specified as line / column
  void registerParseError(ErrorCode errorCode, std::string_view data, int line,
                          int column);

  /// @brief register a warning
  void registerWarning(ErrorCode errorCode, std::string_view data, int line,
                       int column);

  /// @brief push an AstNode array element on top of the stack
  /// the array must be removed from the stack via popArray
  void pushArray(AstNode* array);

  /// @brief pop an array value from the parser's stack
  /// the array must have been added to the stack via pushArray
  AstNode* popArray();

  /// @brief push an AstNode into the array element on top of the stack
  void pushArrayElement(AstNode*);

  /// @brief push an AstNode into the object element on top of the stack
  void pushObjectElement(char const*, size_t, AstNode*);

  /// @brief push an AstNode into the object element on top of the stack
  void pushObjectElement(AstNode*, AstNode*);

  /// @brief push a temporary value on the parser's stack
  void pushStack(void*);

  /// @brief pop a temporary value from the parser's stack
  void* popStack();

  /// @brief peek at a temporary value from the parser's stack
  void* peekStack();

 private:
  /// @brief a pointer to the start of the query string
  QueryString const& queryString() const { return _queryString; }

  /// @brief the query
  QueryContext& _query;

  /// @brief abstract syntax tree for the query, build during parsing
  Ast& _ast;

  /// @brief query string (non-owning!)
  QueryString& _queryString;

  /// @brief lexer / scanner used when parsing the query (Aql/tokens.ll)
  void* _scanner;

  char const* _queryStringStart;

  /// @brief currently processed part of the query string
  char const* _buffer;

  /// @brief remaining length of the query string, used during parsing
  size_t _remainingLength;

  /// @brief current offset into query string, used during parsing
  size_t _offset;

  /// @brief pointer into query string, used temporarily during parsing
  char const* _marker;

  /// @brief a stack of things, used temporarily during parsing
  std::vector<void*> _stack;
};
}  // namespace aql
}  // namespace arangodb

/// @brief forward for the parse function provided by the parser (.y)
int Aqlparse(arangodb::aql::Parser*);

/// @brief forward for the init function provided by the lexer (.l)
int Aqllex_init(void**);

/// @brief forward for the shutdown function provided by the lexer (.l)
int Aqllex_destroy(void*);

/// @brief forward for the context function provided by the lexer (.l)
void Aqlset_extra(arangodb::aql::Parser*, void*);
