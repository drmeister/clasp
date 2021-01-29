/*
    File: llvmoExpose.h
*/


// #define USE_JITLINKER 1


#ifndef imageSaveLoad_H //[
#define imageSaveLoad_H

#include <clasp/core/common.h>
#include <clasp/llvmo/llvmoExpose.h>

template <>
struct gctools::GCInfo<llvmo::ObjectFile_O> {
  static bool constexpr NeedsInitialization = false;
  static bool constexpr NeedsFinalization = true;
  static GCInfo_policy constexpr Policy = normal;
};



// ObjectFile_O
namespace llvmo {
  FORWARD(Code);
FORWARD(ObjectFile);
class ObjectFile_O : public core::CxxObject_O {
  LISP_CLASS(llvmo, LlvmoPkg, ObjectFile_O, "ObjectFile", core::CxxObject_O);
public:
  std::unique_ptr<llvm::MemoryBuffer> _MemoryBuffer;
  size_t        _Size;
  size_t        _StartupID;
  JITDylib_sp   _JITDylib;
  std::string   _FasoName;
  size_t        _FasoIndex;
  void*         _text_segment_start;
  size_t        _text_segment_size;
  size_t        _text_segment_SectionID;
  void*         _stackmap_start;
  size_t        _stackmap_size;
  Code_sp       _Code;
  gctools::GCRootsInModule* _GCRootsInModule;
public:
  static ObjectFile_sp create(std::unique_ptr<llvm::MemoryBuffer> buffer, size_t startupID, JITDylib_sp jitdylib, const std::string& fasoName, size_t fasoIndex);
  ObjectFile_O( std::unique_ptr<llvm::MemoryBuffer> buffer, size_t startupID, JITDylib_sp jitdylib, const std::string& fasoName, size_t fasoIndex) : _MemoryBuffer(std::move(buffer)), _StartupID(startupID), _JITDylib(jitdylib), _FasoName(fasoName), _FasoIndex(fasoIndex), _Code(_Unbound<Code_O>()) {};
  ~ObjectFile_O();
  static void writeToFile(const std::string& filename, const char* start, size_t size);
}; // ObjectFile_O class def
}; // llvmo
/* from_object translators */

#if 0
namespace translate {
template <>
struct from_object<llvm::object::ObjectFile *, std::true_type> {
  typedef llvm::object::ObjectFile *DeclareType;
  DeclareType _v;
  from_object(T_P object) : _v(gc::As<llvmo::ObjectFile_sp>(object)->wrappedPtr()){};
};

};

/* to_object translators */

namespace translate {
template <>
struct to_object<llvm::object::ObjectFile *> {
  static core::T_sp convert(llvm::object::ObjectFile *ptr) {
    return core::RP_Create_wrapped<llvmo::ObjectFile_O, llvm::object::ObjectFile *>(ptr);
  }
};
}; // namespace llvmo - ObjectFile_O done
#endif


namespace llvmo {


  /* Code_O
   * This object contains all of the code and data generated by relocating an object file.
   * The data and code is stored in _DataCode.
   * The layout is | RWData | ROData | Code 
   * We place the RWData at the top of the object so we can scan it for GC managed pointers.
   */
FORWARD(Code);
class Code_O : public core::CxxObject_O {
  LISP_CLASS(llvmo, LlvmoPkg, Code_O, "Code", core::CxxObject_O);
 public:
  static Code_sp make(uintptr_t scanSize, uintptr_t size);
 public:
  typedef uint8_t value_type;
 public:
  // Store the allocation sizes and alignments
  // Keep track of the Head and Tail indices of the memory in _Data;
  uintptr_t     _HeadOffset;
  uintptr_t     _TailOffset;
  ObjectFile_sp _ObjectFile;
  void*         _TextSegmentStart;
  void*         _TextSegmentEnd;
  uintptr_t     _TextSegmentSectionId;
  void*         _StackmapStart;
  uintptr_t     _StackmapSize;
  void*         _LiteralVectorStart;
  size_t        _LiteralVectorLength;
  gctools::GCArray_moveable<uint8_t> _DataCode;

  void* allocateHead(uintptr_t size, uint32_t align);
  void* allocateTail(uintptr_t size, uint32_t align);
  void describe() const;

 Code_O(uintptr_t totalSize ) :
  _TailOffset(totalSize)
    , _ObjectFile(_Unbound<ObjectFile_O>())
    , _DataCode(totalSize,0,true) {};
           
};
  
};



namespace llvmo {

  class ClaspSectionMemoryManager : public SectionMemoryManager {
    bool needsToReserveAllocationSpace() { return true; };
    void reserveAllocationSpace(uintptr_t CodeSize,
                                uint32_t CodeAlign,
                                uintptr_t RODataSize,
                                uint32_t RODataAlign,
                                uintptr_t RWDataSize,
                                uint32_t RWDataAlign);
    uint8_t* allocateCodeSection( uintptr_t Size, unsigned Alignment,
                                  unsigned SectionID,
                                  StringRef SectionName );
    uint8_t* allocateDataSection( uintptr_t Size, unsigned Alignment,
                                  unsigned SectionID,
                                  StringRef SectionName,
                                  bool isReadOnly);
    void 	notifyObjectLoaded (RuntimeDyld &RTDyld, const object::ObjectFile &Obj);
    bool finalizeMemory(std::string* ErrMsg = nullptr);
  public:
    uint8_t*      _CodeStart;
    size_t        _CodeSize;
  };

};




namespace llvmo {
using namespace llvm;
using namespace llvm::jitlink;

class ClaspAllocator final : public JITLinkMemoryManager {
public:
  ClaspAllocator() {
  }
public:
  static Expected<std::unique_ptr<ClaspAllocator>>
  Create() {
    Error Err = Error::success();
    std::unique_ptr<ClaspAllocator> Allocator(
        new ClaspAllocator());
    return std::move(Allocator);
  }

  Expected<std::unique_ptr<JITLinkMemoryManager::Allocation>>
  allocate(const JITLinkDylib *JD, const SegmentsRequestMap &Request) override {

    using AllocationMap = DenseMap<unsigned, sys::MemoryBlock>;

    // Local class for allocation.
    class IPMMAlloc : public Allocation {
    public:
    IPMMAlloc(ClaspAllocator &Parent, AllocationMap SegBlocks)
      : Parent(Parent), SegBlocks(std::move(SegBlocks)) {}
      MutableArrayRef<char> getWorkingMemory(ProtectionFlags Seg) override {
        DEBUG_OBJECT_FILES(("%s:%d:%s Seg = 0x%x base = %p  size = %lu\n", __FILE__, __LINE__, __FUNCTION__, Seg, static_cast<char *>(SegBlocks[Seg].base()), SegBlocks[Seg].allocatedSize() ));
        assert(SegBlocks.count(Seg) && "No allocation for segment");
        return {static_cast<char *>(SegBlocks[Seg].base()), SegBlocks[Seg].allocatedSize()};
      }
      JITTargetAddress getTargetMemory(ProtectionFlags Seg) override {
        assert(SegBlocks.count(Seg) && "No allocation for segment");
        DEBUG_OBJECT_FILES(("%s:%d:%s Seg = 0x%x Returning %p\n", __FILE__, __LINE__, __FUNCTION__, Seg, (void*)pointerToJITTargetAddress(SegBlocks[Seg].base()))); 
        return pointerToJITTargetAddress(SegBlocks[Seg].base());
      }
      void finalizeAsync(FinalizeContinuation OnFinalize) override {
        OnFinalize(applyProtections());
      }
      Error deallocate() override {
        for (auto &KV : SegBlocks)
          if (auto EC = sys::Memory::releaseMappedMemory(KV.second))
            return errorCodeToError(EC);
        return Error::success();
      }

    private:
      Error applyProtections() {
        for (auto &KV : SegBlocks) {
          auto &Prot = KV.first;
          auto &Block = KV.second;
          if (Prot & sys::Memory::MF_EXEC)
            if (auto EC = sys::Memory::protectMappedMemory(Block,sys::Memory::MF_RWE_MASK))
              return errorCodeToError(EC);
            sys::Memory::InvalidateInstructionCache(Block.base(),
                                                    Block.allocatedSize());
        }
        return Error::success();
      }

      ClaspAllocator &Parent;
      AllocationMap SegBlocks;
    };

    AllocationMap Blocks;

    DEBUG_OBJECT_FILES(("%s:%d:%s  Interating Request\n", __FILE__, __LINE__, __FUNCTION__ ));
    size_t totalSize = 0;
    size_t scanSize = 0;
    for (auto &KV : Request) {
      auto &Seg = KV.second;
      uint64_t ZeroFillStart = Seg.getContentSize();
      uint64_t SegmentSize = gctools::AlignUp((ZeroFillStart+Seg.getZeroFillSize()),Seg.getAlignment());
      DEBUG_OBJECT_FILES(("%s:%d:%s    allocation KV.first = 0x%x Seg info align/ContentSize/ZeroFillSize = %llu/%lu/%llu  \n", __FILE__, __LINE__, __FUNCTION__, KV.first, Seg.getAlignment(), Seg.getContentSize(), Seg.getZeroFillSize()));
      // Add Seg.getAlignment() just in case we need a bit more space to make alignment.
      if ((llvm::sys::Memory::MF_RWE_MASK & KV.first) == ( llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE )) {
        // We have to scan the entire RW data region (sigh) for pointers
        scanSize = SegmentSize;
      }
      totalSize += gctools::AlignUp(Seg.getContentSize()+Seg.getZeroFillSize(),Seg.getAlignment())+Seg.getAlignment();
    }
    DEBUG_OBJECT_FILES(("%s:%d:%s allocation scanSize = %lu  totalSize = %lu\n", __FILE__, __LINE__, __FUNCTION__, scanSize, totalSize));
    Code_sp codeObject = Code_O::make(scanSize,totalSize);
    // Associate the Code object with the current ObjectFile
    my_thread->topObjectFile()->_Code = codeObject;
    for (auto &KV : Request) {
      auto &Seg = KV.second;
      uint64_t ZeroFillStart = Seg.getContentSize();
      size_t SegmentSize = (uintptr_t)gctools::AlignUp(ZeroFillStart+Seg.getZeroFillSize(),Seg.getAlignment());
      void* base;
      if ((llvm::sys::Memory::MF_RWE_MASK & KV.first) == ( llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE )) {
        base = codeObject->allocateHead(SegmentSize,Seg.getAlignment());
        DEBUG_OBJECT_FILES(("%s:%d:%s allocating Prot 0x%x from the head base = %p\n", __FILE__, __LINE__, __FUNCTION__, KV.first, base ));
      } else {
        base = codeObject->allocateTail(SegmentSize,Seg.getAlignment());
        DEBUG_OBJECT_FILES(("%s:%d:%s allocating Prot 0x%x from the tail base = %p\n", __FILE__, __LINE__, __FUNCTION__, KV.first, base ));
      }
      sys::MemoryBlock SegMem(base,SegmentSize);
        // Zero out the zero-fill memory
      memset(static_cast<char*>(SegMem.base())+ZeroFillStart, 0,
             Seg.getZeroFillSize());
        // Record the block for this segment
      Blocks[KV.first] = std::move(SegMem);
    }
    return std::unique_ptr<InProcessMemoryManager::Allocation>(new IPMMAlloc(*this, std::move(Blocks)));
  }

};

};


#endif // imageSaveLoad_H

/*
Copyright (c) 2014, Christian E. Schafmeister

CLASP is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

See directory 'clasp/licenses' for full details.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
/* -^- */
