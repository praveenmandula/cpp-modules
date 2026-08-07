module;

#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#endif
#else
#include <netdb.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

export module cppm.net.http;

export namespace http
{
enum class HttpScheme
{
    Http,
    Https
};

enum class HttpMethod
{
    Get,
    Post,
    Put,
    Delete_,
    Patch,
    Head,
    Options
};

struct HttpEndpoint
{
    HttpScheme scheme = HttpScheme::Http;
    std::string host;
    std::uint16_t port = 0;
    std::string target = "/";
};

struct HttpOptions
{
    int timeoutMs = 30000;
    int maxRetries = 0;
    int retryDelayMs = 200;
    bool followRedirects = true;
    int maxRedirects = 5;
};

struct MultipartField
{
    std::string name;
    std::string value;
};

struct MultipartFile
{
    std::string fieldName;
    std::string filePath;
    std::string contentType = "application/octet-stream";
    std::string fileName;
};

struct MultipartFormData
{
    std::vector<MultipartField> fields;
    std::vector<MultipartFile> files;
};

struct HttpRequest
{
    HttpMethod method = HttpMethod::Get;
    std::string target = "/";
    std::string body;
    std::unordered_map<std::string, std::string> headers;

    void setHeader(std::string name, std::string value)
    {
        headers[std::move(name)] = std::move(value);
    }
};

struct HttpResponse
{
    int statusCode = 0;
    std::string reasonPhrase;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    void setHeader(std::string name, std::string value)
    {
        headers[std::move(name)] = std::move(value);
    }

    [[nodiscard]] bool ok() const
    {
        return statusCode >= 200 && statusCode < 300;
    }

    [[nodiscard]] std::string header(std::string_view name) const
    {
        auto toLower = [](std::string_view text)
            {
                std::string lowered;
                lowered.reserve(text.size());
                for (char ch : text)
                    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                return lowered;
            };

        const std::string needle = toLower(name);
        for (const auto& [key, value] : headers)
        {
            if (toLower(key) == needle)
                return value;
        }

        return {};
    }
};

class Client;
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;
class Server;
using Http = Client;
}

using http::HttpEndpoint;
using http::HttpMethod;
using http::HttpOptions;
using http::HttpRequest;
using http::HttpResponse;
using http::HttpScheme;
using http::MultipartField;
using http::MultipartFile;
using http::MultipartFormData;

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle InvalidSocket = -1;
#endif

namespace
{
    struct ParsedUrl
    {
        bool valid = false;
        HttpScheme scheme = HttpScheme::Http;
        std::string host;
        std::uint16_t port = 0;
        std::string target;
    };

    class SocketSystemGuard
    {
    public:
        SocketSystemGuard()
        {
#if defined(_WIN32)
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                throw std::runtime_error("WSAStartup failed");
#endif
        }

        ~SocketSystemGuard()
        {
#if defined(_WIN32)
            WSACleanup();
#endif
        }
    };

    class SocketGuard
    {
    public:
        explicit SocketGuard(SocketHandle handle) : mHandle(handle)
        {
        }

        SocketGuard(const SocketGuard&) = delete;
        SocketGuard& operator=(const SocketGuard&) = delete;

        SocketGuard(SocketGuard&& other) noexcept : mHandle(other.mHandle)
        {
            other.mHandle = InvalidSocket;
        }

        SocketGuard& operator=(SocketGuard&& other) noexcept
        {
            if (this != &other)
            {
                close();
                mHandle = other.mHandle;
                other.mHandle = InvalidSocket;
            }

            return *this;
        }

        ~SocketGuard()
        {
            close();
        }

        [[nodiscard]] SocketHandle get() const
        {
            return mHandle;
        }

        [[nodiscard]] SocketHandle release()
        {
            SocketHandle handle = mHandle;
            mHandle = InvalidSocket;
            return handle;
        }

    private:
        void close()
        {
            if (mHandle == InvalidSocket)
                return;

#if defined(_WIN32)
            closesocket(mHandle);
#else
            ::close(mHandle);
#endif
            mHandle = InvalidSocket;
        }

        SocketHandle mHandle = InvalidSocket;
    };

#if !defined(_WIN32)
    class OpenSslGuard
    {
    public:
        OpenSslGuard()
        {
            static const bool initialized = []
                {
                    SSL_library_init();
                    SSL_load_error_strings();
                    OpenSSL_add_ssl_algorithms();
                    return true;
                }();
            (void)initialized;
        }
    };

    class SslContextGuard
    {
    public:
        SslContextGuard()
            : mContext(SSL_CTX_new(TLS_client_method()))
        {
            if (mContext == nullptr)
                throw std::runtime_error("SSL_CTX_new failed");
        }

        ~SslContextGuard()
        {
            if (mContext != nullptr)
                SSL_CTX_free(mContext);
        }

        [[nodiscard]] SSL_CTX* get() const
        {
            return mContext;
        }

    private:
        SSL_CTX* mContext = nullptr;
    };

    class SslHandleGuard
    {
    public:
        explicit SslHandleGuard(SSL* ssl) : mSsl(ssl)
        {
        }

        ~SslHandleGuard()
        {
            if (mSsl != nullptr)
                SSL_free(mSsl);
        }

        [[nodiscard]] SSL* get() const
        {
            return mSsl;
        }

    private:
        SSL* mSsl = nullptr;
    };
#endif

    std::string methodToString(HttpMethod method)
    {
        switch (method)
        {
        case HttpMethod::Get: return "GET";
        case HttpMethod::Post: return "POST";
        case HttpMethod::Put: return "PUT";
        case HttpMethod::Delete_: return "DELETE";
        case HttpMethod::Patch: return "PATCH";
        case HttpMethod::Head: return "HEAD";
        case HttpMethod::Options: return "OPTIONS";
        }

        return "GET";
    }

    std::uint16_t defaultPort(HttpScheme scheme)
    {
        return scheme == HttpScheme::Https ? 443 : 80;
    }

    std::string normalizeTarget(std::string_view target)
    {
        if (target.empty())
            return "/";

        if (target.front() == '/')
            return std::string(target);

        return "/" + std::string(target);
    }

    bool iequals(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size())
            return false;

        for (std::size_t i = 0; i < left.size(); ++i)
        {
            const auto l = static_cast<unsigned char>(left[i]);
            const auto r = static_cast<unsigned char>(right[i]);
            if (std::tolower(l) != std::tolower(r))
                return false;
        }

        return true;
    }

    std::string trim(const std::string& text)
    {
        auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char ch)
            {
                return std::isspace(ch) != 0;
            });

        auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch)
            {
                return std::isspace(ch) != 0;
            }).base();

        if (first >= last)
            return {};

        return std::string(first, last);
    }

    bool containsToken(std::string_view text, std::string_view token)
    {
        std::size_t start = 0;
        while (start < text.size())
        {
            const auto end = text.find(',', start);
            const auto len = end == std::string_view::npos ? text.size() - start : end - start;
            if (iequals(trim(std::string(text.substr(start, len))), token))
                return true;

            if (end == std::string_view::npos)
                break;

            start = end + 1;
        }

        return false;
    }

    std::string decodeChunkedBody(std::string_view body)
    {
        std::string decoded;
        std::size_t pos = 0;

        while (pos < body.size())
        {
            std::size_t lineEnd = body.find("\r\n", pos);
            std::size_t lineBreakSize = 2;
            if (lineEnd == std::string_view::npos)
            {
                lineEnd = body.find('\n', pos);
                lineBreakSize = 1;
            }

            if (lineEnd == std::string_view::npos)
                throw std::invalid_argument("Invalid chunked HTTP response: missing chunk size");

            const std::string chunkLine = trim(std::string(body.substr(pos, lineEnd - pos)));
            const auto extension = chunkLine.find(';');
            const std::string chunkSizeText = extension == std::string::npos ? chunkLine : chunkLine.substr(0, extension);

            std::size_t chunkSize = 0;
            std::stringstream parser;
            parser << std::hex << chunkSizeText;
            parser >> chunkSize;
            if (!parser || !parser.eof())
                throw std::invalid_argument("Invalid chunked HTTP response: malformed chunk size");

            pos = lineEnd + lineBreakSize;
            if (chunkSize == 0)
                break;

            if (pos + chunkSize > body.size())
                throw std::invalid_argument("Invalid chunked HTTP response: truncated chunk");

            decoded.append(body.substr(pos, chunkSize));
            pos += chunkSize;

            if (body.substr(pos, 2) == "\r\n")
                pos += 2;
            else if (pos < body.size() && body[pos] == '\n')
                ++pos;
            else
                throw std::invalid_argument("Invalid chunked HTTP response: missing chunk terminator");
        }

        return decoded;
    }

    ParsedUrl parseAbsoluteUrl(std::string_view url)
    {
        ParsedUrl parsed{};

        const auto schemeSep = url.find("://");
        if (schemeSep == std::string_view::npos)
            return parsed;

        const auto schemeText = std::string(url.substr(0, schemeSep));
        if (iequals(schemeText, "https"))
            parsed.scheme = HttpScheme::Https;
        else if (iequals(schemeText, "http"))
            parsed.scheme = HttpScheme::Http;
        else
            return parsed;

        const auto hostStart = schemeSep + 3;
        const auto pathStart = url.find('/', hostStart);
        const auto authority = pathStart == std::string_view::npos
            ? url.substr(hostStart)
            : url.substr(hostStart, pathStart - hostStart);

        if (authority.empty())
            return parsed;

        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos)
        {
            parsed.host = std::string(authority.substr(0, colon));
            const std::string portText(authority.substr(colon + 1));
            try
            {
                const auto portNum = std::stoi(portText);
                if (portNum < 1 || portNum > 65535)
                    return {};

                parsed.port = static_cast<std::uint16_t>(portNum);
            }
            catch (...)
            {
                return {};
            }
        }
        else
        {
            parsed.host = std::string(authority);
            parsed.port = defaultPort(parsed.scheme);
        }

        parsed.target = pathStart == std::string_view::npos ? "/" : std::string(url.substr(pathStart));
        parsed.valid = true;
        return parsed;
    }

    HttpEndpoint resolveRedirect(const HttpEndpoint& current, std::string_view location)
    {
        const ParsedUrl absolute = parseAbsoluteUrl(location);
        if (absolute.valid)
        {
            HttpEndpoint endpoint{};
            endpoint.scheme = absolute.scheme;
            endpoint.host = absolute.host;
            endpoint.port = absolute.port;
            endpoint.target = absolute.target;
            return endpoint;
        }

        HttpEndpoint next = current;
        if (!location.empty() && location.front() == '/')
        {
            next.target = std::string(location);
            return next;
        }

        // Relative path without leading slash
        const auto lastSlash = current.target.find_last_of('/');
        const std::string base = lastSlash == std::string::npos ? "/" : current.target.substr(0, lastSlash + 1);
        next.target = base + std::string(location);
        return next;
    }

    bool isRedirectCode(int statusCode)
    {
        return statusCode == 301
            || statusCode == 302
            || statusCode == 303
            || statusCode == 307
            || statusCode == 308;
    }

    bool hasHeader(const HttpRequest& request, std::string_view name)
    {
        for (const auto& [key, _] : request.headers)
        {
            if (iequals(key, name))
                return true;
        }

        return false;
    }

    std::string authority(std::string_view host, std::uint16_t port, HttpScheme scheme)
    {
        if (port == 0 || port == defaultPort(scheme))
            return std::string(host);

        return std::string(host) + ":" + std::to_string(port);
    }

    std::size_t fileContentLength(std::string_view filePath)
    {
        if (filePath.empty())
            return 0;

        return static_cast<std::size_t>(std::filesystem::file_size(std::filesystem::path(filePath)));
    }

    template <typename WriteFn>
    void sendRequestPayload(WriteFn&& write, const HttpRequest& request, std::string_view filePath)
    {
        if (!request.body.empty())
            write(std::string_view(request.body));

        if (filePath.empty())
            return;

        std::ifstream file(std::string(filePath), std::ios::binary);
        if (!file)
            throw std::runtime_error("Unable to open upload file: " + std::string(filePath));

        char buffer[8192];
        while (file.good())
        {
            file.read(buffer, sizeof(buffer));
            const auto count = file.gcount();
            if (count <= 0)
                break;

            write(std::string_view(buffer, static_cast<std::size_t>(count)));
        }
    }

    std::string buildRequestHeaders(const HttpRequest& request,
        std::string_view host,
        std::uint16_t port,
        HttpScheme scheme,
        std::string_view target,
        std::size_t contentLength)
    {
        std::ostringstream out;
        out << methodToString(request.method) << ' '
            << normalizeTarget(target) << ' '
            << "HTTP/1.1\r\n";

        out << "Host: " << authority(host, port, scheme) << "\r\n";

        bool hasContentLength = false;
        bool hasConnection = false;

        for (const auto& [name, value] : request.headers)
        {
            out << name << ": " << value << "\r\n";
            if (iequals(name, "Content-Length"))
                hasContentLength = true;
            if (iequals(name, "Connection"))
                hasConnection = true;
        }

        if (contentLength > 0 && !hasContentLength)
            out << "Content-Length: " << contentLength << "\r\n";

        if (!hasConnection)
            out << "Connection: close\r\n";

        out << "\r\n";
        return out.str();
    }

    void applySocketTimeout(SocketHandle socketHandle, int timeoutMs)
    {
    #if defined(_WIN32)
        const DWORD timeout = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0;
        setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    #else
        timeval tv{};
        tv.tv_sec = timeoutMs > 0 ? timeoutMs / 1000 : 0;
        tv.tv_usec = timeoutMs > 0 ? (timeoutMs % 1000) * 1000 : 0;
        setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    #endif
    }

    void sendAll(SocketHandle socketHandle, std::string_view data)
    {
        std::size_t sent = 0;
        while (sent < data.size())
        {
#if defined(_WIN32)
            const int result = ::send(socketHandle, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
            const auto result = ::send(socketHandle, data.data() + sent, data.size() - sent, 0);
#endif
            if (result <= 0)
                throw std::runtime_error("Failed to send HTTP request");

            sent += static_cast<std::size_t>(result);
        }
    }

    void sendStream(SocketHandle socketHandle, std::istream& stream)
    {
        char buffer[8192];
        while (stream.good())
        {
            stream.read(buffer, sizeof(buffer));
            const auto count = stream.gcount();
            if (count <= 0)
                break;

            sendAll(socketHandle, std::string_view(buffer, static_cast<std::size_t>(count)));
        }
    }

    std::string receiveAll(SocketHandle socketHandle)
    {
        std::string response;
        char buffer[4096];

        for (;;)
        {
#if defined(_WIN32)
            const int result = ::recv(socketHandle, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
            const auto result = ::recv(socketHandle, buffer, sizeof(buffer), 0);
#endif
            if (result == 0)
                break;

            if (result < 0)
                throw std::runtime_error("Failed to receive HTTP response");

            response.append(buffer, static_cast<std::size_t>(result));
        }

        return response;
    }

    SocketHandle connectSocket(std::string_view host, std::uint16_t port, int timeoutMs)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        const std::string hostName(host);
        const std::string service = std::to_string(port);

        addrinfo* addresses = nullptr;
        const int status = getaddrinfo(hostName.c_str(), service.c_str(), &hints, &addresses);
        if (status != 0)
            throw std::runtime_error("getaddrinfo failed for host: " + hostName);

        struct AddrInfoGuard
        {
            addrinfo* value;

            ~AddrInfoGuard()
            {
                if (value != nullptr)
                    freeaddrinfo(value);
            }
        } addressGuard{ addresses };

        SocketHandle connection = InvalidSocket;
        for (addrinfo* current = addresses; current != nullptr; current = current->ai_next)
        {
            SocketHandle candidate = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (candidate == InvalidSocket)
                continue;

            SocketGuard guard(candidate);
            if (connect(candidate, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0)
            {
                applySocketTimeout(candidate, timeoutMs);
                connection = guard.release();
                break;
            }
        }

        if (connection == InvalidSocket)
            throw std::runtime_error("Failed to connect to host: " + hostName);

        return connection;
    }

    std::string sendPlainTextRequest(std::string_view host,
        std::uint16_t port,
        const HttpRequest& request,
        std::string_view target,
        std::string_view filePath,
        int timeoutMs)
    {
        SocketSystemGuard socketSystem;
        SocketGuard socketGuard(connectSocket(host, port, timeoutMs));

        const std::size_t fileSize = fileContentLength(filePath);
        const std::size_t contentLength = request.body.size() + fileSize;
        const std::string headers = buildRequestHeaders(request, host, port, HttpScheme::Http, target, contentLength);
        sendAll(socketGuard.get(), headers);

        sendRequestPayload([&](std::string_view chunk)
            {
                sendAll(socketGuard.get(), chunk);
            }, request, filePath);

        return receiveAll(socketGuard.get());
    }

#if defined(_WIN32)
    std::wstring toWide(std::string_view text)
    {
        if (text.empty())
            return {};

        const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (count <= 0)
            throw std::runtime_error("MultiByteToWideChar failed");

        std::wstring wide(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
        return wide;
    }

    std::string toUtf8(const std::wstring& text)
    {
        if (text.empty())
            return {};

        const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0)
            throw std::runtime_error("WideCharToMultiByte failed");

        std::string utf8(static_cast<std::size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), count, nullptr, nullptr);
        return utf8;
    }

    std::string buildAdditionalHeaders(const HttpRequest& request)
    {
        std::ostringstream out;
        bool hasConnection = false;

        for (const auto& [name, value] : request.headers)
        {
            if (iequals(name, "Host") || iequals(name, "Content-Length"))
                continue;

            if (iequals(name, "Connection"))
                hasConnection = true;

            out << name << ": " << value << "\r\n";
        }

        if (!hasConnection)
            out << "Connection: close\r\n";

        return out.str();
    }

    std::string queryRawHeaders(HINTERNET requestHandle)
    {
        DWORD size = 0;
        WinHttpQueryHeaders(requestHandle,
            WINHTTP_QUERY_RAW_HEADERS_CRLF,
            WINHTTP_HEADER_NAME_BY_INDEX,
            WINHTTP_NO_OUTPUT_BUFFER,
            &size,
            WINHTTP_NO_HEADER_INDEX);

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
            throw std::runtime_error("WinHttpQueryHeaders failed");

        std::wstring headers(size / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(requestHandle,
            WINHTTP_QUERY_RAW_HEADERS_CRLF,
            WINHTTP_HEADER_NAME_BY_INDEX,
            headers.data(),
            &size,
            WINHTTP_NO_HEADER_INDEX))
        {
            throw std::runtime_error("WinHttpQueryHeaders failed");
        }

        while (!headers.empty() && headers.back() == L'\0')
            headers.pop_back();

        return toUtf8(headers);
    }

    std::string stripHeaderLine(std::string headers, std::string_view headerName)
    {
        std::string result;
        std::size_t start = 0;

        while (start < headers.size())
        {
            const auto end = headers.find("\r\n", start);
            const auto length = end == std::string::npos ? headers.size() - start : end - start;
            const std::string_view line(headers.data() + start, length);

            const auto colon = line.find(':');
            const bool shouldSkip = colon != std::string_view::npos
                && iequals(trim(std::string(line.substr(0, colon))), headerName);

            if (!shouldSkip)
            {
                result.append(line);
                result.append("\r\n");
            }

            if (end == std::string::npos)
                break;

            start = end + 2;
        }

        return result;
    }

    std::string readWinHttpBody(HINTERNET requestHandle)
    {
        std::string body;
        char buffer[4096];

        for (;;)
        {
            DWORD bytesRead = 0;
            if (!WinHttpReadData(requestHandle, buffer, sizeof(buffer), &bytesRead))
                throw std::runtime_error("WinHttpReadData failed");

            if (bytesRead == 0)
                break;

            body.append(buffer, buffer + bytesRead);
        }

        return body;
    }

    std::string sendTlsRequestPlatform(const HttpRequest& request,
        std::string_view host,
        std::uint16_t port,
        std::string_view target,
        std::string_view filePath,
        int timeoutMs)
    {
        (void)filePath;

        struct HandleGuard
        {
            HINTERNET value = nullptr;

            ~HandleGuard()
            {
                if (value != nullptr)
                    WinHttpCloseHandle(value);
            }
        };

        const std::wstring wideHost = toWide(host);
        const std::wstring wideTarget = toWide(normalizeTarget(target));
        const std::wstring wideMethod = toWide(methodToString(request.method));
        const std::wstring wideHeaders = toWide(buildAdditionalHeaders(request));

        HandleGuard session{ WinHttpOpen(L"SampleModuleTest/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0) };
        if (session.value == nullptr)
            throw std::runtime_error("WinHttpOpen failed");

        HandleGuard connection{ WinHttpConnect(session.value, wideHost.c_str(), port, 0) };
        if (connection.value == nullptr)
            throw std::runtime_error("WinHttpConnect failed");

        HandleGuard requestHandle{ WinHttpOpenRequest(connection.value,
            wideMethod.c_str(),
            wideTarget.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE) };
        if (requestHandle.value == nullptr)
            throw std::runtime_error("WinHttpOpenRequest failed");

        if (timeoutMs > 0)
        {
            const DWORD timeout = static_cast<DWORD>(timeoutMs);
            WinHttpSetOption(requestHandle.value, WINHTTP_OPTION_RESOLVE_TIMEOUT, const_cast<LPVOID>(static_cast<LPCVOID>(&timeout)), sizeof(timeout));
            WinHttpSetOption(requestHandle.value, WINHTTP_OPTION_CONNECT_TIMEOUT, const_cast<LPVOID>(static_cast<LPCVOID>(&timeout)), sizeof(timeout));
            WinHttpSetOption(requestHandle.value, WINHTTP_OPTION_SEND_TIMEOUT, const_cast<LPVOID>(static_cast<LPCVOID>(&timeout)), sizeof(timeout));
            WinHttpSetOption(requestHandle.value, WINHTTP_OPTION_RECEIVE_TIMEOUT, const_cast<LPVOID>(static_cast<LPCVOID>(&timeout)), sizeof(timeout));
        }

        const wchar_t* headerText = wideHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wideHeaders.c_str();
        const DWORD headerLength = wideHeaders.empty() ? 0 : static_cast<DWORD>(wideHeaders.size());
        const DWORD bodyLength = static_cast<DWORD>(request.body.size());
        void* bodyData = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data());

        if (!WinHttpSendRequest(requestHandle.value,
            headerText,
            headerLength,
            bodyData,
            bodyLength,
            bodyLength,
            0))
        {
            throw std::runtime_error("WinHttpSendRequest failed");
        }

        if (!WinHttpReceiveResponse(requestHandle.value, nullptr))
            throw std::runtime_error("WinHttpReceiveResponse failed");

        std::string headers = stripHeaderLine(queryRawHeaders(requestHandle.value), "Transfer-Encoding");
        if (!headers.ends_with("\r\n\r\n"))
            headers += "\r\n";

        return headers + readWinHttpBody(requestHandle.value);
    }
#else
    void sendAll(SSL* ssl, std::string_view data)
    {
        std::size_t sent = 0;
        while (sent < data.size())
        {
            const int result = SSL_write(ssl, data.data() + sent, static_cast<int>(data.size() - sent));
            if (result <= 0)
                throw std::runtime_error("SSL_write failed");

            sent += static_cast<std::size_t>(result);
        }
    }

    void sendStream(SSL* ssl, std::istream& stream)
    {
        char buffer[8192];
        while (stream.good())
        {
            stream.read(buffer, sizeof(buffer));
            const auto count = stream.gcount();
            if (count <= 0)
                break;

            sendAll(ssl, std::string_view(buffer, static_cast<std::size_t>(count)));
        }
    }

    std::string receiveAll(SSL* ssl)
    {
        std::string response;
        char buffer[4096];

        for (;;)
        {
            const int result = SSL_read(ssl, buffer, static_cast<int>(sizeof(buffer)));
            if (result > 0)
            {
                response.append(buffer, static_cast<std::size_t>(result));
                continue;
            }

            const int error = SSL_get_error(ssl, result);
            if (error == SSL_ERROR_ZERO_RETURN)
                break;

            throw std::runtime_error("SSL_read failed");
        }

        return response;
    }

    std::string sendTlsRequestPlatform(const HttpRequest& request,
        std::string_view host,
        std::uint16_t port,
        std::string_view target,
        std::string_view filePath,
        int timeoutMs)
    {
        SocketSystemGuard socketSystem;
        OpenSslGuard openSslGuard;
        SocketGuard socketGuard(connectSocket(host, port, timeoutMs));

        SslContextGuard contextGuard;
        SslHandleGuard sslGuard(SSL_new(contextGuard.get()));
        if (sslGuard.get() == nullptr)
            throw std::runtime_error("SSL_new failed");

        const std::string hostName(host);
        if (SSL_set_tlsext_host_name(sslGuard.get(), hostName.c_str()) != 1)
            throw std::runtime_error("SSL_set_tlsext_host_name failed");

        if (SSL_set_fd(sslGuard.get(), socketGuard.get()) != 1)
            throw std::runtime_error("SSL_set_fd failed");

        if (SSL_connect(sslGuard.get()) != 1)
            throw std::runtime_error("SSL_connect failed");

        const std::size_t fileSize = fileContentLength(filePath);
        const std::size_t contentLength = request.body.size() + fileSize;
        const std::string headers = buildRequestHeaders(request, host, port, HttpScheme::Https, target, contentLength);
        sendAll(sslGuard.get(), headers);

        sendRequestPayload([&](std::string_view chunk)
            {
                sendAll(sslGuard.get(), chunk);
            }, request, filePath);

        return receiveAll(sslGuard.get());
    }
#endif

    std::string sendTlsRequest(const HttpRequest& request,
        std::string_view host,
        std::uint16_t port,
        std::string_view target,
        std::string_view filePath,
        int timeoutMs)
    {
        return sendTlsRequestPlatform(request, host, port, target, filePath, timeoutMs);
    }

    HttpResponse parseResponse(std::string_view raw)
    {
        HttpResponse response;

        std::size_t lineEnd = raw.find("\r\n");
        std::size_t lineBreakSize = 2;
        if (lineEnd == std::string_view::npos)
        {
            lineEnd = raw.find('\n');
            lineBreakSize = 1;
        }

        if (lineEnd == std::string_view::npos)
            throw std::invalid_argument("Invalid HTTP response: missing status line");

        {
            std::istringstream in(std::string(raw.substr(0, lineEnd)));
            std::string version;
            if (!(in >> version >> response.statusCode))
                throw std::invalid_argument("Invalid HTTP response: malformed status line");

            std::getline(in, response.reasonPhrase);
            response.reasonPhrase = trim(response.reasonPhrase);
        }

        std::size_t pos = lineEnd + lineBreakSize;
        while (pos < raw.size())
        {
            std::size_t nextEnd = raw.find("\r\n", pos);
            std::size_t nextBreakSize = 2;
            if (nextEnd == std::string_view::npos)
            {
                nextEnd = raw.find('\n', pos);
                nextBreakSize = 1;
            }

            if (nextEnd == std::string_view::npos)
            {
                response.body = std::string(raw.substr(pos));
                return response;
            }

            if (nextEnd == pos)
            {
                pos = nextEnd + nextBreakSize;
                break;
            }

            const std::string headerLine(raw.substr(pos, nextEnd - pos));
            const auto colon = headerLine.find(':');
            if (colon != std::string::npos)
            {
                const std::string name = trim(headerLine.substr(0, colon));
                const std::string value = trim(headerLine.substr(colon + 1));
                response.headers[name] = value;
            }

            pos = nextEnd + nextBreakSize;
        }

        if (pos < raw.size())
            response.body = std::string(raw.substr(pos));

        if (containsToken(response.header("Transfer-Encoding"), "chunked"))
            response.body = decodeChunkedBody(response.body);

        return response;
    }

    bool parseMethodToken(std::string_view token, HttpMethod& method)
    {
        if (token == "GET") { method = HttpMethod::Get; return true; }
        if (token == "POST") { method = HttpMethod::Post; return true; }
        if (token == "PUT") { method = HttpMethod::Put; return true; }
        if (token == "DELETE") { method = HttpMethod::Delete_; return true; }
        if (token == "PATCH") { method = HttpMethod::Patch; return true; }
        if (token == "HEAD") { method = HttpMethod::Head; return true; }
        if (token == "OPTIONS") { method = HttpMethod::Options; return true; }
        return false;
    }

    std::string reasonPhraseForStatus(int statusCode)
    {
        switch (statusCode)
        {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
        }
    }

    std::string serializeServerResponse(const HttpResponse& response)
    {
        std::ostringstream out;
        const int code = response.statusCode == 0 ? 200 : response.statusCode;
        const std::string phrase = response.reasonPhrase.empty() ? reasonPhraseForStatus(code) : response.reasonPhrase;

        out << "HTTP/1.1 " << code << ' ' << phrase << "\r\n";

        bool hasContentLength = false;
        bool hasConnection = false;
        bool hasContentType = false;

        for (const auto& [name, value] : response.headers)
        {
            out << name << ": " << value << "\r\n";
            if (iequals(name, "Content-Length"))
                hasContentLength = true;
            if (iequals(name, "Connection"))
                hasConnection = true;
            if (iequals(name, "Content-Type"))
                hasContentType = true;
        }

        if (!hasContentType)
            out << "Content-Type: text/plain; charset=utf-8\r\n";
        if (!hasContentLength)
            out << "Content-Length: " << response.body.size() << "\r\n";
        if (!hasConnection)
            out << "Connection: close\r\n";

        out << "\r\n";
        out << response.body;
        return out.str();
    }

    bool receiveServerRequest(SocketHandle socketHandle, HttpRequest& request)
    {
        std::string raw;
        raw.reserve(4096);

        std::size_t headerEnd = std::string::npos;
        std::size_t lineBreakSize = 4;
        std::size_t contentLength = 0;
        bool hasContentLength = false;

        char buffer[4096];
        for (;;)
        {
#if defined(_WIN32)
            const int result = ::recv(socketHandle, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
            const auto result = ::recv(socketHandle, buffer, sizeof(buffer), 0);
#endif
            if (result <= 0)
                return false;

            raw.append(buffer, static_cast<std::size_t>(result));

            if (headerEnd == std::string::npos)
            {
                headerEnd = raw.find("\r\n\r\n");
                lineBreakSize = 4;
                if (headerEnd == std::string::npos)
                {
                    headerEnd = raw.find("\n\n");
                    lineBreakSize = 2;
                }

                if (headerEnd != std::string::npos)
                {
                    const std::string headerSection = raw.substr(0, headerEnd);
                    std::istringstream hs(headerSection);
                    std::string line;
                    std::getline(hs, line);
                    while (std::getline(hs, line))
                    {
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();

                        const auto colon = line.find(':');
                        if (colon == std::string::npos)
                            continue;

                        const std::string name = trim(line.substr(0, colon));
                        const std::string value = trim(line.substr(colon + 1));
                        if (iequals(name, "Content-Length"))
                        {
                            try
                            {
                                contentLength = static_cast<std::size_t>(std::stoull(value));
                                hasContentLength = true;
                            }
                            catch (...)
                            {
                                return false;
                            }
                            break;
                        }
                    }
                }
            }

            if (headerEnd != std::string::npos)
            {
                const std::size_t bodyStart = headerEnd + lineBreakSize;
                if (!hasContentLength || raw.size() >= bodyStart + contentLength)
                    break;
            }
        }

        if (headerEnd == std::string::npos)
            return false;

        const std::size_t bodyStart = headerEnd + lineBreakSize;
        const std::string headerSection = raw.substr(0, headerEnd);
        std::istringstream in(headerSection);
        std::string firstLine;
        if (!std::getline(in, firstLine))
            return false;
        if (!firstLine.empty() && firstLine.back() == '\r')
            firstLine.pop_back();

        {
            std::istringstream fl(firstLine);
            std::string methodText;
            std::string target;
            std::string version;
            if (!(fl >> methodText >> target >> version))
                return false;

            if (!parseMethodToken(methodText, request.method))
                return false;

            request.target = normalizeTarget(target);
        }

        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty())
                continue;

            const auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;

            const std::string name = trim(line.substr(0, colon));
            const std::string value = trim(line.substr(colon + 1));
            request.headers[name] = value;
        }

        if (hasContentLength)
            request.body = raw.substr(bodyStart, contentLength);
        else if (bodyStart < raw.size())
            request.body = raw.substr(bodyStart);

        return true;
    }

    void closeSocketHandle(SocketHandle socketHandle)
    {
        if (socketHandle == InvalidSocket)
            return;

#if defined(_WIN32)
        closesocket(socketHandle);
#else
        ::close(socketHandle);
#endif
    }

    HttpResponse executeOnce(const HttpEndpoint& endpoint,
        const HttpRequest& request,
        std::string_view target,
        std::string_view filePath,
        const HttpOptions& options)
    {
        const auto port = endpoint.port == 0 ? defaultPort(endpoint.scheme) : endpoint.port;

        const std::string rawResponse = endpoint.scheme == HttpScheme::Https
            ? sendTlsRequest(request, endpoint.host, port, target, filePath, options.timeoutMs)
            : sendPlainTextRequest(endpoint.host, port, request, target, filePath, options.timeoutMs);

        return parseResponse(rawResponse);
    }

    std::string readFileAsString(const std::string& filePath)
    {
        std::ifstream in(filePath, std::ios::binary);
        if (!in)
            throw std::runtime_error("Unable to read file: " + filePath);

        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    std::string guessFileName(const std::string& filePath)
    {
        const auto p = std::filesystem::path(filePath).filename().string();
        return p.empty() ? "file" : p;
    }

    std::string makeBoundary()
    {
        const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return "----SampleModuleBoundary" + std::to_string(ticks);
    }
}

namespace http
{
class Client
{
public:
    Client() = default;

    explicit Client(HttpOptions defaultOptions)
        : mDefaultOptions(std::move(defaultOptions))
    {
    }

    [[nodiscard]] const std::string& lastError() const noexcept
    {
        return mLastError;
    }

    void clearLastError()
    {
        mLastError.clear();
    }

    void setDefaultOptions(HttpOptions options)
    {
        mDefaultOptions = std::move(options);
    }

    [[nodiscard]] const HttpOptions& defaultOptions() const noexcept
    {
        return mDefaultOptions;
    }

    std::string serializeRequest(const HttpRequest& request,
        std::string_view host,
        std::string_view version = "HTTP/1.1")
    {
        (void)version;
        return buildRequestHeaders(request, host, defaultPort(HttpScheme::Http), HttpScheme::Http, request.target, request.body.size())
            + request.body;
    }

    std::string serializeRequest(const HttpRequest& request,
        std::string_view host,
        std::uint16_t port,
        HttpScheme scheme,
        std::string_view version = "HTTP/1.1")
    {
        (void)version;
        return buildRequestHeaders(request, host, port, scheme, request.target, request.body.size())
            + request.body;
    }

    std::string serializeRequest(const HttpRequest& request,
        std::string_view host,
        std::uint16_t port,
        HttpScheme scheme,
        std::string_view version,
        std::string_view target)
    {
        (void)version;
        return buildRequestHeaders(request, host, port, scheme, target, request.body.size())
            + request.body;
    }

    HttpRequest makeMultipartRequest(HttpMethod method,
        std::string_view target,
        const MultipartFormData& form)
    {
        HttpRequest request{};
        request.method = method;
        request.target = normalizeTarget(target);

        const std::string boundary = makeBoundary();
        std::ostringstream body;

        for (const auto& field : form.fields)
        {
            body << "--" << boundary << "\r\n";
            body << "Content-Disposition: form-data; name=\"" << field.name << "\"\r\n\r\n";
            body << field.value << "\r\n";
        }

        for (const auto& file : form.files)
        {
            const std::string fileName = file.fileName.empty() ? guessFileName(file.filePath) : file.fileName;
            const std::string content = readFileAsString(file.filePath);

            body << "--" << boundary << "\r\n";
            body << "Content-Disposition: form-data; name=\"" << file.fieldName
                << "\"; filename=\"" << fileName << "\"\r\n";
            body << "Content-Type: " << (file.contentType.empty() ? "application/octet-stream" : file.contentType)
                << "\r\n\r\n";
            body << content << "\r\n";
        }

        body << "--" << boundary << "--\r\n";

        request.body = body.str();
        request.setHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        return request;
    }

    HttpResponse uploadMultipart(const HttpEndpoint& endpoint,
        const MultipartFormData& form,
        std::string_view target = "/",
        HttpMethod method = HttpMethod::Post)
    {
        return uploadMultipart(endpoint, form, target, method, mDefaultOptions);
    }

    HttpResponse uploadMultipart(const HttpEndpoint& endpoint,
        const MultipartFormData& form,
        std::string_view target = "/",
        HttpMethod method = HttpMethod::Post,
        const HttpOptions& options = HttpOptions{})
    {
        HttpRequest request = makeMultipartRequest(method, target, form);
        return send(endpoint, request, options);
    }

    HttpResponse send(const HttpRequest& request,
        std::string_view host,
        std::uint16_t port = 80)
    {
        return send(request, host, port, HttpScheme::Http, mDefaultOptions);
    }

    HttpResponse send(std::string_view host,
        std::uint16_t port,
        const HttpRequest& request)
    {
        return send(request, host, port, HttpScheme::Http, mDefaultOptions);
    }

    HttpResponse sendHttps(std::string_view host,
        const HttpRequest& request,
        std::uint16_t port = 443)
    {
        return send(request, host, port, HttpScheme::Https, mDefaultOptions);
    }

    HttpResponse send(const HttpEndpoint& endpoint,
        const HttpRequest& request)
    {
        return send(endpoint, request, mDefaultOptions);
    }

    HttpResponse send(const HttpEndpoint& endpoint,
        const HttpRequest& request,
        const HttpOptions& options)
    {
        HttpEndpoint current = endpoint;
        HttpRequest currentRequest = request;

        if (current.target.empty())
            current.target = request.target.empty() ? "/" : request.target;

        int redirects = 0;
        for (int attempt = 0; attempt <= (std::max)(0, options.maxRetries); ++attempt)
        {
            try
            {
                HttpResponse response = executeOnce(current, currentRequest, current.target, {}, options);

                while (options.followRedirects && isRedirectCode(response.statusCode) && redirects < (std::max)(0, options.maxRedirects))
                {
                    const std::string location = response.header("Location");
                    if (location.empty())
                        break;

                    ++redirects;
                    current = resolveRedirect(current, location);

                    if (response.statusCode == 303)
                    {
                        currentRequest.method = HttpMethod::Get;
                        currentRequest.body.clear();
                    }

                    response = executeOnce(current, currentRequest, current.target, {}, options);
                }

                return response;
            }
            catch (const std::exception& ex)
            {
                mLastError = ex.what();
                if (attempt >= (std::max)(0, options.maxRetries))
                    throw;

                std::this_thread::sleep_for(std::chrono::milliseconds((std::max)(0, options.retryDelayMs)));
            }
        }

        throw std::runtime_error("HTTP request failed");
    }

    HttpResponse send(const HttpRequest& request,
        std::string_view host,
        std::uint16_t port,
        HttpScheme scheme)
    {
        return send(request, host, port, scheme, mDefaultOptions);
    }

    HttpResponse send(const HttpRequest& request,
        std::string_view host,
        std::uint16_t port,
        HttpScheme scheme,
        const HttpOptions& options)
    {
        HttpEndpoint endpoint{};
        endpoint.scheme = scheme;
        endpoint.host = std::string(host);
        endpoint.port = port;
        endpoint.target = request.target;
        return send(endpoint, request, options);
    }

    HttpResponse sendFile(const HttpEndpoint& endpoint,
        const HttpRequest& request,
        std::string_view filePath)
    {
        return sendFile(endpoint, request, filePath, mDefaultOptions);
    }

    HttpResponse sendFile(const HttpEndpoint& endpoint,
        const HttpRequest& request,
        std::string_view filePath,
        const HttpOptions& options = HttpOptions{})
    {
        if (filePath.empty())
            throw std::invalid_argument("filePath cannot be empty");

        HttpEndpoint current = endpoint;
        HttpRequest currentRequest = request;

        if (current.target.empty())
            current.target = request.target.empty() ? "/" : request.target;

        for (int attempt = 0; attempt <= (std::max)(0, options.maxRetries); ++attempt)
        {
            try
            {
                return executeOnce(current, currentRequest, current.target, filePath, options);
            }
            catch (const std::exception& ex)
            {
                mLastError = ex.what();
                if (attempt >= (std::max)(0, options.maxRetries))
                    throw;

                std::this_thread::sleep_for(std::chrono::milliseconds((std::max)(0, options.retryDelayMs)));
            }
        }

        throw std::runtime_error("File upload failed");
    }

    HttpResponse sendToFile(const HttpEndpoint& endpoint,
        const HttpRequest& request,
        std::string_view outputPath)
    {
        return sendToFile(endpoint, request, outputPath, mDefaultOptions);
    }

    HttpResponse sendToFile(const HttpEndpoint& endpoint,
        const HttpRequest& request,
        std::string_view outputPath,
        const HttpOptions& options = HttpOptions{})
    {
        HttpResponse response = send(endpoint, request, options);
        std::ofstream out(std::string(outputPath), std::ios::binary);
        if (!out)
            throw std::runtime_error("Unable to open output file: " + std::string(outputPath));

        out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        return response;
    }

    void streamResponseBody(const HttpResponse& response,
        const std::function<void(std::string_view)>& onChunk,
        std::size_t chunkSize = 4096)
    {
        if (!onChunk)
            return;

        const std::size_t actualChunk = chunkSize == 0 ? 4096 : chunkSize;
        std::size_t offset = 0;

        while (offset < response.body.size())
        {
            const std::size_t size = (std::min)(actualChunk, response.body.size() - offset);
            onChunk(std::string_view(response.body.data() + offset, size));
            offset += size;
        }
    }

    std::future<HttpResponse> sendAsync(const HttpEndpoint& endpoint,
        HttpRequest request)
    {
        return sendAsync(endpoint, std::move(request), mDefaultOptions);
    }

    std::future<HttpResponse> sendAsync(const HttpEndpoint& endpoint,
        HttpRequest request,
        HttpOptions options = HttpOptions{})
    {
        return std::async(std::launch::async, [self = *this, endpoint, request = std::move(request), options]() mutable
            {
                return self.send(endpoint, request, options);
            });
    }

    std::future<HttpResponse> sendFileAsync(const HttpEndpoint& endpoint,
        HttpRequest request,
        std::string filePath)
    {
        return sendFileAsync(endpoint, std::move(request), std::move(filePath), mDefaultOptions);
    }

    std::future<HttpResponse> sendFileAsync(const HttpEndpoint& endpoint,
        HttpRequest request,
        std::string filePath,
        HttpOptions options = HttpOptions{})
    {
        return std::async(std::launch::async, [self = *this, endpoint, request = std::move(request), filePath = std::move(filePath), options]() mutable
            {
                return self.sendFile(endpoint, request, filePath, options);
            });
    }

    std::future<HttpResponse> sendToFileAsync(const HttpEndpoint& endpoint,
        HttpRequest request,
        std::string outputPath)
    {
        return sendToFileAsync(endpoint, std::move(request), std::move(outputPath), mDefaultOptions);
    }

    std::future<HttpResponse> sendToFileAsync(const HttpEndpoint& endpoint,
        HttpRequest request,
        std::string outputPath,
        HttpOptions options = HttpOptions{})
    {
        return std::async(std::launch::async, [self = *this, endpoint, request = std::move(request), outputPath = std::move(outputPath), options]() mutable
            {
                return self.sendToFile(endpoint, request, outputPath, options);
            });
    }

    HttpResponse parseResponse(std::string_view raw)
    {
        return ::parseResponse(raw);
    }

private:
    HttpOptions mDefaultOptions{};
    std::string mLastError;
};

class Server
{
public:
    Server() = default;

    ~Server()
    {
        stop();
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void route(HttpMethod method, std::string target, HttpHandler handler)
    {
        if (!handler)
            return;

        std::lock_guard lock(mRoutesMutex);
        mRoutes[routeKey(method, normalizeTarget(target))] = std::move(handler);
    }

    void setNotFoundHandler(HttpHandler handler)
    {
        if (!handler)
            return;

        std::lock_guard lock(mRoutesMutex);
        mNotFoundHandler = std::move(handler);
    }

    bool start(std::uint16_t port, std::string bindAddress = "0.0.0.0")
    {
        std::lock_guard lock(mStateMutex);
        if (mRunning)
            return false;

        mPort = port;
        mBindAddress = std::move(bindAddress);
        mRunning = true;

        mWorker = std::thread([this]()
            {
                run();
            });

        return true;
    }

    void stop()
    {
        {
            std::lock_guard lock(mStateMutex);
            if (!mRunning)
                return;

            mRunning = false;
            closeSocketHandle(mListenSocket);
            mListenSocket = InvalidSocket;
        }

        if (mWorker.joinable())
            mWorker.join();
    }

    [[nodiscard]] bool isRunning() const noexcept
    {
        return mRunning.load();
    }

private:
    static std::string routeKey(HttpMethod method, std::string_view target)
    {
        return methodToString(method) + " " + std::string(target);
    }

    HttpResponse dispatch(const HttpRequest& request)
    {
        HttpHandler handler;
        HttpHandler notFound;

        {
            std::lock_guard lock(mRoutesMutex);
            const auto it = mRoutes.find(routeKey(request.method, normalizeTarget(request.target)));
            if (it != mRoutes.end())
                handler = it->second;
            notFound = mNotFoundHandler;
        }

        if (!handler)
            handler = notFound;

        if (!handler)
        {
            HttpResponse response{};
            response.statusCode = 404;
            response.reasonPhrase = "Not Found";
            response.body = "Not Found";
            response.headers["Content-Type"] = "text/plain; charset=utf-8";
            return response;
        }

        return handler(request);
    }

    void run()
    {
        try
        {
            SocketSystemGuard socketSystem;

            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            hints.ai_flags = AI_PASSIVE;

            const std::string service = std::to_string(mPort);
            const char* host = mBindAddress.empty() ? nullptr : mBindAddress.c_str();

            addrinfo* addresses = nullptr;
            if (getaddrinfo(host, service.c_str(), &hints, &addresses) != 0)
            {
                mRunning = false;
                return;
            }

            struct AddrInfoGuard
            {
                addrinfo* value;
                ~AddrInfoGuard()
                {
                    if (value != nullptr)
                        freeaddrinfo(value);
                }
            } addressGuard{ addresses };

            SocketHandle listener = InvalidSocket;
            for (addrinfo* current = addresses; current != nullptr; current = current->ai_next)
            {
                SocketHandle candidate = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
                if (candidate == InvalidSocket)
                    continue;

                int reuse = 1;
#if defined(_WIN32)
                setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
                setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

                if (bind(candidate, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0
                    && listen(candidate, SOMAXCONN) == 0)
                {
                    listener = candidate;
                    break;
                }

                closeSocketHandle(candidate);
            }

            if (listener == InvalidSocket)
            {
                mRunning = false;
                return;
            }

            {
                std::lock_guard lock(mStateMutex);
                mListenSocket = listener;
            }

            while (mRunning)
            {
                sockaddr_storage clientAddr{};
                socklen_t clientLen = static_cast<socklen_t>(sizeof(clientAddr));
                SocketHandle client = accept(listener, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);

                if (client == InvalidSocket)
                {
                    if (!mRunning)
                        break;
                    continue;
                }

                SocketGuard clientGuard(client);

                HttpRequest request{};
                if (!receiveServerRequest(clientGuard.get(), request))
                    continue;

                HttpResponse response = dispatch(request);
                const std::string rawResponse = serializeServerResponse(response);
                sendAll(clientGuard.get(), rawResponse);
            }
        }
        catch (...)
        {
            // Intentionally swallow server loop exceptions; caller can inspect state and restart.
        }

        std::lock_guard lock(mStateMutex);
        mRunning = false;
        closeSocketHandle(mListenSocket);
        mListenSocket = InvalidSocket;
    }

private:
    std::atomic<bool> mRunning{ false };
    std::thread mWorker;

    std::mutex mStateMutex;
    SocketHandle mListenSocket = InvalidSocket;
    std::uint16_t mPort = 0;
    std::string mBindAddress;

    std::mutex mRoutesMutex;
    std::unordered_map<std::string, HttpHandler> mRoutes;
    HttpHandler mNotFoundHandler;
};
}
