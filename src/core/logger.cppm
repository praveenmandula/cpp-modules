module;

#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <utility>
#include <ctime>

export module cppm.core.logger;

export namespace logger
{
enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
    Off
};

//using Level = LogLevel;
//inline constexpr Level Trace = Level::Trace;
//inline constexpr Level Debug = Level::Debug;
//inline constexpr Level Info = Level::Info;
//inline constexpr Level Warning = Level::Warning;
//inline constexpr Level Error = Level::Error;
//inline constexpr Level Fatal = Level::Fatal;
//inline constexpr Level Off = Level::Off;

class Logger
{
public:

    static Logger& instance()
    {
        static Logger logger;
        return logger;
    }

    void setLevel(LogLevel level)
    {
        mLevel = level;
    }

    template<typename... Args>
    void log(LogLevel level, Args&&... args)
    {
        write(level, std::forward<Args>(args)...);
    }

private:

    struct LogRecord
    {
        LogLevel level;
        std::string message;
        std::chrono::system_clock::time_point timestamp;
    };

    Logger() = default;

    void appendPart(std::ostringstream& stream, std::string_view value)
    {
        stream << value;
    }

    void appendPart(std::ostringstream& stream, const char* value)
    {
        stream << (value != nullptr ? value : "<null>");
    }

    void appendPart(std::ostringstream& stream, char* value)
    {
        stream << (value != nullptr ? value : const_cast<char*>("<null>"));
    }

    template<std::size_t N>
    void appendPart(std::ostringstream& stream, const char (&value)[N])
    {
        stream << std::string_view(value, N - 1);
    }

    template<std::size_t N>
    void appendPart(std::ostringstream& stream, char (&value)[N])
    {
        stream << std::string_view(value, N - 1);
    }

    template<typename T>
    void appendPart(std::ostringstream& stream, T&& value)
    {
        stream << std::forward<T>(value);
    }

    template<typename... Args>
    void write(LogLevel level,
        Args&&... args)
    {
        if (level < mLevel)
            return;

        std::ostringstream msg;
        (appendPart(msg, std::forward<Args>(args)), ...);

        LogRecord record
        {
            level,
            msg.str(),
            std::chrono::system_clock::now()
        };

        output(record);
    }

    void output(const LogRecord& r)
    {
        std::lock_guard lock(mMutex);

        auto tt = std::chrono::system_clock::to_time_t(r.timestamp);
        std::tm localTm{};

#if defined(_WIN32)
        localtime_s(&localTm, &tt);
#else
        localtime_r(&tt, &localTm);
#endif

        std::cout
            << "[" << std::put_time(&localTm, "%H:%M:%S") << "] "
            << "[" << toString(r.level) << "] "
            << r.message << "\n";
    }

    constexpr std::string_view toString(LogLevel level) const
    {
        switch (level)
        {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "";
        }
    }

private:

    LogLevel mLevel = LogLevel::Info;
    std::mutex mMutex;
};

inline void setLevel(LogLevel level)
{
    Logger::instance().setLevel(level);
}

template<typename... Args>
void log(LogLevel level, Args&&... args)
{
    Logger::instance().log(level, std::forward<Args>(args)...);
}
}