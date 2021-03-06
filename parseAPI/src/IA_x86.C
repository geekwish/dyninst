/*
 * See the dyninst/COPYRIGHT file for copyright information.
 * 
 * We provide the Paradyn Tools (below described as "Paradyn")
 * on an AS IS basis, and do not warrant its validity or performance.
 * We reserve the right to update, modify, or discontinue this
 * software at any time.  We shall have no obligation to supply such
 * updates or modifications or any other form of support to you.
 * 
 * By your use of Paradyn, you understand and agree that we (or any
 * other person or entity with proprietary rights in Paradyn) are
 * under no obligation to provide either maintenance services,
 * update services, notices of latent defects, or correction of
 * defects for Paradyn.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */


#include "IA_IAPI.h"
#include "Register.h"
#include "Dereference.h"
#include "Immediate.h"
#include "BinaryFunction.h"
#include "debug_parse.h"
#include "dataflowAPI/h/slicing.h"
#include "dataflowAPI/h/SymEval.h"
//#include "StackTamperVisitor.h"
#include "instructionAPI/h/Visitor.h"

#include <deque>

using namespace Dyninst;
using namespace InstructionAPI;
using namespace Dyninst::InsnAdapter;
using namespace Dyninst::ParseAPI;


bool IA_IAPI::isFrameSetupInsn(Instruction::Ptr i) const
{
    if(i->getOperation().getID() == e_mov)
    {
        if(i->readsMemory() || i->writesMemory())
        {
            parsing_printf("%s[%d]: discarding insn %s as stack frame preamble, not a reg-reg move\n",
                           FILE__, __LINE__, i->format().c_str());
            //return false;
        }
        if(i->isRead(stackPtr[_isrc->getArch()]) &&
           i->isWritten(framePtr[_isrc->getArch()]))
        {
            if((unsigned) i->getOperand(0).getValue()->size() == _isrc->getAddressWidth())
            {
                return true;
            }
            else
            {
                parsing_printf("%s[%d]: discarding insn %s as stack frame preamble, size mismatch for %d-byte addr width\n",
                               FILE__, __LINE__, i->format().c_str(), _isrc->getAddressWidth());
            }
        }
    }
    return false;
}

class nopVisitor : public InstructionAPI::Visitor
{
    public:
        nopVisitor() : foundReg(false), foundImm(false), foundBin(false), isNop(true) {}
        virtual ~nopVisitor() {}

        bool foundReg;
        bool foundImm;
        bool foundBin;
        bool isNop;

        virtual void visit(BinaryFunction*)
        {
            if (foundBin) isNop = false;
            if (!foundImm) isNop = false;
            if (!foundReg) isNop = false;
            foundBin = true;
        }
        virtual void visit(Immediate *imm)
        {
            if (imm != 0) isNop = false;
            foundImm = true;
        }
        virtual void visit(RegisterAST *)
        {
            foundReg = true;
        }
        virtual void visit(Dereference *)
        {
            isNop = false;
        }
};

bool isNopInsn(Instruction::Ptr insn) 
{
    // TODO: add LEA no-ops
    if(insn->getOperation().getID() == e_nop)
        return true;
    if(insn->getOperation().getID() == e_lea)
    {
        std::set<Expression::Ptr> memReadAddr;
        insn->getMemoryReadOperands(memReadAddr);
        std::set<RegisterAST::Ptr> writtenRegs;
        insn->getWriteSet(writtenRegs);

        if(memReadAddr.size() == 1 && writtenRegs.size() == 1)
        {
            if(**(memReadAddr.begin()) == **(writtenRegs.begin()))
            {
                return true;
            }
        }
        // Check for zero displacement
        nopVisitor visitor;

	// We need to get the src operand
        insn->getOperand(1).getValue()->apply(&visitor);
        if (visitor.isNop) return true; 
    }
    return false;
}

bool IA_IAPI::isNop() const
{
    Instruction::Ptr ci = curInsn();

    assert(ci);
   
    return isNopInsn(ci);

}

/*
 * A `thunk' is a function composed of the following pair of instructions:
 
 thunk:
    mov (%esp), <some register>
    ret
 
 * It has the effect of putting the address following a call to `thunk' into
 * the register, and is used in position independent code.
 */
namespace {
    class ThunkVisitor : public InstructionAPI::Visitor {
     public:
        ThunkVisitor() : offset_(0) { }
        virtual void visit(BinaryFunction *) {
            return;
        }
        virtual void visit(Immediate *i) {
            offset_ = i->eval().convert<Address>();
        }
        virtual void visit(RegisterAST*) {
            return;
        }
        virtual void visit(Dereference*) {
            return;
        }
        Address offset() const { return offset_; }

     private:
        Address offset_;
    };
}
bool IA_IAPI::isThunk() const {
  // Before we go a-wandering, check the target
   bool valid; Address addr;
   boost::tie(valid, addr) = getCFT();
   if (!valid ||
       !_isrc->isValidAddress(addr)) {
        parsing_printf("... Call to 0x%lx is invalid (outside code or data)\n",
                       addr);
        return false;
    }

    const unsigned char *target =
       (const unsigned char *)_isrc->getPtrToInstruction(addr);
    InstructionDecoder targetChecker(target,
            2*InstructionDecoder::maxInstructionLength, _isrc->getArch());
    Instruction::Ptr thunkFirst = targetChecker.decode();
    Instruction::Ptr thunkSecond = targetChecker.decode();
    if(thunkFirst && thunkSecond && 
        (thunkFirst->getOperation().getID() == e_mov) &&
        (thunkSecond->getCategory() == c_ReturnInsn))
    {
        if(thunkFirst->isRead(stackPtr[_isrc->getArch()]))
        {
            // it is not enough that the stack pointer is read; it must
            // be a zero-offset read from the stack pointer
            ThunkVisitor tv;
            Operand op = thunkFirst->getOperand(1);
            op.getValue()->apply(&tv); 
    
            return tv.offset() == 0; 
        }
    }
    return false;
}

bool IA_IAPI::isTailCall(Function * context, EdgeTypeEnum type, unsigned int, const set<Address>& knownTargets) const
{
   // Collapse down to "branch" or "fallthrough"
    switch(type) {
       case COND_TAKEN:
       case DIRECT:
       case INDIRECT:
          type = DIRECT;
          break;
       case CALL:
       case RET:
       case COND_NOT_TAKEN:
       case FALLTHROUGH:
       case CALL_FT:
       default:
          return false;
    }

    parsing_printf("Checking for Tail Call \n");
    context->obj()->cs()->incrementCounter(PARSE_TAILCALL_COUNT); 
    if (tailCalls.find(type) != tailCalls.end()) {
        parsing_printf("\tReturning cached tail call check result: %d\n", tailCalls[type]);
        if (tailCalls[type]) {
            context->obj()->cs()->incrementCounter(PARSE_TAILCALL_FAIL);
            return true;
        }
        return false;
    }
    
    bool valid; Address addr;
    boost::tie(valid, addr) = getCFT();

    Function* callee = _obj->findFuncByEntry(_cr, addr);
    Block* target = _obj->findBlockByEntry(_cr, addr);

    // check if addr is in a block if it is not entry.
    if (target == NULL) {
        std::set<Block*> blocks;
        _obj->findCurrentBlocks(_cr, addr, blocks);
        if (blocks.size() == 1) {
            target = *blocks.begin();
        } else if (blocks.size() == 0) {
	    // This case can happen when the jump target is a function entry,
	    // but we have not parsed the function yet,
	    // or when this is an indirect jump 
	    target = NULL;
	} else {
	    // If this case happens, it means the jump goes into overlapping instruction streams,
	    // it is not likely to be a tail call.
	    parsing_printf("\tjumps into overlapping instruction streams\n");
	    for (auto bit = blocks.begin(); bit != blocks.end(); ++bit) {
	        parsing_printf("\t block [%lx,%lx)\n", (*bit)->start(), (*bit)->end());
	    }
	    parsing_printf("\tjump to 0x%lx, NOT TAIL CALL\n", addr);
	    tailCalls[type] = false;
	    return false;
	}
    }

    if(curInsn()->getCategory() == c_BranchInsn &&
       valid &&
       callee && 
       callee != context &&
       target &&
       !context->contains(target)
       )
    {
      parsing_printf("\tjump to 0x%lx, TAIL CALL\n", addr);
      tailCalls[type] = true;
      return true;
    }

    if (curInsn()->getCategory() == c_BranchInsn &&
            valid &&
            !callee) {
	if (target) {
	    parsing_printf("\tjump to 0x%lx is known block, but not func entry, NOT TAIL CALL\n", addr);
	    tailCalls[type] = false;
	    return false;
	} else if (knownTargets.find(addr) != knownTargets.end()) {
	    parsing_printf("\tjump to 0x%lx is known target in this function, NOT TAIL CALL\n", addr);
	    tailCalls[type] = false;
	    return false;
	}
    }

    if(allInsns.size() < 2) {
      if(context->addr() == _curBlk->start() && curInsn()->getCategory() == c_BranchInsn)
      {
	parsing_printf("\tjump as only insn in entry block, TAIL CALL\n");
	tailCalls[type] = true;
	return true;
      }
      else
      {
        parsing_printf("\ttoo few insns to detect tail call\n");
        context->obj()->cs()->incrementCounter(PARSE_TAILCALL_FAIL);
        tailCalls[type] = false;
        return false;
      }
    }

    if ((curInsn()->getCategory() == c_BranchInsn))
    {
        //std::map<Address, Instruction::Ptr>::const_iterator prevIter =
                //allInsns.find(current);
        
        // Updated: there may be zero or more nops between leave->jmp
       
        allInsns_t::const_iterator prevIter = curInsnIter;
        --prevIter;
        Instruction::Ptr prevInsn = prevIter->second;
    
        while ( isNopInsn(prevInsn) && (prevIter != allInsns.begin()) ) {
           --prevIter;
           prevInsn = prevIter->second;
        }
	prevInsn = prevIter->second;
        if(prevInsn->getOperation().getID() == e_leave)
        {
           parsing_printf("\tprev insn was leave, TAIL CALL\n");
           tailCalls[type] = true;
           return true;
        }
        else if(prevInsn->getOperation().getID() == e_pop)
        {
            if(prevInsn->isWritten(framePtr[_isrc->getArch()]))
            {
                parsing_printf("\tprev insn was %s, TAIL CALL\n", prevInsn->format().c_str());
                tailCalls[type] = true;
                return true;
            }
        }
        else if(prevInsn->getOperation().getID() == e_add)
        {			
            if(prevInsn->isWritten(stackPtr[_isrc->getArch()]))
            {
				bool call_fallthrough = false;
				if (_curBlk->start() == prevIter->first) {				
					for (auto eit = _curBlk->sources().begin(); eit != _curBlk->sources().end(); ++eit) {						
						if ((*eit)->type() == CALL_FT) {
							call_fallthrough = true;
							break;
						}
					}
				}
				if (call_fallthrough) {
					parsing_printf("\tprev insn was %s, but it is the next instruction of a function call, not a tail call %x %x\n", prevInsn->format().c_str()); 
				}	else {
					parsing_printf("\tprev insn was %s, TAIL CALL\n", prevInsn->format().c_str());
					tailCalls[type] = true;
					return true;
				}
			} else
				parsing_printf("\tprev insn was %s, not tail call\n", prevInsn->format().c_str());
        }
    }

    tailCalls[type] = false;
    context->obj()->cs()->incrementCounter(PARSE_TAILCALL_FAIL);
    return false;
}

bool IA_IAPI::savesFP() const
{
	std::vector<Instruction::Ptr> insns;
	insns.push_back(curInsn());
#if defined(os_windows)
	// Windows functions can start with a noop...
	InstructionDecoder tmp(dec);
	insns.push_back(tmp.decode());
#endif
	for (unsigned i = 0; i < insns.size(); ++i) {
		InstructionAPI::Instruction::Ptr ci = insns[i];
	    if(ci->getOperation().getID() == e_push)
		{
			if (ci->isRead(framePtr[_isrc->getArch()])) {
				return true;
			}
			else return false;
		}
	}	
	return false;
}

bool IA_IAPI::isStackFramePreamble() const
{
#if defined(os_windows)
	// Windows pads with a noop
	const int limit = 3;
#else 
	const int limit = 2;
#endif
	if (!savesFP()) return false;
    InstructionDecoder tmp(dec);
    std::vector<Instruction::Ptr> nextTwoInsns;
    for (int i = 0; i < limit; ++i) {
       Instruction::Ptr insn = tmp.decode();
       if (isFrameSetupInsn(insn)) {
          return true;
       }
    }
	return false;
}

bool IA_IAPI::cleansStack() const
{
    Instruction::Ptr ci = curInsn();
	if (ci->getCategory() != c_ReturnInsn) return false;
    std::vector<Operand> ops;
	ci->getOperands(ops);
	return (ops.size() > 1);
}

bool IA_IAPI::isReturn(Dyninst::ParseAPI::Function * /*context*/, 
			Dyninst::ParseAPI::Block* /*currBlk*/) const
{
    // For x86, we check if an instruction is return based on the category. 
    // However, for powerpc, the return instruction BLR can be a return or
    // an indirect jump used for jump tables etc. Hence, we need to function and block
    // to determine if an instruction is a return. But these parameters are unused for x86. 
    return curInsn()->getCategory() == c_ReturnInsn;
}

bool IA_IAPI::isReturnAddrSave(Dyninst::Address&) const
{
    // not implemented on non-power
    return false;
}

bool IA_IAPI::sliceReturn(ParseAPI::Block* /*bit*/, Address /*ret_addr*/, ParseAPI::Function * /*func*/) const {
   return true;
}

//class ST_Predicates : public Slicer::Predicates {};


/* returns true if the call leads to:
 * -an invalid instruction (or immediately branches/calls to an invalid insn)
 * -a block not ending in a return instruction that pops the return address 
 *  off of the stack
 */
bool IA_IAPI::isFakeCall() const
{
    assert(_obj->defensiveMode());

    if (isDynamicCall()) {
        return false;
    }

    // get func entry
    bool tampers = false;
    bool valid; Address entry;
    boost::tie(valid, entry) = getCFT();

    if (!valid) return false;

    if (! _cr->contains(entry) ) {
       return false;
    }

    if ( ! _isrc->isCode(entry) ) {
        mal_printf("WARNING: found function call at %lx "
                   "to invalid address %lx %s[%d]\n", current, 
                   entry, FILE__,__LINE__);
        return false;
    }

    // get instruction at func entry
    const unsigned char* bufPtr =
     (const unsigned char *)(_cr->getPtrToInstruction(entry));
    Offset entryOff = entry - _cr->offset();
    InstructionDecoder newdec( bufPtr,
                              _cr->length() - entryOff,
                              _cr->getArch() );
    IA_IAPI *ah = new IA_IAPI(newdec, entry, _obj, _cr, _isrc, _curBlk);
    Instruction::Ptr insn = ah->curInsn();

    // follow ctrl transfers until you get a block containing non-ctrl 
    // transfer instructions, or hit a return instruction
    while (insn->getCategory() == c_CallInsn ||
           insn->getCategory() == c_BranchInsn) 
    {
       boost::tie(valid, entry) = ah->getCFT();
       if ( !valid || ! _cr->contains(entry) || ! _isrc->isCode(entry) ) {
          mal_printf("WARNING: found call to function at %lx that "
                     "leaves to %lx, out of the code region %s[%d]\n", 
                     current, entry, FILE__,__LINE__);
          return false;
       }
        bufPtr = (const unsigned char *)(_cr->getPtrToInstruction(entry));
        entryOff = entry - _cr->offset();
        delete(ah);
        newdec = InstructionDecoder(bufPtr, 
                                    _cr->length() - entryOff, 
                                    _cr->getArch());
        ah = new IA_IAPI(newdec, entry, _obj, _cr, _isrc, _curBlk);
        insn = ah->curInsn();
    }

    // calculate instruction stack deltas for the block, leaving the iterator
    // at the last ins'n if it's a control transfer, or after calculating the 
    // last instruction's delta if we run off the end of initialized memory
    int stackDelta = 0;
    int addrWidth = _isrc->getAddressWidth();
    static Expression::Ptr theStackPtr
        (new RegisterAST(MachRegister::getStackPointer(_isrc->getArch())));
    Address curAddr = entry;

    while(true) {

        // exit condition 1
        if (insn->getCategory() == c_CallInsn ||
            insn->getCategory() == c_ReturnInsn ||
            insn->getCategory() == c_BranchInsn) 
        {
            break;
        }

        // calculate instruction delta
        if(insn->isWritten(theStackPtr)) {
            entryID what = insn->getOperation().getID();
            int sign = 1;
            switch(what) 
            {
            case e_push:
                sign = -1;
                //FALLTHROUGH
            case e_pop: {
                int size = insn->getOperand(0).getValue()->size();
                stackDelta += sign * size;
                break;
            }
            case e_pusha:
            case e_pushad:
                sign = -1;
                //FALLTHROUGH
            case e_popa:
            case e_popad:
                if (1 == sign) {
                    mal_printf("popad ins'n at %lx in func at %lx changes sp "
                               "by %d. %s[%d]\n", ah->getAddr(), 
                               entry, 8 * sign * addrWidth, FILE__, __LINE__);
                }
                stackDelta += sign * 8 * addrWidth;
                break;
            case e_pushf:
            case e_pushfd:
                sign = -1;
                //FALLTHROUGH
            case e_popf:
            case e_popfd:
                stackDelta += sign * 4;
                if (1 == sign) {
                    mal_printf("popf ins'n at %lx in func at %lx changes sp "
                               "by %d. %s[%d]\n", ah->getAddr(), entry, 
                               sign * 4, FILE__, __LINE__);
                }
                break;
            case e_enter:
                //mal_printf("Saw enter instruction at %lx in isFakeCall, "
                //           "quitting early, assuming not fake "
                //           "%s[%d]\n",curAddr, FILE__,__LINE__);
                // unhandled case, but not essential for correct analysis
                delete ah;
                return false;
                break;
            case e_leave:
                mal_printf("WARNING: saw leave instruction "
                           "at %lx that is not handled by isFakeCall %s[%d]\n",
                           curAddr, FILE__,__LINE__);
                // unhandled, not essential for correct analysis, would
                // be a red flag if there wasn't an enter ins'n first and 
                // we didn't end in a return instruction
                break;
			case e_and:
				// Rounding off the stack pointer. 
				mal_printf("WARNING: saw and instruction at %lx that is not handled by isFakeCall %s[%d]\n",
					curAddr, FILE__, __LINE__);
				delete ah;
				return false;
				break;

            case e_sub:
                sign = -1;
                //FALLTHROUGH
            case e_add: {
                Operand arg = insn->getOperand(1);
                Result delta = arg.getValue()->eval();
                if(delta.defined) {
                    int delta_int = sign;
                    switch (delta.type) {
                    case u8:
                    case s8:
                        delta_int *= (int)delta.convert<char>();
                        break;
                    case u16:
                    case s16:
                        delta_int *= (int)delta.convert<short>();
                        break;
                    case u32:
                    case s32:
                        delta_int *= delta.convert<int>();
                        break;
                    default:
                        assert(0 && "got add/sub operand of unusual size");
                        break;
                    }
                    stackDelta += delta_int;
                } else if (sign == -1) {
                    delete ah;
                    return false;
                } else {
                    mal_printf("ERROR: in isFakeCall, add ins'n "
                               "at %lx (in first block of function at "
                               "%lx) modifies the sp but failed to evaluate "
                               "its arguments %s[%d]\n", 
                               ah->getAddr(), entry, FILE__, __LINE__);
                    delete ah;
                    return true;
                }
                break;
            }
            default: {
                fprintf(stderr,"WARNING: in isFakeCall non-push/pop "
                        "ins'n at %lx (in first block of function at "
                        "%lx) modifies the sp by an unknown amount. "
                        "%s[%d]\n", ah->getAddr(), entry, 
                        FILE__, __LINE__);
                break;
            } // end default block
            } // end switch
        }

        if (stackDelta > 0) {
            tampers=true;
        }

        // exit condition 2
        ah->advance();
        Instruction::Ptr next = ah->curInsn();
        if (NULL == next) {
            break;
        }
        curAddr += insn->size();
        insn = next;
    } 

    // not a fake call if it ends w/ a return instruction
    if (insn->getCategory() == c_ReturnInsn) {
        delete ah;
        return false;
    }

    // if the stack delta is positive or the return address has been replaced
    // with an absolute value, it's a fake call, since in both cases 
    // the return address is gone and we cannot return to the caller
    if ( 0 < stackDelta || tampers ) {

        delete ah;
        return true;
    }

    delete ah;
    return false;
}

bool IA_IAPI::isIATcall(std::string &calleeName) const
{
    if (!isDynamicCall()) {
        return false;
    }

    if (!curInsn()->readsMemory()) {
        return false;
    }

    std::set<Expression::Ptr> memReads;
    curInsn()->getMemoryReadOperands(memReads);
    if (memReads.size() != 1) {
        return false;
    }

    Result memref = (*memReads.begin())->eval();
    if (!memref.defined) {
        return false;
    }
    Address entryAddr = memref.convert<Address>();

    // convert to a relative address
    if (_obj->cs()->loadAddress() < entryAddr) {
        entryAddr -= _obj->cs()->loadAddress();
    }
    
    if (!_obj->cs()->isValidAddress(entryAddr)) {
        return false;
    }

    // calculate the address of the ASCII string pointer, 
    // skip over the IAT entry's two-byte hint
    void * asciiPtr = _obj->cs()->getPtrToInstruction(entryAddr);
    if (!asciiPtr) {
        return false;
    }
    Address funcAsciiAddr = 2 + *(Address*) asciiPtr;
    if (!_obj->cs()->isValidAddress(funcAsciiAddr)) {
        return false;
    }

    // see if it's really a string that could be a function name
    char *funcAsciiPtr = (char*) _obj->cs()->getPtrToData(funcAsciiAddr);
    if (!funcAsciiPtr) {
        return false;
    }
    char cur = 'a';
    int count=0;
    do {
        cur = funcAsciiPtr[count];
        count++;
    }while (count < 100 && 
            _obj->cs()->isValidAddress(funcAsciiAddr+count) &&
            ((cur >= 'A' && cur <= 'z') ||
             (cur >= '0' && cur <= '9')));
    if (cur != 0 || count <= 1) 
        return false;

    mal_printf("found IAT call at %lx to %s\n", current, funcAsciiPtr);
    calleeName = string(funcAsciiPtr);
    return true;
}

bool IA_IAPI::isNopJump() const
{
    InsnCategory cat = curInsn()->getCategory();
    if (c_BranchInsn != cat) {
        return false;
    }
    bool valid; Address addr;
    boost::tie(valid, addr) = getCFT();
    if(valid && current+1 == addr) {
        return true;
    }
    return false;
}

bool IA_IAPI::isLinkerStub() const
{
    // No need for linker stubs on x86 platforms.
    return false;
}
