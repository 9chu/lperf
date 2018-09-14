/**
 * @file
 * @author chu
 * @date 2018/9/7
 */
#include "LuaSampler.hpp"
#include "RemoteLuaWrapper.hpp"

#include <csignal>
#include <functional>

#include <Moe.Core/Logging.hpp>

using namespace std;
using namespace moe;
using namespace lperf;

class ProcessPauseScope
{
public:
    ProcessPauseScope(Debugger& dbg)
        : m_pDebugger(dbg)
    {
        if (m_pDebugger.GetStatus() == ProcessStatus::Running)
            m_pDebugger.Interrupt();

        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
    }

    ~ProcessPauseScope()
    {
        if (m_pDebugger.GetStatus() == ProcessStatus::Paused)
            m_pDebugger.ContinueSafe();

        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
    }

private:
    Debugger& m_pDebugger;
};

class ProcessWatchScope
{
private:
    static Debugger*& GetDebuggerInstance()noexcept
    {
        static Debugger* s_pInstance = nullptr;
        return s_pInstance;
    }

    static void OnSignal(int)
    {
        if (!GetDebuggerInstance())
            return;

        auto& debugger = *GetDebuggerInstance();
        if (debugger.GetStatus() == ProcessStatus::Running)
        {
            try
            {
                debugger.SendSignal(SIGINT);
            }
            catch (const ExceptionBase& ex)
            {
                MOE_LOG_ERROR("Cannot send signal to process: {0}", ex.GetDescription());
            }
        }
    }

public:
    ProcessWatchScope(Debugger& dbg)
    {
        assert(!GetDebuggerInstance());
        GetDebuggerInstance() = &dbg;

        signal(SIGINT, OnSignal);
        signal(SIGTERM, OnSignal);
        signal(SIGHUP, OnSignal);
    }

    ~ProcessWatchScope()
    {
        GetDebuggerInstance() = nullptr;

        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
    }
};

class LuaStateFetcher
{
public:
    LuaStateFetcher(Debugger& dbg)
        : m_pDebugger(dbg) {}

    ~LuaStateFetcher()
    {
        ProcessPauseScope scope(m_pDebugger);

        try
        {
            for (auto p : m_stBreakpoints)
                m_pDebugger.RemoveBreakpoint(p);
            m_stBreakpoints.clear();
        }
        catch (const ExceptionBase& ex)
        {
            MOE_LOG_ERROR("Cannot clear hooks on process: {0}", ex.GetDescription());
        }
        catch (const std::exception& ex)
        {
            MOE_LOG_ERROR("Cannot clear hooks on process: {0}", ex.what());
        }
    }

    bool Hook(uintptr_t func)
    {
        Breakpoint* p = nullptr;
        try
        {
            MOE_LOG_INFO("Hook lua function 0x{0,16[0]:H}", func);

            p = m_pDebugger.CreateBreakpoint(func + m_pDebugger.GetAddressOffset());
            p->Enable();
            m_stBreakpoints.push_back(p);
        }
        catch (const ExceptionBase& ex)
        {
            MOE_LOG_ERROR("Hook function 0x{0,16[0]} failed: {1}", func, ex.GetDescription());

            if (p)
                m_pDebugger.RemoveBreakpoint(p);
            return false;
        }
        return true;
    }

    bool Hook(const char* func)
    {
        Breakpoint* p = nullptr;
        try
        {
            MOE_LOG_INFO("Hook lua function {0}", func);

            p = m_pDebugger.CreateBreakpoint(func, false);
            p->Enable();
            m_stBreakpoints.push_back(p);
        }
        catch (const ExceptionBase& ex)
        {
            MOE_LOG_ERROR("Hook function {0} failed: {1}", func, ex.GetDescription());

            if (p)
                m_pDebugger.RemoveBreakpoint(p);
            return false;
        }
        return true;
    }

    bool IsHitHook(Breakpoint* p)
    {
        return std::find(m_stBreakpoints.begin(), m_stBreakpoints.end(), p) != m_stBreakpoints.end();
    }

    size_t GetHookCount()
    {
        return m_stBreakpoints.size();
    }

private:
    Debugger& m_pDebugger;
    std::vector<Breakpoint*> m_stBreakpoints;
};

class MemoryAccessor :
    public MemoryAccessorBase<>
{
public:
    MemoryAccessor(Debugger& dbg)
        : m_pDebugger(dbg) {}

public:
    void Read(uintptr_t address, moe::MutableBytesView output)
    {
        assert(address % sizeof(Word) == 0);
        assert(output.GetSize() % sizeof(Word) == 0);
        auto sz = m_pDebugger.ReadBytes(address, output.GetBuffer(), output.GetSize());
        assert(sz == output.GetSize());
    }

    std::string ReadString(uintptr_t address, size_t maxlen)
    {
        return m_pDebugger.ReadString(address, maxlen);
    }

private:
    Debugger& m_pDebugger;
};

class MemoryAccessorScope
{
public:
    MemoryAccessorScope(MemoryAccessorPtr p)
    {
        SetGlobalMemoryAccessor(p);
    }

    ~MemoryAccessorScope()
    {
        SetGlobalMemoryAccessor(nullptr);
    }
};

//////////////////////////////////////////////////////////////////////////////// LuaSampler

LuaSampler::LuaSampler(Debugger& dbg)
    : m_pDebugger(dbg)
{
}

uintptr_t LuaSampler::FetchLuaState(const std::vector<uintptr_t>& customEntryPoints)
{
    assert(m_pDebugger.GetStatus() == ProcessStatus::Running);

    LuaStateFetcher fetcher(m_pDebugger);

    {
        ProcessPauseScope scope(m_pDebugger);
        fetcher.Hook("lua_callk");  // LUA53下lua_call为lua_callk的宏，下同
        fetcher.Hook("lua_pcallk");

        for (auto i : customEntryPoints)
            fetcher.Hook(i);
    }

    if (fetcher.GetHookCount() == 0)
        MOE_THROW(OperationNotSupportException, "No hook could be inserted");

    {
        ProcessWatchScope scope(m_pDebugger);
        while (m_pDebugger.Wait())
        {
            if (m_pDebugger.GetLastSignal() == SIGINT)
            {
                MOE_LOG_ERROR("Debugger interrupt by SIGINT, cancel");
                MOE_THROW(OperationCancelledException, "User cancelled");
            }
            else if (m_pDebugger.GetLastSignal() == SIGTRAP)
            {
                auto p = m_pDebugger.IsHitBreakpoint();
                if (fetcher.IsHitHook(p))
                {
                    auto L = m_pDebugger.GetRegister(Registers::RDI);  // lua_State总是第一个参数，因此总是放在RDI里面
                    m_pDebugger.Continue();  // 恢复程序执行
                    return L;
                }
            }
            else
                MOE_THROW(OperationNotSupportException, "Unknown signal {0} actived", m_pDebugger.GetLastSignal());

            m_pDebugger.Continue();
        }
        MOE_THROW(InvalidCallException, "Target terminated");
    }
}

std::vector<LuaStackFrame> LuaSampler::DumpStack(uintptr_t address)
{
    ProcessPauseScope scope(m_pDebugger);

    auto accessor = static_pointer_cast<MemoryAccessorBase<>>(make_shared<MemoryAccessor>(m_pDebugger));
    MemoryAccessorScope memScope(accessor);

    vector<LuaStackFrame> ret;

    // 遍历LUA堆栈
    RemotePtr<LuaObjects::lua_State> luaStatePtr { reinterpret_cast<LuaObjects::lua_State*>(address) };
    auto luaState(*luaStatePtr);
    auto callInfoPtr = luaState.ci;
    while (callInfoPtr && callInfoPtr != address + offsetof(LuaObjects::lua_State, base_ci))
    {
        LuaObjects::lua_Debug debug {};
        debug.i_ci = callInfoPtr;
        luaState.GetInfo("nSlt", debug);

        LuaStackFrame frame;
        frame.Source = debug.short_src;
        frame.Line = debug.linedefined == -1 ? 0u : static_cast<unsigned>(debug.linedefined);
        frame.Address = debug.address;
        frame.Name = debug.name;
        if (strcmp(debug.what, "C") == 0)
        {
            frame.Type = LuaFunctionType::Native;
            if (frame.Address)
            {
                auto& name = m_pDebugger.GetFunctionName(frame.Address);
                frame.Name = name;
            }
        }
        else
            frame.Type = LuaFunctionType::Lua;

        ret.emplace_back(std::move(frame));
        callInfoPtr = (*callInfoPtr).previous;
    }

    return ret;
}
