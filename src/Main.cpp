#include "LuaSampler.hpp"

#include <iostream>
#include <Moe.Core/Logging.hpp>
#include <Moe.Core/CmdParser.hpp>

using namespace std;
using namespace moe;
using namespace lperf;

struct Config
{
    uint64_t Pid = 0;
    bool Verbose = false;

    uint32_t SampleInterval = 0;
    uint32_t SampleCount = 0;

    string HookEntry;
};

namespace
{
    string FormatStack(const LuaStackFrame& frame)
    {
        switch (frame.Type)
        {
            case LuaFunctionType::Native:
                if (frame.Name.empty())
                    return StringUtils::Format("[0x{0,16[0]:H}]", frame.Address);
                return StringUtils::Format("[{0}]", frame.Name);
            case LuaFunctionType::Lua:
                return StringUtils::Format("{0} @ {1}:{2}", frame.Name.empty() ? "?" : frame.Name, frame.Source,
                    frame.Line);
            case LuaFunctionType::Unknown:
            default:
                return "?";
        }
    }

    vector<uintptr_t> MakeCustomHookEntries(const std::string& val)
    {
        vector<string> container;
        StringUtils::Split(container, val, ',', StringUtils::SplitFlags::RemoveEmptyEntries);

        vector<uintptr_t> ret;
        for (const auto& i : container)
        {
            size_t processed = 0;
            auto v = Convert::ParseUInt(i.c_str(), i.size(), processed);
            if (processed < i.size())
                MOE_THROW(BadFormatException, "Invalid custom entry point: {0}", i);
            ret.push_back(v);
        }
        return ret;
    }

    void Process(const Config& cfg)
    {
        auto customEntryPoints = MakeCustomHookEntries(cfg.HookEntry);

        shared_ptr<Debugger> debugger = make_shared<Debugger>(cfg.Pid);
        LuaSampler sampler(*debugger.get());

        // 获取LuaState
        MOE_LOG_DEBUG("Fetching lua_State*");
        auto L = sampler.FetchLuaState(customEntryPoints);

        // 捕捉堆栈
        map<string, size_t> stackCounter;
        vector<LuaStackFrame> stacks;
        string formattedStack;
        formattedStack.reserve(1024);
        for (size_t i = 0; i < cfg.SampleCount; ++i)
        {
            this_thread::sleep_for(chrono::milliseconds(cfg.SampleInterval));

            MOE_LOG_DEBUG("Capturing lua stack {0}/{1}", i + 1, cfg.SampleCount);
            try
            {
                stacks.clear();
                stacks = sampler.DumpStack(L);
            }
            catch (const ExceptionBase& ex)
            {
                MOE_LOG_ERROR("Capture frame failure: {0}", ex.GetDescription());
                continue;
            }
            formattedStack.clear();
            formattedStack.append("(base);");
            for (auto it = stacks.rbegin(); it != stacks.rend(); ++it)
            {
                formattedStack.append(FormatStack(*it));
                formattedStack.push_back(';');
            }
            MOE_LOG_INFO("Captured stack: {0}", formattedStack);

            ++stackCounter[formattedStack];
        }

        // 打印结果
        for (const auto& it : stackCounter)
            cout << it.first << " " << it.second << endl;
    }

    Config GetCommandline(int argc, const char** argv)
    {
        Config cfg;
        bool needHelp = false;

        CmdParser parser;
        parser << CmdParser::Option(cfg.Pid, "pid", 'p', "Specific the process id");
        parser << CmdParser::Option(needHelp, "help", 'h', "Show this help", false);
        parser << CmdParser::Option(cfg.Verbose, "verbose", 'v', "Show debug log", false);
        parser << CmdParser::Option(cfg.SampleInterval, "interval", 'i', "Specific sample interval (ms)", 1000u);
        parser << CmdParser::Option(cfg.SampleCount, "count", 'c', "Specific sample count", 10u);
        parser << CmdParser::Option(cfg.HookEntry, "hook", 'k',
            "Specific custom hook entry address (must be a lua api), eg: -k 0x12FFBB0,12345678", string());

        try
        {
            parser(argc, argv);
        }
        catch (const ExceptionBase& ex)
        {
            fprintf(stderr, "%s\n\n", ex.GetDescription().c_str());
            needHelp = true;
        }

        if (needHelp)
        {
            auto name = PathUtils::GetFileName(argv[0]);
            auto nameStr = string(name.GetBuffer(), name.GetSize());

            fprintf(stderr, "%s\n", parser.BuildUsageText(nameStr.c_str()).c_str());
            fprintf(stderr, "%s\n", parser.BuildOptionsText(2, 10).c_str());
            exit(1);
        }
        return cfg;
    }
}

int main(int argc, const char** argv)
{
    // 解析命令行
    auto config = GetCommandline(argc, argv);

    // 初始化日志
    if (config.Verbose)
    {
        auto sink = make_shared<Logging::TerminalSink>();
        auto formatter = make_shared<Logging::AnsiColorFormatter>();
        sink->SetFormatter(formatter);
        Logging::GetInstance().AppendSink(sink);
    }
    else
    {
        auto errSink = make_shared<Logging::TerminalSink>(Logging::TerminalSink::OutputType::StdErr);
        auto errFormatter = make_shared<Logging::AnsiColorFormatter>();
        errFormatter->SetFormat("{level}: {msg}");
        errSink->SetFormatter(errFormatter);
        errSink->SetMinLevel(Logging::Level::Warn);
        Logging::GetInstance().AppendSink(errSink);
    }
    Logging::GetInstance().Commit();

    try
    {
        Process(config);
    }
    catch (const ExceptionBase& ex)
    {
        MOE_LOG_FATAL("{0}", ex.GetDescription());
        return 1;
    }
    catch (const exception& ex)
    {
        MOE_LOG_FATAL("{0}", ex.what());
        return 1;
    }
    return 0;
}
