#pragma once

#include "KittyUtils.hpp"
#include "KittyIOFile.hpp"
#include "KittyMemoryEx.hpp"
#include "KittyMemOp.hpp"
#include "MemoryPatch.hpp"
#include "MemoryBackup.hpp"
#include "KittyScanner.hpp"
#include "KittyTrace.hpp"
#include "KittyAsm.hpp"
#include "KittyPtrValidator.hpp"
#include "KittyPerfEvent.hpp"

using KittyMemoryEx::EProcMapFilter;
using KittyMemoryEx::ProcMap;
using KittyMemoryEx::ProcStatus;

class KittyMemoryMgr
{
private:
    bool _init;
    pid_t _pid;
    std::string _process_name;
    EKittyMemOP _eMemOp;
    std::unique_ptr<IKittyMemOp> _pMemOp;
    std::unique_ptr<IKittyMemOp> _pMemOpPatch;

public:

    MemoryPatchMgr memPatch;

    MemoryBackupMgr memBackup;

    KittyScannerMgr memScanner;

    ElfScannerMgr elfScanner;

#ifdef __ANDROID__

    LinkerScannerMgr linkerScanner;

    NativeBridgeScannerMgr nbScanner;
#endif


    KittyTraceMgr trace;

    KittyMemoryMgr() : _init(false), _pid(0), _eMemOp(EK_MEM_OP_NONE)
    {
    }



    bool initialize(pid_t pid, EKittyMemOP eMemOp, bool initMemPatch);



    inline pid_t processID() const
    {
        return _pid;
    }



    inline std::string processName() const
    {
        return _process_name;
    }



    inline bool isMemValid() const
    {
        return _init && _pid && _pMemOp.get();
    }



    inline IKittyMemOp *memOp() const
    {
        return _pMemOp.get();
    }



    size_t readMem(uintptr_t address, void *buffer, size_t len) const;



    size_t writeMem(uintptr_t address, void *buffer, size_t len) const;



    std::string readMemStr(uintptr_t address, size_t maxLen) const;



    bool writeMemStr(uintptr_t address, std::string str) const;



    bool dumpMemRange(uintptr_t start, uintptr_t end, const std::string &path) const;



    bool dumpMemFile(const std::string &memFile, const std::string &destination) const;



    bool dumpMemELF(const ElfScanner &elf, const std::string &destination) const;
};
