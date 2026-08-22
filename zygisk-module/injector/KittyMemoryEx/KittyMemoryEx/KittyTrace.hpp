#pragma once

#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <cerrno>
#include <functional>
#include <type_traits>

#include "KittyUtils.hpp"
#include "KittyMemoryEx.hpp"

#if defined(__LP64__)
#define KT_PTRACE_GETREG_REQ PTRACE_GETREGSET
#define KT_PTRACE_SETREG_REQ PTRACE_SETREGSET
#else
#define KT_PTRACE_GETREG_REQ PTRACE_GETREGS
#define KT_PTRACE_SETREG_REQ PTRACE_SETREGS
#endif

#if defined(__arm__)
#define user_regs_struct user_regs
#endif

#if defined(__aarch64__) || defined(__arm__)
#define KT_CPSR_T_MASK (1u << 5)
#endif

#if defined(__i386__)
#define KT_REG_RET eax
#define KT_REG_PC eip
#define KT_REG_IP eip
#define KT_REG_SP esp
#define KT_REG_SYSNR eax
#define KT_REG_ARGS_NUM 0

#elif defined(__x86_64__)
#define KT_REG_RET rax
#define KT_REG_PC rip
#define KT_REG_IP rip
#define KT_REG_SP rsp
#define KT_REG_SYSNR rax
#define KT_REG_ARGS_NUM 6

#elif defined(__aarch64__)
#define KT_REG_RET regs[0]
#define KT_REG_PC pc
#define KT_REG_IP pc
#define KT_REG_SP sp
#define KT_REG_SYSNR regs[8]
#define KT_REG_LR regs[30]
#define KT_REG_CPSR pstate
#define KT_REG_ARGS_NUM 8

#elif defined(__arm__)
#define KT_REG_RET uregs[0]
#define KT_REG_PC uregs[15]
#define KT_REG_IP uregs[15]
#define KT_REG_SP uregs[13]
#define KT_REG_SYSNR uregs[7]
#define KT_REG_LR uregs[14]
#define KT_REG_CPSR uregs[16]
#define KT_REG_ARGS_NUM 4

#endif

enum KT_RP_CALL_STATUS
{
    KT_RP_CALL_FAILED,
    KT_RP_CALL_SUCCESS,
    KT_RP_CALL_TIMEOUT,
    KT_RP_CALL_EXITED,
    KT_RP_CALL_CONT_FAILED,
    KT_RP_CALL_REGS_FAILED,
    KT_RP_CALL_WAIT_FAILED,
    KT_RP_CALL_MEM_FAILED,
    KT_RP_CALL_STEP_FAILED,
    KT_RP_CALL_NOT_STOPPED,
    KT_RP_CALL_MISMATCH_STOP,
};

enum KT_HW_BP_TYPE
{
    KT_HW_BP_EXECUTE = 0,
    KT_HW_BP_READ,
    KT_HW_BP_WRITE,
    KT_HW_BP_ACCESS
};

enum KT_HW_BP_SIZE
{
    KT_HW_BP_SIZE_EXEC = 0,
    KT_HW_BP_SIZE_1 = 1,
    KT_HW_BP_SIZE_2 = 2,
    KT_HW_BP_SIZE_3 = 3,
    KT_HW_BP_SIZE_4 = 4,
    KT_HW_BP_SIZE_5 = 5,
    KT_HW_BP_SIZE_6 = 6,
    KT_HW_BP_SIZE_7 = 7,
    KT_HW_BP_SIZE_8 = 8,
};

enum KT_BP_RESULT
{
    KT_BP_FAILED,
    KT_BP_SUCCESS,
    KT_BP_TIMEOUT,
    KT_BP_EXITED,
    KT_BP_CONT_FAILED,
    KT_BP_STEP_FAILED,
    KT_BP_REGS_FAILED,
    KT_BP_WAIT_FAILED,
    KT_BP_MEM_FAILED,
    KT_BP_NOT_STOPPED,
    KT_BP_MISMATCH_STOP,
};

namespace KittyTraceInsns
{
#if defined(__aarch64__)
    static constexpr int EXEC_SIZE = 4;
    static constexpr uint8_t BRKP[] = {0x00, 0x00, 0x20, 0xd4};
    static constexpr uint8_t SYSCALL[] = {0x01, 0x00, 0x00, 0xd4};
    static constexpr uint8_t NOP[] = {0x1f, 0x20, 0x03, 0xd5};
#elif defined(__arm__)
    static constexpr int THUMB_EXEC_SIZE = 2;
    static constexpr uint8_t THUMB_BRKP[] = {0x00, 0xbe};
    static constexpr uint8_t THUMB_SYSCALL[] = {0x00, 0xdf};
    static constexpr uint8_t THUMB_NOP[] = {0x00, 0xbf};

    static constexpr int EXEC_SIZE = 4;
    static constexpr uint8_t BRKP[] = {0x70, 0x00, 0x20, 0xe1};
    static constexpr uint8_t SYSCALL[] = {0x00, 0x00, 0x00, 0xef};
    static constexpr uint8_t NOP[] = {0x00, 0xf0, 0x20, 0xe3};
#elif defined(__x86_64__)
    static constexpr int EXEC_SIZE = 1;
    static constexpr uint8_t BRKP[] = {0xcc};
    static constexpr uint8_t SYSCALL[] = {0x0f, 0x05};
    static constexpr uint8_t NOP[] = {0x90};
#elif defined(__i386__)
    static constexpr int EXEC_SIZE = 1;
    static constexpr uint8_t BRKP[] = {0xcc};
    static constexpr uint8_t SYSCALL[] = {0xcd, 0x80};
    static constexpr uint8_t NOP[] = {0x90};
#endif
}

#define KT_REGS_ALIGN_STACK(regs) regs.KT_REG_SP = uintptr_t(intptr_t(intptr_t(regs.KT_REG_SP) & intptr_t(~0xF)))
#define KT_REGS_ALIGN_STACK_N(regs, n)                                                                                 \
    regs.KT_REG_SP = uintptr_t(intptr_t((intptr_t(regs.KT_REG_SP) - intptr_t(n)) & intptr_t(~0xF)))

#define KT_ALIGN_STACK(s) s = uintptr_t(intptr_t(intptr_t(s) & intptr_t(~0xF)))
#define KT_ALIGN_STACK_N(s, n) s = uintptr_t(intptr_t((intptr_t(s) - intptr_t(n)) & intptr_t(~0xF)))

struct kitty_rp_call_t
{
    KT_RP_CALL_STATUS status = KT_RP_CALL_FAILED;
    union
    {
        intptr_t val = 0;
        uintptr_t ptr;
    } result;
};

class KittyTraceMgr
{
private:
    pid_t _pid;
    uintptr_t _defaultCaller;
    bool _attached, _seized, _autoRestoreRegs;
    int _remoteCallTimeout;

    kitty_rp_call_t _callFunctionFrom(uintptr_t callerAddress, uintptr_t functionAddress, int nargs, ...);
    kitty_rp_call_t _callSyscall(long sysnr, int nargs, ...);

public:
    KittyTraceMgr()
        : _pid(0), _defaultCaller(0), _attached(false), _seized(false), _autoRestoreRegs(true), _remoteCallTimeout(0)
    {
    }



    KittyTraceMgr(pid_t pid, uintptr_t defaultCaller = 0, bool autoRestoreRegs = true, int remoteCallTimeout = 0)
        : _pid(pid), _defaultCaller(defaultCaller), _attached(isAttached()), _seized(false),
          _autoRestoreRegs(autoRestoreRegs), _remoteCallTimeout(remoteCallTimeout)
    {
    }



    inline pid_t pid() const
    {
        return _pid;
    }



    inline bool isAttached() const
    {
        KittyMemoryEx::ProcStatus pstatus{};
        KittyMemoryEx::ProcStatus::parse(_pid, &pstatus);
        int tracerPID = pstatus.getInt("TracerPid");
        return _pid >= 0 && getpid() == tracerPID;
    }



    inline std::vector<pid_t> threads() const
    {
        return KittyMemoryEx::getAllThreads(_pid);
    }



    bool attach(int options = 0);



    bool seize(int options = 0);



    bool setOptions(int options);



    bool detach();



    inline bool stopAllThreads()
    {
        return kill(_pid, SIGSTOP) != -1;
    }



    inline bool contAllThreads()
    {
        return kill(_pid, SIGCONT) != -1;
    }



    bool stop();



    bool cont(int sig = 0);



    pid_t wait(int *status, int options, int timeout_ms = 0) const;



    bool waitSyscall() const;



    bool step(int steps = 1) const;



    bool waitStep(int steps = 1) const;



    inline bool getSignalInfo(siginfo_t *si) const
    {
        if (!si || !_pid || !_attached)
            return false;
        return ptrace(PTRACE_GETSIGINFO, _pid, 0, si) == 0;
    }



    bool getRegs(user_regs_struct *regs) const;



    bool setRegs(user_regs_struct *regs) const;



    uintptr_t getReturnAddressFromRegs(user_regs_struct *regs)
    {
#if defined(__x86_64__) || defined(__i386__)
        uintptr_t ret = 0;
        peekMem(regs->KT_REG_SP, &ret, sizeof(uintptr_t));
        return ret;

#elif defined(__aarch64__) || defined(__arm__)
        return regs->KT_REG_LR;
#endif
    }



    template <typename T>
    T getArgFromRegs(user_regs_struct *regs, uint32_t arg_num)
    {
        static_assert(std::is_arithmetic<T>::value, "T must be a numeric type!");

#if defined(__x86_64__)
        switch (arg_num)
        {
        case 0:
            return regs->rdi;
        case 1:
            return regs->rsi;
        case 2:
            return regs->rdx;
        case 3:
            return regs->rcx;
        case 4:
            return regs->r8;
        case 5:
            return regs->r9;
        default:
            break;
        }

        uintptr_t arg = 0;
        peekMem(regs->KT_REG_SP + 8 + (arg_num - 6) * 8, &arg, sizeof(uintptr_t));
        return arg;

#elif defined(__i386__)
        uintptr_t arg = 0;
        peekMem(regs->KT_REG_SP + 4 + (arg_num * 4), &arg, sizeof(uintptr_t));
        return arg;

#elif defined(__aarch64__)
        if (arg_num < 8)
            return regs->regs[arg_num];

        uintptr_t arg = 0;
        peekMem(regs->KT_REG_SP + (arg_num - 8) * 8, &arg, sizeof(uintptr_t));
        return arg;

#elif defined(__arm__)
        if (arg_num < 4)
            return regs->uregs[arg_num];

        uintptr_t arg = 0;
        peekMem(regs->KT_REG_SP + (arg_num - 4) * 4, &arg, sizeof(uintptr_t));
        return arg;
#endif
    }



    size_t peekMem(uintptr_t addr, void *buf, size_t size) const;



    size_t pokeMem(uintptr_t addr, const void *buf, size_t size) const;



    inline uintptr_t defaultCaller() const
    {
        return _defaultCaller;
    }



    inline void setDefaultCaller(uintptr_t caller)
    {
        _defaultCaller = caller;
    }



    inline bool autoRestoreRegs() const
    {
        return _autoRestoreRegs;
    }



    inline void setAutoRestoreRegs(bool flag)
    {
        _autoRestoreRegs = flag;
    }



    inline int remoteCallTimeout() const
    {
        return _remoteCallTimeout;
    }



    inline void setRemoteCallTimeout(int ms)
    {
        _remoteCallTimeout = ms > 0 ? ms : 0;
    }



    template <class... Args>
    kitty_rp_call_t callFunctionFrom(uintptr_t callerAddress, uintptr_t functionAddress, Args &&...a)
    {
        return _callFunctionFrom(callerAddress, functionAddress, sizeof...(a), std::forward<Args>(a)...);
    }



    template <class... Args>
    kitty_rp_call_t callFunction(uintptr_t functionAddress, Args &&...a)
    {
        return _callFunctionFrom(_defaultCaller, functionAddress, sizeof...(a), std::forward<Args>(a)...);
    }



    template <class... Args>
    kitty_rp_call_t callSyscall(long sysnr, Args &&...a)
    {
        return _callSyscall(sysnr, sizeof...(a), std::forward<Args>(a)...);
    }



    KT_BP_RESULT setSoftBreakpointAndWait(uintptr_t address,
                                          const std::function<bool(user_regs_struct regs)> &cb,
                                          int timeout_ms);



    KT_BP_RESULT setHardBreakpointAndWait(uintptr_t address,
                                          KT_HW_BP_TYPE type,
                                          KT_HW_BP_SIZE size,
                                          int slot,
                                          const std::function<bool(user_regs_struct regs)> &cb,
                                          int timeout_ms);



    bool setHwBreakpoint(uintptr_t address, KT_HW_BP_TYPE type, KT_HW_BP_SIZE size, int slot);



    bool clearHwBreakpoint(KT_HW_BP_TYPE type, int slot);
};
