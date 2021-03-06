//===--------------  LCM.cpp - Lazy Code Motion --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//  
//      
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/IR/InstIterator.h"
#include "valueNumbering.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;
STATISTIC(NumLCMCompInserted,  "[526] Number of computations INSERTED");
STATISTIC(NumLCMCompReplaced,   "[526] Number of computations REPLACED");
STATISTIC(NumLCMMaskedInserts,   "[526] Number of INSERTS masked out");
STATISTIC(NumLoopCodeMotion,    "[526] Number of loop invariant code motion");
STATISTIC(NumValueNumbers, "[526] Total count of distinct value numbers" );
STATISTIC(BitVectorWidth, "[526] Value-numbers given a bit-vector slot");

namespace {
  
  // enum for the bitVectors associated with each basic block
  enum bitVectors {
   ANTLOC = 0,
   TRANSP,
   XCOMP,
   ANTIN, 
   ANTOUT,
   AVAILIN,
   AVAILOUT,
   EARLIN,
   EARLOUT,
   DELAYIN,
   DELAYOUT,
   LATESTIN,
   LATESTOUT,
   ISOLIN,
   ISOLOUT,
   INSERTIN,
   INSERTOUT,
   REPLACEIN,
   REPLACEOUT,
   TOTALBITVECTORS // = 19
  };
  
  struct LCM : public FunctionPass {
    
    typedef std::vector<SmallBitVector*> dfva;
    
    static char ID;
    LCM() : FunctionPass(ID) {}
    bool runOnFunction(Function &F);
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesCFG();
      AU.addRequired<LoopInfo>();
      //AU.addRequired<DominatorTree>();
      //AU.addPreserved<DominatorTree>();  
      AU.addRequired<DominatorTreeWrapperPass>();
      AU.addPreserved<DominatorTreeWrapperPass>();
      AU.addRequiredID(BreakCriticalEdgesID);
    }

    private:
      Function* Func;
      RPO* rpo;
      LoopInfo* LI;
      DominatorTree *DT;

      // Maps each Basic Block to a vector of SmallBitVectors, each of which
      // represents a property as defined in bitVectors enum 
      DenseMap<BasicBlock*, dfva*> BBMap;
     
      uint32_t label;

      // This list captures the instructions which we need to delete as part of
      // the PRE-REPLACE stage
      std::vector<Instruction*> deadList;
      
      // Each bitVector is only as wide as the number of value numbers which occur 
      // more than once in the program (except if they are in a loop) 
      uint32_t bitVectorWidth;

      // Holds the new alloca instructions created for INSERT-REPLACE
      std::vector<AllocaInst*> allocaVector;

      // To mask out the insert points where no expression can be cloned and
      // inserted
      SmallBitVector insertMask;

      // functions - description along with the definition 
      void performDFA() ;
      void initializeDFAFramework();
      void dumpSmallBitVector(SmallBitVector*);
      void performConstDFA();
      void performGlobalDFA();
      void changeIR();
      void setupInsertReplace();
      void doInsertReplace(uint32_t vn, BasicBlock* BB, bool insert, bool replace);
      void cleanUp();
      
      void callFramework(uint32_t out, uint32_t in, std::vector<uint32_t> alpha, std::vector<uint32_t> beta, std::vector<uint32_t> gamma, bool meetOp, bool bottom, bool top, bool direction);
      void calculateEarliest();
      void calculateLatest();
      void calculateInsertReplace();
      Instruction* getInstructionToClone(uint32_t vn, BasicBlock* BB);
      void buildInsertMask(SmallBitVector insertVector, BasicBlock* BB);
      SmallBitVector getSBVForExpression(std::vector<uint32_t> input, BasicBlock* BB);
      SmallBitVector getSBVForElement(uint32_t num, BasicBlock* BB);
      Value* getExpressionFromBasicBlock(uint32_t vn, BasicBlock* BB);
      void printFlowEquations();
      void performLocalCSE();

      SmallBitVector calculateAntloc(BasicBlock*);
      SmallBitVector calculateTrans(BasicBlock*);
      SmallBitVector calculateXcomp(BasicBlock*);
  };
}  

char LCM::ID = 0;
static RegisterPass<LCM> X("lcm",
			    "Lazy Code Motion (CS526)",
			    false /* does not modify the CFG */,
			    false /* transformation, not just analysis */);

/*******************************************************************
 * Function :   runOnFunction
 * Purpose  :   Entry point for LCM
********************************************************************/
bool LCM::runOnFunction(Function &F) 
{
  Func = &F;
  LI = &getAnalysis<LoopInfo>();
  //DT = &getAnalysis<DominatorTree>();
  DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  
  bool Changed = false;
  uint32_t endCount, startCount = NumLCMCompInserted;

  // 'rpo' is the interface theperform the value numbering. Do value numbering.
  rpo = new RPO(F,LI);
  rpo->performVN(); 
  rpo->print();  
  
  label = 0;
  bitVectorWidth = rpo->getRepeatedValues().size();
  allocaVector.insert(allocaVector.begin(), bitVectorWidth, NULL);
  insertMask.resize(bitVectorWidth, false); 

  if(0 == bitVectorWidth) {
    DEBUG(errs() << "Nothing to do\n"); 
    return Changed;
  }
  
  // run PRE
  performLocalCSE();
  performDFA();
  changeIR();
  
  // handle stats
  //NumValueNumbers += rpo->getMaxValueNumber();  
  BitVectorWidth += bitVectorWidth;
  NumLCMMaskedInserts += insertMask.count();
  endCount = NumLCMCompInserted;
  assert(endCount >= startCount && "Stat counting error");
  if(!(endCount == startCount))
    Changed = true;

  cleanUp();  // free-up memory
  return Changed;
}

/*******************************************************************
 * Function :   performLocalCSE
 * Purpose  :   To do local CSE (on each BB)
********************************************************************/
// Find all the instructions in a basic block which have the same value number.
// Only the first one of these instructions is required, and the rest are marked
// for deletion 
void LCM::performLocalCSE() 
{
  for (Function::iterator BI = Func->begin(), E = Func->end(); BI != E; ++BI) {
    BasicBlock* BB = BI;
    DEBUG(errs() << *BB << "\n");
    for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
      Instruction* BBI = I;
      DEBUG(errs() << "\tInstruction: " << *BBI << " \n");
      SmallVector<Value*, 8> equalValues;
      rpo->getEqualValues(&*BBI, equalValues);

      for (unsigned j = 0, e = equalValues.size(); j != e;) {
        Instruction* EQI = dyn_cast<Instruction>(equalValues[j]);
        if(BBI == EQI) {
          j++;
          continue;
        }
        if(BB == EQI->getParent()) {
          j++;
          
          EQI->replaceAllUsesWith(BBI);
          rpo->eraseValue(EQI);
          deadList.push_back(EQI);
        } else {
          ++j;
        }
      }
    }
  }
}

/*******************************************************************
 * Function :   performDFA
 * Purpose  :   Entry point for DFA
********************************************************************/
void LCM::performDFA() {

  initializeDFAFramework();
  
  // This function is responsible for calculating the local sets for each basic
  // block, namely the transp, xcomp and antloc bitvectors
  performConstDFA();
  
  // This function calls the various data flow equations of LCM
  performGlobalDFA();
  printFlowEquations();
}

/*******************************************************************
 * Function :   initializeDFAFramework
 * Purpose  :   Allocates the bitvectors 
********************************************************************/
void LCM::initializeDFAFramework() {

  for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) {

    dfva* dfvaInstance = new dfva();
    for(uint32_t i = 0; i < TOTALBITVECTORS; i++) 
      dfvaInstance->push_back(new SmallBitVector(bitVectorWidth, false));
    
    BBMap[BB] = dfvaInstance;
  }
}

/*******************************************************************
 * Function :   performConstDFA
 * Purpose  :   Calculate the local sets (Transp, Xcomp, Antloc) for all the BBs
********************************************************************/
void LCM::performConstDFA()
{

  dfva* dfvaInstance;
  for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) {
    dfvaInstance = BBMap[BB];

    *((*dfvaInstance)[TRANSP]) = calculateTrans(BB);
    *((*dfvaInstance)[ANTLOC]) = calculateAntloc(BB);
    *((*dfvaInstance)[XCOMP]) = calculateXcomp(BB);
  }
}

/*******************************************************************
 * Function :   calculateTrans
 * Purpose  :   Fill out the TRANSP bit-vector for the basic block
********************************************************************/
SmallBitVector LCM::calculateTrans(BasicBlock* BB) {

  DEBUG(errs() << "Finding Trans BB\n");

  // TRANSP for all the value-numbers is marked as true initially. It can only
  // be changed to false for the basic block if the leader of that value-number 
  // has either of its operands being defined in that basic block
  SmallBitVector returnValue(bitVectorWidth, true);
  std::vector<Value*> allLeaders = rpo->getAllLeaders();

  for(uint32_t i = 0; i < allLeaders.size(); i++) {
    Instruction* I = cast<Instruction>(allLeaders[i]);
    uint32_t VI = rpo->getBitVectorPosition(I);
    if(VI >= bitVectorWidth) 
      continue;
    DEBUG(errs() << "\tLeader Instruction: " << *I << "Value " << VI << " size " << bitVectorWidth<<" \n");
    
    assert(rpo->getLeader(I) == I && "This instruction should have been its own leader; we screwed up somewhere");
    
    for (User::op_iterator OP = I->op_begin(), E = I->op_end(); OP != E; ++OP) {
    
      if(Instruction *operandIns = dyn_cast<Instruction>(OP)) { 
        DEBUG(errs() << "\toperand Instruction: " << *operandIns << "\n");
        if(BB == operandIns->getParent()) {
          returnValue[VI] = false;
          break;
        }
      }
    }
  }

  return returnValue;
}

/*******************************************************************
 * Function :   calculateAntloc
 * Purpose  :   defined(B) <intersection> transp(B)
 *              defined(B) = {v | v is generated in B}
********************************************************************/
SmallBitVector LCM::calculateAntloc(BasicBlock* BB) {

  DEBUG(errs() << "\nFinding Antloc BB\n");
  dfva* dfvaInstance = BBMap[BB];
  SmallBitVector transp = *(*dfvaInstance)[TRANSP] ;

  SmallBitVector returnValue(bitVectorWidth, false);

  for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
    Instruction* BBI = I;
    uint32_t  VI  = rpo->getBitVectorPosition(BBI);  
    DEBUG(errs() << "\tInstruction-: " << *BBI << " Value " << VI << " Size " << bitVectorWidth<< " \n");
    if(VI >= bitVectorWidth) 
      continue;

    // ANTLOC = DEFINED <intersection> TRANSP
    if(true == transp[VI]) {
      DEBUG(errs() << "\tAntloc \n"); 
      returnValue[VI] = true;
    }
  }
  return returnValue;
}

/*******************************************************************
 * Function :   calculateXcomp
 * Purpose  :   defined(B) <intersection> ~transp(B)
 *              defined(B) = {v | v is generated in B}
********************************************************************/
SmallBitVector LCM::calculateXcomp(BasicBlock* BB)
{
  SmallBitVector returnValue(bitVectorWidth, false);

  for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
    Instruction* BBI = I;
    uint32_t  VI  = rpo->getBitVectorPosition(BBI);  
    if(VI >= bitVectorWidth) 
      continue;
    DEBUG(errs() << "\tInstruction: " << *BBI << " Value " << VI  << " \n");
    Value* V = rpo->getLeader(BBI);
    Instruction* LI =  cast<Instruction>(V);
    
    assert(LI != NULL && "getLeader() returned NULL");    
    DEBUG(errs() << "\tLeader Instruction: " << *LI << " \n");

    for (User::op_iterator OP = LI->op_begin(), E = LI->op_end(); OP != E; ++OP) {
      if(Instruction *operandIns = dyn_cast<Instruction>(OP)) {
        if(BB == operandIns->getParent()) {
          returnValue[VI] = true;
          assert(BBI == LI && "If XCOMP(BB) is true for a value V, then V must be its own leader");
          break;
        }
      }
    }
  }

  return returnValue;
}

// See desciption of getSBVForExpression function
SmallBitVector LCM::getSBVForElement(uint32_t num, BasicBlock* BB) {
    
  dfva* dfvaInstance = BBMap[BB];
  uint32_t pos;
  bool compliment = false;

  if(num < TOTALBITVECTORS)
    pos = num;
  else {
    pos = num - TOTALBITVECTORS;
    compliment = true;
  }

  SmallBitVector returnValue = *(*dfvaInstance)[pos];
  if(compliment)
     return ~returnValue;

  return returnValue;
}

// For a basic block, this function takes in an expression (in the form of
// std::vector) and returns the SmallBitVector computed for that expression.
// The vector values are inserted based on the enum definition 
//
//    e.g. ~ANTLOC U ~TRANSP .The corresponding input vector would be :
//     19 - for ~ANTLOC (0 for ANTLOC, plus 19 since it is compliment)
//     0 - for union operation
//     20 - for ~ TRANSP (1 for TRANSP, plus 19 since it is compliment)
//
// Note that 19 is used in example above since TOTALBITVECTORS enum = 19
// The individual values of an expression (~ANTLOC and ~TRANSP) are computed by
// the function getSBVForElement()
SmallBitVector LCM::getSBVForExpression(std::vector<uint32_t> input, BasicBlock* BB) {
    
  SmallBitVector returnValue(bitVectorWidth, false);
  bool first = true;

  for(uint32_t I = 0; I < input.size(); ++I) {
    if(first) {
      first = false;
      returnValue = getSBVForElement(input[I], BB);
    }  

    // this position in the vector is for an union/intersection operator
    // 1 = intersection, 0 = union
    if(I%2 != 0) {
      if(input[I])    
        returnValue &= getSBVForElement(input[++I], BB);
      else
        returnValue |= getSBVForElement(input[++I], BB);
    }
  }

  return returnValue;
}

// For a given value number and a basic block BB, check if BB contains the
// value number. The function return the associated Value*, and NULL otherwise
Value* LCM::getExpressionFromBasicBlock(uint32_t vn, BasicBlock* BB) {

  bool match = false;
  Value* returnValue = NULL;

  for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
    if(rpo->getNumberForValue(&*I) == vn){
    assert(!match && "Two expressions in a basic block can not have the same value number post local CSE. Please ensure this function is called after local CSE is performed");
    match = true;
    returnValue = &*I;
    }
  }
  
  return returnValue;
}

// This is the generalized data-flow equation solving framework. This works for
// all the equations involved in Lazy Code Motion. Inputs are the following :
// out, in : enum values for the SmallBitVector associated with the data flow
// property being computed at In[B] and Out[B]
// alpha, beta, gamma : std::vectors which describe the expressions that form
// the GEN and KILL of the data flow equations 
// meetOp : 1=Intersection, 0=Union
// bottom : lattice property (0=null, 1=all)
// top : lattice property (0=null, 1=all)
// direction : 1=forward data flow, 0=backward data flow

//  e.g for DELAY (check out report for derivation of alpha, beta, gamma)
//  in : DELAYIN
//  out : DELAYOUT
//  alpha : EARLIN
//  beta : vector with entries - 21 (~XCOMP), 1 (Intersection), DELAYOUT
//  gamma : vector with entries - DELAYIN, 1 (Intersection), 19 (~ANTLOC), 0 (Union), EARLOUT
//  meetOp : 1 (intersection)
//  bottom : 0 (null)
//  top : 1 (all)
//  direction : 1 (fwd.)
void LCM::callFramework(uint32_t out, uint32_t in, std::vector<uint32_t> alpha, std::vector<uint32_t> beta, std::vector<uint32_t> gamma, bool meetOp, bool bottom, bool top, bool direction) {

  bool Changed = false;
  SmallBitVector allTrue(bitVectorWidth, true);
  SmallBitVector allFalse(bitVectorWidth, false);
                                       
  // direction == 1 : Forward Data Flow
  if(direction) {

    // initialize OUT set of each basic block to top
    for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) {
      dfva* dfvaInstance = BBMap[BB];
      *(*dfvaInstance)[out] = (top ? allTrue : allFalse);
    }

    do {
      Changed = false;
      ReversePostOrderTraversal<Function*> RPOT(Func);
      
      for (ReversePostOrderTraversal<Function*>::rpo_iterator I = RPOT.begin(), E = RPOT.end(); I != E; ++I) {
        BasicBlock* BB = *I;
        dfva* dfvaInstance = BBMap[BB];
        SmallBitVector* outVector = (*dfvaInstance)[out];
        SmallBitVector* inVector = (*dfvaInstance)[in];

        // bitVector for in[B] of start node
        SmallBitVector& initVector = (bottom ? allTrue : allFalse);

        // this vector would be initialized accordingly later by the 
        // first predecessor while taking a meet over predecessors
        SmallBitVector meetOverPreds(bitVectorWidth, false);

        // go over predecessors and take a meet
        bool first = true;
        for(pred_iterator PI = pred_begin(BB), PE = pred_end(BB); PI!=PE; ++PI) {
          
          SmallBitVector meetExpression =  getSBVForExpression(beta, *PI);

          if(first) {
           first = false;
           meetOverPreds = meetExpression;
          }
          else {
            if(meetOp)
              meetOverPreds &= meetExpression;
            else
              meetOverPreds |= meetExpression;
          }
        }
   
        // no predecessor, this is the start block s.
        if(first)
          meetOverPreds = initVector;

        // 1st data flow eq. 'In' as a function of 'Out'
        *(inVector) = getSBVForExpression(alpha, BB) | meetOverPreds;   

        SmallBitVector oldOutVector = *(outVector);
        // 2nd data flow eq. 'Out' as a function of 'In'
        *(outVector) = getSBVForExpression(gamma, BB);
        SmallBitVector newOutVector = *(outVector);
        if(oldOutVector != newOutVector)
          Changed = true;
      }
    } while(Changed);
  
  }

  // Backward data-flow analysis
  else {
    
    // initialize IN set of each basic block to top
    for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) {
      dfva* dfvaInstance = BBMap[BB];
      *(*dfvaInstance)[in] = (top ? allTrue : allFalse);
    }

    do {
      Changed = false;
      
      for (po_iterator<BasicBlock *> I = po_begin(&Func->getEntryBlock()), E = po_end(&Func->getEntryBlock()); I != E; ++I) {
        BasicBlock* BB = *I;
        dfva* dfvaInstance = BBMap[BB];
        SmallBitVector* outVector = (*dfvaInstance)[out];
        SmallBitVector* inVector = (*dfvaInstance)[in];

        // bitVector for out[B] of exit node
        SmallBitVector& initVector = (bottom ? allTrue : allFalse);

        // this vector would be initialized accordingly later by the 
        // first successor while taking a meet over successors
        SmallBitVector meetOverSucc(bitVectorWidth, false);

        // go over successors and take a meet
        bool first = true;
        for (succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE; SI++) {
          
          SmallBitVector meetExpression = getSBVForExpression(beta, *SI);

          if(first) {
           first = false;
           meetOverSucc = meetExpression;
          }
          else {
            if(meetOp)
              meetOverSucc &= meetExpression;
            else
              meetOverSucc |= meetExpression;
          }
        }
   
        // no successor, this is the exit block e.
        if(first)
          meetOverSucc = initVector;

        // 1st data flow eq. 'Out' as a function of 'In'
        *(outVector) = getSBVForExpression(alpha, BB) | meetOverSucc; 

        SmallBitVector oldInVector = *(inVector);
        // 2nd data flow eq. 'In' as a function of 'Out'
        *(inVector) = getSBVForExpression(gamma, BB);
        SmallBitVector newInVector = *(inVector);
        if(oldInVector != newInVector)
          Changed = true;
      }
    } while(Changed);
  
  }
}

// For a given value number, this function checks all the instructions
// associated with that value number to find the first which dominates the end
// of the basic block passed as parameter. If no such instruction exits, then
// NULL is returned
Instruction* LCM::getInstructionToClone(uint32_t vn, BasicBlock* BB) {
  
  Instruction* terminator = BB->getTerminator();
  SmallVector<Value*, 8> equalValues;
  rpo->getEqualValues(vn, equalValues);
  bool dominates = false;

  for(SmallVectorImpl<Value *>::iterator I = equalValues.begin(), E = equalValues.end(); I!=E; ++I) {
    Instruction* ins = cast<Instruction>(*I);
    dominates = true;
      
    for (User::op_iterator OP = ins->op_begin(), E_OP = ins->op_end(); OP != E_OP; ++OP) {
      if(Instruction *operandIns = dyn_cast<Instruction>(OP)) {
        if(!DT->dominates(operandIns, terminator)) {
          dominates = false;
          break;
        }
      }
    }

    if(dominates)
      return ins;
  }

  return NULL;
}

// For the provided insertVector (identifying the value-numbers which need
// insertion in this basic block), check if there exists an instruction to
// clone. If not, set the mask!
void LCM::buildInsertMask(SmallBitVector insertVector, BasicBlock* BB) {

  int nextPos = insertVector.find_first();
  
  while(nextPos != -1) {

    // no instruction found that can be cloned and inserted at the end of basic
    // block, set the insertMask for this position
    if(getInstructionToClone(rpo->getVNFromBVPos(nextPos), BB) == NULL)
      insertMask.set(nextPos);

    nextPos = insertVector.find_next(nextPos);
  }
}

// This function calculates the INSERT and REPLACE SmallBitVector for each Basic Block
void LCM::calculateInsertReplace() {
  
  dfva* dfvaInstance;
  for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) {
    dfvaInstance = BBMap[BB];

    // eq. for INSERT
    *(*dfvaInstance)[INSERTIN] = *(*dfvaInstance)[LATESTIN] & ~(*(*dfvaInstance)[ISOLIN]);
    *(*dfvaInstance)[INSERTOUT] = *(*dfvaInstance)[LATESTOUT] & ~(*(*dfvaInstance)[ISOLOUT]);

    buildInsertMask(*(*dfvaInstance)[INSERTIN] | *(*dfvaInstance)[INSERTOUT], BB);

    // eq. for REPLACE
    *(*dfvaInstance)[REPLACEIN] = *(*dfvaInstance)[ANTLOC] & ~(*(*dfvaInstance)[LATESTIN] & *(*dfvaInstance)[ISOLIN]);
    *(*dfvaInstance)[REPLACEOUT] = *(*dfvaInstance)[XCOMP] & ~(*(*dfvaInstance)[LATESTOUT] & *(*dfvaInstance)[ISOLOUT]);
  }
}

// This function calculates the LATESTIN and LATESTOUT SmallBitVector for each Basic Block
void LCM::calculateLatest() {

  dfva* dfvaInstance;
  for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) {
    dfvaInstance = BBMap[BB];
    
    bool first = true;
    SmallBitVector meetOverSucc(bitVectorWidth, false);  

    // go over successors and take a meet
    for (succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE; SI++) {
          
      dfva* succDfvaInstance = BBMap[*SI];
          
      if(first) {
        first = false;
        meetOverSucc = ~(*(*succDfvaInstance)[DELAYIN]);
      }
      else 
        meetOverSucc |= ~(*(*succDfvaInstance)[DELAYIN]);
    }

    // eq. for LATESTIN
    *(*dfvaInstance)[LATESTIN] = *(*dfvaInstance)[DELAYIN] & *(*dfvaInstance)[ANTLOC];

    // eq. for LATESTOUT
    *(*dfvaInstance)[LATESTOUT] = *(*dfvaInstance)[DELAYOUT] & (*(*dfvaInstance)[XCOMP] | meetOverSucc);
  }
}

// This function calculates the EARLIESTIN and EARLIESTOUT SmallBitVector for each Basic Block
void LCM::calculateEarliest() {

  dfva* dfvaInstance;
  for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) {
    dfvaInstance = BBMap[BB];

    bool first = true;
    SmallBitVector meetOverPreds(bitVectorWidth, true);

    // go over predecessors and take a meet
    for(pred_iterator PI = pred_begin(BB), PE = pred_end(BB); PI!=PE; ++PI) {

      dfva* predDfvaInstance = BBMap[*PI];
      SmallBitVector meetExpression = *(*predDfvaInstance)[AVAILOUT] | *(*predDfvaInstance)[ANTOUT];
      meetExpression = ~meetExpression;

      if(first) {
        first = false;
        meetOverPreds = meetExpression;
      }
      else
        meetOverPreds &= meetExpression;
    }

    // eq. for EARLIESTIN
    *(*dfvaInstance)[EARLIN] = *(*dfvaInstance)[ANTIN] & meetOverPreds;

    // eq. for EARLIESTOUT
    *(*dfvaInstance)[EARLOUT] = *(*dfvaInstance)[ANTOUT] & ~(*(*dfvaInstance)[TRANSP]);
  }
}

// This function calls the data-flow framework with differnt 
// initializations for different properties
void LCM::performGlobalDFA() {
  
  std::vector<uint32_t> alpha, beta, gamma;

  // -- ANTICIPATABLE
  alpha.push_back(XCOMP);
  beta.push_back(ANTIN);
  gamma.push_back(TRANSP);
  gamma.push_back(1);
  gamma.push_back(ANTOUT);
  gamma.push_back(0);
  gamma.push_back(ANTLOC);
  callFramework(ANTOUT, ANTIN, alpha, beta, gamma, 1, 0, 1, 0);
  alpha.clear();
  beta.clear();
  gamma.clear();

  // -- AVAILABLE
  beta.push_back(XCOMP);
  beta.push_back(0);
  beta.push_back(AVAILOUT);
  gamma.push_back(ANTLOC);
  gamma.push_back(0);
  gamma.push_back(AVAILIN);
  gamma.push_back(1);
  gamma.push_back(TRANSP);
  callFramework(AVAILOUT, AVAILIN, alpha, beta, gamma, 1, 0, 1, 1);
  beta.clear();
  gamma.clear();

  // EARLIEST is not DATA-FLOW!
  calculateEarliest();

  // -- DELAY
  alpha.push_back(EARLIN);
  beta.push_back(TOTALBITVECTORS + XCOMP);
  beta.push_back(1);
  beta.push_back(DELAYOUT);
  gamma.push_back(DELAYIN);
  gamma.push_back(1);
  gamma.push_back(TOTALBITVECTORS + ANTLOC);
  gamma.push_back(0);
  gamma.push_back(EARLOUT);
  callFramework(DELAYOUT, DELAYIN, alpha, beta, gamma, 1, 0, 1, 1);
  alpha.clear();
  beta.clear();
  gamma.clear();

  // LATEST is not DATA-FLOW!
  calculateLatest();

  // -- ISOLATEDNESS
  beta.push_back(TOTALBITVECTORS + ANTLOC);
  beta.push_back(1);
  beta.push_back(ISOLIN);
  beta.push_back(0);
  beta.push_back(EARLIN);
  gamma.push_back(EARLOUT);
  gamma.push_back(0);
  gamma.push_back(ISOLOUT);
  callFramework(ISOLOUT, ISOLIN, alpha, beta, gamma, 1, 1, 1, 0);
  beta.clear();
  gamma.clear();

  // calculating INSERT and REPLACE positions
  calculateInsertReplace();
}

// print
void LCM::dumpSmallBitVector(SmallBitVector* BV) {

  DEBUG(errs() << "{");
  for(unsigned VI=0; VI < BV->size(); VI++) {
      DEBUG(errs() << (*BV)[VI]);
      DEBUG(errs() << " ");
  }
  DEBUG(errs() << "}\n");
}

// This function allocates the space for the temporaries on the stack, and then
// calls a function to insert and replace expressions
void LCM::changeIR() {

  bool first = true;
  SmallBitVector insertsRequired(bitVectorWidth, false);
  dfva* dfvaInstance;

  // go over all the basic blocks and fill up the insertsRequired
  // SmallBitVector. The size of this vector is the number of allocas required
  for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) {
    dfvaInstance = BBMap[BB];
    
    if(first) {
      insertsRequired = *(*dfvaInstance)[INSERTIN] | *(*dfvaInstance)[INSERTOUT];
      first = false;
      continue;
    }
      
    insertsRequired |= *(*dfvaInstance)[INSERTIN] | *(*dfvaInstance)[INSERTOUT];
  }

  uint32_t allocasRequired = insertsRequired.count();
  uint32_t nextPos = insertsRequired.find_first();
  DEBUG(errs() << allocasRequired << " allocas are required\n");  

  BasicBlock::iterator startInstruction = Func->getEntryBlock().getFirstInsertionPt();

  // get the relevant TYPE and insert ALLOCA at the start of the function
  // the ALLOCA are name preINSERT<value-number>
  for(uint32_t i = 0; i < allocasRequired; i++) {

    uint32_t vn = rpo->getVNFromBVPos(nextPos);
    allocaVector[nextPos] = new AllocaInst(rpo->getLeader(vn)->getType(), "preINSERT"+Twine(vn), startInstruction);
    nextPos = insertsRequired.find_next(nextPos);
  }

  setupInsertReplace();
}

// This function is responsible for adding/removing code for INSERT-REPLACE.
// For INSERT, we clone the expression to be inserted, and store the value to
// a stack location. For REPLACE, we load from a stack location, replace all
// uses of the original instruction with the load instruction, and mark the
// original instruction for deletion
// mem2Reg pass converts the stack operations to register operations
void LCM::doInsertReplace(uint32_t vn, BasicBlock* BB,  bool insert, bool replace) {

  if(replace && !insert && NULL != LI->getLoopFor(BB)) 
    NumLoopCodeMotion++;
  
  Instruction* oldInst = NULL;
  AllocaInst* myAlloca = allocaVector[rpo->getBitVectorPosition(vn)];
  
  // getExpressionFromBasicBlock returns NULL if no instruction of value number
  // 'vn' exists in that BB
  Value* V = getExpressionFromBasicBlock(vn, BB);
  if(V!=NULL)
    oldInst = cast<Instruction>(V);
  
  if(insert) {
     
    NumLCMCompInserted++;
    
    Instruction* newInst;
    // if the expression originally exists in the basic block we clone that,
    // otherwise we clone the leader. In the latter case, the leader dominates 
    // the insertion point (SSA guarantees)
    if(oldInst!=NULL) {
      newInst = oldInst->clone();
      newInst->insertBefore(oldInst);
      new StoreInst(newInst, myAlloca, oldInst);
    }
    else {
      // getInstructionToClone would always return a valid entry since we have
      // already set a mask for the insert points where no cloning is possible
      Instruction* toClone = getInstructionToClone(vn, BB);
      assert(toClone != NULL && "No instruction found for cloning. We should not have made it here");
      newInst = toClone->clone();
      newInst->insertBefore(BB->getTerminator());
      new StoreInst(newInst, myAlloca, BB->getTerminator());
    }

    // add the newly created instruction to the value numbering table
    rpo->addValue(newInst, vn);
  }

  if(replace) {

    NumLCMCompReplaced++;
    
    assert(oldInst != NULL && "Nothing to replace");
    LoadInst* LI = new LoadInst(myAlloca, "preLOAD"+Twine(label++), oldInst);
    oldInst->replaceAllUsesWith(LI);
    // we can't delete the instruction here because Instruction->clone() used 
    // subsequently allocates memory at the exact same location evacuated 
    // by this instruction. This then causes problems in value numbering
    // since the hash table is looked up using Value*. We add it to a list, and
    // delete in the cleanUp()
    rpo->eraseValue(oldInst);
    deadList.push_back(oldInst);
  }

}

// This function performs certain sanity checks before calling doInsertReplace()
// The checks are different for INSERT/REPLACE at IN and OUT of a basic block
void LCM::setupInsertReplace() {

  dfva* dfvaInstance;
  
  for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) {
    dfvaInstance = BBMap[BB];

    SmallBitVector insertReplaceInVector = *(*dfvaInstance)[INSERTIN] | *(*dfvaInstance)[REPLACEIN];
    uint32_t nextPos = insertReplaceInVector.find_first();

    // for a particular basic block, go over all the positions in the bit-vector
    // which need either an INSERTIN or REPLACEIN, perform the sanity checks,
    // and finally call doInsertReplace()  
    for(uint32_t i = 0; i < insertReplaceInVector.count(); i++) {

      bool insert = (*(*dfvaInstance)[INSERTIN])[nextPos];
      bool replace = (*(*dfvaInstance)[REPLACEIN])[nextPos];
      bool antloc = (*(*dfvaInstance)[ANTLOC])[nextPos];
      bool xcomp = (*(*dfvaInstance)[XCOMP])[nextPos];
      bool transp = (*(*dfvaInstance)[TRANSP])[nextPos];
      
      // sanity checks 
      assert(~xcomp && "XCOMP has to be false if either of INSERTIN or REPLACEIN is true");
      assert(transp && "TRANSP has to be true if either of INSERTIN or REPLACEIN is true");
      if(insert & replace)
        assert(antloc && "ANTLOC should be true if both INSERTIN AND REPLACEIN are true");
      
      // do not perform insert and replace for this value number if at some
      // insertion point of some basic block (for this value number), we can not
      // find an expression to clone and put there
      insert = insert & ~insertMask[nextPos];
      replace = replace & ~insertMask[nextPos];

      doInsertReplace(rpo->getVNFromBVPos(nextPos), BB, insert, replace);
      nextPos = insertReplaceInVector.find_next(nextPos);
    }
  
    SmallBitVector insertReplaceOutVector = *(*dfvaInstance)[INSERTOUT] | *(*dfvaInstance)[REPLACEOUT];
    nextPos = insertReplaceOutVector.find_first();

    // for a particular basic block, go over all the positions in the bit-vector
    // which need either an INSERTOUT or REPLACEOUT, perform the sanity checks,
    // and finally call doInsertReplace()  
    for(uint32_t i = 0; i < insertReplaceOutVector.count(); i++) {

      bool insert = (*(*dfvaInstance)[INSERTOUT])[nextPos];
      bool replace = (*(*dfvaInstance)[REPLACEOUT])[nextPos];
      bool xcomp = (*(*dfvaInstance)[XCOMP])[nextPos];
      bool transp = (*(*dfvaInstance)[TRANSP])[nextPos];

      // sanity check
      if(replace)
        assert((xcomp & ~transp) && "if REPLACEOUT is true, then XCOMP should be true and TRANSP should be false");
      
      // same reason as above
      insert = insert & ~insertMask[nextPos];
      replace = replace & ~insertMask[nextPos];
     
      doInsertReplace(rpo->getVNFromBVPos(nextPos), BB, insert, replace);
      nextPos = insertReplaceOutVector.find_next(nextPos);
    }
  }
}

// Function called at the very end for clean-up operations / deallocating memory 
void LCM::cleanUp() {
  
  // drain the deadList
  while(!deadList.empty()) {
    Instruction* I = deadList.front();
    I->eraseFromParent();
    deadList.erase(deadList.begin());
  }

  // clear the value numbering table
  rpo->cleanUp();
  delete rpo;

  // destroy the DFA framework 
  for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) 
    delete(BBMap[BB]);
}

void LCM::printFlowEquations() {

  // Print VN to BitVectorPosition Map
  std::vector<std::pair<uint32_t, uint32_t> > repeatedValues = rpo->getRepeatedValues();
  for(std::vector<std::pair<uint32_t, uint32_t> >::iterator I = repeatedValues.begin(), E = repeatedValues.end(); I!=E; ++I) 
    DEBUG(errs() << "[VN]:"<< I->first << "\t[POS]:" << rpo->getBitVectorPosition(I->first) << "\n");

  // Print the bitVectors for each BB
  dfva* dfvaInstance;
  for (Function::iterator BB = Func->begin(), E = Func->end(); BB != E; ++BB) {
    dfvaInstance = BBMap[BB];
    DEBUG(errs() << *BB << "\n");
    DEBUG(errs() << "-----\n");
    for(uint32_t i = 0; i < TOTALBITVECTORS; i++){
  
      switch(i) {
        case(ANTLOC) : DEBUG(errs() << " ANTLOC "); break;
        case(TRANSP) : DEBUG(errs() << " TRANSP "); break;
        case(XCOMP) : DEBUG(errs() << " XCOMP "); break;
        case(ANTIN) : DEBUG(errs() << " ANTIN "); break;
        case(ANTOUT) : DEBUG(errs() << " ANTOUT "); break;
        case(AVAILIN) : DEBUG(errs() << " AVAILIN "); break;
        case(AVAILOUT) : DEBUG(errs() << " AVAILOUT "); break;
        case(EARLIN) : DEBUG(errs() << " EARLIN "); break;
        case(EARLOUT) : DEBUG(errs() << " EARLOUT "); break;
        case(DELAYIN) : DEBUG(errs() << " DELAYIN "); break;
        case(DELAYOUT) : DEBUG(errs() << " DELAYOUT "); break;
        case(LATESTIN) : DEBUG(errs() << " LATESTIN "); break;
        case(LATESTOUT) : DEBUG(errs() << " LATESTOUT "); break;
        case(ISOLIN) : DEBUG(errs() << " ISOLIN "); break;
        case(ISOLOUT) : DEBUG(errs() << " ISOLOUT "); break;
        case(INSERTIN) : DEBUG(errs() << " INSERTIN "); break;
        case(INSERTOUT) : DEBUG(errs() << " INSERTOUT "); break;
        case(REPLACEIN) : DEBUG(errs() << " REPLACEIN "); break;
        case(REPLACEOUT) : DEBUG(errs() << " REPLACEOUT "); break;
      }
      
      dumpSmallBitVector((*dfvaInstance)[i]);
    }
    DEBUG(errs() << "-----\n"); 
  }
}
