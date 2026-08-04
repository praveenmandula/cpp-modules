module;

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

export module cppm.core.json;

export namespace json
{
struct Json
{
    using Array = std::vector<Json>;
    using Object = std::unordered_map<std::string, Json>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Storage value = nullptr;

    Json() = default;
    Json(std::nullptr_t) : value(nullptr) {}
    Json(bool v) : value(v) {}
    Json(double v) : value(v) {}
    Json(int v) : value(static_cast<double>(v)) {}
    Json(std::string v) : value(std::move(v)) {}
    Json(const char* v) : value(std::string(v != nullptr ? v : "")) {}
    Json(Array v) : value(std::move(v)) {}
    Json(Object v) : value(std::move(v)) {}

    [[nodiscard]] bool isNull() const { return std::holds_alternative<std::nullptr_t>(value); }
    [[nodiscard]] bool isBool() const { return std::holds_alternative<bool>(value); }
    [[nodiscard]] bool isNumber() const { return std::holds_alternative<double>(value); }
    [[nodiscard]] bool isString() const { return std::holds_alternative<std::string>(value); }
    [[nodiscard]] bool isArray() const { return std::holds_alternative<Array>(value); }
    [[nodiscard]] bool isObject() const { return std::holds_alternative<Object>(value); }

    [[nodiscard]] bool asBool() const { return std::get<bool>(value); }
    [[nodiscard]] double asNumber() const { return std::get<double>(value); }
    [[nodiscard]] const std::string& asString() const { return std::get<std::string>(value); }
    [[nodiscard]] const Array& asArray() const { return std::get<Array>(value); }
    [[nodiscard]] const Object& asObject() const { return std::get<Object>(value); }
    [[nodiscard]] Array& asArray() { return std::get<Array>(value); }
    [[nodiscard]] Object& asObject() { return std::get<Object>(value); }
};

inline constexpr bool available = true;

namespace detail
{
    class Parser
    {
    public:
        explicit Parser(std::string_view text) : mText(text) {}

        Json parseDocument()
        {
            skipWhitespace();
            Json v = parseValue();
            skipWhitespace();
            if (!isEnd())
                fail("Unexpected trailing content");
            return v;
        }

    private:
        [[noreturn]] void fail(const char* message) const
        {
            throw std::runtime_error(std::string(message) + " at position " + std::to_string(mPos));
        }

        bool isEnd() const
        {
            return mPos >= mText.size();
        }

        char peek() const
        {
            return isEnd() ? '\0' : mText[mPos];
        }

        char take()
        {
            if (isEnd())
                fail("Unexpected end of input");
            return mText[mPos++];
        }

        bool takeIf(char c)
        {
            if (peek() == c)
            {
                ++mPos;
                return true;
            }
            return false;
        }

        void expect(char c)
        {
            if (!takeIf(c))
                fail("Unexpected character");
        }

        void skipWhitespace()
        {
            while (!isEnd() && std::isspace(static_cast<unsigned char>(peek())) != 0)
                ++mPos;
        }

        void expectKeyword(std::string_view keyword)
        {
            for (char c : keyword)
            {
                if (take() != c)
                    fail("Invalid keyword");
            }
        }

        static void appendUtf8(std::string& out, std::uint32_t cp)
        {
            if (cp <= 0x7F)
            {
                out.push_back(static_cast<char>(cp));
                return;
            }

            if (cp <= 0x7FF)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                return;
            }

            if (cp <= 0xFFFF)
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                return;
            }

            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }

        static int hexDigit(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        }

        std::uint32_t parseHex4()
        {
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i)
            {
                const int h = hexDigit(take());
                if (h < 0)
                    fail("Invalid unicode escape");
                value = (value << 4) | static_cast<std::uint32_t>(h);
            }
            return value;
        }

        std::string parseString()
        {
            expect('"');
            std::string out;

            while (!isEnd())
            {
                const char c = take();
                if (c == '"')
                    return out;

                if (static_cast<unsigned char>(c) < 0x20)
                    fail("Invalid control character in string");

                if (c != '\\')
                {
                    out.push_back(c);
                    continue;
                }

                const char esc = take();
                switch (esc)
                {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                {
                    std::uint32_t cp = parseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF)
                    {
                        if (!(takeIf('\\') && takeIf('u')))
                            fail("Invalid surrogate pair");
                        const std::uint32_t low = parseHex4();
                        if (low < 0xDC00 || low > 0xDFFF)
                            fail("Invalid surrogate pair");
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    fail("Invalid escape sequence");
                }
            }

            fail("Unterminated string");
        }

        Json parseNumber()
        {
            const std::size_t start = mPos;

            takeIf('-');
            if (takeIf('0'))
            {
                // no-op
            }
            else
            {
                if (!std::isdigit(static_cast<unsigned char>(peek())))
                    fail("Invalid number");
                while (std::isdigit(static_cast<unsigned char>(peek())))
                    ++mPos;
            }

            if (takeIf('.'))
            {
                if (!std::isdigit(static_cast<unsigned char>(peek())))
                    fail("Invalid fraction in number");
                while (std::isdigit(static_cast<unsigned char>(peek())))
                    ++mPos;
            }

            if (peek() == 'e' || peek() == 'E')
            {
                ++mPos;
                if (peek() == '+' || peek() == '-')
                    ++mPos;
                if (!std::isdigit(static_cast<unsigned char>(peek())))
                    fail("Invalid exponent in number");
                while (std::isdigit(static_cast<unsigned char>(peek())))
                    ++mPos;
            }

            const std::string numText(mText.substr(start, mPos - start));
            char* end = nullptr;
            const double value = std::strtod(numText.c_str(), &end);
            if (end == nullptr || *end != '\0' || !std::isfinite(value))
                fail("Invalid numeric value");
            return Json(value);
        }

        Json parseArray()
        {
            expect('[');
            skipWhitespace();

            Json::Array out;
            if (takeIf(']'))
                return Json(std::move(out));

            while (true)
            {
                skipWhitespace();
                out.push_back(parseValue());
                skipWhitespace();
                if (takeIf(']'))
                    break;
                expect(',');
            }

            return Json(std::move(out));
        }

        Json parseObject()
        {
            expect('{');
            skipWhitespace();

            Json::Object out;
            if (takeIf('}'))
                return Json(std::move(out));

            while (true)
            {
                skipWhitespace();
                if (peek() != '"')
                    fail("Object key must be string");
                std::string key = parseString();

                skipWhitespace();
                expect(':');
                skipWhitespace();
                out[std::move(key)] = parseValue();

                skipWhitespace();
                if (takeIf('}'))
                    break;
                expect(',');
            }

            return Json(std::move(out));
        }

        Json parseValue()
        {
            skipWhitespace();
            switch (peek())
            {
            case '"': return Json(parseString());
            case '{': return parseObject();
            case '[': return parseArray();
            case 't': expectKeyword("true"); return Json(true);
            case 'f': expectKeyword("false"); return Json(false);
            case 'n': expectKeyword("null"); return Json(nullptr);
            default:
                if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek())))
                    return parseNumber();
                fail("Invalid JSON value");
            }
        }

    private:
        std::string_view mText;
        std::size_t mPos = 0;
    };

    std::string escapeString(std::string_view in)
    {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    const char* digits = "0123456789abcdef";
                    const unsigned char uc = static_cast<unsigned char>(c);
                    out += "\\u00";
                    out.push_back(digits[(uc >> 4) & 0x0F]);
                    out.push_back(digits[uc & 0x0F]);
                }
                else
                {
                    out.push_back(c);
                }
            }
        }
        return out;
    }

    void writeJson(const Json& value, std::ostringstream& out, int indent, int depth)
    {
        auto writeIndent = [&](int d)
            {
                if (indent <= 0)
                    return;
                for (int i = 0; i < d * indent; ++i)
                    out.put(' ');
            };

        if (value.isNull())
        {
            out << "null";
            return;
        }
        if (value.isBool())
        {
            out << (value.asBool() ? "true" : "false");
            return;
        }
        if (value.isNumber())
        {
            std::ostringstream num;
            num.precision(std::numeric_limits<double>::max_digits10);
            num << value.asNumber();
            out << num.str();
            return;
        }
        if (value.isString())
        {
            out << '"' << escapeString(value.asString()) << '"';
            return;
        }
        if (value.isArray())
        {
            const auto& arr = value.asArray();
            out << '[';
            if (!arr.empty())
            {
                if (indent > 0)
                    out << '\n';
                for (std::size_t i = 0; i < arr.size(); ++i)
                {
                    if (indent > 0)
                        writeIndent(depth + 1);
                    writeJson(arr[i], out, indent, depth + 1);
                    if (i + 1 < arr.size())
                        out << ',';
                    if (indent > 0)
                        out << '\n';
                }
                if (indent > 0)
                    writeIndent(depth);
            }
            out << ']';
            return;
        }

        const auto& obj = value.asObject();
        out << '{';
        if (!obj.empty())
        {
            if (indent > 0)
                out << '\n';

            std::size_t index = 0;
            for (const auto& [k, v] : obj)
            {
                if (indent > 0)
                    writeIndent(depth + 1);
                out << '"' << escapeString(k) << '"' << (indent > 0 ? ": " : ":");
                writeJson(v, out, indent, depth + 1);
                if (++index < obj.size())
                    out << ',';
                if (indent > 0)
                    out << '\n';
            }

            if (indent > 0)
                writeIndent(depth);
        }
        out << '}';
    }
}

inline Json parse(std::string_view text)
{
    return detail::Parser(text).parseDocument();
}

inline std::string stringify(const Json& value, int indent = -1)
{
    std::ostringstream out;
    detail::writeJson(value, out, indent, 0);
    return out.str();
}

inline std::string minify(std::string_view text)
{
    return stringify(parse(text), -1);
}

inline std::string pretty(std::string_view text, int indent = 2)
{
    return stringify(parse(text), indent);
}
}
