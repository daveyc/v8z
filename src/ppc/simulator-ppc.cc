// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <stdlib.h>
#include <math.h>
#include <cstdarg>
#include "v8.h"

#if defined(V8_TARGET_ARCH_PPC)

#include "disasm.h"
#include "assembler.h"
#include "ppc/constants-ppc.h"
#include "ppc/simulator-ppc.h"

#define INCLUDE_ARM

#if defined(USE_SIMULATOR)

// Only build the simulator if not compiling for real ARM hardware.
namespace v8 {
namespace internal {

// This macro provides a platform independent use of sscanf. The reason for
// SScanF not being implemented in a platform independent way through
// ::v8::internal::OS in the same way as SNPrintF is that the
// Windows C Run-Time Library does not provide vsscanf.
#define SScanF sscanf  // NOLINT

// The PPCDebugger class is used by the simulator while debugging simulated
// PowerPC code.
class PPCDebugger {
 public:
  explicit PPCDebugger(Simulator* sim) : sim_(sim) { }
  ~PPCDebugger();

  void Stop(Instruction* instr);
  void Debug();

 private:
  static const Instr kBreakpointInstr = (TWI | 0x1f * B21 );
  static const Instr kNopInstr = (ORI);  // ori, 0,0,0

  Simulator* sim_;

  int32_t GetRegisterValue(int regnum);
  double GetRegisterPairDoubleValue(int regnum);
  double GetFPDoubleRegisterValue(int regnum);
  bool GetValue(const char* desc, int32_t* value);
  bool GetFPSingleValue(const char* desc, float* value);
  bool GetFPDoubleValue(const char* desc, double* value);

  // Set or delete a breakpoint. Returns true if successful.
  bool SetBreakpoint(Instruction* breakpc);
  bool DeleteBreakpoint(Instruction* breakpc);

  // Undo and redo all breakpoints. This is needed to bracket disassembly and
  // execution to skip past breakpoints when run from the debugger.
  void UndoBreakpoints();
  void RedoBreakpoints();
};


PPCDebugger::~PPCDebugger() {
}



#ifdef GENERATED_CODE_COVERAGE
static FILE* coverage_log = NULL;


static void InitializeCoverage() {
  char* file_name = getenv("V8_GENERATED_CODE_COVERAGE_LOG");
  if (file_name != NULL) {
    coverage_log = fopen(file_name, "aw+");
  }
}


void PPCDebugger::Stop(Instruction* instr) {  //roohack need to fix for PPC
  // Get the stop code.
  uint32_t code = instr->SvcValue() & kStopCodeMask;
  // Retrieve the encoded address, which comes just after this stop.
  char** msg_address =
    reinterpret_cast<char**>(sim_->get_pc() + Instruction::kInstrSize);
  char* msg = *msg_address;
  ASSERT(msg != NULL);

  // Update this stop description.
  if (isWatchedStop(code) && !watched_stops[code].desc) {
    watched_stops[code].desc = msg;
  }

  if (strlen(msg) > 0) {
    if (coverage_log != NULL) {
      fprintf(coverage_log, "%s\n", msg);
      fflush(coverage_log);
    }
    // Overwrite the instruction and address with nops.
    instr->SetInstructionBits(kNopInstr);
    reinterpret_cast<Instruction*>(msg_address)->SetInstructionBits(kNopInstr);
  }
  sim_->set_pc(sim_->get_pc() + 2 * Instruction::kInstrSize);
}

#else  // ndef GENERATED_CODE_COVERAGE

static void InitializeCoverage() {
}


void PPCDebugger::Stop(Instruction* instr) {
  // Get the stop code.
  uint32_t code = instr->SvcValue() & kStopCodeMask;  //roohack remove kStopCodeMask
  // Retrieve the encoded address, which comes just after this stop.
  char* msg = *reinterpret_cast<char**>(sim_->get_pc()
                                        + Instruction::kInstrSize);
  // Update this stop description.
  if (sim_->isWatchedStop(code) && !sim_->watched_stops[code].desc) {
    sim_->watched_stops[code].desc = msg;
  }
  // Print the stop message and code if it is not the default code.
  if (code != kMaxStopCode) {
    PrintF("Simulator hit stop %u: %s\n", code, msg);
  } else {
    PrintF("Simulator hit %s\n", msg);
  }
  sim_->set_pc(sim_->get_pc() + 2 * Instruction::kInstrSize);
  Debug();
}
#endif


int32_t PPCDebugger::GetRegisterValue(int regnum) {
  if (regnum == kPCRegister) {
    return sim_->get_pc();
  } else {
    return sim_->get_register(regnum);
  }
}


double PPCDebugger::GetRegisterPairDoubleValue(int regnum) {
  return sim_->get_double_from_register_pair(regnum);
}


double PPCDebugger::GetFPDoubleRegisterValue(int regnum) {
  return sim_->get_double_from_d_register(regnum);
}


bool PPCDebugger::GetValue(const char* desc, int32_t* value) {
  int regnum = Registers::Number(desc);
  if (regnum != kNoRegister) {
    *value = GetRegisterValue(regnum);
    return true;
  } else {
    if (strncmp(desc, "0x", 2) == 0) {
      return SScanF(desc + 2, "%x", reinterpret_cast<uint32_t*>(value)) == 1;
    } else {
      return SScanF(desc, "%u", reinterpret_cast<uint32_t*>(value)) == 1;
    }
  }
  return false;
}


bool PPCDebugger::GetFPSingleValue(const char* desc, float* value) {
  bool is_double;
  int regnum = FPRegisters::Number(desc, &is_double);
  if (regnum != kNoRegister && !is_double) {
    *value = sim_->get_float_from_s_register(regnum);
    return true;
  }
  return false;
}


bool PPCDebugger::GetFPDoubleValue(const char* desc, double* value) {
  bool is_double;
  int regnum = FPRegisters::Number(desc, &is_double);
  if (regnum != kNoRegister && is_double) {
    *value = sim_->get_double_from_d_register(regnum);
    return true;
  }
  return false;
}


bool PPCDebugger::SetBreakpoint(Instruction* breakpc) {
  // Check if a breakpoint can be set. If not return without any side-effects.
  if (sim_->break_pc_ != NULL) {
    return false;
  }

  // Set the breakpoint.
  sim_->break_pc_ = breakpc;
  sim_->break_instr_ = breakpc->InstructionBits();
  // Not setting the breakpoint instruction in the code itself. It will be set
  // when the debugger shell continues.
  return true;
}


bool PPCDebugger::DeleteBreakpoint(Instruction* breakpc) {
  if (sim_->break_pc_ != NULL) {
    sim_->break_pc_->SetInstructionBits(sim_->break_instr_);
  }

  sim_->break_pc_ = NULL;
  sim_->break_instr_ = 0;
  return true;
}


void PPCDebugger::UndoBreakpoints() {
  if (sim_->break_pc_ != NULL) {
    sim_->break_pc_->SetInstructionBits(sim_->break_instr_);
  }
}


void PPCDebugger::RedoBreakpoints() {
  if (sim_->break_pc_ != NULL) {
    sim_->break_pc_->SetInstructionBits(kBreakpointInstr);
  }
}


void PPCDebugger::Debug() {
  intptr_t last_pc = -1;
  bool done = false;

#define COMMAND_SIZE 63
#define ARG_SIZE 255

#define STR(a) #a
#define XSTR(a) STR(a)

  char cmd[COMMAND_SIZE + 1];
  char arg1[ARG_SIZE + 1];
  char arg2[ARG_SIZE + 1];
  char* argv[3] = { cmd, arg1, arg2 };

  // make sure to have a proper terminating character if reaching the limit
  cmd[COMMAND_SIZE] = 0;
  arg1[ARG_SIZE] = 0;
  arg2[ARG_SIZE] = 0;

  // Undo all set breakpoints while running in the debugger shell. This will
  // make them invisible to all commands.
  UndoBreakpoints();
  // Disable tracing while simulating
  bool trace=::v8::internal::FLAG_trace_sim;
  ::v8::internal::FLAG_trace_sim = false;

  while (!done && !sim_->has_bad_pc()) {
    if (last_pc != sim_->get_pc()) {
      disasm::NameConverter converter;
      disasm::Disassembler dasm(converter);
      // use a reasonably large buffer
      v8::internal::EmbeddedVector<char, 256> buffer;
      dasm.InstructionDecode(buffer,
                             reinterpret_cast<byte*>(sim_->get_pc()));
      PrintF("  0x%08x  %s\n", sim_->get_pc(), buffer.start());
      last_pc = sim_->get_pc();
    }
    char* line = ReadLine("sim> ");
    if (line == NULL) {
      break;
    } else {
      char* last_input = sim_->last_debugger_input();
      if (strcmp(line, "\n") == 0 && last_input != NULL) {
        line = last_input;
      } else {
        // Ownership is transferred to sim_;
        sim_->set_last_debugger_input(line);
      }
      // Use sscanf to parse the individual parts of the command line. At the
      // moment no command expects more than two parameters.
      int argc = SScanF(line,
                        "%" XSTR(COMMAND_SIZE) "s "
                        "%" XSTR(ARG_SIZE) "s "
                        "%" XSTR(ARG_SIZE) "s",
                        cmd, arg1, arg2);
      if ((strcmp(cmd, "si") == 0) || (strcmp(cmd, "stepi") == 0)) {
        sim_->InstructionDecode(reinterpret_cast<Instruction*>(sim_->get_pc()));
      } else if ((strcmp(cmd, "c") == 0) || (strcmp(cmd, "cont") == 0)) {
        // Execute the one instruction we broke at with breakpoints disabled.
        sim_->InstructionDecode(reinterpret_cast<Instruction*>(sim_->get_pc()));
        // Leave the debugger shell.
        done = true;
      } else if ((strcmp(cmd, "p") == 0) || (strcmp(cmd, "print") == 0)) {
        if (argc == 2 || (argc == 3 && strcmp(arg2, "fp") == 0)) {
          int32_t value;
          float svalue;
          double dvalue;
          if (strcmp(arg1, "all") == 0) {
            for (int i = 0; i < kNumRegisters; i++) {
              value = GetRegisterValue(i);
              PrintF("%3s: 0x%08x %10d", Registers::Name(i), value, value);
              if ((argc == 3 && strcmp(arg2, "fp") == 0) &&
                  i < 8 &&
                  (i % 2) == 0) {
                dvalue = GetRegisterPairDoubleValue(i);
                PrintF(" (%f)\n", dvalue);
              } else {
                PrintF("\n");
              }
            }
            for (int i = 0; i < kNumFPDoubleRegisters; i++) {
              dvalue = GetFPDoubleRegisterValue(i);
              uint64_t as_words = BitCast<uint64_t>(dvalue);
              PrintF("%3s: %f 0x%08x %08x\n",
                     FPRegisters::Name(i, true),
                     dvalue,
                     static_cast<uint32_t>(as_words >> 32),
                     static_cast<uint32_t>(as_words & 0xffffffff));
            }
//            PrintF(" cr: 0x%08x", condition_reg_);
          } else {
            if (GetValue(arg1, &value)) {
              PrintF("%s: 0x%08x %d \n", arg1, value, value);
            } else if (GetFPSingleValue(arg1, &svalue)) {
              uint32_t as_word = BitCast<uint32_t>(svalue);
              PrintF("%s: %f 0x%08x\n", arg1, svalue, as_word);
            } else if (GetFPDoubleValue(arg1, &dvalue)) {
              uint64_t as_words = BitCast<uint64_t>(dvalue);
              PrintF("%s: %f 0x%08x %08x\n",
                     arg1,
                     dvalue,
                     static_cast<uint32_t>(as_words >> 32),
                     static_cast<uint32_t>(as_words & 0xffffffff));
            } else {
              PrintF("%s unrecognized\n", arg1);
            }
          }
        } else {
          PrintF("print <register>\n");
        }
      } else if ((strcmp(cmd, "po") == 0)
                 || (strcmp(cmd, "printobject") == 0)) {
        if (argc == 2) {
          int32_t value;
          if (GetValue(arg1, &value)) {
            Object* obj = reinterpret_cast<Object*>(value);
            PrintF("%s: \n", arg1);
#ifdef DEBUG
            obj->PrintLn();
#else
            obj->ShortPrint();
            PrintF("\n");
#endif
          } else {
            PrintF("%s unrecognized\n", arg1);
          }
        } else {
          PrintF("printobject <value>\n");
        }
      } else if (strcmp(cmd, "stack") == 0 || strcmp(cmd, "mem") == 0) {
        int32_t* cur = NULL;
        int32_t* end = NULL;
        int next_arg = 1;

        if (strcmp(cmd, "stack") == 0) {
          cur = reinterpret_cast<int32_t*>(sim_->get_register(Simulator::sp));
        } else {  // "mem"
          int32_t value;
          if (!GetValue(arg1, &value)) {
            PrintF("%s unrecognized\n", arg1);
            continue;
          }
          cur = reinterpret_cast<int32_t*>(value);
          next_arg++;
        }

        int32_t words;
        if (argc == next_arg) {
          words = 10;
        } else if (argc == next_arg + 1) {
          if (!GetValue(argv[next_arg], &words)) {
            words = 10;
          }
        }
        end = cur + words;

        while (cur < end) {
          PrintF("  0x%08x:  0x%08x %10d",
                 reinterpret_cast<intptr_t>(cur), *cur, *cur);
          HeapObject* obj = reinterpret_cast<HeapObject*>(*cur);
          int value = *cur;
          Heap* current_heap = v8::internal::Isolate::Current()->heap();
          if (current_heap->Contains(obj) || ((value & 1) == 0)) {
            PrintF(" (");
            if ((value & 1) == 0) {
              PrintF("smi %d", value / 2);
            } else {
              obj->ShortPrint();
            }
            PrintF(")");
          }
          PrintF("\n");
          cur++;
        }
      } else if (strcmp(cmd, "disasm") == 0 || strcmp(cmd, "di") == 0) {
        disasm::NameConverter converter;
        disasm::Disassembler dasm(converter);
        // use a reasonably large buffer
        v8::internal::EmbeddedVector<char, 256> buffer;

        byte* prev = NULL;
        byte* cur = NULL;
        byte* end = NULL;

        if (argc == 1) {
          cur = reinterpret_cast<byte*>(sim_->get_pc());
          end = cur + (10 * Instruction::kInstrSize);
        } else if (argc == 2) {
          int regnum = Registers::Number(arg1);
          if (regnum != kNoRegister || strncmp(arg1, "0x", 2) == 0) {
            // The argument is an address or a register name.
            int32_t value;
            if (GetValue(arg1, &value)) {
              cur = reinterpret_cast<byte*>(value);
              // Disassemble 10 instructions at <arg1>.
              end = cur + (10 * Instruction::kInstrSize);
            }
          } else {
            // The argument is the number of instructions.
            int32_t value;
            if (GetValue(arg1, &value)) {
              cur = reinterpret_cast<byte*>(sim_->get_pc());
              // Disassemble <arg1> instructions.
              end = cur + (value * Instruction::kInstrSize);
            }
          }
        } else {
          int32_t value1;
          int32_t value2;
          if (GetValue(arg1, &value1) && GetValue(arg2, &value2)) {
            cur = reinterpret_cast<byte*>(value1);
            end = cur + (value2 * Instruction::kInstrSize);
          }
        }

        while (cur < end) {
          prev = cur;
          cur += dasm.InstructionDecode(buffer, cur);
          PrintF("MOO3  0x%08x  %s\n",
                 reinterpret_cast<intptr_t>(prev), buffer.start());
        }
      } else if (strcmp(cmd, "gdb") == 0) {
        PrintF("relinquishing control to gdb\n");
        v8::internal::OS::DebugBreak();
        PrintF("regaining control from gdb\n");
      } else if (strcmp(cmd, "break") == 0) {
        if (argc == 2) {
          int32_t value;
          if (GetValue(arg1, &value)) {
            if (!SetBreakpoint(reinterpret_cast<Instruction*>(value))) {
              PrintF("setting breakpoint failed\n");
            }
          } else {
            PrintF("%s unrecognized\n", arg1);
          }
        } else {
          PrintF("break <address>\n");
        }
      } else if (strcmp(cmd, "del") == 0) {
        if (!DeleteBreakpoint(NULL)) {
          PrintF("deleting breakpoint failed\n");
        }
      } else if (strcmp(cmd, "flags") == 0) {
        PrintF("N flag: %d; ", sim_->n_flag_);
        PrintF("Z flag: %d; ", sim_->z_flag_);
        PrintF("C flag: %d; ", sim_->c_flag_);
        PrintF("V flag: %d\n", sim_->v_flag_);
        PrintF("INVALID OP flag: %d; ", sim_->inv_op_vfp_flag_);
        PrintF("DIV BY ZERO flag: %d; ", sim_->div_zero_vfp_flag_);
        PrintF("OVERFLOW flag: %d; ", sim_->overflow_vfp_flag_);
        PrintF("UNDERFLOW flag: %d; ", sim_->underflow_vfp_flag_);
        PrintF("INEXACT flag: %d;\n", sim_->inexact_vfp_flag_);
      } else if (strcmp(cmd, "stop") == 0) {
        int32_t value;
        intptr_t stop_pc = sim_->get_pc() - 2 * Instruction::kInstrSize;
        Instruction* stop_instr = reinterpret_cast<Instruction*>(stop_pc);
        Instruction* msg_address =
          reinterpret_cast<Instruction*>(stop_pc + Instruction::kInstrSize);
        if ((argc == 2) && (strcmp(arg1, "unstop") == 0)) {
          // Remove the current stop.
          if (sim_->isStopInstruction(stop_instr)) {
            stop_instr->SetInstructionBits(kNopInstr);
            msg_address->SetInstructionBits(kNopInstr);
          } else {
            PrintF("Not at debugger stop.\n");
          }
        } else if (argc == 3) {
          // Print information about all/the specified breakpoint(s).
          if (strcmp(arg1, "info") == 0) {
            if (strcmp(arg2, "all") == 0) {
              PrintF("Stop information:\n");
              for (uint32_t i = 0; i < sim_->kNumOfWatchedStops; i++) {
                sim_->PrintStopInfo(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->PrintStopInfo(value);
            } else {
              PrintF("Unrecognized argument.\n");
            }
          } else if (strcmp(arg1, "enable") == 0) {
            // Enable all/the specified breakpoint(s).
            if (strcmp(arg2, "all") == 0) {
              for (uint32_t i = 0; i < sim_->kNumOfWatchedStops; i++) {
                sim_->EnableStop(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->EnableStop(value);
            } else {
              PrintF("Unrecognized argument.\n");
            }
          } else if (strcmp(arg1, "disable") == 0) {
            // Disable all/the specified breakpoint(s).
            if (strcmp(arg2, "all") == 0) {
              for (uint32_t i = 0; i < sim_->kNumOfWatchedStops; i++) {
                sim_->DisableStop(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->DisableStop(value);
            } else {
              PrintF("Unrecognized argument.\n");
            }
          }
        } else {
          PrintF("Wrong usage. Use help command for more information.\n");
        }
      } else if ((strcmp(cmd, "t") == 0) || strcmp(cmd, "trace") == 0) {
        ::v8::internal::FLAG_trace_sim = !::v8::internal::FLAG_trace_sim;
        PrintF("Trace of executed instructions is %s\n",
               ::v8::internal::FLAG_trace_sim ? "on" : "off");
      } else if ((strcmp(cmd, "h") == 0) || (strcmp(cmd, "help") == 0)) {
        PrintF("cont\n");
        PrintF("  continue execution (alias 'c')\n");
        PrintF("stepi\n");
        PrintF("  step one instruction (alias 'si')\n");
        PrintF("print <register>\n");
        PrintF("  print register content (alias 'p')\n");
        PrintF("  use register name 'all' to print all registers\n");
        PrintF("  add argument 'fp' to print register pair double values\n");
        PrintF("printobject <register>\n");
        PrintF("  print an object from a register (alias 'po')\n");
        PrintF("flags\n");
        PrintF("  print flags\n");
        PrintF("stack [<words>]\n");
        PrintF("  dump stack content, default dump 10 words)\n");
        PrintF("mem <address> [<words>]\n");
        PrintF("  dump memory content, default dump 10 words)\n");
        PrintF("disasm [<instructions>]\n");
        PrintF("disasm [<address/register>]\n");
        PrintF("disasm [[<address/register>] <instructions>]\n");
        PrintF("  disassemble code, default is 10 instructions\n");
        PrintF("  from pc (alias 'di')\n");
        PrintF("gdb\n");
        PrintF("  enter gdb\n");
        PrintF("break <address>\n");
        PrintF("  set a break point on the address\n");
        PrintF("del\n");
        PrintF("  delete the breakpoint\n");
        PrintF("trace (alias 't')\n");
        PrintF("  toogle the tracing of all executed statements\n");
        PrintF("stop feature:\n");
        PrintF("  Description:\n");
        PrintF("    Stops are debug instructions inserted by\n");
        PrintF("    the Assembler::stop() function.\n");
        PrintF("    When hitting a stop, the Simulator will\n");
        PrintF("    stop and and give control to the PPCDebugger.\n");
        PrintF("    The first %d stop codes are watched:\n",
               Simulator::kNumOfWatchedStops);
        PrintF("    - They can be enabled / disabled: the Simulator\n");
        PrintF("      will / won't stop when hitting them.\n");
        PrintF("    - The Simulator keeps track of how many times they \n");
        PrintF("      are met. (See the info command.) Going over a\n");
        PrintF("      disabled stop still increases its counter. \n");
        PrintF("  Commands:\n");
        PrintF("    stop info all/<code> : print infos about number <code>\n");
        PrintF("      or all stop(s).\n");
        PrintF("    stop enable/disable all/<code> : enables / disables\n");
        PrintF("      all or number <code> stop(s)\n");
        PrintF("    stop unstop\n");
        PrintF("      ignore the stop instruction at the current location\n");
        PrintF("      from now on\n");
      } else {
        PrintF("Unknown command: %s\n", cmd);
      }
    }
  }

  // Add all the breakpoints back to stop execution and enter the debugger
  // shell when hit.
  RedoBreakpoints();
  // Restore tracing
  ::v8::internal::FLAG_trace_sim = trace;

#undef COMMAND_SIZE
#undef ARG_SIZE

#undef STR
#undef XSTR
}


static bool ICacheMatch(void* one, void* two) {
  ASSERT((reinterpret_cast<intptr_t>(one) & CachePage::kPageMask) == 0);
  ASSERT((reinterpret_cast<intptr_t>(two) & CachePage::kPageMask) == 0);
  return one == two;
}


static uint32_t ICacheHash(void* key) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key)) >> 2;
}


static bool AllOnOnePage(uintptr_t start, int size) {
  intptr_t start_page = (start & ~CachePage::kPageMask);
  intptr_t end_page = ((start + size) & ~CachePage::kPageMask);
  return start_page == end_page;
}


void Simulator::set_last_debugger_input(char* input) {
  DeleteArray(last_debugger_input_);
  last_debugger_input_ = input;
}


void Simulator::FlushICache(v8::internal::HashMap* i_cache,
                            void* start_addr,
                            size_t size) {
  intptr_t start = reinterpret_cast<intptr_t>(start_addr);
  int intra_line = (start & CachePage::kLineMask);
  start -= intra_line;
  size += intra_line;
  size = ((size - 1) | CachePage::kLineMask) + 1;
  int offset = (start & CachePage::kPageMask);
  while (!AllOnOnePage(start, size - 1)) {
    int bytes_to_flush = CachePage::kPageSize - offset;
    FlushOnePage(i_cache, start, bytes_to_flush);
    start += bytes_to_flush;
    size -= bytes_to_flush;
    ASSERT_EQ(0, start & CachePage::kPageMask);
    offset = 0;
  }
  if (size != 0) {
    FlushOnePage(i_cache, start, size);
  }
}


CachePage* Simulator::GetCachePage(v8::internal::HashMap* i_cache, void* page) {
  v8::internal::HashMap::Entry* entry = i_cache->Lookup(page,
                                                        ICacheHash(page),
                                                        true);
  if (entry->value == NULL) {
    CachePage* new_page = new CachePage();
    entry->value = new_page;
  }
  return reinterpret_cast<CachePage*>(entry->value);
}


// Flush from start up to and not including start + size.
void Simulator::FlushOnePage(v8::internal::HashMap* i_cache,
                             intptr_t start,
                             int size) {
  ASSERT(size <= CachePage::kPageSize);
  ASSERT(AllOnOnePage(start, size - 1));
  ASSERT((start & CachePage::kLineMask) == 0);
  ASSERT((size & CachePage::kLineMask) == 0);
  void* page = reinterpret_cast<void*>(start & (~CachePage::kPageMask));
  int offset = (start & CachePage::kPageMask);
  CachePage* cache_page = GetCachePage(i_cache, page);
  char* valid_bytemap = cache_page->ValidityByte(offset);
  memset(valid_bytemap, CachePage::LINE_INVALID, size >> CachePage::kLineShift);
}


void Simulator::CheckICache(v8::internal::HashMap* i_cache,
                            Instruction* instr) {
  intptr_t address = reinterpret_cast<intptr_t>(instr);
  void* page = reinterpret_cast<void*>(address & (~CachePage::kPageMask));
  void* line = reinterpret_cast<void*>(address & (~CachePage::kLineMask));
  int offset = (address & CachePage::kPageMask);
  CachePage* cache_page = GetCachePage(i_cache, page);
  char* cache_valid_byte = cache_page->ValidityByte(offset);
  bool cache_hit = (*cache_valid_byte == CachePage::LINE_VALID);
  char* cached_line = cache_page->CachedData(offset & ~CachePage::kLineMask);
  if (cache_hit) {
    // Check that the data in memory matches the contents of the I-cache.
    CHECK(memcmp(reinterpret_cast<void*>(instr),
                 cache_page->CachedData(offset),
                 Instruction::kInstrSize) == 0);
  } else {
    // Cache miss.  Load memory into the cache.
    memcpy(cached_line, line, CachePage::kLineLength);
    *cache_valid_byte = CachePage::LINE_VALID;
  }
}


void Simulator::Initialize(Isolate* isolate) {
  if (isolate->simulator_initialized()) return;
  isolate->set_simulator_initialized(true);
  ::v8::internal::ExternalReference::set_redirector(isolate,
                                                    &RedirectExternalReference);
}


Simulator::Simulator(Isolate* isolate) : isolate_(isolate) {
  i_cache_ = isolate_->simulator_i_cache();
  if (i_cache_ == NULL) {
    i_cache_ = new v8::internal::HashMap(&ICacheMatch);
    isolate_->set_simulator_i_cache(i_cache_);
  }
  Initialize(isolate);
  // Set up simulator support first. Some of this information is needed to
  // setup the architecture state.
  size_t stack_size = 1 * 1024*1024;  // allocate 1MB for stack
  stack_ = reinterpret_cast<char*>(malloc(stack_size));
  pc_modified_ = false;
  icount_ = 0;
  break_pc_ = NULL;
  break_instr_ = 0;

  // Set up architecture state.
  // All registers are initialized to zero to start with.
  for (int i = 0; i < num_registers; i++) {
    registers_[i] = 0;
  }
  condition_reg_ = 0;  // PowerPC
  special_reg_lr_ = 0;  // PowerPC
  special_reg_ctr_ = 0;  // PowerPC
  n_flag_ = false;
  z_flag_ = false;
  c_flag_ = false;
  v_flag_ = false;

  // Initializing FP registers.
  // All registers are initialized to zero to start with
  // even though s_registers_ & d_registers_ share the same
  // physical registers in the target.
  for (int i = 0; i < num_s_registers; i++) {
    fp_register[i] = 0;
  }
  n_flag_FPSCR_ = false;
  z_flag_FPSCR_ = false;
  c_flag_FPSCR_ = false;
  v_flag_FPSCR_ = false;
  FPSCR_rounding_mode_ = RZ;

  inv_op_vfp_flag_ = false;
  div_zero_vfp_flag_ = false;
  overflow_vfp_flag_ = false;
  underflow_vfp_flag_ = false;
  inexact_vfp_flag_ = false;

  // The sp is initialized to point to the bottom (high address) of the
  // allocated stack area. To be safe in potential stack underflows we leave
  // some buffer below.
  registers_[sp] = reinterpret_cast<int32_t>(stack_) + stack_size - 64;
  // The lr and pc are initialized to a known bad value that will cause an
  // access violation if the simulator ever tries to execute it.
  registers_[pc] = bad_lr;
  registers_[lr] = bad_lr;
  InitializeCoverage();

  last_debugger_input_ = NULL;
}


// When the generated code calls an external reference we need to catch that in
// the simulator.  The external reference will be a function compiled for the
// host architecture.  We need to call that function instead of trying to
// execute it with the simulator.  We do that by redirecting the external
// reference to a svc (Supervisor Call) instruction that is handled by
// the simulator.  We write the original destination of the jump just at a known
// offset from the svc instruction so the simulator knows what to call.
class Redirection {
 public:
  Redirection(void* external_function, ExternalReference::Type type)
      : external_function_(external_function),
        swi_instruction_(rtCallRedirInstr | kCallRtRedirected),
        type_(type),
        next_(NULL) {
    Isolate* isolate = Isolate::Current();
    next_ = isolate->simulator_redirection();
    Simulator::current(isolate)->
        FlushICache(isolate->simulator_i_cache(),
                    reinterpret_cast<void*>(&swi_instruction_),
                    Instruction::kInstrSize);
    isolate->set_simulator_redirection(this);
  }

  void* address_of_swi_instruction() {
    return reinterpret_cast<void*>(&swi_instruction_);
  }

  void* external_function() { return external_function_; }
  ExternalReference::Type type() { return type_; }

  static Redirection* Get(void* external_function,
                          ExternalReference::Type type) {
    Isolate* isolate = Isolate::Current();
    Redirection* current = isolate->simulator_redirection();
    for (; current != NULL; current = current->next_) {
      if (current->external_function_ == external_function) return current;
    }
    return new Redirection(external_function, type);
  }

  static Redirection* FromSwiInstruction(Instruction* swi_instruction) {
    char* addr_of_swi = reinterpret_cast<char*>(swi_instruction);
    char* addr_of_redirection =
        addr_of_swi - OFFSET_OF(Redirection, swi_instruction_);
    return reinterpret_cast<Redirection*>(addr_of_redirection);
  }

 private:
  void* external_function_;
  uint32_t swi_instruction_;
  ExternalReference::Type type_;
  Redirection* next_;
};


void* Simulator::RedirectExternalReference(void* external_function,
                                           ExternalReference::Type type) {
  Redirection* redirection = Redirection::Get(external_function, type);
  return redirection->address_of_swi_instruction();
}


// Get the active Simulator for the current thread.
Simulator* Simulator::current(Isolate* isolate) {
  v8::internal::Isolate::PerIsolateThreadData* isolate_data =
      isolate->FindOrAllocatePerThreadDataForThisThread();
  ASSERT(isolate_data != NULL);

  Simulator* sim = isolate_data->simulator();
  if (sim == NULL) {
    // TODO(146): delete the simulator object when a thread/isolate goes away.
    sim = new Simulator(isolate);
    isolate_data->set_simulator(sim);
  }
  return sim;
}


// Sets the register in the architecture state. It will also deal with updating
// Simulator internal state for special registers such as PC.
void Simulator::set_register(int reg, int32_t value) {
  ASSERT((reg >= 0) && (reg < num_registers));
  if (reg == pc) {
    pc_modified_ = true;
  }
  registers_[reg] = value;
}


// Get the register from the architecture state. This function does handle
// the special case of accessing the PC register.
int32_t Simulator::get_register(int reg) const {
  ASSERT((reg >= 0) && (reg < num_registers));
  // Stupid code added to avoid bug in GCC.
  // See: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=43949
  if (reg >= num_registers) return 0;
  // End stupid code.
  return registers_[reg] + ((reg == pc) ? Instruction::kPCReadOffset : 0);
}


double Simulator::get_double_from_register_pair(int reg) {
  ASSERT((reg >= 0) && (reg < num_registers) && ((reg % 2) == 0));

  double dm_val = 0.0;
  // Read the bits from the unsigned integer register_[] array
  // into the double precision floating point value and return it.
  char buffer[2 * sizeof(fp_register[0])];
  memcpy(buffer, &registers_[reg], 2 * sizeof(registers_[0]));
  memcpy(&dm_val, buffer, 2 * sizeof(registers_[0]));
  return(dm_val);
}


void Simulator::set_dw_register(int dreg, const int* dbl) {
  ASSERT((dreg >= 0) && (dreg < num_d_registers));
  registers_[dreg] = dbl[0];
  registers_[dreg + 1] = dbl[1];
}


// Raw access to the PC register.
void Simulator::set_pc(int32_t value) {
  pc_modified_ = true;
  registers_[pc] = value;
}


bool Simulator::has_bad_pc() const {
  return ((registers_[pc] == bad_lr) || (registers_[pc] == end_sim_pc));
}


// Raw access to the PC register without the special adjustment when reading.
int32_t Simulator::get_pc() const {
  return registers_[pc];
}


// Getting from and setting into FP registers.
void Simulator::set_s_register(int sreg, unsigned int value) {
  ASSERT((sreg >= 0) && (sreg < num_s_registers));
  fp_register[sreg] = value;
}


unsigned int Simulator::get_s_register(int sreg) const {
  ASSERT((sreg >= 0) && (sreg < num_s_registers));
  return fp_register[sreg];
}


template<class InputType, int register_size>
void Simulator::SetFPRegister(int reg_index, const InputType& value) {
  ASSERT(reg_index >= 0);
  if (register_size == 1) ASSERT(reg_index < num_s_registers);
  if (register_size == 2) ASSERT(reg_index < num_d_registers);

  char buffer[register_size * sizeof(fp_register[0])];
  memcpy(buffer, &value, register_size * sizeof(fp_register[0]));
  memcpy(&fp_register[reg_index * register_size], buffer,
         register_size * sizeof(fp_register[0]));
}


template<class ReturnType, int register_size>
ReturnType Simulator::GetFromFPRegister(int reg_index) {
  ASSERT(reg_index >= 0);
  if (register_size == 1) ASSERT(reg_index < num_s_registers);
  if (register_size == 2) ASSERT(reg_index < num_d_registers);

  ReturnType value = 0;
  char buffer[register_size * sizeof(fp_register[0])];
  memcpy(buffer, &fp_register[register_size * reg_index],
         register_size * sizeof(fp_register[0]));
  memcpy(&value, buffer, register_size * sizeof(fp_register[0]));
  return value;
}


// For use in calls that take two double values which are currently
// in r3-r6 and need to move to d0 and d1 (roohack??)
void Simulator::GetFpArgs(double* x, double* y) {
  *x = fp_register[0];
  *y = fp_register[1];
}

// For use in calls that take one double value, constructed either
// from r3 and r4 or d0. (roohack??)
void Simulator::GetFpArgs(double* x) {
  *x = vfp_register[0];
}


// For use in calls that take one double value constructed either
// from r3 and r4 or d0 and one integer value. (roohack??)
void Simulator::GetFpArgs(double* x, int32_t* y) {
  *x = vfp_register[0];
  *y = registers_[1];
}


// The return value is either in r0/r1 or d0.
void Simulator::SetFpResult(const double& result) {
  char buffer[2 * sizeof(fp_register[0])];
  memcpy(buffer, &result, sizeof(buffer));
  // Copy result to d0.
  memcpy(fp_register, buffer, sizeof(buffer));
}


void Simulator::TrashCallerSaveRegisters() {
  // We don't trash the registers with the return value.
#if 0 
-- roohack 
A good idea to trash volatile registers, needs to be done 
  registers_[2] = 0x50Bad4U;
  registers_[3] = 0x50Bad4U;
  registers_[12] = 0x50Bad4U;
#endif
}


int Simulator::ReadW(int32_t addr, Instruction* instr) {
  intptr_t* ptr = reinterpret_cast<intptr_t*>(addr);
  return *ptr;
}


void Simulator::WriteW(int32_t addr, int value, Instruction* instr) {
  intptr_t* ptr = reinterpret_cast<intptr_t*>(addr);
  *ptr = value;
  return;
}


uint16_t Simulator::ReadHU(int32_t addr, Instruction* instr) {
  uint16_t* ptr = reinterpret_cast<uint16_t*>(addr);
  return *ptr;
}


int16_t Simulator::ReadH(int32_t addr, Instruction* instr) {
  int16_t* ptr = reinterpret_cast<int16_t*>(addr);
  return *ptr;
}


void Simulator::WriteH(int32_t addr, uint16_t value, Instruction* instr) {
  uint16_t* ptr = reinterpret_cast<uint16_t*>(addr);
  *ptr = value;
  return;
}


void Simulator::WriteH(int32_t addr, int16_t value, Instruction* instr) {
  int16_t* ptr = reinterpret_cast<int16_t*>(addr);
  *ptr = value;
  return;
}


uint8_t Simulator::ReadBU(int32_t addr) {
  uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
  return *ptr;
}


int8_t Simulator::ReadB(int32_t addr) {
  int8_t* ptr = reinterpret_cast<int8_t*>(addr);
  return *ptr;
}


void Simulator::WriteB(int32_t addr, uint8_t value) {
  uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
  *ptr = value;
}


void Simulator::WriteB(int32_t addr, int8_t value) {
  int8_t* ptr = reinterpret_cast<int8_t*>(addr);
  *ptr = value;
}


int32_t* Simulator::ReadDW(int32_t addr) {
  int32_t* ptr = reinterpret_cast<int32_t*>(addr);
  return ptr;
}


void Simulator::WriteDW(int32_t addr, int32_t value1, int32_t value2) {
  int32_t* ptr = reinterpret_cast<int32_t*>(addr);
  *ptr++ = value1;
  *ptr = value2;
  return;
}


// Returns the limit of the stack area to enable checking for stack overflows.
uintptr_t Simulator::StackLimit() const {
  // Leave a safety margin of 1024 bytes to prevent overrunning the stack when
  // pushing values.
  return reinterpret_cast<uintptr_t>(stack_) + 1024;
}


// Unsupported instructions use Format to print an error and stop execution.
void Simulator::Format(Instruction* instr, const char* format) {
  PrintF("Simulator found unsupported instruction:\n 0x%08x: %s\n",
         reinterpret_cast<intptr_t>(instr), format);
  UNIMPLEMENTED();
}


// Checks if the current instruction should be executed based on its
// condition bits.
bool Simulator::ConditionallyExecute(Instruction* instr) {
  switch (instr->ConditionField()) {
    case eq: return z_flag_;
    case ne: return !z_flag_;
    case cs: return c_flag_;
    case cc: return !c_flag_;
    case mi: return n_flag_;
    case pl: return !n_flag_;
    case vs: return v_flag_;
    case vc: return !v_flag_;
    case hi: return c_flag_ && !z_flag_;
    case ls: return !c_flag_ || z_flag_;
    case ge: return n_flag_ == v_flag_;
    case lt: return n_flag_ != v_flag_;
    case gt: return !z_flag_ && (n_flag_ == v_flag_);
    case le: return z_flag_ || (n_flag_ != v_flag_);
    case al: return true;
    default: UNREACHABLE();
  }
  return false;
}


// Calculate and set the Negative and Zero flags.
void Simulator::SetNZFlags(int32_t val) {
  n_flag_ = (val < 0);
  z_flag_ = (val == 0);
}


// Set the Carry flag.
void Simulator::SetCFlag(bool val) {
  c_flag_ = val;
}


// Set the oVerflow flag.
void Simulator::SetVFlag(bool val) {
  v_flag_ = val;
}


// Calculate C flag value for additions.
bool Simulator::CarryFrom(int32_t left, int32_t right, int32_t carry) {
  uint32_t uleft = static_cast<uint32_t>(left);
  uint32_t uright = static_cast<uint32_t>(right);
  uint32_t urest  = 0xffffffffU - uleft;

  return (uright > urest) ||
         (carry && (((uright + 1) > urest) || (uright > (urest - 1))));
}


// Calculate C flag value for subtractions.
bool Simulator::BorrowFrom(int32_t left, int32_t right) {
  uint32_t uleft = static_cast<uint32_t>(left);
  uint32_t uright = static_cast<uint32_t>(right);

  return (uright > uleft);
}


// Calculate V flag value for additions and subtractions.
bool Simulator::OverflowFrom(int32_t alu_out,
                             int32_t left, int32_t right, bool addition) {
  bool overflow;
  if (addition) {
               // operands have the same sign
    overflow = ((left >= 0 && right >= 0) || (left < 0 && right < 0))
               // and operands and result have different sign
               && ((left < 0 && alu_out >= 0) || (left >= 0 && alu_out < 0));
  } else {
               // operands have different signs
    overflow = ((left < 0 && right >= 0) || (left >= 0 && right < 0))
               // and first operand and result have different signs
               && ((left < 0 && alu_out >= 0) || (left >= 0 && alu_out < 0));
  }
  return overflow;
}


// Support for VFP comparisons.
void Simulator::Compute_FPSCR_Flags(double val1, double val2) {
  if (isnan(val1) || isnan(val2)) {
    n_flag_FPSCR_ = false;
    z_flag_FPSCR_ = false;
    c_flag_FPSCR_ = true;
    v_flag_FPSCR_ = true;
  // All non-NaN cases.
  } else if (val1 == val2) {
    n_flag_FPSCR_ = false;
    z_flag_FPSCR_ = true;
    c_flag_FPSCR_ = true;
    v_flag_FPSCR_ = false;
  } else if (val1 < val2) {
    n_flag_FPSCR_ = true;
    z_flag_FPSCR_ = false;
    c_flag_FPSCR_ = false;
    v_flag_FPSCR_ = false;
  } else {
    // Case when (val1 > val2).
    n_flag_FPSCR_ = false;
    z_flag_FPSCR_ = false;
    c_flag_FPSCR_ = true;
    v_flag_FPSCR_ = false;
  }
}


void Simulator::Copy_FPSCR_to_APSR() {
  n_flag_ = n_flag_FPSCR_;
  z_flag_ = z_flag_FPSCR_;
  c_flag_ = c_flag_FPSCR_;
  v_flag_ = v_flag_FPSCR_;
}


// Addressing Mode 1 - Data-processing operands:
// Get the value based on the shifter_operand with register.
int32_t Simulator::GetShiftRm(Instruction* instr, bool* carry_out) {
  ShiftOp shift = instr->ShiftField();
  int shift_amount = instr->ShiftAmountValue();
  int32_t result = get_register(instr->RmValue());
  if (instr->Bit(4) == 0) {
    // by immediate
    if ((shift == ROR) && (shift_amount == 0)) {
      UNIMPLEMENTED();
      return result;
    } else if (((shift == LSR) || (shift == ASR)) && (shift_amount == 0)) {
      shift_amount = 32;
    }
    switch (shift) {
      case ASR: {
        if (shift_amount == 0) {
          if (result < 0) {
            result = 0xffffffff;
            *carry_out = true;
          } else {
            result = 0;
            *carry_out = false;
          }
        } else {
          result >>= (shift_amount - 1);
          *carry_out = (result & 1) == 1;
          result >>= 1;
        }
        break;
      }

      case LSL: {
        if (shift_amount == 0) {
          *carry_out = c_flag_;
        } else {
          result <<= (shift_amount - 1);
          *carry_out = (result < 0);
          result <<= 1;
        }
        break;
      }

      case LSR: {
        if (shift_amount == 0) {
          result = 0;
          *carry_out = c_flag_;
        } else {
          uint32_t uresult = static_cast<uint32_t>(result);
          uresult >>= (shift_amount - 1);
          *carry_out = (uresult & 1) == 1;
          uresult >>= 1;
          result = static_cast<int32_t>(uresult);
        }
        break;
      }

      case ROR: {
        UNIMPLEMENTED();
        break;
      }

      default: {
        UNREACHABLE();
        break;
      }
    }
  } else {
    // by register
    int rs = instr->RsValue();
    shift_amount = get_register(rs) &0xff;
    switch (shift) {
      case ASR: {
        if (shift_amount == 0) {
          *carry_out = c_flag_;
        } else if (shift_amount < 32) {
          result >>= (shift_amount - 1);
          *carry_out = (result & 1) == 1;
          result >>= 1;
        } else {
          ASSERT(shift_amount >= 32);
          if (result < 0) {
            *carry_out = true;
            result = 0xffffffff;
          } else {
            *carry_out = false;
            result = 0;
          }
        }
        break;
      }

      case LSL: {
        if (shift_amount == 0) {
          *carry_out = c_flag_;
        } else if (shift_amount < 32) {
          result <<= (shift_amount - 1);
          *carry_out = (result < 0);
          result <<= 1;
        } else if (shift_amount == 32) {
          *carry_out = (result & 1) == 1;
          result = 0;
        } else {
          ASSERT(shift_amount > 32);
          *carry_out = false;
          result = 0;
        }
        break;
      }

      case LSR: {
        if (shift_amount == 0) {
          *carry_out = c_flag_;
        } else if (shift_amount < 32) {
          uint32_t uresult = static_cast<uint32_t>(result);
          uresult >>= (shift_amount - 1);
          *carry_out = (uresult & 1) == 1;
          uresult >>= 1;
          result = static_cast<int32_t>(uresult);
        } else if (shift_amount == 32) {
          *carry_out = (result < 0);
          result = 0;
        } else {
          *carry_out = false;
          result = 0;
        }
        break;
      }

      case ROR: {
        UNIMPLEMENTED();
        break;
      }

      default: {
        UNREACHABLE();
        break;
      }
    }
  }
  return result;
}


// Addressing Mode 1 - Data-processing operands:
// Get the value based on the shifter_operand with immediate.
int32_t Simulator::GetImm(Instruction* instr, bool* carry_out) {
  int rotate = instr->RotateValue() * 2;
  int immed8 = instr->Immed8Value();
  int imm = (immed8 >> rotate) | (immed8 << (32 - rotate));
  *carry_out = (rotate == 0) ? c_flag_ : (imm < 0);
  return imm;
}


static int count_bits(int bit_vector) {
  int count = 0;
  while (bit_vector != 0) {
    if ((bit_vector & 1) != 0) {
      count++;
    }
    bit_vector >>= 1;
  }
  return count;
}


void Simulator::ProcessPUW(Instruction* instr,
                           int num_regs,
                           int reg_size,
                           intptr_t* start_address,
                           intptr_t* end_address) {
  int rn = instr->RnValue();
  int32_t rn_val = get_register(rn);
  switch (instr->PUField()) {
    case da_x: {
      UNIMPLEMENTED();
      break;
    }
    case ia_x: {
      *start_address = rn_val;
      *end_address = rn_val + (num_regs * reg_size) - reg_size;
      rn_val = rn_val + (num_regs * reg_size);
      break;
    }
    case db_x: {
      *start_address = rn_val - (num_regs * reg_size);
      *end_address = rn_val - reg_size;
      rn_val = *start_address;
      break;
    }
    case ib_x: {
      *start_address = rn_val + reg_size;
      *end_address = rn_val + (num_regs * reg_size);
      rn_val = *end_address;
      break;
    }
    default: {
      UNREACHABLE();
      break;
    }
  }
  if (instr->HasW()) {
    set_register(rn, rn_val);
  }
}

// Addressing Mode 4 - Load and Store Multiple
void Simulator::HandleRList(Instruction* instr, bool load) {
  int rlist = instr->RlistValue();
  int num_regs = count_bits(rlist);

  intptr_t start_address = 0;
  intptr_t end_address = 0;
  ProcessPUW(instr, num_regs, kPointerSize, &start_address, &end_address);

  intptr_t* address = reinterpret_cast<intptr_t*>(start_address);
  // Catch null pointers a little earlier.
  ASSERT(start_address > 8191 || start_address < 0);
  int reg = 0;
  while (rlist != 0) {
    if ((rlist & 1) != 0) {
      if (load) {
        set_register(reg, *address);
      } else {
        *address = get_register(reg);
      }
      address += 1;
    }
    reg++;
    rlist >>= 1;
  }
  ASSERT(end_address == ((intptr_t)address) - 4);
}


// Addressing Mode 6 - Load and Store Multiple Coprocessor registers.
void Simulator::HandleVList(Instruction* instr) {
  VFPRegPrecision precision =
      (instr->SzValue() == 0) ? kSinglePrecision : kDoublePrecision;
  int operand_size = (precision == kSinglePrecision) ? 4 : 8;

  bool load = (instr->VLValue() == 0x1);

  int vd;
  int num_regs;
  vd = instr->VFPDRegValue(precision);
  if (precision == kSinglePrecision) {
    num_regs = instr->Immed8Value();
  } else {
    num_regs = instr->Immed8Value() / 2;
  }

  intptr_t start_address = 0;
  intptr_t end_address = 0;
  ProcessPUW(instr, num_regs, operand_size, &start_address, &end_address);

  intptr_t* address = reinterpret_cast<intptr_t*>(start_address);
  for (int reg = vd; reg < vd + num_regs; reg++) {
    if (precision == kSinglePrecision) {
      if (load) {
        set_s_register_from_sinteger(
            reg, ReadW(reinterpret_cast<int32_t>(address), instr));
      } else {
        WriteW(reinterpret_cast<int32_t>(address),
               get_sinteger_from_s_register(reg), instr);
      }
      address += 1;
    } else {
      if (load) {
        set_s_register_from_sinteger(
            2 * reg, ReadW(reinterpret_cast<int32_t>(address), instr));
        set_s_register_from_sinteger(
            2 * reg + 1, ReadW(reinterpret_cast<int32_t>(address + 1), instr));
      } else {
        WriteW(reinterpret_cast<int32_t>(address),
               get_sinteger_from_s_register(2 * reg), instr);
        WriteW(reinterpret_cast<int32_t>(address + 1),
               get_sinteger_from_s_register(2 * reg + 1), instr);
      }
      address += 2;
    }
  }
  ASSERT(reinterpret_cast<intptr_t>(address) - operand_size == end_address);
}


// Calls into the V8 runtime are based on this very simple interface.
// Note: To be able to return two values from some calls the code in runtime.cc
// uses the ObjectPair which is essentially two 32-bit values stuffed into a
// 64-bit value. With the code below we assume that all runtime calls return
// 64 bits of result. If they don't, the r1 result register contains a bogus
// value, which is fine because it is caller-saved.
typedef int64_t (*SimulatorRuntimeCall)(int32_t arg0,
                                        int32_t arg1,
                                        int32_t arg2,
                                        int32_t arg3,
                                        int32_t arg4,
                                        int32_t arg5);
#if defined(V8_HOST_ARCH_PPC)
typedef double (*SimulatorRuntimeFPCall)(double arg0,
                                         double arg1);
typedef int64_t (*SimulatorRuntimeFPCallX)(double arg0,
                                         double arg1);
typedef double (*SimulatorRuntimeFPCallY)(double arg0,
                                         int32_t arg1);
#else
typedef double (*SimulatorRuntimeFPCall)(int32_t arg0,
                                         int32_t arg1,
                                         int32_t arg2,
                                         int32_t arg3);
#endif

// This signature supports direct call in to API function native callback
// (refer to InvocationCallback in v8.h).
typedef v8::Handle<v8::Value> (*SimulatorRuntimeDirectApiCall)(int32_t arg0);

// This signature supports direct call to accessor getter callback.
typedef v8::Handle<v8::Value> (*SimulatorRuntimeDirectGetterCall)(int32_t arg0,
                                                                  int32_t arg1);

// Software interrupt instructions are used by the simulator to call into the
// C-based V8 runtime.
void Simulator::SoftwareInterrupt(Instruction* instr) {
  int svc = instr->SvcValue();
  switch (svc) {
    case kCallRtRedirected: {
      // Check if stack is aligned. Error if not aligned is reported below to
      // include information on the function called.
      bool stack_aligned =
          (get_register(sp)
           & (::v8::internal::FLAG_sim_stack_alignment - 1)) == 0;
      Redirection* redirection = Redirection::FromSwiInstruction(instr);
      int32_t arg0 = get_register(r3);
      int32_t arg1 = get_register(r4);
      int32_t arg2 = get_register(r5);
      int32_t arg3 = get_register(r6);
      int32_t* stack_pointer = reinterpret_cast<int32_t*>(get_register(sp));
      int32_t arg4 = stack_pointer[0];
      int32_t arg5 = stack_pointer[1];
      bool fp_call =
         (redirection->type() == ExternalReference::BUILTIN_FP_FP_CALL) ||
         (redirection->type() == ExternalReference::BUILTIN_COMPARE_CALL) ||
         (redirection->type() == ExternalReference::BUILTIN_FP_CALL) ||
         (redirection->type() == ExternalReference::BUILTIN_FP_INT_CALL);
      if (use_eabi_hardfloat()) {
        // With the hard floating point calling convention, double
        // arguments are passed in VFP registers. Fetch the arguments
        // from there and call the builtin using soft floating point
        // convention.
        switch (redirection->type()) {
        case ExternalReference::BUILTIN_FP_FP_CALL:
        case ExternalReference::BUILTIN_COMPARE_CALL:
          arg0 = vfp_register[0];
          arg1 = vfp_register[1];
          arg2 = vfp_register[2];
          arg3 = vfp_register[3];
          break;
        case ExternalReference::BUILTIN_FP_CALL:
          arg0 = vfp_register[0];
          arg1 = vfp_register[1];
          break;
        case ExternalReference::BUILTIN_FP_INT_CALL:
          arg0 = vfp_register[0];
          arg1 = vfp_register[1];
          arg2 = get_register(0);
          break;
        default:
          break;
        }
      }
      // This is dodgy but it works because the C entry stubs are never moved.
      // See comment in codegen-arm.cc and bug 1242173.
      int32_t saved_lr = special_reg_lr_;
      intptr_t external =
          reinterpret_cast<intptr_t>(redirection->external_function());
      if (fp_call) {
        if (::v8::internal::FLAG_trace_sim || !stack_aligned) {
          SimulatorRuntimeFPCall target =
              reinterpret_cast<SimulatorRuntimeFPCall>(external);
          double dval0, dval1;
          int32_t ival;
          switch (redirection->type()) {
          case ExternalReference::BUILTIN_FP_FP_CALL:
          case ExternalReference::BUILTIN_COMPARE_CALL:
            GetFpArgs(&dval0, &dval1);
            PrintF("Call to host function at %p with args %f, %f",
                FUNCTION_ADDR(target), dval0, dval1);
            break;
          case ExternalReference::BUILTIN_FP_CALL:
            GetFpArgs(&dval0);
            PrintF("Call to host function at %p with arg %f",
                FUNCTION_ADDR(target), dval0);
            break;
          case ExternalReference::BUILTIN_FP_INT_CALL:
            GetFpArgs(&dval0, &ival);
            PrintF("Call to host function at %p with args %f, %d",
                FUNCTION_ADDR(target), dval0, ival);
            break;
          default:
            UNREACHABLE();
            break;
          }
          if (!stack_aligned) {
            PrintF(" with unaligned stack %08x\n", get_register(sp));
          }
          PrintF("\n");
        }
        CHECK(stack_aligned);
#if defined(V8_HOST_ARCH_PPC)
        SimulatorRuntimeFPCall target =
            reinterpret_cast<SimulatorRuntimeFPCall>(external);
        SimulatorRuntimeFPCallX targetx =
            reinterpret_cast<SimulatorRuntimeFPCallX>(external);
        SimulatorRuntimeFPCallY targety =
            reinterpret_cast<SimulatorRuntimeFPCallY>(external);
        double dval0, dval1, result;
        int32_t ival, lo_res, hi_res;
        int64_t iresult;
        switch (redirection->type()) {
        case ExternalReference::BUILTIN_FP_FP_CALL:
          GetFpArgs(&dval0, &dval1);
          result = target(dval0, dval1);
          SetFpResult(result);
          break;
        case ExternalReference::BUILTIN_COMPARE_CALL:
          GetFpArgs(&dval0, &dval1);
          iresult = targetx(dval0, dval1);
          hi_res = static_cast<int32_t>(iresult);
          lo_res = static_cast<int32_t>(iresult >> 32);
          set_register(r0, lo_res);
          set_register(r1, hi_res);
          break;
        case ExternalReference::BUILTIN_FP_CALL:
          GetFpArgs(&dval0);
          result = target(dval0, dval1); // 2nd parm ignored
          SetFpResult(result);
          break;
        case ExternalReference::BUILTIN_FP_INT_CALL:
          GetFpArgs(&dval0, &ival);
          result = targety(dval0, ival);
          SetFpResult(result);
          break;
        default:
          UNREACHABLE();
          break;
        }
#else
        if (redirection->type() != ExternalReference::BUILTIN_COMPARE_CALL) {
          SimulatorRuntimeFPCall target =
              reinterpret_cast<SimulatorRuntimeFPCall>(external);
          double result = target(arg0, arg1, arg2, arg3);
          SetFpResult(result);
        } else {
          SimulatorRuntimeCall target =
              reinterpret_cast<SimulatorRuntimeCall>(external);
          int64_t result = target(arg0, arg1, arg2, arg3, arg4, arg5);
          int32_t lo_res = static_cast<int32_t>(result);
          int32_t hi_res = static_cast<int32_t>(result >> 32);
          if (::v8::internal::FLAG_trace_sim) {
            PrintF("Returned %08x\n", lo_res);
          }
          set_register(r3, lo_res);
          set_register(r4, hi_res);
        }
#endif

      } else if (redirection->type() == ExternalReference::DIRECT_API_CALL) {
        SimulatorRuntimeDirectApiCall target =
            reinterpret_cast<SimulatorRuntimeDirectApiCall>(external);
        if (::v8::internal::FLAG_trace_sim || !stack_aligned) {
          PrintF("Call to host function at %p args %08x",
              FUNCTION_ADDR(target), arg0);
          if (!stack_aligned) {
            PrintF(" with unaligned stack %08x\n", get_register(sp));
          }
          PrintF("\n");
        }
        CHECK(stack_aligned);
        v8::Handle<v8::Value> result = target(arg0);
        if (::v8::internal::FLAG_trace_sim) {
          PrintF("Returned %p\n", reinterpret_cast<void *>(*result));
        }
        set_register(r0, (int32_t) *result);
      } else if (redirection->type() == ExternalReference::DIRECT_GETTER_CALL) {
        SimulatorRuntimeDirectGetterCall target =
            reinterpret_cast<SimulatorRuntimeDirectGetterCall>(external);
        if (::v8::internal::FLAG_trace_sim || !stack_aligned) {
          PrintF("Call to host function at %p args %08x %08x",
              FUNCTION_ADDR(target), arg0, arg1);
          if (!stack_aligned) {
            PrintF(" with unaligned stack %08x\n", get_register(sp));
          }
          PrintF("\n");
        }
        CHECK(stack_aligned);
        v8::Handle<v8::Value> result = target(arg0, arg1);
        if (::v8::internal::FLAG_trace_sim) {
          PrintF("Returned %p\n", reinterpret_cast<void *>(*result));
        }
        set_register(r0, (int32_t) *result);
      } else {
        // builtin call.
        ASSERT(redirection->type() == ExternalReference::BUILTIN_CALL);
        SimulatorRuntimeCall target =
            reinterpret_cast<SimulatorRuntimeCall>(external);
        if (::v8::internal::FLAG_trace_sim || !stack_aligned) {
          PrintF(
              "Call to host function at %p"
              "args %08x, %08x, %08x, %08x, %08x, %08x",
              FUNCTION_ADDR(target),
              arg0,
              arg1,
              arg2,
              arg3,
              arg4,
              arg5);
          if (!stack_aligned) {
            PrintF(" with unaligned stack %08x\n", get_register(sp));
          }
          PrintF("\n");
        }
        CHECK(stack_aligned);
        int64_t result = target(arg0, arg1, arg2, arg3, arg4, arg5);
#if V8_HOST_ARCH_PPC
        int32_t hi_res = static_cast<int32_t>(result);
        int32_t lo_res = static_cast<int32_t>(result >> 32);
#else
        int32_t lo_res = static_cast<int32_t>(result);
        int32_t hi_res = static_cast<int32_t>(result >> 32);
#endif
        if (::v8::internal::FLAG_trace_sim) {
          PrintF("Returned %08x\n", lo_res);
        }
        set_register(r3, lo_res);
        set_register(r4, hi_res);
      }
      set_pc(saved_lr);
      break;
    }
    case kBreakpoint: {
      PPCDebugger dbg(this);
      dbg.Debug();
      break;
    }
    // stop uses all codes greater than 1 << 23.
    default: {
      if (svc >= (1 << 23)) {
        uint32_t code = svc & kStopCodeMask;
        if (isWatchedStop(code)) {
          IncreaseStopCounter(code);
        }
        // Stop if it is enabled, otherwise go on jumping over the stop
        // and the message address.
        if (isEnabledStop(code)) {
          PPCDebugger dbg(this);
          dbg.Stop(instr);
        } else {
          set_pc(get_pc() + 2 * Instruction::kInstrSize);
        }
      } else {
        // This is not a valid svc code.
        UNREACHABLE();
        break;
      }
    }
  }
}


// Stop helper functions.
bool Simulator::isStopInstruction(Instruction* instr) {
  return (instr->Bits(27, 24) == 0xF) && (instr->SvcValue() >= kStopCode);
}


bool Simulator::isWatchedStop(uint32_t code) {
  ASSERT(code <= kMaxStopCode);
  return code < kNumOfWatchedStops;
}


bool Simulator::isEnabledStop(uint32_t code) {
  ASSERT(code <= kMaxStopCode);
  // Unwatched stops are always enabled.
  return !isWatchedStop(code) ||
    !(watched_stops[code].count & kStopDisabledBit);
}


void Simulator::EnableStop(uint32_t code) {
  ASSERT(isWatchedStop(code));
  if (!isEnabledStop(code)) {
    watched_stops[code].count &= ~kStopDisabledBit;
  }
}


void Simulator::DisableStop(uint32_t code) {
  ASSERT(isWatchedStop(code));
  if (isEnabledStop(code)) {
    watched_stops[code].count |= kStopDisabledBit;
  }
}


void Simulator::IncreaseStopCounter(uint32_t code) {
  ASSERT(code <= kMaxStopCode);
  ASSERT(isWatchedStop(code));
  if ((watched_stops[code].count & ~(1 << 31)) == 0x7fffffff) {
    PrintF("Stop counter for code %i has overflowed.\n"
           "Enabling this code and reseting the counter to 0.\n", code);
    watched_stops[code].count = 0;
    EnableStop(code);
  } else {
    watched_stops[code].count++;
  }
}


// Print a stop status.
void Simulator::PrintStopInfo(uint32_t code) {
  ASSERT(code <= kMaxStopCode);
  if (!isWatchedStop(code)) {
    PrintF("Stop not watched.");
  } else {
    const char* state = isEnabledStop(code) ? "Enabled" : "Disabled";
    int32_t count = watched_stops[code].count & ~kStopDisabledBit;
    // Don't print the state of unused breakpoints.
    if (count != 0) {
      if (watched_stops[code].desc) {
        PrintF("stop %i - 0x%x: \t%s, \tcounter = %i, \t%s\n",
               code, code, state, count, watched_stops[code].desc);
      } else {
        PrintF("stop %i - 0x%x: \t%s, \tcounter = %i\n",
               code, code, state, count);
      }
    }
  }
}


// Handle execution based on instruction types.
void Simulator::DecodeExt1(Instruction* instr) {
  switch(instr->Bits(10,1) << 1) {
    case MCRF:
      UNIMPLEMENTED();  // Not used by V8.
    case BCLRX: {
        // need to check BO flag
        int old_pc = get_pc();
        set_pc(special_reg_lr_);
        if(instr->Bit(0) == 1) {  // LK flag set 
          special_reg_lr_ = old_pc + 4;
        }
      break;
    }
    case BCCTRX: {
        // need to check BO flag
        int old_pc = get_pc();
        set_pc(special_reg_ctr_);
        if(instr->Bit(0) == 1) {  // LK flag set 
          special_reg_lr_ = old_pc + 4;
        }
      break;
    }
    case CRNOR:
    case RFI:
    case CRANDC:
    case ISYNC:
      UNIMPLEMENTED();
    case CRXOR: {
      int bt = instr->Bits(25,21);
      int ba = instr->Bits(20,16);
      int bb = instr->Bits(15,11);
      int ba_val = ((0x10000000 >> ba) & condition_reg_) == 0 ? 0 : 1;
      int bb_val = ((0x10000000 >> bb) & condition_reg_) == 0 ? 0 : 1;
      int bt_val = ba_val ^ bb_val;
      bt_val = bt_val << (31-bt);  // shift bit to correct destination
      condition_reg_ &= ~(0x10000000 >> bt);
      condition_reg_ |= bt_val;
      break;
    }
    case CRNAND:
    case CRAND:
    case CREQV:
    case CRORC:
    case CROR:
    default: {
      UNIMPLEMENTED();  // Not used by V8.
    }
  }
}

void Simulator::DecodeExt2(Instruction* instr) {
  // Check first the 10-1 bit versions
  switch(instr->Bits(10,1) << 1) {
    case SRWX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      uint32_t rs_val = get_register(rs);
      uint32_t rb_val = get_register(rb);
      uint32_t result = rs_val >> rb_val;
      set_register(ra, result);
      return;
    }
    case SRAW: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int32_t rs_val = get_register(rs);
      int32_t rb_val = get_register(rb);
      int32_t result = rs_val >> rb_val;
      set_register(ra, result);
      return;
    }
    case SRAWIX: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      int32_t rs_val = get_register(rs);
      int sh = instr->Bits(15,11);
      int rc = instr->Bit(0);
      int result = rs_val >> sh;
      set_register(ra, result);
      if(rc) {
        int bf = 0;
        if(result < 0) { bf |= 0x80000000; }
        if(result > 0) { bf |= 0x40000000; }
        if(result == 0) { bf |= 0x20000000; }
        condition_reg_ = (condition_reg_ & ~0xF0000000) | bf;
      }
      return;
    }
  }
  // Now look at the lesser encodings
  switch(instr->Bits(9,1) << 1) {
    case CMP: {
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int32_t ra_val = get_register(ra);
      int32_t rb_val = get_register(rb);
      int cr = instr->Bits(25,23);
      int bf = 0;
      if(ra_val < rb_val) { bf |= 0x80000000; }
      if(ra_val > rb_val) { bf |= 0x40000000; }
      if(ra_val == rb_val) { bf |= 0x20000000; }
      int condition_mask = 0xF0000000 >> (cr*4);
      int condition =  bf >> (cr*4);
      condition_reg_ = (condition_reg_ & ~condition_mask) | condition;
      break;
    }
    case SUBFCX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      // int oe = instr->Bit(10);
      // int rc = instr->Bit(0);
      uint32_t ra_val = get_register(ra);
      uint32_t rb_val = get_register(rb);
      uint32_t alu_out = rb_val - ra_val;
      set_register(rt, alu_out);
      if(ra_val > rb_val) {
        special_reg_xer_ = (special_reg_xer_ & ~0xF0000000) | 0x20000000;
      } else {
        special_reg_xer_ &= ~0xF0000000;
      }
      // todo - handle OE and RC bits
      break;
    }
    case ADDCX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      // int oe = instr->Bit(10);
      // int rc = instr->Bit(0);
      int32_t ra_val = get_register(ra);
      int32_t rb_val = get_register(rb);
      int64_t alu_out = ra_val + rb_val;
      if(alu_out >> 32) {
        alu_out &= 0xFFFFFFFF;
        special_reg_xer_ = (special_reg_xer_ & ~0xF0000000) | 0x20000000;
      } else {
        special_reg_xer_ &= ~0xF0000000;
      }
      set_register(rt, alu_out);
      // todo - handle OE and RC bits
      break;
    }
    case SLWX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      uint32_t rs_val = get_register(rs);
      uint32_t rb_val = get_register(rb);
      uint32_t result = rs_val << rb_val;
      set_register(ra, result);
      break;
    }
    case ANDX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int32_t rs_val = get_register(rs);
      int32_t rb_val = get_register(rb);
      int32_t alu_out = rs_val & rb_val;
      set_register(ra, alu_out);
      if(instr->Bit(0)) {  // RCBit set
        int bf = 0;
        if(alu_out < 0) { bf |= 0x80000000; }
        if(alu_out > 0) { bf |= 0x40000000; }
        if(alu_out == 0) { bf |= 0x20000000; }
        condition_reg_ = (condition_reg_ & ~0xF0000000) | bf;
      } 
      break;
    }
    case CMPL: {
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      uint32_t ra_val = get_register(ra);
      uint32_t rb_val = get_register(rb);
      int cr = instr->Bits(25,23);
      int bf = 0;
      if(ra_val < rb_val) { bf |= 0x80000000; }
      if(ra_val > rb_val) { bf |= 0x40000000; }
      if(ra_val == rb_val) { bf |= 0x20000000; }
      int condition_mask = 0xF0000000 >> (cr*4);
      int condition =  bf >> (cr*4);
      condition_reg_ = (condition_reg_ & ~condition_mask) | condition;
      break;
    }
    case SUBFX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      // int oe = instr->Bit(10);
      int rc = instr->Bit(0);
      int32_t ra_val = get_register(ra);
      int32_t rb_val = get_register(rb);
      int32_t alu_out = rb_val - ra_val;
      if(rc) {
        int bf = 0;
        if(alu_out < 0) { bf |= 0x80000000; }
        if(alu_out > 0) { bf |= 0x40000000; }
        if(alu_out == 0) { bf |= 0x20000000; }
        condition_reg_ = (condition_reg_ & ~0xF0000000) | bf;
      } 
      // todo - figure out underflow
      set_register(rt, alu_out);
      // todo - handle OE and RC bits
      break;
    }
    case ADDZEX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int32_t ra_val = get_register(ra);
      if(special_reg_xer_ & 0x20000000) {
        ra_val += 1;
      }
      set_register(rt, ra_val);
      if(instr->Bit(0)) {  // RCBit set
        int bf = 0;
        if(ra_val < 0) { bf |= 0x80000000; }
        if(ra_val > 0) { bf |= 0x40000000; }
        if(ra_val == 0) { bf |= 0x20000000; }
        condition_reg_ = (condition_reg_ & ~0xF0000000) | bf;
      }
      // todo - handle OE bit
      break;
    }
    case MULLW: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int32_t ra_val = get_register(ra);
      int32_t rb_val = get_register(rb);
      int32_t alu_out = ra_val * rb_val;
      set_register(rt, alu_out);
      // todo - handle OE and RC bits
      break;
    }
    case ADDX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      // int oe = instr->Bit(10);
      // int rc = instr->Bit(0);
      int32_t ra_val = get_register(ra);
      int32_t rb_val = get_register(rb);
      int32_t alu_out = ra_val + rb_val;
      set_register(rt, alu_out);
      // todo - handle OE and RC bits
      break;
    }
    case XORX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int32_t rs_val = get_register(rs);
      int32_t rb_val = get_register(rb);
      int32_t alu_out = rs_val ^ rb_val;
      set_register(ra, alu_out);
      // todo - handle RC bit
      break;
    }
    case ORX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int32_t rs_val = get_register(rs);
      int32_t rb_val = get_register(rb);
      int32_t alu_out = rs_val | rb_val;
      set_register(ra, alu_out);
      // todo - handle RC bit
      break;
    }
    case MFSPR: {
      int rt = instr->RTValue();
      int spr = instr->Bits(20,11);
      if (spr!=256) {
        UNIMPLEMENTED();  // Only LR supported 
      }
      set_register(rt, special_reg_lr_);
      break;
    }
    case MTSPR: {
      int rt = instr->RTValue();
      int32_t rt_val = get_register(rt);
      int spr = instr->Bits(20,11);
      if (spr==256) {
        special_reg_lr_ = rt_val;
      } else if (spr==288) {
        special_reg_ctr_ = rt_val;
      } else { 
        UNIMPLEMENTED();  // Only LR supported 
      }
      break;
    }
    default: {
      UNIMPLEMENTED();  // Not used by V8.
    }
  }
}
void Simulator::DecodeExt4(Instruction* instr) {
  switch(instr->Bits(5,1) << 1) {
    case FCMPU: {
      int fra = instr->RAValue();
      int frb = instr->RBValue();
      double fra_val = get_double_from_d_register(fra);
      double frb_val = get_double_from_d_register(frb);
      int cr = instr->Bits(25,23);
      int bf = 0;
      if(fra_val < frb_val) { bf |= 0x80000000; }
      if(fra_val > frb_val) { bf |= 0x40000000; }
      if(fra_val == frb_val) { bf |= 0x20000000; }
      if(isunordered(fra_val, frb_val)) { bf |= 0x10000000; } 
      int condition_mask = 0xF0000000 >> (cr*4);
      int condition =  bf >> (cr*4);
      condition_reg_ = (condition_reg_ & ~condition_mask) | condition;
      break;
    }
    case FDIV: {
      int frt = instr->RTValue();
      int fra = instr->RAValue();
      int frb = instr->RBValue();
      double fra_val = get_double_from_d_register(fra);
      double frb_val = get_double_from_d_register(frb);
      double frt_val = fra_val / frb_val;
      set_d_register_from_double(frt, frt_val);
      break;
    }
    case FSUB: {
      int frt = instr->RTValue();
      int fra = instr->RAValue();
      int frb = instr->RBValue();
      double fra_val = get_double_from_d_register(fra);
      double frb_val = get_double_from_d_register(frb);
      double frt_val = fra_val - frb_val;
      set_d_register_from_double(frt, frt_val);
      break;
    }
    case FADD: {
      int frt = instr->RTValue();
      int fra = instr->RAValue();
      int frb = instr->RBValue();
      double fra_val = get_double_from_d_register(fra);
      double frb_val = get_double_from_d_register(frb);
      double frt_val = fra_val + frb_val;
      set_d_register_from_double(frt, frt_val);
      break;
    }
    case FMUL: {
      int frt = instr->RTValue();
      int fra = instr->RAValue();
      int frb = instr->RBValue();
      double fra_val = get_double_from_d_register(fra);
      double frb_val = get_double_from_d_register(frb);
      double frt_val = fra_val * frb_val;
      set_d_register_from_double(frt, frt_val);
      break;
    }
    default: {
      UNIMPLEMENTED();  // Not used by V8.
    }
  }
}
// Instruction types 0 and 1 are both rolled into one function because they
// only differ in the handling of the shifter_operand.
void Simulator::DecodeType01(Instruction* instr) {
  int type = instr->TypeValue();
  if ((type == 0) && instr->IsSpecialType0()) {
    // multiply instruction or extra loads and stores
    if (instr->Bits(7, 4) == 9) {
      if (instr->Bit(24) == 0) {
        // Raw field decoding here. Multiply instructions have their Rd in
        // funny places.
        int rn = instr->RnValue();
        int rm = instr->RmValue();
        int rs = instr->RsValue();
        int32_t rs_val = get_register(rs);
        int32_t rm_val = get_register(rm);
        if (instr->Bit(23) == 0) {
          if (instr->Bit(21) == 0) {
            // The MUL instruction description (A 4.1.33) refers to Rd as being
            // the destination for the operation, but it confusingly uses the
            // Rn field to encode it.
            // Format(instr, "mul'cond's 'rn, 'rm, 'rs");
            int rd = rn;  // Remap the rn field to the Rd register.
            int32_t alu_out = rm_val * rs_val;
            set_register(rd, alu_out);
            if (instr->HasS()) {
              SetNZFlags(alu_out);
            }
          } else {
            // The MLA instruction description (A 4.1.28) refers to the order
            // of registers as "Rd, Rm, Rs, Rn". But confusingly it uses the
            // Rn field to encode the Rd register and the Rd field to encode
            // the Rn register.
            Format(instr, "mla'cond's 'rn, 'rm, 'rs, 'rd");
          }
        } else {
          // The signed/long multiply instructions use the terms RdHi and RdLo
          // when referring to the target registers. They are mapped to the Rn
          // and Rd fields as follows:
          // RdLo == Rd
          // RdHi == Rn (This is confusingly stored in variable rd here
          //             because the mul instruction from above uses the
          //             Rn field to encode the Rd register. Good luck figuring
          //             this out without reading the ARM instruction manual
          //             at a very detailed level.)
          // Format(instr, "'um'al'cond's 'rd, 'rn, 'rs, 'rm");
          int rd_hi = rn;  // Remap the rn field to the RdHi register.
          int rd_lo = instr->RdValue();
          int32_t hi_res = 0;
          int32_t lo_res = 0;
          if (instr->Bit(22) == 1) {
            int64_t left_op  = static_cast<int32_t>(rm_val);
            int64_t right_op = static_cast<int32_t>(rs_val);
            uint64_t result = left_op * right_op;
            hi_res = static_cast<int32_t>(result >> 32);
            lo_res = static_cast<int32_t>(result & 0xffffffff);
          } else {
            // unsigned multiply
            uint64_t left_op  = static_cast<uint32_t>(rm_val);
            uint64_t right_op = static_cast<uint32_t>(rs_val);
            uint64_t result = left_op * right_op;
            hi_res = static_cast<int32_t>(result >> 32);
            lo_res = static_cast<int32_t>(result & 0xffffffff);
          }
          set_register(rd_lo, lo_res);
          set_register(rd_hi, hi_res);
          if (instr->HasS()) {
            UNIMPLEMENTED();
          }
        }
      } else {
        UNIMPLEMENTED();  // Not used by V8.
      }
    } else {
      // extra load/store instructions
      int rd = instr->RdValue();
      int rn = instr->RnValue();
      int32_t rn_val = get_register(rn);
      int32_t addr = 0;
      if (instr->Bit(22) == 0) {
        int rm = instr->RmValue();
        int32_t rm_val = get_register(rm);
        switch (instr->PUField()) {
          case da_x: {
            // Format(instr, "'memop'cond'sign'h 'rd, ['rn], -'rm");
            ASSERT(!instr->HasW());
            addr = rn_val;
            rn_val -= rm_val;
            set_register(rn, rn_val);
            break;
          }
          case ia_x: {
            // Format(instr, "'memop'cond'sign'h 'rd, ['rn], +'rm");
            ASSERT(!instr->HasW());
            addr = rn_val;
            rn_val += rm_val;
            set_register(rn, rn_val);
            break;
          }
          case db_x: {
            // Format(instr, "'memop'cond'sign'h 'rd, ['rn, -'rm]'w");
            rn_val -= rm_val;
            addr = rn_val;
            if (instr->HasW()) {
              set_register(rn, rn_val);
            }
            break;
          }
          case ib_x: {
            // Format(instr, "'memop'cond'sign'h 'rd, ['rn, +'rm]'w");
            rn_val += rm_val;
            addr = rn_val;
            if (instr->HasW()) {
              set_register(rn, rn_val);
            }
            break;
          }
          default: {
            // The PU field is a 2-bit field.
            UNREACHABLE();
            break;
          }
        }
      } else {
        int32_t imm_val = (instr->ImmedHValue() << 4) | instr->ImmedLValue();
        switch (instr->PUField()) {
          case da_x: {
            // Format(instr, "'memop'cond'sign'h 'rd, ['rn], #-'off8");
            ASSERT(!instr->HasW());
            addr = rn_val;
            rn_val -= imm_val;
            set_register(rn, rn_val);
            break;
          }
          case ia_x: {
            // Format(instr, "'memop'cond'sign'h 'rd, ['rn], #+'off8");
            ASSERT(!instr->HasW());
            addr = rn_val;
            rn_val += imm_val;
            set_register(rn, rn_val);
            break;
          }
          case db_x: {
            // Format(instr, "'memop'cond'sign'h 'rd, ['rn, #-'off8]'w");
            rn_val -= imm_val;
            addr = rn_val;
            if (instr->HasW()) {
              set_register(rn, rn_val);
            }
            break;
          }
          case ib_x: {
            // Format(instr, "'memop'cond'sign'h 'rd, ['rn, #+'off8]'w");
            rn_val += imm_val;
            addr = rn_val;
            if (instr->HasW()) {
              set_register(rn, rn_val);
            }
            break;
          }
          default: {
            // The PU field is a 2-bit field.
            UNREACHABLE();
            break;
          }
        }
      }
      if (((instr->Bits(7, 4) & 0xd) == 0xd) && (instr->Bit(20) == 0)) {
        ASSERT((rd % 2) == 0);
        if (instr->HasH()) {
          // The strd instruction.
          int32_t value1 = get_register(rd);
          int32_t value2 = get_register(rd+1);
          WriteDW(addr, value1, value2);
        } else {
          // The ldrd instruction.
          int* rn_data = ReadDW(addr);
          set_dw_register(rd, rn_data);
        }
      } else if (instr->HasH()) {
        if (instr->HasSign()) {
          if (instr->HasL()) {
            int16_t val = ReadH(addr, instr);
            set_register(rd, val);
          } else {
            int16_t val = get_register(rd);
            WriteH(addr, val, instr);
          }
        } else {
          if (instr->HasL()) {
            uint16_t val = ReadHU(addr, instr);
            set_register(rd, val);
          } else {
            uint16_t val = get_register(rd);
            WriteH(addr, val, instr);
          }
        }
      } else {
        // signed byte loads
        ASSERT(instr->HasSign());
        ASSERT(instr->HasL());
        int8_t val = ReadB(addr);
        set_register(rd, val);
      }
      return;
    }
  } else if ((type == 0) && instr->IsMiscType0()) {
    if (instr->Bits(22, 21) == 1) {
      int rm = instr->RmValue();
      switch (instr->BitField(7, 4)) {
        case BX:
          set_pc(get_register(rm));
          break;
        case BLX: {
          uint32_t old_pc = get_pc();
          set_pc(get_register(rm));
          set_register(lr, old_pc + Instruction::kInstrSize);
          break;
        }
        case BKPT: {
          PPCDebugger dbg(this);
          PrintF("Simulator hit BKPT.\n");
          dbg.Debug();
          break;
        }
        default:
          UNIMPLEMENTED();
      }
    } else if (instr->Bits(22, 21) == 3) {
      int rm = instr->RmValue();
      int rd = instr->RdValue();
      switch (instr->BitField(7, 4)) {
        case CLZ: {
          uint32_t bits = get_register(rm);
          int leading_zeros = 0;
          if (bits == 0) {
            leading_zeros = 32;
          } else {
            while ((bits & 0x80000000u) == 0) {
              bits <<= 1;
              leading_zeros++;
            }
          }
          set_register(rd, leading_zeros);
          break;
        }
        default:
          UNIMPLEMENTED();
      }
    } else {
      PrintF("%08x\n", instr->InstructionBits());
      UNIMPLEMENTED();
    }
  } else {
    int rd = instr->RdValue();
    int rn = instr->RnValue();
    int32_t rn_val = get_register(rn);
    int32_t shifter_operand = 0;
    bool shifter_carry_out = 0;
    if (type == 0) {
      shifter_operand = GetShiftRm(instr, &shifter_carry_out);
    } else {
      ASSERT(instr->TypeValue() == 1);
      shifter_operand = GetImm(instr, &shifter_carry_out);
    }
    int32_t alu_out;

    switch (instr->OpcodeField()) {
      case AND: {
        // Format(instr, "and'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "and'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val & shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      case EOR: {
        // Format(instr, "eor'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "eor'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val ^ shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      case SUB: {
        // Format(instr, "sub'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "sub'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val - shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(!BorrowFrom(rn_val, shifter_operand));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, false));
        }
        break;
      }

      case RSB: {
        // Format(instr, "rsb'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "rsb'cond's 'rd, 'rn, 'imm");
        alu_out = shifter_operand - rn_val;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(!BorrowFrom(shifter_operand, rn_val));
          SetVFlag(OverflowFrom(alu_out, shifter_operand, rn_val, false));
        }
        break;
      }

      case ADD: {
        // Format(instr, "add'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "add'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val + shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(CarryFrom(rn_val, shifter_operand));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, true));
        }
        break;
      }

      case ADC: {
        // Format(instr, "adc'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "adc'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val + shifter_operand + GetCarry();
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(CarryFrom(rn_val, shifter_operand, GetCarry()));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, true));
        }
        break;
      }

      case SBC: {
        Format(instr, "sbc'cond's 'rd, 'rn, 'shift_rm");
        Format(instr, "sbc'cond's 'rd, 'rn, 'imm");
        break;
      }

      case RSC: {
        Format(instr, "rsc'cond's 'rd, 'rn, 'shift_rm");
        Format(instr, "rsc'cond's 'rd, 'rn, 'imm");
        break;
      }

      case TST: {
        if (instr->HasS()) {
          // Format(instr, "tst'cond 'rn, 'shift_rm");
          // Format(instr, "tst'cond 'rn, 'imm");
          alu_out = rn_val & shifter_operand;
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        } else {
          // Format(instr, "movw'cond 'rd, 'imm").
          alu_out = instr->ImmedMovwMovtValue();
          set_register(rd, alu_out);
        }
        break;
      }

      case TEQ: {
        if (instr->HasS()) {
          // Format(instr, "teq'cond 'rn, 'shift_rm");
          // Format(instr, "teq'cond 'rn, 'imm");
          alu_out = rn_val ^ shifter_operand;
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        } else {
          // Other instructions matching this pattern are handled in the
          // miscellaneous instructions part above.
          UNREACHABLE();
        }
        break;
      }

#if 0
      case CMP: {
        if (instr->HasS()) {
          // Format(instr, "cmp'cond 'rn, 'shift_rm");
          // Format(instr, "cmp'cond 'rn, 'imm");
          alu_out = rn_val - shifter_operand;
          SetNZFlags(alu_out);
          SetCFlag(!BorrowFrom(rn_val, shifter_operand));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, false));
        } else {
          // Format(instr, "movt'cond 'rd, 'imm").
          alu_out = (get_register(rd) & 0xffff) |
              (instr->ImmedMovwMovtValue() << 16);
          set_register(rd, alu_out);
        }
        break;
      }
#endif


      case CMN: {
        if (instr->HasS()) {
          // Format(instr, "cmn'cond 'rn, 'shift_rm");
          // Format(instr, "cmn'cond 'rn, 'imm");
          alu_out = rn_val + shifter_operand;
          SetNZFlags(alu_out);
          SetCFlag(CarryFrom(rn_val, shifter_operand));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, true));
        } else {
          // Other instructions matching this pattern are handled in the
          // miscellaneous instructions part above.
          UNREACHABLE();
        }
        break;
      }

      case ORR: {
        // Format(instr, "orr'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "orr'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val | shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      case MOV: {
        // Format(instr, "mov'cond's 'rd, 'shift_rm");
        // Format(instr, "mov'cond's 'rd, 'imm");
        alu_out = shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      case BIC: {
        // Format(instr, "bic'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "bic'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val & ~shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      case MVN: {
        // Format(instr, "mvn'cond's 'rd, 'shift_rm");
        // Format(instr, "mvn'cond's 'rd, 'imm");
        alu_out = ~shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      default: {
        UNREACHABLE();
        break;
      }
    }
  }
}


void Simulator::DecodeType2(Instruction* instr) {
  int rd = instr->RdValue();
  int rn = instr->RnValue();
  int32_t rn_val = get_register(rn);
  int32_t im_val = instr->Offset12Value();
  int32_t addr = 0;
  switch (instr->PUField()) {
    case da_x: {
      // Format(instr, "'memop'cond'b 'rd, ['rn], #-'off12");
      ASSERT(!instr->HasW());
      addr = rn_val;
      rn_val -= im_val;
      set_register(rn, rn_val);
      break;
    }
    case ia_x: {
      // Format(instr, "'memop'cond'b 'rd, ['rn], #+'off12");
      ASSERT(!instr->HasW());
      addr = rn_val;
      rn_val += im_val;
      set_register(rn, rn_val);
      break;
    }
    case db_x: {
      // Format(instr, "'memop'cond'b 'rd, ['rn, #-'off12]'w");
      rn_val -= im_val;
      addr = rn_val;
      if (instr->HasW()) {
        set_register(rn, rn_val);
      }
      break;
    }
    case ib_x: {
      // Format(instr, "'memop'cond'b 'rd, ['rn, #+'off12]'w");
      rn_val += im_val;
      addr = rn_val;
      if (instr->HasW()) {
        set_register(rn, rn_val);
      }
      break;
    }
    default: {
      UNREACHABLE();
      break;
    }
  }
  if (instr->HasB()) {
    if (instr->HasL()) {
      byte val = ReadBU(addr);
      set_register(rd, val);
    } else {
      byte val = get_register(rd);
      WriteB(addr, val);
    }
  } else {
    if (instr->HasL()) {
      set_register(rd, ReadW(addr, instr));
    } else {
      WriteW(addr, get_register(rd), instr);
    }
  }
}


void Simulator::DecodeType3(Instruction* instr) {
  int rd = instr->RdValue();
  int rn = instr->RnValue();
  int32_t rn_val = get_register(rn);
  bool shifter_carry_out = 0;
  int32_t shifter_operand = GetShiftRm(instr, &shifter_carry_out);
  int32_t addr = 0;
  switch (instr->PUField()) {
    case da_x: {
      ASSERT(!instr->HasW());
      Format(instr, "'memop'cond'b 'rd, ['rn], -'shift_rm");
      UNIMPLEMENTED();
      break;
    }
    case ia_x: {
      if (instr->HasW()) {
        ASSERT(instr->Bits(5, 4) == 0x1);

        if (instr->Bit(22) == 0x1) {  // USAT.
          int32_t sat_pos = instr->Bits(20, 16);
          int32_t sat_val = (1 << sat_pos) - 1;
          int32_t shift = instr->Bits(11, 7);
          int32_t shift_type = instr->Bit(6);
          int32_t rm_val = get_register(instr->RmValue());
          if (shift_type == 0) {  // LSL
            rm_val <<= shift;
          } else {  // ASR
            rm_val >>= shift;
          }
          // If saturation occurs, the Q flag should be set in the CPSR.
          // There is no Q flag yet, and no instruction (MRS) to read the
          // CPSR directly.
          if (rm_val > sat_val) {
            rm_val = sat_val;
          } else if (rm_val < 0) {
            rm_val = 0;
          }
          set_register(rd, rm_val);
        } else {  // SSAT.
          UNIMPLEMENTED();
        }
        return;
      } else {
        Format(instr, "'memop'cond'b 'rd, ['rn], +'shift_rm");
        UNIMPLEMENTED();
      }
      break;
    }
    case db_x: {
      // Format(instr, "'memop'cond'b 'rd, ['rn, -'shift_rm]'w");
      addr = rn_val - shifter_operand;
      if (instr->HasW()) {
        set_register(rn, addr);
      }
      break;
    }
    case ib_x: {
      if (instr->HasW() && (instr->Bits(6, 4) == 0x5)) {
        uint32_t widthminus1 = static_cast<uint32_t>(instr->Bits(20, 16));
        uint32_t lsbit = static_cast<uint32_t>(instr->Bits(11, 7));
        uint32_t msbit = widthminus1 + lsbit;
        if (msbit <= 31) {
          if (instr->Bit(22)) {
            // ubfx - unsigned bitfield extract.
            uint32_t rm_val =
                static_cast<uint32_t>(get_register(instr->RmValue()));
            uint32_t extr_val = rm_val << (31 - msbit);
            extr_val = extr_val >> (31 - widthminus1);
            set_register(instr->RdValue(), extr_val);
          } else {
            // sbfx - signed bitfield extract.
            int32_t rm_val = get_register(instr->RmValue());
            int32_t extr_val = rm_val << (31 - msbit);
            extr_val = extr_val >> (31 - widthminus1);
            set_register(instr->RdValue(), extr_val);
          }
        } else {
          UNREACHABLE();
        }
        return;
      } else if (!instr->HasW() && (instr->Bits(6, 4) == 0x1)) {
        uint32_t lsbit = static_cast<uint32_t>(instr->Bits(11, 7));
        uint32_t msbit = static_cast<uint32_t>(instr->Bits(20, 16));
        if (msbit >= lsbit) {
          // bfc or bfi - bitfield clear/insert.
          uint32_t rd_val =
              static_cast<uint32_t>(get_register(instr->RdValue()));
          uint32_t bitcount = msbit - lsbit + 1;
          uint32_t mask = (1 << bitcount) - 1;
          rd_val &= ~(mask << lsbit);
          if (instr->RmValue() != 15) {
            // bfi - bitfield insert.
            uint32_t rm_val =
                static_cast<uint32_t>(get_register(instr->RmValue()));
            rm_val &= mask;
            rd_val |= rm_val << lsbit;
          }
          set_register(instr->RdValue(), rd_val);
        } else {
          UNREACHABLE();
        }
        return;
      } else {
        // Format(instr, "'memop'cond'b 'rd, ['rn, +'shift_rm]'w");
        addr = rn_val + shifter_operand;
        if (instr->HasW()) {
          set_register(rn, addr);
        }
      }
      break;
    }
    default: {
      UNREACHABLE();
      break;
    }
  }
  if (instr->HasB()) {
    if (instr->HasL()) {
      uint8_t byte = ReadB(addr);
      set_register(rd, byte);
    } else {
      uint8_t byte = get_register(rd);
      WriteB(addr, byte);
    }
  } else {
    if (instr->HasL()) {
      set_register(rd, ReadW(addr, instr));
    } else {
      WriteW(addr, get_register(rd), instr);
    }
  }
}


void Simulator::DecodeType4(Instruction* instr) {
  ASSERT(instr->Bit(22) == 0);  // only allowed to be set in privileged mode
  if (instr->HasL()) {
    // Format(instr, "ldm'cond'pu 'rn'w, 'rlist");
    HandleRList(instr, true);
  } else {
    // Format(instr, "stm'cond'pu 'rn'w, 'rlist");
    HandleRList(instr, false);
  }
}


void Simulator::DecodeType5(Instruction* instr) {
  // Format(instr, "b'l'cond 'target");
  int off = (instr->SImmed24Value() << 2);
  intptr_t pc_address = get_pc();
  if (instr->HasLink()) {
    set_register(lr, pc_address + Instruction::kInstrSize);
  }
  int pc_reg = get_register(pc);
  set_pc(pc_reg + off);
}


void Simulator::DecodeType6(Instruction* instr) {
  DecodeType6CoprocessorIns(instr);
}


void Simulator::DecodeType7(Instruction* instr) {
  if (instr->Bit(24) == 1) {
    SoftwareInterrupt(instr);
  } else {
    DecodeTypeVFP(instr);
  }
}


// void Simulator::DecodeTypeVFP(Instruction* instr)
// The Following ARMv7 VFPv instructions are currently supported.
// vmov :Sn = Rt
// vmov :Rt = Sn
// vcvt: Dd = Sm
// vcvt: Sd = Dm
// Dd = vabs(Dm)
// Dd = vneg(Dm)
// Dd = vadd(Dn, Dm)
// Dd = vsub(Dn, Dm)
// Dd = vmul(Dn, Dm)
// Dd = vdiv(Dn, Dm)
// vcmp(Dd, Dm)
// vmrs
// Dd = vsqrt(Dm)
void Simulator::DecodeTypeVFP(Instruction* instr) {
  ASSERT((instr->TypeValue() == 7) && (instr->Bit(24) == 0x0) );
  ASSERT(instr->Bits(11, 9) == 0x5);

  // Obtain double precision register codes.
  int vm = instr->VFPMRegValue(kDoublePrecision);
  int vd = instr->VFPDRegValue(kDoublePrecision);
  int vn = instr->VFPNRegValue(kDoublePrecision);

  if (instr->Bit(4) == 0) {
    if (instr->Opc1Value() == 0x7) {
      // Other data processing instructions
      if ((instr->Opc2Value() == 0x0) && (instr->Opc3Value() == 0x1)) {
        // vmov register to register.
        if (instr->SzValue() == 0x1) {
          int m = instr->VFPMRegValue(kDoublePrecision);
          int d = instr->VFPDRegValue(kDoublePrecision);
          set_d_register_from_double(d, get_double_from_d_register(m));
        } else {
          int m = instr->VFPMRegValue(kSinglePrecision);
          int d = instr->VFPDRegValue(kSinglePrecision);
          set_s_register_from_float(d, get_float_from_s_register(m));
        }
      } else if ((instr->Opc2Value() == 0x0) && (instr->Opc3Value() == 0x3)) {
        // vabs
        double dm_value = get_double_from_d_register(vm);
        double dd_value = fabs(dm_value);
        set_d_register_from_double(vd, dd_value);
      } else if ((instr->Opc2Value() == 0x1) && (instr->Opc3Value() == 0x1)) {
        // vneg
        double dm_value = get_double_from_d_register(vm);
        double dd_value = -dm_value;
        set_d_register_from_double(vd, dd_value);
      } else if ((instr->Opc2Value() == 0x7) && (instr->Opc3Value() == 0x3)) {
        DecodeVCVTBetweenDoubleAndSingle(instr);
      } else if ((instr->Opc2Value() == 0x8) && (instr->Opc3Value() & 0x1)) {
        DecodeVCVTBetweenFloatingPointAndInteger(instr);
      } else if (((instr->Opc2Value() >> 1) == 0x6) &&
                 (instr->Opc3Value() & 0x1)) {
        DecodeVCVTBetweenFloatingPointAndInteger(instr);
      } else if (((instr->Opc2Value() == 0x4) || (instr->Opc2Value() == 0x5)) &&
                 (instr->Opc3Value() & 0x1)) {
        DecodeVCMP(instr);
      } else if (((instr->Opc2Value() == 0x1)) && (instr->Opc3Value() == 0x3)) {
        // vsqrt
        double dm_value = get_double_from_d_register(vm);
        double dd_value = sqrt(dm_value);
        set_d_register_from_double(vd, dd_value);
      } else if (instr->Opc3Value() == 0x0) {
        // vmov immediate.
        if (instr->SzValue() == 0x1) {
          set_d_register_from_double(vd, instr->DoubleImmedVmov());
        } else {
          UNREACHABLE();  // Not used by v8.
        }
      } else {
        UNREACHABLE();  // Not used by V8.
      }
    } else if (instr->Opc1Value() == 0x3) {
      if (instr->SzValue() != 0x1) {
        UNREACHABLE();  // Not used by V8.
      }

      if (instr->Opc3Value() & 0x1) {
        // vsub
        double dn_value = get_double_from_d_register(vn);
        double dm_value = get_double_from_d_register(vm);
        double dd_value = dn_value - dm_value;
        set_d_register_from_double(vd, dd_value);
      } else {
        // vadd
        double dn_value = get_double_from_d_register(vn);
        double dm_value = get_double_from_d_register(vm);
        double dd_value = dn_value + dm_value;
        set_d_register_from_double(vd, dd_value);
      }
    } else if ((instr->Opc1Value() == 0x2) && !(instr->Opc3Value() & 0x1)) {
      // vmul
      if (instr->SzValue() != 0x1) {
        UNREACHABLE();  // Not used by V8.
      }

      double dn_value = get_double_from_d_register(vn);
      double dm_value = get_double_from_d_register(vm);
      double dd_value = dn_value * dm_value;
      set_d_register_from_double(vd, dd_value);
    } else if ((instr->Opc1Value() == 0x4) && !(instr->Opc3Value() & 0x1)) {
      // vdiv
      if (instr->SzValue() != 0x1) {
        UNREACHABLE();  // Not used by V8.
      }

      double dn_value = get_double_from_d_register(vn);
      double dm_value = get_double_from_d_register(vm);
      double dd_value = dn_value / dm_value;
      div_zero_vfp_flag_ = (dm_value == 0);
      set_d_register_from_double(vd, dd_value);
    } else {
      UNIMPLEMENTED();  // Not used by V8.
    }
  } else {
    if ((instr->VCValue() == 0x0) &&
        (instr->VAValue() == 0x0)) {
      DecodeVMOVBetweenCoreAndSinglePrecisionRegisters(instr);
    } else if ((instr->VLValue() == 0x1) &&
               (instr->VCValue() == 0x0) &&
               (instr->VAValue() == 0x7) &&
               (instr->Bits(19, 16) == 0x1)) {
      // vmrs
      uint32_t rt = instr->RtValue();
      if (rt == 0xF) {
        Copy_FPSCR_to_APSR();
      } else {
        // Emulate FPSCR from the Simulator flags.
        uint32_t fpscr = (n_flag_FPSCR_ << 31) |
                         (z_flag_FPSCR_ << 30) |
                         (c_flag_FPSCR_ << 29) |
                         (v_flag_FPSCR_ << 28) |
                         (inexact_vfp_flag_ << 4) |
                         (underflow_vfp_flag_ << 3) |
                         (overflow_vfp_flag_ << 2) |
                         (div_zero_vfp_flag_ << 1) |
                         (inv_op_vfp_flag_ << 0) |
                         (FPSCR_rounding_mode_);
        set_register(rt, fpscr);
      }
    } else if ((instr->VLValue() == 0x0) &&
               (instr->VCValue() == 0x0) &&
               (instr->VAValue() == 0x7) &&
               (instr->Bits(19, 16) == 0x1)) {
      // vmsr
      uint32_t rt = instr->RtValue();
      if (rt == pc) {
        UNREACHABLE();
      } else {
        uint32_t rt_value = get_register(rt);
        n_flag_FPSCR_ = (rt_value >> 31) & 1;
        z_flag_FPSCR_ = (rt_value >> 30) & 1;
        c_flag_FPSCR_ = (rt_value >> 29) & 1;
        v_flag_FPSCR_ = (rt_value >> 28) & 1;
        inexact_vfp_flag_ = (rt_value >> 4) & 1;
        underflow_vfp_flag_ = (rt_value >> 3) & 1;
        overflow_vfp_flag_ = (rt_value >> 2) & 1;
        div_zero_vfp_flag_ = (rt_value >> 1) & 1;
        inv_op_vfp_flag_ = (rt_value >> 0) & 1;
        FPSCR_rounding_mode_ =
            static_cast<VFPRoundingMode>((rt_value) & kVFPRoundingModeMask);
      }
    } else {
      UNIMPLEMENTED();  // Not used by V8.
    }
  }
}


void Simulator::DecodeVMOVBetweenCoreAndSinglePrecisionRegisters(
    Instruction* instr) {
  ASSERT((instr->Bit(4) == 1) && (instr->VCValue() == 0x0) &&
         (instr->VAValue() == 0x0));

  int t = instr->RtValue();
  int n = instr->VFPNRegValue(kSinglePrecision);
  bool to_arm_register = (instr->VLValue() == 0x1);

  if (to_arm_register) {
    int32_t int_value = get_sinteger_from_s_register(n);
    set_register(t, int_value);
  } else {
    int32_t rs_val = get_register(t);
    set_s_register_from_sinteger(n, rs_val);
  }
}


void Simulator::DecodeVCMP(Instruction* instr) {
  ASSERT((instr->Bit(4) == 0) && (instr->Opc1Value() == 0x7));
  ASSERT(((instr->Opc2Value() == 0x4) || (instr->Opc2Value() == 0x5)) &&
         (instr->Opc3Value() & 0x1));
  // Comparison.

  VFPRegPrecision precision = kSinglePrecision;
  if (instr->SzValue() == 1) {
    precision = kDoublePrecision;
  }

  int d = instr->VFPDRegValue(precision);
  int m = 0;
  if (instr->Opc2Value() == 0x4) {
    m = instr->VFPMRegValue(precision);
  }

  if (precision == kDoublePrecision) {
    double dd_value = get_double_from_d_register(d);
    double dm_value = 0.0;
    if (instr->Opc2Value() == 0x4) {
      dm_value = get_double_from_d_register(m);
    }

    // Raise exceptions for quiet NaNs if necessary.
    if (instr->Bit(7) == 1) {
      if (isnan(dd_value)) {
        inv_op_vfp_flag_ = true;
      }
    }

    Compute_FPSCR_Flags(dd_value, dm_value);
  } else {
    UNIMPLEMENTED();  // Not used by V8.
  }
}


void Simulator::DecodeVCVTBetweenDoubleAndSingle(Instruction* instr) {
  ASSERT((instr->Bit(4) == 0) && (instr->Opc1Value() == 0x7));
  ASSERT((instr->Opc2Value() == 0x7) && (instr->Opc3Value() == 0x3));

  VFPRegPrecision dst_precision = kDoublePrecision;
  VFPRegPrecision src_precision = kSinglePrecision;
  if (instr->SzValue() == 1) {
    dst_precision = kSinglePrecision;
    src_precision = kDoublePrecision;
  }

  int dst = instr->VFPDRegValue(dst_precision);
  int src = instr->VFPMRegValue(src_precision);

  if (dst_precision == kSinglePrecision) {
    double val = get_double_from_d_register(src);
    set_s_register_from_float(dst, static_cast<float>(val));
  } else {
    float val = get_float_from_s_register(src);
    set_d_register_from_double(dst, static_cast<double>(val));
  }
}

bool get_inv_op_vfp_flag(VFPRoundingMode mode,
                         double val,
                         bool unsigned_) {
  ASSERT((mode == RN) || (mode == RM) || (mode == RZ));
  double max_uint = static_cast<double>(0xffffffffu);
  double max_int = static_cast<double>(kMaxInt);
  double min_int = static_cast<double>(kMinInt);

  // Check for NaN.
  if (val != val) {
    return true;
  }

  // Check for overflow. This code works because 32bit integers can be
  // exactly represented by ieee-754 64bit floating-point values.
  switch (mode) {
    case RN:
      return  unsigned_ ? (val >= (max_uint + 0.5)) ||
                          (val < -0.5)
                        : (val >= (max_int + 0.5)) ||
                          (val < (min_int - 0.5));

    case RM:
      return  unsigned_ ? (val >= (max_uint + 1.0)) ||
                          (val < 0)
                        : (val >= (max_int + 1.0)) ||
                          (val < min_int);

    case RZ:
      return  unsigned_ ? (val >= (max_uint + 1.0)) ||
                          (val <= -1)
                        : (val >= (max_int + 1.0)) ||
                          (val <= (min_int - 1.0));
    default:
      UNREACHABLE();
      return true;
  }
}


// We call this function only if we had a vfp invalid exception.
// It returns the correct saturated value.
int VFPConversionSaturate(double val, bool unsigned_res) {
  if (val != val) {
    return 0;
  } else {
    if (unsigned_res) {
      return (val < 0) ? 0 : 0xffffffffu;
    } else {
      return (val < 0) ? kMinInt : kMaxInt;
    }
  }
}


void Simulator::DecodeVCVTBetweenFloatingPointAndInteger(Instruction* instr) {
  ASSERT((instr->Bit(4) == 0) && (instr->Opc1Value() == 0x7) &&
         (instr->Bits(27, 23) == 0x1D));
  ASSERT(((instr->Opc2Value() == 0x8) && (instr->Opc3Value() & 0x1)) ||
         (((instr->Opc2Value() >> 1) == 0x6) && (instr->Opc3Value() & 0x1)));

  // Conversion between floating-point and integer.
  bool to_integer = (instr->Bit(18) == 1);

  VFPRegPrecision src_precision = (instr->SzValue() == 1) ? kDoublePrecision
                                                          : kSinglePrecision;

  if (to_integer) {
    // We are playing with code close to the C++ standard's limits below,
    // hence the very simple code and heavy checks.
    //
    // Note:
    // C++ defines default type casting from floating point to integer as
    // (close to) rounding toward zero ("fractional part discarded").

    int dst = instr->VFPDRegValue(kSinglePrecision);
    int src = instr->VFPMRegValue(src_precision);

    // Bit 7 in vcvt instructions indicates if we should use the FPSCR rounding
    // mode or the default Round to Zero mode.
    VFPRoundingMode mode = (instr->Bit(7) != 1) ? FPSCR_rounding_mode_
                                                : RZ;
    ASSERT((mode == RM) || (mode == RZ) || (mode == RN));

    bool unsigned_integer = (instr->Bit(16) == 0);
    bool double_precision = (src_precision == kDoublePrecision);

    double val = double_precision ? get_double_from_d_register(src)
                                  : get_float_from_s_register(src);

    int temp = unsigned_integer ? static_cast<uint32_t>(val)
                                : static_cast<int32_t>(val);

    inv_op_vfp_flag_ = get_inv_op_vfp_flag(mode, val, unsigned_integer);

    double abs_diff =
      unsigned_integer ? fabs(val - static_cast<uint32_t>(temp))
                       : fabs(val - temp);

    inexact_vfp_flag_ = (abs_diff != 0);

    if (inv_op_vfp_flag_) {
      temp = VFPConversionSaturate(val, unsigned_integer);
    } else {
      switch (mode) {
        case RN: {
          int val_sign = (val > 0) ? 1 : -1;
          if (abs_diff > 0.5) {
            temp += val_sign;
          } else if (abs_diff == 0.5) {
            // Round to even if exactly halfway.
            temp = ((temp % 2) == 0) ? temp : temp + val_sign;
          }
          break;
        }

        case RM:
          temp = temp > val ? temp - 1 : temp;
          break;

        case RZ:
          // Nothing to do.
          break;

        default:
          UNREACHABLE();
      }
    }

    // Update the destination register.
    set_s_register_from_sinteger(dst, temp);

  } else {
    bool unsigned_integer = (instr->Bit(7) == 0);

    int dst = instr->VFPDRegValue(src_precision);
    int src = instr->VFPMRegValue(kSinglePrecision);

    int val = get_sinteger_from_s_register(src);

    if (src_precision == kDoublePrecision) {
      if (unsigned_integer) {
        set_d_register_from_double(dst,
                                   static_cast<double>((uint32_t)val));
      } else {
        set_d_register_from_double(dst, static_cast<double>(val));
      }
    } else {
      if (unsigned_integer) {
        set_s_register_from_float(dst,
                                  static_cast<float>((uint32_t)val));
      } else {
        set_s_register_from_float(dst, static_cast<float>(val));
      }
    }
  }
}


// void Simulator::DecodeType6CoprocessorIns(Instruction* instr)
// Decode Type 6 coprocessor instructions.
// Dm = vmov(Rt, Rt2)
// <Rt, Rt2> = vmov(Dm)
// Ddst = MEM(Rbase + 4*offset).
// MEM(Rbase + 4*offset) = Dsrc.
void Simulator::DecodeType6CoprocessorIns(Instruction* instr) {
  ASSERT((instr->TypeValue() == 6));

  if (instr->CoprocessorValue() == 0xA) {
    switch (instr->OpcodeValue()) {
      case 0x8:
      case 0xA:
      case 0xC:
      case 0xE: {  // Load and store single precision float to memory.
        int rn = instr->RnValue();
        int vd = instr->VFPDRegValue(kSinglePrecision);
        int offset = instr->Immed8Value();
        if (!instr->HasU()) {
          offset = -offset;
        }

        int32_t address = get_register(rn) + 4 * offset;
        if (instr->HasL()) {
          // Load double from memory: vldr.
          set_s_register_from_sinteger(vd, ReadW(address, instr));
        } else {
          // Store double to memory: vstr.
          WriteW(address, get_sinteger_from_s_register(vd), instr);
        }
        break;
      }
      case 0x4:
      case 0x5:
      case 0x6:
      case 0x7:
      case 0x9:
      case 0xB:
        // Load/store multiple single from memory: vldm/vstm.
        HandleVList(instr);
        break;
      default:
        UNIMPLEMENTED();  // Not used by V8.
    }
  } else if (instr->CoprocessorValue() == 0xB) {
    switch (instr->OpcodeValue()) {
      case 0x2:
        // Load and store double to two GP registers
        if (instr->Bits(7, 4) != 0x1) {
          UNIMPLEMENTED();  // Not used by V8.
        } else {
          int rt = instr->RtValue();
          int rn = instr->RnValue();
          int vm = instr->VmValue();
          if (instr->HasL()) {
            int32_t rt_int_value = get_sinteger_from_s_register(2*vm);
            int32_t rn_int_value = get_sinteger_from_s_register(2*vm+1);

            set_register(rt, rt_int_value);
            set_register(rn, rn_int_value);
          } else {
            int32_t rs_val = get_register(rt);
            int32_t rn_val = get_register(rn);

            set_s_register_from_sinteger(2*vm, rs_val);
            set_s_register_from_sinteger((2*vm+1), rn_val);
          }
        }
        break;
      case 0x8:
      case 0xC: {  // Load and store double to memory.
        int rn = instr->RnValue();
        int vd = instr->VdValue();
        int offset = instr->Immed8Value();
        if (!instr->HasU()) {
          offset = -offset;
        }
        int32_t address = get_register(rn) + 4 * offset;
        if (instr->HasL()) {
          // Load double from memory: vldr.
          set_s_register_from_sinteger(2*vd, ReadW(address, instr));
          set_s_register_from_sinteger(2*vd + 1, ReadW(address + 4, instr));
        } else {
          // Store double to memory: vstr.
          WriteW(address, get_sinteger_from_s_register(2*vd), instr);
          WriteW(address + 4, get_sinteger_from_s_register(2*vd + 1), instr);
        }
        break;
      }
      case 0x4:
      case 0x5:
      case 0x9:
        // Load/store multiple double from memory: vldm/vstm.
        HandleVList(instr);
        break;
      default:
        UNIMPLEMENTED();  // Not used by V8.
    }
  } else {
    UNIMPLEMENTED();  // Not used by V8.
  }
}


// Executes the current instruction.
void Simulator::InstructionDecode(Instruction* instr) {
  if (v8::internal::FLAG_check_icache) {
    CheckICache(isolate_->simulator_i_cache(), instr);
  }
  pc_modified_ = false;
  if (::v8::internal::FLAG_trace_sim) {
    disasm::NameConverter converter;
    disasm::Disassembler dasm(converter);
    // use a reasonably large buffer
    v8::internal::EmbeddedVector<char, 256> buffer;
    dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(instr));
    PrintF("%05d  0x%08x  %s\n", icount_, reinterpret_cast<intptr_t>(instr), buffer.start());
  }
  switch (instr->OpcodeValue() << 26) {
    case TWI: {
      // used for call redirection in simulation mode
      SoftwareInterrupt(instr);
      break;
    }
    case MULLI:
    case SUBFIC: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int32_t ra_val = get_register(ra);
      int32_t im_val = instr->Bits(15,0);
      int32_t alu_out = im_val - ra_val;
      set_register(rt, alu_out);
      // todo - handle RC bit
      break;
    }
    case CMPLI: {
      int ra = instr->RAValue();
      uint32_t ra_val = get_register(ra);
      uint32_t im_val = instr->Bits(15,0);
      int cr = instr->Bits(25,23);
      int bf = 0;
      if(ra_val < im_val) { bf |= 0x80000000; }
      if(ra_val > im_val) { bf |= 0x40000000; }
      if(ra_val == im_val) { bf |= 0x20000000; }
      int condition_mask = 0xF0000000 >> (cr*4);
      int condition =  bf >> (cr*4);
      condition_reg_ = (condition_reg_ & ~condition_mask) | condition;
      break;
    }
    case CMPI: {
      int ra = instr->RAValue();
      int32_t ra_val = get_register(ra);
      int32_t im_val = instr->Bits(15,0);
      im_val = (im_val << 16) >> 16;  // sign extend if needed
      int cr = instr->Bits(25,23);
      int bf = 0;
      if(ra_val < im_val) { bf |= 0x80000000; }
      if(ra_val > im_val) { bf |= 0x40000000; }
      if(ra_val == im_val) { bf |= 0x20000000; }
      int condition_mask = 0xF0000000 >> (cr*4);
      int condition =  bf >> (cr*4);
      condition_reg_ = (condition_reg_ & ~condition_mask) | condition;
      break;
    }
    case ADDIC: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int32_t ra_val = get_register(ra);
      int32_t im_val = (instr->Bits(15,0) << 16) >> 16;
      int32_t alu_out = ra_val + im_val;
      set_register(rt, alu_out);
      // todo - handle Carry ? 
      break;
    }
    case ADDICx:
    case ADDI: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int32_t im_val = (instr->Bits(15,0) << 16) >> 16;
      int32_t alu_out;
      if(ra == 0) {
        alu_out = im_val;
      } else {
        int32_t ra_val = get_register(ra);
        alu_out = ra_val + im_val;
      } 
      set_register(rt, alu_out);
      // todo - handle RC bit
      break;
    }
    case ADDIS: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int32_t im_val = (instr->Bits(15,0) << 16);
      int32_t alu_out;
      if(ra==0) { // treat r0 as zero
        alu_out = im_val;
      } else {
        int32_t ra_val = get_register(ra);
        alu_out = ra_val + im_val;
      }
      set_register(rt, alu_out);
      break;
    }
    case BCX: {
      int bo = instr->Bits(25,21) << 21;
      int offset = (instr->Bits(15,2) << 18) >> 16;
      int condition_bit = instr->Bits(20,16);
      int condition_mask = 0x80000000 >> condition_bit;
      switch(bo) {
        case DCBNZF: // Decrement CTR; branch if CTR != 0 and condition false
        case DCBEZF: // Decrement CTR; branch if CTR == 0 and condition false
          UNIMPLEMENTED(); 
        case BF: {   // Branch if condition false
          if (!(condition_reg_ & condition_mask)) {
            set_pc(get_pc() + offset);
          }
          break;
        }
        case DCBNZT: // Decrement CTR; branch if CTR != 0 and condition true
        case DCBEZT: // Decrement CTR; branch if CTR == 0 and condition true
          UNIMPLEMENTED(); 
        case BT: {   // Branch if condition true
          if (condition_reg_ & condition_mask) {
            set_pc(get_pc() + offset);
          }
          break;
        }
        case DCBNZ:  // Decrement CTR; branch if CTR != 0
        case DCBEZ:  // Decrement CTR; branch if CTR == 0
          UNIMPLEMENTED(); 
        case BA: {   // Branch always
          set_pc(get_pc() + offset);
          break;
        }
        default:
          UNIMPLEMENTED(); // Invalid encoding
      }
      break;
    }
    case SC:
    case BX: {
      int offset = (instr->Bits(25,2) << 8) >> 6;
      if(instr->Bit(0) == 1) {  // LK flag set 
        special_reg_lr_ = get_pc() + 4;
      }
      set_pc(get_pc() + offset);
      // todo - AA flag
      break;
    }
    case EXT1: {
      DecodeExt1(instr);
      break;
    }
    case RLWIMIX: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      int32_t rs_val = get_register(rs);
      int32_t ra_val = get_register(ra);
      int sh = instr->Bits(15,11);
      int mb = instr->Bits(10,6);
      int me = instr->Bits(5,1);
      int rc = instr->Bit(0);
      // rotate left
      int result = (rs_val<<sh) | (rs_val>>(32-sh));
      int mask = 0;
      if(mb < me+1) {
        int bit = 0x80000000 >> mb;
        for(; mb<=me; mb++) {
          mask |= bit;
          bit >>= 1;
        }
      } else if(mb == me+1) {
         mask = 0xffffffff;
      } else { // mb > me+1
        int bit = 0x80000000 >> (me+1);  // needs to be tested
        mask = 0xffffffff;
        for(;me<mb;me++) {
          mask ^= bit;
          bit >>= 1;
        }
      }
      result &= mask;
      ra_val &= ~mask;
      result |= ra_val;
      set_register(ra, result);
      if(rc) {
        int bf = 0;
        if(result < 0) { bf |= 0x80000000; }
        if(result > 0) { bf |= 0x40000000; }
        if(result == 0) { bf |= 0x20000000; }
        condition_reg_ = (condition_reg_ & ~0xF0000000) | bf;
      }
      break;
    }
    case RLWINMX: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      int32_t rs_val = get_register(rs);
      int sh = instr->Bits(15,11);
      int mb = instr->Bits(10,6);
      int me = instr->Bits(5,1);
      int rc = instr->Bit(0);
      // rotate left
      int result = (rs_val<<sh) | (rs_val>>(32-sh));
      int mask = 0;
      if(mb < me+1) {
        int bit = 0x80000000 >> mb;
        for(; mb<=me; mb++) {
          mask |= bit;
          bit >>= 1;
        }
      } else if(mb == me+1) {
         mask = 0xffffffff;
      } else { // mb > me+1 
        int bit = 0x80000000 >> (me+1);  // needs to be tested
        mask = 0xffffffff;
        for(;me<mb;me++) {
          mask ^= bit;
          bit >>= 1;
        }
      }
      result &= mask;  
      set_register(ra, result);
      if(rc) {
        int bf = 0;
        if(result < 0) { bf |= 0x80000000; }
        if(result > 0) { bf |= 0x40000000; }
        if(result == 0) { bf |= 0x20000000; }
        condition_reg_ = (condition_reg_ & ~0xF0000000) | bf;
      }
      break;
    }
    case RLWNMX:
    case ORI: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int32_t ra_val = get_register(ra);
      uint32_t im_val = instr->Bits(15,0);
      int32_t alu_out = ra_val | im_val;
      set_register(rt, alu_out);
      // todo - set condition based SO bit
    }
    case ORIS:
    case XORI:
    case XORIS:
    case ANDIx: {
      int rs= instr->RSValue();
      int ra = instr->RAValue();
      int32_t rs_val = get_register(rs);
      uint32_t im_val = instr->Bits(15,0);
      int32_t alu_out = rs_val & im_val;
      set_register(ra, alu_out);
      // todo - set condition based SO bit
      break;
    }
    case ANDISx:
    case EXT2: {
      DecodeExt2(instr);
      break;
    }
    case LWZ: {
      int ra = instr->RAValue();
      int rt = instr->RTValue();
      int32_t ra_val = get_register(ra);
      int offset = (instr->Bits(15,0) << 16) >> 16;
      if(ra != 0) {
        offset += ra_val;
      }
      set_register(rt, ReadW(offset, instr));
      break;
    }
    case LWZU: {
      int ra = instr->RAValue();
      int rt = instr->RTValue();
      int32_t ra_val = get_register(ra);
      int offset = (instr->Bits(15,0) << 16) >> 16;
      if(ra != 0) {
        offset += ra_val;
        set_register(ra, offset);
      }
      set_register(rt, ReadW(offset, instr));
      break;
    }
    case LBZ: {
      int ra = instr->RAValue();
      int rt = instr->RTValue();
      int32_t ra_val = get_register(ra);
      int offset = (instr->Bits(15,0) << 16) >> 16;
      if(ra != 0) {
        offset += ra_val;
      }
      set_register(rt, ReadB(offset) & 0xFF);
      break;
    }
    case LBZU:
    case STW: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      int32_t ra_val = get_register(ra);
      int32_t rs_val = get_register(rs);
      int offset = (instr->Bits(15,0) << 16) >> 16;
      if(ra != 0) {
	offset += ra_val;
      }
      WriteW(offset, rs_val, instr);
      // printf("r%d %08x -> %08x\n", rs, rs_val, offset); // 0xdead
      break;
    }
    case STWU: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      int32_t ra_val = get_register(ra);
      int32_t rs_val = get_register(rs);
      int offset = (instr->Bits(15,0) << 16) >> 16;
      if(ra != 0) {
        offset += ra_val;
        set_register(ra, offset);
      }
      WriteW(offset, rs_val, instr);
      break;
    }
    case STB: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      int32_t ra_val = get_register(ra);
      int8_t rs_val = get_register(rs);
      int offset = (instr->Bits(15,0) << 16) >> 16;
      if(ra != 0) {
        offset += ra_val;
      }
      WriteB(offset, rs_val);
      break;
    }
    case STBU:
    case LHZ: {
      int ra = instr->RAValue();
      int rt = instr->RTValue();
      int32_t ra_val = get_register(ra);
      int offset = (instr->Bits(15,0) << 16) >> 16;
      if(ra != 0) {
        offset += ra_val;
      }
      set_register(rt, ReadH(offset, instr));
      break;
    }
    case LHZU:
    case LHA:
    case LHAU:
    case STH: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      int32_t ra_val = get_register(ra);
      int16_t rs_val = get_register(rs);
      int offset = (instr->Bits(15,0) << 16) >> 16;
      if(ra != 0) {
        offset += ra_val;
      }
      WriteH(offset, rs_val, instr);
      break;
    }
    case STHU:
    case LMW:
    case STMW:
    case LFS:
    case LFSU:
    case LFD: {
      int frt = instr->RTValue();
      int ra = instr->RAValue();
      int32_t offset = (instr->Bits(15,0) << 16) >> 16;
      int32_t ra_val = get_register(ra);
      double *dptr = (double*)ReadDW(ra_val + offset);
      set_d_register_from_double(frt, static_cast<double>(*dptr));
      break;
    }
    case LFDU:
    case STFS:
    case STFSU:
    case STFD: {
      int frt = instr->RTValue();
      int ra = instr->RAValue();
      int32_t offset = (instr->Bits(15,0) << 16) >> 16;
      int32_t ra_val = get_register(ra);
      double frt_val = get_double_from_d_register(frt);
      int *p = reinterpret_cast<int*>(&frt_val);
      WriteDW(ra_val + offset, p[0], p[1]);
      break;
    }
    case STFDU:
    case EXT3:
    case EXT4: {
      DecodeExt4(instr);
      break;
    }
    default: {
      UNIMPLEMENTED();
      break;
    }
  }
  if (!pc_modified_) {
    set_register(pc, reinterpret_cast<int32_t>(instr)
                         + Instruction::kInstrSize);
  }
}


void Simulator::Execute() {
  // Get the PC to simulate. Cannot use the accessor here as we need the
  // raw PC value and not the one used as input to arithmetic instructions.
  int program_counter = get_pc();

  if (::v8::internal::FLAG_stop_sim_at == 0) {
    // Fast version of the dispatch loop without checking whether the simulator
    // should be stopping at a particular executed instruction.
    while (program_counter != end_sim_pc) {
      Instruction* instr = reinterpret_cast<Instruction*>(program_counter);
      icount_++;
      InstructionDecode(instr);
      program_counter = get_pc();
    }
  } else {
    // FLAG_stop_sim_at is at the non-default value. Stop in the debugger when
    // we reach the particular instuction count.
    while (program_counter != end_sim_pc) {
      Instruction* instr = reinterpret_cast<Instruction*>(program_counter);
      icount_++;
      if (icount_ == ::v8::internal::FLAG_stop_sim_at) {
        PPCDebugger dbg(this);
        dbg.Debug();
      } else {
        InstructionDecode(instr);
      }
      program_counter = get_pc();
    }
  }
}


int32_t Simulator::Call(byte* entry, int argument_count, ...) {
  va_list parameters;
  va_start(parameters, argument_count);
  // Set up arguments

  // First five arguments passed in registers. << probably wrong for PPC
  ASSERT(argument_count >= 5);
  set_register(r3, va_arg(parameters, int32_t));
  set_register(r4, va_arg(parameters, int32_t));
  set_register(r5, va_arg(parameters, int32_t));
  set_register(r6, va_arg(parameters, int32_t));
  set_register(r7, va_arg(parameters, int32_t));

  // Remaining arguments passed on stack.
  int original_stack = get_register(sp);
  // Compute position of stack on entry to generated code.
  int entry_stack = (original_stack - (argument_count - 4) * sizeof(int32_t)
    - 8); // -8 extra stack is a hack for the LR slot + old SP on PPC
  if (OS::ActivationFrameAlignment() != 0) {
    entry_stack &= -OS::ActivationFrameAlignment();
  }
  // Store remaining arguments on stack, from low to high memory.
  intptr_t* stack_argument = reinterpret_cast<intptr_t*>(entry_stack);
  for (int i = 4; i < argument_count; i++) {
    stack_argument[i - 4] = va_arg(parameters, int32_t);
  }
  va_end(parameters);
  set_register(sp, entry_stack);

  // Prepare to execute the code at entry
  set_register(pc, reinterpret_cast<int32_t>(entry));
  // Put down marker for end of simulation. The simulator will stop simulation
  // when the PC reaches this value. By saving the "end simulation" value into
  // the LR the simulation stops when returning to this call point.
  special_reg_lr_ = end_sim_pc;

  // Remember the values of callee-saved registers.
  int32_t r14_val = get_register(r14);
//  int32_t r15_val = get_register(r15);  -- hack r15 is overloaded as ARM PC
  int32_t r16_val = get_register(r16);
  int32_t r17_val = get_register(r17);
  int32_t r18_val = get_register(r18);
  int32_t r19_val = get_register(r19);
//  int32_t r20_val = get_register(r20);  -- this is cp
  int32_t r21_val = get_register(r21);
//  int32_t r22_val = get_register(r22);  -- hacked for r9
  int32_t r23_val = get_register(r23);
  int32_t r24_val = get_register(r24);
  int32_t r25_val = get_register(r25);
  int32_t r26_val = get_register(r26);
  int32_t r27_val = get_register(r27);
  int32_t r28_val = get_register(r28);
  int32_t r29_val = get_register(r29);
  int32_t r30_val = get_register(r30);
	  int32_t fp_val = get_register(fp);

	  // Set up the callee-saved registers with a known value. To be able to check
	  // that they are preserved properly across JS execution.
	  int32_t callee_saved_value = icount_;
	  set_register(r14, callee_saved_value);
	//  set_register(r15, callee_saved_value);  hack for r15 pc
	  set_register(r16, callee_saved_value);
	  set_register(r17, callee_saved_value);
	  set_register(r18, callee_saved_value);
	  set_register(r19, callee_saved_value);
	//  set_register(r20, callee_saved_value); -- this is cp
	  set_register(r21, callee_saved_value);
//	  set_register(r22, callee_saved_value);
	  set_register(r23, callee_saved_value);
	  set_register(r24, callee_saved_value);
	  set_register(r25, callee_saved_value);
	  set_register(r26, callee_saved_value);
	  set_register(r27, callee_saved_value);
	  set_register(r28, callee_saved_value);
	  set_register(r29, callee_saved_value);
	  set_register(r30, callee_saved_value);
	  set_register(fp, callee_saved_value);

	  // Start the simulation
	  Execute();

	  // Check that the callee-saved registers have been preserved.
	  CHECK_EQ(callee_saved_value, get_register(r14));
	//  CHECK_EQ(callee_saved_value, get_register(r15));  hack for r15 PC
	  CHECK_EQ(callee_saved_value, get_register(r16));
	  CHECK_EQ(callee_saved_value, get_register(r17));
	  CHECK_EQ(callee_saved_value, get_register(r18));
	  CHECK_EQ(callee_saved_value, get_register(r19));
	  // CHECK_EQ(callee_saved_value, get_register(r20)); -- cp
	  CHECK_EQ(callee_saved_value, get_register(r21));
//	  CHECK_EQ(callee_saved_value, get_register(r22));
	  CHECK_EQ(callee_saved_value, get_register(r23));
	  CHECK_EQ(callee_saved_value, get_register(r24));
	  CHECK_EQ(callee_saved_value, get_register(r25));
	  CHECK_EQ(callee_saved_value, get_register(r26));
	  CHECK_EQ(callee_saved_value, get_register(r27));
	  CHECK_EQ(callee_saved_value, get_register(r28));
	  CHECK_EQ(callee_saved_value, get_register(r29));
	  CHECK_EQ(callee_saved_value, get_register(r30));
	  CHECK_EQ(callee_saved_value, get_register(fp));

	  // Restore callee-saved registers with the original value.
	  set_register(r14, r14_val);
	//  set_register(r15, r15_val);  hack for R15 PC
	  set_register(r16, r16_val);
	  set_register(r17, r17_val);
	  set_register(r18, r18_val);
	  set_register(r19, r19_val);
	  // set_register(r20, r20_val); -- cp
	  set_register(r21, r21_val);
//	  set_register(r22, r22_val);
	  set_register(r23, r23_val);
	  set_register(r24, r24_val);
	  set_register(r25, r25_val);
	  set_register(r26, r26_val);
	  set_register(r27, r27_val);
	  set_register(r28, r28_val);
	  set_register(r29, r29_val);
	  set_register(r30, r30_val);
	  set_register(fp, fp_val);

  // Pop stack passed arguments.
  CHECK_EQ(entry_stack, get_register(sp));
  set_register(sp, original_stack);

  int32_t result = get_register(r3);   // PowerPC
  return result;
}


uintptr_t Simulator::PushAddress(uintptr_t address) {
  int new_sp = get_register(sp) - sizeof(uintptr_t);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(new_sp);
  *stack_slot = address;
  set_register(sp, new_sp);
  return new_sp;
}


uintptr_t Simulator::PopAddress() {
  int current_sp = get_register(sp);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(current_sp);
  uintptr_t address = *stack_slot;
  set_register(sp, current_sp + sizeof(uintptr_t));
  return address;
}

} }  // namespace v8::internal

#endif  // USE_SIMULATOR
#undef INCLUDE_ARM
#endif  // V8_TARGET_ARCH_PPC
