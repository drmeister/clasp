#include <clasp/core/foundation.h>
#include <clasp/core/object.h>
#include <clasp/core/lisp.h>
#include <clasp/core/ql.h>
#include <clasp/core/array.h>
#include <clasp/core/pointer.h>
#include <clasp/core/multipleValues.h>
#include <clasp/core/functor.h> // function description stuff
#include <clasp/core/sourceFileInfo.h>
#include <clasp/llvmo/debugInfoExpose.h>
#include <clasp/llvmo/code.h>
#include <clasp/core/stackmap.h>
#include <clasp/core/backtrace.h>
#include <stdlib.h> // calloc, realloc, free
#include <execinfo.h> // backtrace

/*
 * This file contains the low-ish level code to get backtrace information.
 * The main entry point is core:call-with-frame, which collects this information
 * and passes it to a provided thunk. This makes more sense than a function
 * that simply _returns_ this information, since after all the control stack
 * inherently has only dynamic extent.
 * This information takes the form of objects called "frames"; see backtrace.h
 * for definitions. Hopefully should be simple to understand.
 * This mechanism uses backtrace, backtrace_symbols, and DWARF metadata.
 *
 * The function core:call-with-operating-system-backtrace is similar but lower
 * level, collecting only information from the system backtrace and
 * backtrace_symbols function, and stack frame pointers. This is not used by
 * call-with-frame but may very occasionally be useful on its own, mostly for
 * debugging the debugger (gods help you).
 */

namespace core {
NEVER_OPTIMIZE
CL_DEFUN T_mv core__call_with_operating_system_backtrace(Function_sp function) {
  // Get an operating system backtrace, i.e. with the backtrace and
  // backtrace_symbols functions (which are not POSIX, but are present in both
  // GNU and Apple systems).
  // backtrace requires a number of frames to get, and will fill only that many
  // entries. To get the full backtrace, we repeatedly try larger frame numbers
  // until backtrace finally doesn't fill everything in.
#define START_BACKTRACE_SIZE 512
#define MAX_BACKTRACE_SIZE_LOG2 20
  size_t num = START_BACKTRACE_SIZE;
  void** buffer = (void**)calloc(sizeof(void*), num);
  for (size_t attempt = 0; attempt < MAX_BACKTRACE_SIZE_LOG2; ++attempt) {
    size_t returned = backtrace(buffer,num);
    if (returned < num) {
      char **strings = backtrace_symbols(buffer, returned);
      ql::list pointers;
      ql::list names;
      ql::list basepointers;
      void* fbp = __builtin_frame_address(0);
      uintptr_t bplow = (uintptr_t)&fbp;
      uintptr_t bphigh = (uintptr_t)my_thread_low_level->_StackTop;
      void* newfbp;
      for (size_t j = 0; j < returned; ++j) {
        pointers << Pointer_O::create(buffer[j]);
        names << SimpleBaseString_O::make(strings[j]);
        basepointers << Pointer_O::create(fbp);
        if (fbp) newfbp = *(void**)fbp;
        if (newfbp && !(bplow<(uintptr_t)newfbp && (uintptr_t)newfbp<bphigh)) {  // newfbp is out of the stack.
//          printf("%s:%d:%s The frame pointer walk went out of bounds bplow is %p bphigh is %p and newfbp is %p\n",
//                 __FILE__, __LINE__, __FUNCTION__, (void*)bplow, (void*)bphigh, newfbp );
          newfbp = NULL;
        } else if (newfbp && !((uintptr_t)fbp < (uintptr_t)newfbp) ) {             // fbp < newfbp
          printf("%s:%d:%s The frame pointer is not monotonically increasing fbp is %p and newfbp is %p\n",
                 __FILE__, __LINE__, __FUNCTION__, fbp, newfbp );
          newfbp = NULL;
        }
        fbp = newfbp;
      }
      free(buffer);
      free(strings);
      return eval::funcall(function, pointers.cons(), names.cons(),
                           basepointers.cons());
    }
    // realloc_array would be nice, but macs don't have it
    num *= 2;
    buffer = (void**)realloc(buffer, sizeof(void*)*num);
  }
  printf("%s:%d Couldn't get backtrace\n", __FILE__, __LINE__ );
  abort();
}

static T_sp dwarf_spi(llvmo::DWARFContext_sp dcontext,
                      llvmo::SectionedAddress_sp sa) {
    // FIXME: Might be better to just use the llvm::DILineInfo directly.
  T_mv lineinfo = llvmo::getLineInfoForAddress(dcontext, sa);
  if (lineinfo.notnilp()) {
    SimpleBaseString_sp filename = gc::As<SimpleBaseString_sp>(lineinfo);
    Integer_sp line = gc::As<Integer_sp>(lineinfo.valueGet_(3));
    Integer_sp column = gc::As<Integer_sp>(lineinfo.valueGet_(4));
    return SourcePosInfo_O::make(filename->get_std_string(),
                                 0, // eck
                                 // See above FIXME; these should all be fine,
                                 // but then why go through Integer_O at all?
                                 line.unsafe_fixnum(), column.unsafe_fixnum());
  } else return _Nil<T_O>();
}

static T_sp dwarf_ep(llvmo::ObjectFile_sp ofi,
                     llvmo::DWARFContext_sp dcontext,
                     llvmo::SectionedAddress_sp sa) {
  auto eranges = llvmo::getAddressRangesForAddressInner(dcontext, sa);
  if (eranges) {
    auto ranges = eranges.get();
    llvmo::Code_sp code = ofi->_Code;
    uintptr_t code_start = code->codeStart();
    T_O** rliterals = code->TOLiteralsStart();
    size_t nliterals = code->TOLiteralsSize();
    for (size_t i = 0; i < nliterals; ++i) {
      T_sp literal((gc::Tagged)(rliterals[i]));
      if (gc::IsA<LocalEntryPoint_sp>(literal)) {
        LocalEntryPoint_sp ep = gc::As_unsafe<LocalEntryPoint_sp>(literal);
        uintptr_t ip = (uintptr_t)(ep->_EntryPoint) - code_start;
        for (auto range : ranges) {
          if ((range.LowPC <= ip) && (ip < range.HighPC))
            return ep;
        }
      } else if (gc::IsA<GlobalEntryPoint_sp>(literal)) {
        GlobalEntryPoint_sp ep = gc::As_unsafe<GlobalEntryPoint_sp>(literal);
        for (size_t j = 0; j < NUMBER_OF_ENTRY_POINTS; ++j) {
          uintptr_t ip = (uintptr_t)(ep->_EntryPoints[j]) - code_start;
          for (auto range : ranges) {
            if ((range.LowPC <= ip) && (ip < range.HighPC))
              return ep;
          }
        }
      }
    }
    // no hits
    return _Nil<T_O>();
  } else {
    // FIXME: signal error?
    return _Nil<T_O>();
  }
}

static void args_from_offset(void* frameptr, int32_t offset,
                             T_sp& closure, T_sp& args) {
  if (frameptr) {
    T_O** register_save_area = (T_O**)((uintptr_t)frameptr + offset);
    T_sp tclosure((gc::Tagged)register_save_area[LCC_CLOSURE_REGISTER]);
    closure = tclosure;
    size_t nargs = (size_t)(register_save_area[LCC_NARGS_REGISTER]);
    if (nargs>256) {
      printf("%s:%d:%s  There are too many arguments %lu\n", __FILE__, __LINE__, __FUNCTION__, nargs);
    }
    ql::list largs;
    // Get the first args from the register save area
    for (size_t i = 0; i < std::min(nargs, (size_t)LCC_ARGS_IN_REGISTERS);
         ++i) {
      T_sp temp((gctools::Tagged)(register_save_area[i+2]));
      largs << temp;
    }
    // and the rest from the stack frame
    for (size_t i = LCC_ARGS_IN_REGISTERS; i < nargs; ++i) {
      T_O* rarg = ((T_O**)frameptr)[i + 2 - LCC_ARGS_IN_REGISTERS];
      T_sp temp((gctools::Tagged)rarg);
      largs << temp;
    }
    args = largs.cons();
  }
}

static void args_for_entry_point(llvmo::ObjectFile_sp ofi, T_sp ep,
                                 void* frameptr,
                                 T_sp& closure, T_sp& args) {
  if (ep.nilp()) return;
  uintptr_t stackmap_start = (uintptr_t)(ofi->_Code->_StackmapStart);
  if (!stackmap_start) return; // ends up as null sometimes apparently
  uintptr_t stackmap_end = stackmap_start + ofi->_Code->_StackmapSize;
  // FIXME: we could check the entry point type ahead of time
  auto thunk = [&](size_t _, const smStkSizeRecord& function,
                   int32_t offsetOrSmallConstant) {
    if (gc::IsA<LocalEntryPoint_sp>(ep)) {
      LocalEntryPoint_sp lep = gc::As_unsafe<LocalEntryPoint_sp>(ep);
      if (function.FunctionAddress == (uintptr_t)(lep->_EntryPoint)) {
        args_from_offset(frameptr, offsetOrSmallConstant, closure, args);
        return;
      }
    } else if (gc::IsA<GlobalEntryPoint_sp>(ep)) {
      GlobalEntryPoint_sp gep = gc::As_unsafe<GlobalEntryPoint_sp>(ep);
      for (size_t j = 0; j < NUMBER_OF_ENTRY_POINTS; ++j) {
        if (function.FunctionAddress == (uintptr_t)(gep->_EntryPoints[j])) {
          args_from_offset(frameptr, offsetOrSmallConstant, closure, args);
          return;
        }
      }
    }
  };
  walk_one_llvm_stackmap(thunk, stackmap_start, stackmap_end);
}

static DebuggerFrame_sp make_lisp_frame(void* ip, llvmo::ObjectFile_sp ofi,
                                        void* fbp) {
  llvmo::SectionedAddress_sp sa = object_file_sectioned_address(ip, ofi, false);
  llvmo::DWARFContext_sp dcontext = llvmo::DWARFContext_O::createDwarfContext(ofi);
  T_sp spi = dwarf_spi(dcontext, sa);
  T_sp ep = dwarf_ep(ofi, dcontext, sa);
  T_sp fd = ep.notnilp() ? gc::As_unsafe<EntryPointBase_sp>(ep)->_FunctionDescription : _Nil<FunctionDescription_O>();
  T_sp closure = _Nil<T_O>(), args = _Nil<T_O>();
  args_for_entry_point(ofi, ep, fbp, closure, args);
  T_sp fname = _Nil<T_O>();
  if (fd.notnilp())
    fname = gc::As_unsafe<FunctionDescription_sp>(fd)->functionName();
  return DebuggerFrame_O::make(fname, spi, fd, closure, args, INTERN_(kw, lisp));
}

bool maybe_demangle(const std::string& fnName, std::string& output)
{
  char *funcname = (char *)malloc(1024);
  size_t funcnamesize = 1024;
  int status;
  char *ret = abi::__cxa_demangle(fnName.c_str(), funcname, &funcnamesize, &status);
  if (status == 0) {
    std::string demangled(funcname);
    free(ret);
    output = demangled;
    return true;
  }
  if (fnName[0] == '_') {
      // try stripping off the first underscore
    std::string shortFnName = fnName.substr(1,std::string::npos);
    char *ret = abi::__cxa_demangle(shortFnName.c_str(), funcname, &funcnamesize, &status);
    if (status == 0) {
      std::string demangled(funcname);
      free(ret);
      output = demangled;
      return true;
    }
  }
  if (funcname) free(funcname);
  return false;
}

static DebuggerFrame_sp make_cxx_frame(void* ip, char* cstring) {
  std::string str(cstring);
  std::string linkname;
#if defined(_TARGET_OS_DARWIN)
  // The string is divided into:
  // index executable address name plus offset
  // but we only care about the name.
  std::vector<std::string> parts = split(str, " ");
  if (parts.size()>3) {
    linkname = parts[3];
  } else {
    linkname = str;
  }
#elif defined(_TARGET_OS_LINUX)
    /* Some examples of what backtrace_symbols(...) returns on Linux...
"/home/meister/Development/cando-main/build/boehm_d/iclasp-boehm-d(_ZN4core6Lisp_O24readEvalPrintInteractiveEv+0x5b) [0x4989f2b]"
"/home/meister/Development/cando-main/build/boehm_d/iclasp-boehm-d(_ZN4core6Lisp_O3runEv+0xb85) [0x498e5c5]"
"/home/meister/Development/cando-main/build/boehm_d/iclasp-boehm-d() [0x47235db]"
*/
  size_t nameStart = str.find('(');
  size_t nameEnd = str.rfind(')');
  if ((nameEnd-nameStart)>1) {
    linkname = str.substr(nameStart+1,nameEnd);
    size_t pluspos = linkname.rfind('+');
    if (pluspos!=std::string::npos) {
      linkname = linkname.substr(0,pluspos); // remove +0x###
    }
  } else {
    linkname = str;
  }
#else
    // I don't know what other OS's backtrace_symbols return - punt
  linkname = str;
#endif
  std::string name;
  if (!(maybe_demangle(linkname, name)))
    // couldn't demangle, so just use the unadulterated string
    name = linkname;
  T_sp lname = SimpleBaseString_O::make(name);
  return DebuggerFrame_O::make(lname, _Nil<T_O>(), _Nil<T_O>(), _Nil<T_O>(),
                               _Nil<T_O>(), INTERN_(kw, c_PLUS__PLUS_));
}

static DebuggerFrame_sp make_frame(void* ip, char* string, void* fbp) {
  T_sp of = llvmo::only_object_file_for_instruction_pointer(ip);
  if (of.nilp()) return make_cxx_frame(ip, string);
  else return make_lisp_frame(ip, gc::As_unsafe<llvmo::ObjectFile_sp>(of), fbp);
}

T_mv call_with_frame(std::function<T_mv(DebuggerFrame_sp)> f) {
  size_t num = START_BACKTRACE_SIZE;
  void** buffer = (void**)calloc(sizeof(void*), num);
  for (size_t attempt = 0; attempt < MAX_BACKTRACE_SIZE_LOG2; ++attempt) {
    size_t returned = backtrace(buffer,num);
    if (returned < num) {
      char **strings = backtrace_symbols(buffer, returned);
      void* fbp = __builtin_frame_address(0); // TODO later
      uintptr_t bplow = (uintptr_t)&fbp;
      uintptr_t bphigh = (uintptr_t)my_thread_low_level->_StackTop;
      DebuggerFrame_sp bot = make_frame(buffer[0], strings[0], fbp);
      DebuggerFrame_sp prev = bot;
      void* newfbp;
      for (size_t j = 1; j < returned; ++j) {
        if (fbp) newfbp = *(void**)fbp;
        if (newfbp && !(bplow<(uintptr_t)newfbp && (uintptr_t)newfbp<bphigh)) {  // newfbp is out of the stack.
//          printf("%s:%d:%s The frame pointer walk went out of bounds bplow is %p bphigh is %p and newfbp is %p\n",
//                 __FILE__, __LINE__, __FUNCTION__, (void*)bplow, (void*)bphigh, newfbp );
          newfbp = NULL;
        } else if (newfbp && !((uintptr_t)fbp < (uintptr_t)newfbp) ) {             // fbp < newfbp
          printf("%s:%d:%s The frame pointer is not monotonically increasing fbp is %p and newfbp is %p\n",
                 __FILE__, __LINE__, __FUNCTION__, fbp, newfbp );
          newfbp = NULL;
        }
        fbp = newfbp;
        DebuggerFrame_sp frame = make_frame(buffer[j], strings[j], fbp);
        frame->down = prev;
        prev->up = frame;
        prev = frame;
      }
      free(buffer);
      free(strings);
      return f(bot);
    }
    // realloc_array would be nice, but macs don't have it
    num *= 2;
    buffer = (void**)realloc(buffer, sizeof(void*)*num);
  }
  printf("%s:%d Couldn't get backtrace\n", __FILE__, __LINE__ );
  abort();
}

CL_DEFUN T_mv core__call_with_frame(Function_sp function) {
  auto th = [&](DebuggerFrame_sp bot){ return eval::funcall(function, bot); };
  return call_with_frame(th);
}

CL_DEFUN T_sp core__debugger_frame_fname(DebuggerFrame_sp df) {
  return df->fname;
}
CL_DEFUN T_sp core__debugger_frame_source_position(DebuggerFrame_sp df) {
  return df->source_position;
}
CL_DEFUN T_sp core__debugger_frame_function_description(DebuggerFrame_sp df) {
  return df->function_description;
}
CL_DEFUN T_sp core__debugger_frame_lang(DebuggerFrame_sp df) {
  return df->lang;
}
CL_DEFUN T_sp core__debugger_frame_closure(DebuggerFrame_sp df) {
  return df->closure;
}
CL_DEFUN T_sp core__debugger_frame_args(DebuggerFrame_sp df) {
  return df->args;
}
CL_DEFUN T_sp core__debugger_frame_up(DebuggerFrame_sp df) {
  return df->up;
}
CL_DEFUN T_sp core__debugger_frame_down(DebuggerFrame_sp df) {
  return df->down;
}

CL_DEFUN T_sp core__debugger_local_fname(DebuggerLocal_sp dl) {
  return dl->fname;
}
CL_DEFUN T_sp core__debugger_local_name(DebuggerLocal_sp dl) {
  return dl->name;
}
CL_DEFUN T_sp core__debugger_local_declfile(DebuggerLocal_sp dl) {
  return dl->declfile;
}
CL_DEFUN T_sp core__debugger_local_declline(DebuggerLocal_sp dl) {
  return dl->declline;
}

}; // namespace core