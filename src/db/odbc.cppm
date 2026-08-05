module;

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "odbc32.lib")
#pragma comment(lib, "odbccp32.lib")
#endif
#endif

#include <sql.h>
#include <sqlext.h>

export module cppm.db.odbc;

export namespace odbc
{
struct QueryRow
{
    std::unordered_map<std::string, std::string> values;

    [[nodiscard]] std::string get(std::string_view column, std::string defaultValue = "") const
    {
        auto found = values.find(std::string(column));
        if (found == values.end())
            return std::move(defaultValue);
        return found->second;
    }
};

struct QueryResult
{
    std::vector<std::string> columns;
    std::vector<QueryRow> rows;
    std::int64_t affectedRows = 0;
};

class Connection
{
public:
    Connection()
    {
        if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &mEnv) != SQL_SUCCESS)
        {
            mLastError = "Failed to allocate ODBC environment";
            return;
        }

        if (SQLSetEnvAttr(mEnv, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) != SQL_SUCCESS)
        {
            mLastError = "Failed to set ODBC version";
            cleanup();
            return;
        }

        if (SQLAllocHandle(SQL_HANDLE_DBC, mEnv, &mDbc) != SQL_SUCCESS)
        {
            mLastError = "Failed to allocate ODBC connection handle";
            cleanup();
            return;
        }

        mReady = true;
    }

    ~Connection()
    {
        disconnect();
        cleanup();
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept
    {
        moveFrom(std::move(other));
    }

    Connection& operator=(Connection&& other) noexcept
    {
        if (this != &other)
        {
            disconnect();
            cleanup();
            moveFrom(std::move(other));
        }
        return *this;
    }

    [[nodiscard]] bool connect(std::string_view connectionString)
    {
        mLastError.clear();

        if (!mReady)
        {
            if (mLastError.empty())
                mLastError = "Connection object is not initialized";
            return false;
        }

        disconnect();

        std::string connIn(connectionString);
        SQLCHAR outConn[1024]{};
        SQLSMALLINT outLen = 0;

        const SQLRETURN rc = SQLDriverConnectA(
            mDbc,
            nullptr,
            reinterpret_cast<SQLCHAR*>(connIn.data()),
            SQL_NTS,
            outConn,
            static_cast<SQLSMALLINT>(sizeof(outConn)),
            &outLen,
            SQL_DRIVER_NOPROMPT);

        if (!succeeded(rc))
        {
            mLastError = collectError(SQL_HANDLE_DBC, mDbc);
            return false;
        }

        mConnected = true;
        return true;
    }

    void disconnect()
    {
        if (mConnected && mDbc != SQL_NULL_HDBC)
        {
            SQLDisconnect(mDbc);
            mConnected = false;
        }
    }

    [[nodiscard]] bool isConnected() const
    {
        return mConnected;
    }

    [[nodiscard]] const std::string& lastError() const
    {
        return mLastError;
    }

    [[nodiscard]] bool execute(std::string_view sql)
    {
        mLastError.clear();
        QueryResult ignored;
        return run(sql, false, ignored);
    }

    [[nodiscard]] QueryResult query(std::string_view sql)
    {
        mLastError.clear();
        QueryResult result;
        if (!run(sql, true, result))
            return {};
        return result;
    }

private:
    static bool succeeded(SQLRETURN rc)
    {
        return rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
    }

    static std::string trimRight(std::string text)
    {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
            text.pop_back();
        return text;
    }

    static std::string collectError(SQLSMALLINT handleType, SQLHANDLE handle)
    {
        if (handle == SQL_NULL_HANDLE)
            return "ODBC operation failed";

        std::string combined;
        SQLCHAR state[6]{};
        SQLCHAR message[1024]{};
        SQLINTEGER nativeError = 0;
        SQLSMALLINT textLen = 0;

        for (SQLSMALLINT i = 1;; ++i)
        {
            const SQLRETURN rc = SQLGetDiagRecA(
                handleType,
                handle,
                i,
                state,
                &nativeError,
                message,
                static_cast<SQLSMALLINT>(sizeof(message)),
                &textLen);

            if (rc == SQL_NO_DATA)
                break;

            if (!succeeded(rc))
                break;

            if (!combined.empty())
                combined += " | ";

            combined += "[";
            combined += reinterpret_cast<const char*>(state);
            combined += "] ";
            combined += trimRight(std::string(reinterpret_cast<const char*>(message), static_cast<std::size_t>(textLen)));
        }

        if (combined.empty())
            return "ODBC operation failed";

        return combined;
    }

    [[nodiscard]] bool run(std::string_view sql, bool expectRows, QueryResult& outResult)
    {
        if (!mConnected)
        {
            mLastError = "Not connected";
            return false;
        }

        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (SQLAllocHandle(SQL_HANDLE_STMT, mDbc, &stmt) != SQL_SUCCESS)
        {
            mLastError = "Failed to allocate ODBC statement handle";
            return false;
        }

        auto stmtGuard = [&]() {
            if (stmt != SQL_NULL_HSTMT)
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        };

        std::string sqlText(sql);
        const SQLRETURN execRc = SQLExecDirectA(stmt, reinterpret_cast<SQLCHAR*>(sqlText.data()), SQL_NTS);
        if (!succeeded(execRc))
        {
            mLastError = collectError(SQL_HANDLE_STMT, stmt);
            stmtGuard();
            return false;
        }

        SQLLEN changed = 0;
        if (SQLRowCount(stmt, &changed) == SQL_SUCCESS)
            outResult.affectedRows = static_cast<std::int64_t>(changed);

        SQLSMALLINT columnCount = 0;
        if (SQLNumResultCols(stmt, &columnCount) != SQL_SUCCESS || columnCount <= 0)
        {
            stmtGuard();
            return true;
        }

        outResult.columns.reserve(static_cast<std::size_t>(columnCount));
        for (SQLSMALLINT col = 1; col <= columnCount; ++col)
        {
            SQLCHAR nameBuf[256]{};
            SQLSMALLINT nameLen = 0;
            SQLDescribeColA(stmt, col, nameBuf, static_cast<SQLSMALLINT>(sizeof(nameBuf)), &nameLen, nullptr, nullptr, nullptr, nullptr);
            outResult.columns.emplace_back(reinterpret_cast<const char*>(nameBuf), static_cast<std::size_t>(nameLen));
        }

        if (!expectRows)
        {
            stmtGuard();
            return true;
        }

        while (true)
        {
            const SQLRETURN fetchRc = SQLFetch(stmt);
            if (fetchRc == SQL_NO_DATA)
                break;

            if (!succeeded(fetchRc))
            {
                mLastError = collectError(SQL_HANDLE_STMT, stmt);
                stmtGuard();
                return false;
            }

            QueryRow row;

            for (SQLSMALLINT col = 1; col <= columnCount; ++col)
            {
                std::string value;
                bool isNull = false;

                while (true)
                {
                    char chunk[1024]{};
                    SQLLEN indicator = 0;

                    const SQLRETURN getRc = SQLGetData(
                        stmt,
                        col,
                        SQL_C_CHAR,
                        chunk,
                        static_cast<SQLLEN>(sizeof(chunk)),
                        &indicator);

                    if (getRc == SQL_NO_DATA)
                        break;

                    if (indicator == SQL_NULL_DATA)
                    {
                        isNull = true;
                        value.clear();
                        break;
                    }

                    if (!(getRc == SQL_SUCCESS || getRc == SQL_SUCCESS_WITH_INFO))
                    {
                        mLastError = collectError(SQL_HANDLE_STMT, stmt);
                        stmtGuard();
                        return false;
                    }

                    if (indicator >= 0)
                    {
                        const std::size_t totalLen = static_cast<std::size_t>(indicator);
                        const std::size_t appendLen = std::min(totalLen > value.size() ? totalLen - value.size() : static_cast<std::size_t>(0), sizeof(chunk) - 1);
                        value.append(chunk, appendLen);
                    }
                    else
                    {
                        value.append(chunk);
                    }

                    if (getRc == SQL_SUCCESS)
                        break;
                }

                row.values[outResult.columns[static_cast<std::size_t>(col - 1)]] = isNull ? std::string() : value;
            }

            outResult.rows.emplace_back(std::move(row));
        }

        stmtGuard();
        return true;
    }

    void cleanup()
    {
        if (mDbc != SQL_NULL_HDBC)
        {
            SQLFreeHandle(SQL_HANDLE_DBC, mDbc);
            mDbc = SQL_NULL_HDBC;
        }

        if (mEnv != SQL_NULL_HENV)
        {
            SQLFreeHandle(SQL_HANDLE_ENV, mEnv);
            mEnv = SQL_NULL_HENV;
        }

        mReady = false;
        mConnected = false;
    }

    void moveFrom(Connection&& other) noexcept
    {
        mEnv = other.mEnv;
        mDbc = other.mDbc;
        mReady = other.mReady;
        mConnected = other.mConnected;
        mLastError = std::move(other.mLastError);

        other.mEnv = SQL_NULL_HENV;
        other.mDbc = SQL_NULL_HDBC;
        other.mReady = false;
        other.mConnected = false;
    }

private:
    SQLHENV mEnv = SQL_NULL_HENV;
    SQLHDBC mDbc = SQL_NULL_HDBC;
    bool mReady = false;
    bool mConnected = false;
    std::string mLastError;
};

inline Connection connect(std::string_view connectionString)
{
    Connection conn;
    [[maybe_unused]] const bool connected = conn.connect(connectionString);
    return conn;
}

}
