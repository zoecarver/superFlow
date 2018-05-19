#include "../lexer/lexer.h"
#include "../ast/ast.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Value.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Parser {
  private:
    Lexer *mLexer = new Lexer();
    std::vector<std::string> namedFunctions;

  public:
    // Basics
    int currentToken;
    int getNextToken();
    
    // Binary Parsing
    std::map<char, int> BinaryOpporatorRank;

    // Basic
    static std::unique_ptr<AST> LogError (const char *str);
    static std::unique_ptr<PrototypeAST> LogErrorPlain(const char *str);

    static llvm::Value *LogErrorV(const char *str);

    // Number Parsing
    std::unique_ptr<AST> ParseNumber();
    std::unique_ptr<AST> ParseParens();  
    std::unique_ptr<AST> ParseArray(std::string name);

    // Identification (funcs and vars)
    std::unique_ptr<AST> ParseIdentifier();
    std::unique_ptr<AST> ParsePrimary(std::string name = ""); // Desicion maker
    std::unique_ptr<FuncAST> ParseVariable(VarType type);

    // Binary Parsing eg (4+4)
    int getTokenRank();
    std::unique_ptr<AST> ParseExpression(std::string name = "");

    // Binary opporators
    std::unique_ptr<AST> ParseBinaryOporatorRHS(int exprRank, std::unique_ptr<AST> LHS);

    // Other Things
    std::unique_ptr<PrototypeAST> ParsePrototype();
    std::unique_ptr<FuncAST> ParseDefinition();
    std::unique_ptr<FuncAST> ParseTopLevel();
    std::unique_ptr<PrototypeAST> ParseExtern();
    std::unique_ptr<AST> ParseFor();
    std::unique_ptr<AST> ParsePrint();
};
