/**
 * @file
 * @author chu
 * @date 2018/9/7
 */
#pragma once
#include "Debugger.hpp"

namespace lperf
{
    enum class LuaFunctionType
    {
        Unknown,
        Native,
        Lua,
    };

    struct LuaStackFrame
    {
        LuaFunctionType Type = LuaFunctionType::Unknown;
        uintptr_t Address = 0;
        std::string Source;
        std::string Name;
        unsigned Line = 0;
    };

    /**
     * @brief LUA采样器
     *
     * TODO: 检查ABI。目前只支持了lua53。正确的做法是从drawf中读取struct定义。
     */
    class LuaSampler
    {
    public:
        LuaSampler(Debugger& dbg);

    public:
        /**
         * @brief 抓取lua_State的地址
         * @param customEntryPoints 自定义入口
         *
         * 原理上，通过Hook LUA的热点函数来抓取lua_State。
         * 因此，对于进程中具备多个lua_State或者lua_State被释放的情况不能被处理。
         */
        uintptr_t FetchLuaState(const std::vector<uintptr_t>& customEntryPoints);

        /**
         * @brief 导出LUA堆栈
         * @param address 指示lua_State对象的地址
         */
        std::vector<LuaStackFrame> DumpStack(uintptr_t address);

    private:
        Debugger& m_pDebugger;
    };
}
