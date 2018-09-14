/**
 * @file
 * @author chu
 * @date 2018/9/4
 * @see https://github.com/uber/pyflame
 * @see https://blog.tartanllama.xyz/writing-a-linux-debugger-breakpoints/
 * @see http://sigalrm.blogspot.com/2010/07/writing-minimal-debugger.html
 */
#include "Debugger.hpp"

#include <Moe.Core/Logging.hpp>

extern "C" {
#include <pmparser.h>
}

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>

using namespace std;
using namespace moe;
using namespace lperf;

//////////////////////////////////////////////////////////////////////////////// Breakpoint

Breakpoint::Breakpoint(Debugger& dbg, uintptr_t address)
    : m_pDebugger(dbg), m_uAddress(address)
{
}

Breakpoint::~Breakpoint()
{
    try
    {
        if (m_pDebugger.GetStatus() == ProcessStatus::Terminated)
            return;
        Disable();
    }
    catch (const ExceptionBase& ex)
    {
        MOE_LOG_ERROR("Cannot disable breakpoint, address {0}: {1}", m_uAddress, ex.GetDescription());
    }
    catch (const std::exception& ex)
    {
        MOE_LOG_ERROR("Cannot disable breakpoint, address {0}: {1}", m_uAddress, ex.what());
    }
    catch (...)
    {
        MOE_LOG_ERROR("Cannot disable breakpoint, address {0}: Unknown exception", m_uAddress);
    }
}

void Breakpoint::Enable()
{
    auto code = m_pDebugger.ReadByte(m_uAddress);
    if (m_bEnabled && code == 0xCC)
        return;

    m_pDebugger.WriteByte(m_uAddress, 0xCC);

    m_bEnabled = true;
    m_uOriginalByte = code;
    MOE_LOG_INFO("Breakpoint enabled, address {0}", m_uAddress);
}

void Breakpoint::Disable()
{
    if (!m_bEnabled)
        return;

    auto code = m_pDebugger.ReadByte(m_uAddress);
    if (code != 0xCC)
    {
        m_bEnabled = false;
        m_uOriginalByte = code;
        MOE_LOG_WARN("Code at breakpoint modified, address {0}", m_uAddress);
        return;
    }

    m_pDebugger.WriteByte(m_uAddress, m_uOriginalByte);
    m_bEnabled = false;
    MOE_LOG_INFO("Breakpoint disabled, address {0}", m_uAddress);
}

//////////////////////////////////////////////////////////////////////////////// Debugger

Debugger::Debugger(ProcessId pid, bool interrupt)
    : m_uPid(pid)
{
    // 打开可执行文件
    string path = StringUtils::Format("/proc/{0}/exe", pid);
    auto fd = open(path.c_str(), O_RDONLY);
    if (fd == -1)
        MOE_THROW(ApiException, "Open executable file \"{0}\" error, errno={1}({2})", path, errno, strerror(errno));
    
    m_stElfParser = elf::elf(elf::create_mmap_loader(fd));  // FIXME: leak fd?
    try
    {
        m_stDwarfParser = dwarf::dwarf(dwarf::elf::create_loader(m_stElfParser));
    }
    catch (const std::exception& ex)
    {
        MOE_LOG_WARN("Load dwarf error: {0}", ex.what());
        m_stDwarfParser = dwarf::dwarf();
    }

    if (m_stElfParser.get_hdr().type == elf::et::dyn)
        GetProcessBaseAddress();

    // 挂到进程上
    if (::ptrace(PTRACE_SEIZE, pid, 0, 0) != 0)  // PTRACE_ATTACH下不能用用INTERRUPT，这很蛋疼
        MOE_THROW(ApiException, "Attach to process {0} error, errno={1}({2})", pid, errno, strerror(errno));

    if (interrupt)
    {
        if (::ptrace(PTRACE_INTERRUPT, pid, 0, 0) != 0)
            MOE_THROW(ApiException, "Interrupt process {0} error, errno={1}({2})", m_uPid, errno, strerror(errno));

        int status = 0;
        if (::waitpid(static_cast<pid_t>(pid), &status, __WALL) != pid || !WIFSTOPPED(status))
        {
            int err = errno;
            ::ptrace(PTRACE_DETACH, pid, 0, 0);  // nothrow

            MOE_THROW(ApiException, "Attach and wait on process {0} error, errno={1}({2})", pid, err, strerror(err));
        }

        m_uStatus = ProcessStatus::Paused;
        m_iLastSignal = WSTOPSIG(status);
    }
    else
        m_uStatus = ProcessStatus::Running;
}

Debugger::~Debugger()
{
    if (m_uStatus == ProcessStatus::Terminated)
        return;

    if (m_uStatus == ProcessStatus::Running)  // 设置断点时必须先打断运行
    {
        MOE_LOG_TRACE("Debugger destroyed, pause process first");
        InterruptSafe();
    }

    MOE_LOG_TRACE("Cleanup breakpoints from process {0}, count {1}", m_uPid, m_stBreakpoints.size());
    m_stBreakpoints.clear();

    MOE_LOG_TRACE("Debugger destroyed, continue process");
    ContinueSafe();

    MOE_LOG_TRACE("Detach from process {0}", m_uPid);
    ::ptrace(PTRACE_DETACH, m_uPid, 0, 0);  // nothrow
}

bool Debugger::Wait()
{
    if (m_uStatus == ProcessStatus::Terminated)
        MOE_THROW(InvalidCallException, "Process {0} already terminated", m_uPid);

    int status = 0;
    while (true)
    {
        auto progeny = ::waitpid(static_cast<pid_t>(m_uPid), &status, 0);
        if (progeny < 0)
        {
            if (errno == EINTR)
            {
                MOE_LOG_DEBUG("waitpid received EINTR on process {0}", m_uPid);
                continue;
            }
            MOE_THROW(ApiException, "Wait on process {0} error, errno={1}({2})", m_uPid, errno, strerror(errno));
        }

        assert(progeny == m_uPid);
        if (WIFSTOPPED(status))
        {
            m_uStatus = ProcessStatus::Paused;
            m_iLastSignal = WSTOPSIG(status);
            MOE_LOG_TRACE("Process {0} stopped on signal {1}", m_uPid, m_iLastSignal);
            
            if (m_iLastSignal == SIGCHLD)
            {
                Continue();
                continue;
            }
            else
                break;
        }
        else if (WIFEXITED(status))
        {
            m_uStatus = ProcessStatus::Terminated;
            m_iLastSignal = WTERMSIG(status);
            m_iExitCode = WEXITSTATUS(status);
            MOE_LOG_TRACE("Process {0} terminated", m_uPid);

            return false;
        }
        else
        {
            MOE_THROW(ApiException, "Wait on process {0} got unexpected code {1}, errno={2}({3})", m_uPid, status,
                errno, strerror(errno));
        }
    }
    return true;
}

void Debugger::Interrupt()
{
    if (::ptrace(PTRACE_INTERRUPT, m_uPid, 0, 0) != 0)
        MOE_THROW(ApiException, "Interrupt process {0} error, errno={1}({2})", m_uPid, errno, strerror(errno));
    if (!Wait())
        MOE_THROW(InvalidCallException, "Process {0} terminated on interrupt", m_uPid);
}

void Debugger::InterruptSafe()noexcept
{
    try
    {
        Interrupt();
    }
    catch (const ExceptionBase& ex)
    {
        MOE_LOG_EXCEPTION(ex);
    }
    catch (const std::exception& ex)
    {
        MOE_LOG_ERROR("{0}", ex.what());
    }
}

void Debugger::Continue()
{
    if (m_uStatus != ProcessStatus::Paused)
        MOE_THROW(InvalidCallException, "Invalid call on process {0}", m_uPid);

    if (m_iLastSignal == SIGTRAP)
        StepOverBreakpoint();

    if (::ptrace(PTRACE_CONT, m_uPid, 0, 0) != 0)
        MOE_THROW(ApiException, "Continue on process {0} error, errno={1}({2})", m_uPid, errno, strerror(errno));
    m_uStatus = ProcessStatus::Running;
    m_iLastSignal = 0;
}

void Debugger::ContinueSafe()noexcept
{
    try
    {
        Continue();
    }
    catch (const ExceptionBase& ex)
    {
        MOE_LOG_EXCEPTION(ex);
    }
    catch (const std::exception& ex)
    {
        MOE_LOG_ERROR("{0}", ex.what());
    }
}

void Debugger::SingleStep()
{
    if (m_uStatus != ProcessStatus::Paused)
        MOE_THROW(InvalidCallException, "Invalid call on process {0}", m_uPid);

    if (m_iLastSignal == SIGTRAP)
    {
        if (StepOverBreakpoint())
            return;
    }
    InternalStepOver();
}

Word Debugger::GetRegister(Registers reg)
{
    if (m_uStatus != ProcessStatus::Paused)
        MOE_THROW(InvalidCallException, "Invalid call on process {0}", m_uPid);

    ::user_regs_struct ret {};
    memset(&ret, 0, sizeof(ret));
    if (::ptrace(PTRACE_GETREGS, m_uPid, 0, &ret) != 0)
        MOE_THROW(ApiException, "Get register of process {0} error, errno={1}({2})", m_uPid, errno, strerror(errno));
    
    switch (reg)
    {
        case Registers::RAX:
            return ret.rax;
        case Registers::RBX:
            return ret.rbx;
        case Registers::RCX:
            return ret.rcx;
        case Registers::RDX:
            return ret.rdx;
        case Registers::RDI:
            return ret.rdi;
        case Registers::RSI:
            return ret.rsi;
        case Registers::RBP:
            return ret.rbp;
        case Registers::RSP:
            return ret.rsp;
        case Registers::R8:
            return ret.r8;
        case Registers::R9:
            return ret.r9;
        case Registers::R10:
            return ret.r10;
        case Registers::R11:
            return ret.r11;
        case Registers::R12:
            return ret.r12;
        case Registers::R13:
            return ret.r13;
        case Registers::R14:
            return ret.r14;
        case Registers::R15:
            return ret.r15;
        case Registers::RIP:
            return ret.rip;
        case Registers::EFLAGS:
            return ret.eflags;
        case Registers::CS:
            return ret.cs;
        case Registers::ORIG_RAX:
            return ret.orig_rax;
        case Registers::FS_BASE:
            return ret.fs_base;
        case Registers::GS_BASE:
            return ret.gs_base;
        case Registers::FS:
            return ret.fs;
        case Registers::GS:
            return ret.gs;
        case Registers::SS:
            return ret.ss;
        case Registers::DS:
            return ret.ds;
        case Registers::ES:
            return ret.es;
        default:
            assert(false);
            return 0;
    }
}

void Debugger::SetRegister(Registers reg, Word val)
{
    if (m_uStatus != ProcessStatus::Paused)
        MOE_THROW(InvalidCallException, "Invalid call on process {0}", m_uPid);

    ::user_regs_struct regs {};
    memset(&regs, 0, sizeof(regs));
    if (::ptrace(PTRACE_GETREGS, m_uPid, 0, &regs) != 0)
        MOE_THROW(ApiException, "Get register of process {0} error, errno={1}({2})", m_uPid, errno, strerror(errno));
    
    switch (reg)
    {
        case Registers::RAX:
            regs.rax = val;
            break;
        case Registers::RBX:
            regs.rbx = val;
            break;
        case Registers::RCX:
            regs.rcx = val;
            break;
        case Registers::RDX:
            regs.rdx = val;
            break;
        case Registers::RDI:
            regs.rdi = val;
            break;
        case Registers::RSI:
            regs.rsi = val;
            break;
        case Registers::RBP:
            regs.rbp = val;
            break;
        case Registers::RSP:
            regs.rsp = val;
            break;
        case Registers::R8:
            regs.r8 = val;
            break;
        case Registers::R9:
            regs.r9 = val;
            break;
        case Registers::R10:
            regs.r10 = val;
            break;
        case Registers::R11:
            regs.r11 = val;
            break;
        case Registers::R12:
            regs.r12 = val;
            break;
        case Registers::R13:
            regs.r13 = val;
            break;
        case Registers::R14:
            regs.r14 = val;
            break;
        case Registers::R15:
            regs.r15 = val;
            break;
        case Registers::RIP:
            regs.rip = val;
            break;
        case Registers::EFLAGS:
            regs.eflags = val;
            break;
        case Registers::CS:
            regs.cs = val;
            break;
        case Registers::ORIG_RAX:
            regs.orig_rax = val;
            break;
        case Registers::FS_BASE:
            regs.fs_base = val;
            break;
        case Registers::GS_BASE:
            regs.gs_base = val;
            break;
        case Registers::FS:
            regs.fs = val;
            break;
        case Registers::GS:
            regs.gs = val;
            break;
        case Registers::SS:
            regs.ss = val;
            break;
        case Registers::DS:
            regs.ds = val;
            break;
        case Registers::ES:
            regs.es = val;
            break;
        default:
            assert(false);
            return;
    }
    
    if (::ptrace(PTRACE_SETREGS, m_uPid, 0, &regs) != 0)
        MOE_THROW(ApiException, "Set register of process {0} error, errno={1}({2})", m_uPid, errno, strerror(errno));
}

Word Debugger::Read(uintptr_t address)
{
    if (m_uStatus != ProcessStatus::Paused)
        MOE_THROW(InvalidCallException, "Invalid call on process {0}", m_uPid);

    errno = 0;
    auto data = static_cast<Word>(::ptrace(PTRACE_PEEKDATA, m_uPid, address, 0));
    if (data == static_cast<Word>(-1) && errno != 0)
    {
        MOE_THROW(ApiException, "Read data on process {0} error, address={1}, errno={2}({3})", m_uPid, address,
            errno, strerror(errno));
    }
    return data;
}

uint8_t Debugger::ReadByte(uintptr_t address)
{
    auto data = Read(address);
    return reinterpret_cast<const uint8_t*>(&data)[0];
}

std::string Debugger::ReadString(uintptr_t address, size_t maxlen)
{
    string ret;
    ret.reserve(128);

    size_t offset = 0;
    while (offset < maxlen)
    {
        auto val = Read(address + offset);
        auto b = reinterpret_cast<const uint8_t*>(&val);
        for (auto i = 0; i < sizeof(val); ++i)
        {
            auto ch = b[i];
            if (ch == 0)
                return ret;
            ret.push_back(static_cast<char>(ch));
        }
        offset += sizeof(val);
    }

    if (ret.size() > maxlen)
        ret.resize(maxlen);
    return ret;
}

size_t Debugger::ReadBytes(uintptr_t address, uint8_t buffer[], size_t count)
{
    // 需要对齐到字长
    if (count % sizeof(Word) != 0)
        count -= count % sizeof(Word);

    size_t offset = 0;
    while (offset < count)
    {
        Word val = Read(address + offset);
        memcpy(buffer + offset, &val, sizeof(val));
        offset += sizeof(Word);
    }
    return count;
}

void Debugger::Write(uintptr_t address, Word data)
{
    if (m_uStatus != ProcessStatus::Paused)
        MOE_THROW(InvalidCallException, "Invalid call on process {0}", m_uPid);

    if (::ptrace(PTRACE_POKEDATA, m_uPid, address, reinterpret_cast<void*>(data)) != 0)
    {
        MOE_THROW(ApiException, "Poke data on process {0} error, address={1}, data={2}, errno={3}({4})", m_uPid,
            address, data, errno, strerror(errno));
    }
}

void Debugger::WriteByte(uintptr_t address, uint8_t data)
{
    auto val = Read(address);
    auto b = reinterpret_cast<uint8_t*>(&val);
    b[0] = data;
    Write(address, val);
}

void Debugger::SendSignal(int signum)
{
    if (m_uStatus == ProcessStatus::Terminated)
        MOE_THROW(InvalidCallException, "Invalid call on process {0}", m_uPid);

    if (kill(m_uPid, signum) != 0)
    {
        MOE_THROW(ApiException, "Send signal to process {0} failed, errno={1}({2})", m_uPid, errno,
            strerror(errno));
    }
}

Breakpoint* Debugger::CreateBreakpoint(uintptr_t address)
{
    auto it = m_stBreakpoints.find(address);
    if (it != m_stBreakpoints.end())
        return it->second.get();

    unique_ptr<Breakpoint> p;
    p.reset(new Breakpoint(*this, address));
    auto ret = p.get();
    m_stBreakpoints.emplace(address, std::move(p));
    return ret;
}

Breakpoint* Debugger::CreateBreakpoint(const char* func, bool skipPrologue)
{
    for (const auto& cu : m_stDwarfParser.compilation_units())
    {
        for (const auto& die : cu.root())
        {
            if (die.has(dwarf::DW_AT::name) && dwarf::at_name(die) == func)
            {
                auto pc = at_low_pc(die);
                auto entry = GetLineEntryFromPC(pc);
                if (skipPrologue)
                    ++entry;  // skip prologue
                return CreateBreakpoint(entry->address + m_uAddressOffset);
            }
        }
    }
    MOE_THROW(ObjectNotFoundException, "Function {0} not found", func);
}

Breakpoint* Debugger::GetBreakpoint(uintptr_t address)
{
    auto it = m_stBreakpoints.find(address);
    if (it != m_stBreakpoints.end())
        return it->second.get();
    return nullptr;
}

Breakpoint* Debugger::IsHitBreakpoint()
{
    auto lastLocation = GetPC() - 1;
    return GetBreakpoint(lastLocation);
}

void Debugger::RemoveBreakpoint(Breakpoint* breakpoint)
{
    assert(breakpoint);
    auto it = m_stBreakpoints.find(breakpoint->GetAddress());
    if (it == m_stBreakpoints.end())
        return;
    m_stBreakpoints.erase(it);
}

const std::string& Debugger::GetFunctionName(uintptr_t address)
{
    address -= GetAddressOffset();

    auto it = m_stSymbolCacheMap.find(address);
    if (it != m_stSymbolCacheMap.end())
        return it->second;

    for (auto& cu : m_stDwarfParser.compilation_units())
    {
        if (!cu.root().has(dwarf::DW_AT::low_pc))
            continue;

        if (dwarf::die_pc_range(cu.root()).contains(address))
        {
            for (const auto& die : cu.root())
            {
                if (die.has(dwarf::DW_AT::low_pc) && die.has(dwarf::DW_AT::name))
                {
                    if (dwarf::die_pc_range(die).contains(address))
                    {
                        auto name = dwarf::at_name(die);
                        m_stSymbolCacheMap.emplace(address, std::move(name));
                        return m_stSymbolCacheMap[address];
                    }
                }
            }
        }
    }

    m_stSymbolCacheMap.emplace(address, string());
    return m_stSymbolCacheMap[address];
}

void Debugger::GetProcessBaseAddress()
{
    string path = StringUtils::Format("/proc/{0}/exe", m_uPid);
    static thread_local char real[PATH_MAX];
    memset(real, 0, sizeof(real));
    if (!realpath(path.c_str(), real))
        MOE_THROW(ApiException, "Cannot get real path of process {0}", m_uPid);

    procmaps_struct* maps = pmparser_parse(m_uPid);
    if (!maps)
        MOE_THROW(ApiException, "Cannot parse memory map of process {0}", m_uPid);

    procmaps_struct* p = nullptr;
    while ((p = pmparser_next()) != nullptr)
    {
        if (p->is_x && strcmp(real, p->pathname) == 0)  // FIXME: p->pathname can only contain 600 bytes
        {
            m_uAddressOffset = reinterpret_cast<uintptr_t>(p->addr_start);
            break;
        }
    }

    pmparser_free(maps);

    if (m_uAddressOffset == 0)
        MOE_THROW(ApiException, "Cannot get base address of process {0}", m_uPid);
}

void Debugger::InternalStepOver()
{
    if (::ptrace(PTRACE_SINGLESTEP, m_uPid, 0, 0) != 0)
    {
        MOE_THROW(ApiException, "Single step on process {0} error, errno={1}({2})", m_uPid, errno,
            strerror(errno));
    }
    Wait();
}

bool Debugger::StepOverBreakpoint()
{
    auto lastLocation = GetPC() - 1;
    auto p = GetBreakpoint(lastLocation);
    if (p && p->IsEnabled())
    {
        SetPC(lastLocation);

        p->Disable();
        InternalStepOver();
        p->Enable();
        return true;
    }
    return false;
}

dwarf::line_table::iterator Debugger::GetLineEntryFromPC(uint64_t pc)
{
    for (const auto& cu : m_stDwarfParser.compilation_units())
    {
        if (dwarf::die_pc_range(cu.root()).contains(pc))
        {
            const auto& lt = cu.get_line_table();
            const auto it = lt.find_address(pc);
            if (it == lt.end())
                MOE_THROW(ObjectNotFoundException, "Cannot find line entry");
            else
                return it;
        }
    }
    MOE_THROW(ObjectNotFoundException, "Cannot find line entry");
}
