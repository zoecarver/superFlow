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
#include "ast.h"
#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

Value *NumberAST::codeGen() {
  return ConstantFP::get(mContext, APFloat(val));
}

Value *VariableAST::codeGen() {
  Value *v = namedValues[name];
  if (!v) Parser::LogErrorV("Unknown Variable Name");

  return mBuilder.CreateLoad(v, name.c_str()); // Its okay to rutun v even if it is a nullptr - that will just bubble up the error
}

Value *VarAST::codeGen() {
  Function *func = mBuilder.GetInsertBlock()->getParent();
  
  std::string name = var.first;
  
  AST *init = var.second.get();

  Value *initVal;
  if (init) {
    initVal = init->codeGen();
    if (!initVal) return nullptr;
  } else {
    initVal = (type == VarType::type_double) ? Constant::getNullValue(dType) : Constant::getNullValue(vType);
  }

  AllocaInst *alloca = (type == VarType::type_double) ? 
    entryCreateBlockAlloca(func, name) : entryCreateBlockAllocaArray(func, name);
  namedValues[name] = alloca;

  return mBuilder.CreateStore(initVal, alloca);
}

Value *ArrayAST::codeGen() {
  Type *dType = Type::getDoubleTy(mContext);
  Type *vectorType = VectorType::get(dType, 4);
  Value *emptyVector = UndefValue::get(vectorType);
  Constant *indexT = Constant::getIntegerValue(dType, llvm::APInt(32, 0));
  
  std::vector<Value *> numberValues;
  Instruction *fullVector = InsertElementInst::Create(emptyVector, numbers[0]->codeGen(), indexT);
  mBuilder.Insert(fullVector);

  int i = 0;

  for(auto const& n: numbers) {
    if (i == 0) goto end; // We already did this one when we set fullVector
    indexT = Constant::getIntegerValue(dType, llvm::APInt(32, i));
    fullVector = InsertElementInst::Create(fullVector, n->codeGen(), indexT);
    mBuilder.Insert(fullVector);
end:;
    i++;
  }
  
  return fullVector;
}

Value *ArrayElementAST::codeGen() {
  std::vector<Value *> emptyArgs; // We need to pass it this so we make an empty one
  Function *func = mBuilder.GetInsertBlock()->getParent();
  AllocaInst *alloca = namedValues[name];
  Value *refVector = mBuilder.CreateLoad(alloca, name.c_str()); 
  Constant *indexT = Constant::getIntegerValue(dType, llvm::APInt(32, index));

  Instruction *newVector = ExtractElementInst::Create(refVector, indexT);

  mBuilder.Insert(newVector);
  return newVector;
}

Value *ArrayElementSetAST::codeGen() {
  std::vector<Value *> emptyArgs; // We need to pass it this so we make an empty one
  Function *func = mBuilder.GetInsertBlock()->getParent();
  AllocaInst *alloca = namedValues[name];
  Value *refVector = mBuilder.CreateLoad(alloca, name.c_str()); 
  Constant *indexT = Constant::getIntegerValue(dType, llvm::APInt(32, index));

  Instruction *newVector = InsertElementInst::Create(refVector, newVal->codeGen(), indexT);

  mBuilder.Insert(newVector);
  return Constant::getNullValue(vType);
}

Value *BinaryAST::codeGen() {
  Value *L = LHS->codeGen();
  Value *R = RHS->codeGen();

  if (!L || !R) return nullptr;

  switch(option) {// TODO: Oporator, not option :P
    case '+': // TODO: these should be enums
      return mBuilder.CreateFAdd(L, R, "addtmp"); // TODO: Thesse hsould also be enums
    case '-': 
      return mBuilder.CreateFSub(L, R, "subtmp");
    case '*':
      return mBuilder.CreateFMul(L, R, "multmp");
    case '<':
      L = mBuilder.CreateFCmpULT(L, R, "cmptmp");
      return mBuilder.CreateUIToFP(L, Type::getDoubleTy(mContext), "booltmp"); // Convertes the above comparison to a double (0.0 or 1.0)
    default:
      return Parser::LogErrorV("invalid binary oporator \n Oporator must be: (+, -, *, <)");
  }
}

Value *CallAST::codeGen() {
  Function *fCallee = mModule->getFunction(callee);
  if (!fCallee) return Parser::LogErrorV("Unknown function referenced");

  if (fCallee->arg_size() != arguments.size()) return Parser::LogErrorV("Incorrect # arguments passed");

  std::vector<Value *> argsV;
  for (unsigned i = 0, e = arguments.size(); i != e; ++i) {
    argsV.push_back(arguments[i]->codeGen());
    if (!argsV.back()) return nullptr;
  }

  return mBuilder.CreateCall(fCallee, argsV, "calltmp");
}

Function *PrototypeAST::codeGen() {
  std::vector<Type*> doubles(arguments.size(), dType);
  
  FunctionType *FT = FunctionType::get(type == VarType::type_double ? 
      dType : VectorType::get(dType, 4)
      , doubles, false);
  Function *f = Function::Create(FT, Function::ExternalLinkage, name, M);

  unsigned index = 0;
  for (auto &arg: f->args())
    arg.setName(arguments[index++]);

  return f;
}

void PrototypeAST::createArgumentAllocas(Function *func) {
  Function::arg_iterator argItr = func->arg_begin();
  for (unsigned index = 0, end = arguments.size(); index != end; ++index, ++argItr) {
    AllocaInst *alloca = entryCreateBlockAlloca(func, arguments[index]);

    mBuilder.CreateStore(argItr, alloca);

    namedValues[arguments[index]] = alloca;
  }
}


Function *FuncAST::codeGen() {
  Function *func = mModule->getFunction(prototype->getName()); // this checks if it already exists as part of llvm
  
  if (!func) func = prototype->codeGen();
  
  if (!func) return nullptr;

  if (!func->empty()) return (Function*) Parser::LogErrorV("Function cannot be redefined.");

  BasicBlock *block = BasicBlock::Create(mContext, "entry", func);
  mBuilder.SetInsertPoint(block);

  for (auto &arg: func->args()) {
    AllocaInst *alloca = entryCreateBlockAlloca(func, arg.getName());

    mBuilder.CreateStore(&arg, alloca);

    namedValues[arg.getName()] = alloca;
  }

  if (Value *returnValue = body->codeGen()) {
    mBuilder.CreateRet(returnValue);

    llvm::verifyFunction(*func);

    return func;
  }

  func->removeFromParent(); // If there is an error get rid of the function
  return nullptr;
}

// Long Functions have multiple expressions in them
Function *LongFuncAST::codeGen() {
  Function *func = prototype->codeGen();
  
  if (!func) return nullptr;

  if (!func->empty()) return (Function*) Parser::LogErrorV("Function cannot be redefined.");

  BasicBlock *block = BasicBlock::Create(mContext, "entry", func);
  mBuilder.SetInsertPoint(block);

  for (auto &arg: func->args()) {
    AllocaInst *alloca = entryCreateBlockAlloca(func, arg.getName());

    mBuilder.CreateStore(&arg, alloca);

    namedValues[arg.getName()] = alloca;
  }

  Constant *exprConst;
  for (auto &expr: body) {
    Value *exprValue = expr->codeGen();
    exprConst = dyn_cast<Constant>(exprValue);
    mBuilder.Insert(exprConst);
  }

  if (exprConst) { // TODO: make a return for null value if this is null
    mBuilder.CreateRet(exprConst); // TODO: This should not matter, but we might want to change this to exprValue

    llvm::verifyFunction(*func);

    return func;
  } else {
    mBuilder.CreateRet(Constant::getNullValue(dType));

    llvm::verifyFunction(*func);

    return func;
  }

  std::cerr << "Could not create function - null value" << std::endl;
  func->removeFromParent(); // If there is an error get rid of the function
  return nullptr;
}

// Function *AnnonFuncAST::codeGen() {
//   Function *func = prototype->codeGen();
//   
//   if (!func) return nullptr;
// 
//   if (!func->empty()) return (Function*) Parser::LogErrorV("Function cannot be redefined.");
// 
//   if (!annonBlock) annonBlock = BasicBlock::Create(mContext, "entry", func);
//   mBuilder.SetInsertPoint(annonBlock);
// 
//   Constant *exprConst;
//   for (auto &expr: body) {
//     Value *exprValue = expr->codeGen();
//     exprConst = dyn_cast<Constant>(exprValue);
//     mBuilder.Insert(exprConst);
//   }
// 
//   mBuilder.CreateRet(Constant::getNullValue(dType));
//   llvm::verifyFunction(*func);
// 
//   return func;
// }

Value *ForAST::codeGen() {
  Function *func = mBuilder.GetInsertBlock()->getParent();

  AllocaInst *alloca = entryCreateBlockAlloca(func, varName);

  Value *startV = start->codeGen();
  if (!startV) return nullptr;

  mBuilder.CreateStore(startV, alloca);

//  BasicBlock *preHeaderBlock = mBuilder.GetInsertBlock();
  BasicBlock *loopBlock = BasicBlock::Create(mContext, "loop", func);

  mBuilder.CreateBr(loopBlock);
  mBuilder.SetInsertPoint(loopBlock);

//  PHINode *var = mBuilder.CreatePHI(dType, 2, varName);
//  var->addIncoming(startV, preHeaderBlock);

  AllocaInst *oldVal = namedValues[varName];
  namedValues[varName] = alloca;

  if (!body->codeGen())
    return nullptr;

  Value *stepVal = nullptr;
  if (step) {
    stepVal = step->codeGen();
    if (!stepVal) return nullptr;
  } else {
    stepVal = ConstantFP::get(mContext, APFloat(1.0)); // If nothing was specified then just add 1.0
  }

  Value *currentVar = mBuilder.CreateLoad(alloca, varName.c_str());
  Value *nextVar = mBuilder.CreateFAdd(currentVar, stepVal, "nextvar");
  mBuilder.CreateStore(nextVar, alloca);

  Value *endCondition = end->codeGen();
  if (!endCondition) return nullptr;

  endCondition = mBuilder.CreateFCmpONE(endCondition, ConstantFP::get(mContext, APFloat(0.0)), "loopcond");

  BasicBlock *loopEndBlock = mBuilder.GetInsertBlock();
  BasicBlock *afterBlock = BasicBlock::Create(mContext, "afterloop", func);

  mBuilder.CreateCondBr(endCondition, loopBlock, afterBlock);

  mBuilder.SetInsertPoint(afterBlock);
//  var->addIncoming(nextVar, loopEndBlock);

  if (oldVal)
    namedValues[varName] = oldVal;
  else
    namedValues.erase(varName);

  return Constant::getNullValue(dType); // For loops always return 0.0
}

Value *PrintAST::codeGen() {
	Constant *calleeFunc = mModule->getOrInsertFunction("printf", 
    FunctionType::get(dType, 
    	true /* this is var arg func type*/
  	)
	);

	std::vector<Value *> argsV;
	for (unsigned i = 0, e = arguments.size(); i != e; ++i)
		argsV.push_back(arguments[i]->codeGen());

	return mBuilder.CreateCall(calleeFunc, argsV, "printfCall");
}
