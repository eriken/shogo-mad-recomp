#include "engine/strings.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace shogo {

// ---------------------------------------------------------------------------
// FormatMessage-style formatting

namespace {
struct Insert {
    int index = 0;      // 1..99
    std::string spec;   // printf conversion, e.g. "s", "d", "5.2f"
};

// Parses an insert starting just after '%' at fmt[p]. On success sets ins and
// returns the position after the insert; returns 0 if it isn't a numbered insert.
size_t parseInsert(const std::string &fmt, size_t p, Insert &ins)
{
    if (p >= fmt.size() || !isdigit((unsigned char)fmt[p]) || fmt[p] == '0') return 0;
    int n = fmt[p++] - '0';
    if (p < fmt.size() && isdigit((unsigned char)fmt[p])) n = n * 10 + (fmt[p++] - '0');
    ins.index = n;
    ins.spec = "s";
    if (p < fmt.size() && fmt[p] == '!') {
        size_t end = fmt.find('!', p + 1);
        if (end != std::string::npos) {
            ins.spec = fmt.substr(p + 1, end - p - 1);
            p = end + 1;
        }
    }
    return p;
}

enum class ArgKind { Int, Long, Double, String };

ArgKind kindOf(const std::string &spec)
{
    char conv = spec.empty() ? 's' : spec.back();
    switch (conv) {
    case 's': case 'S': return ArgKind::String;
    case 'f': case 'e': case 'E': case 'g': case 'G': return ArgKind::Double;
    default: return spec.find('l') != std::string::npos ? ArgKind::Long : ArgKind::Int;
    }
}

struct Arg {
    ArgKind kind = ArgKind::Int;
    long long i = 0;
    double d = 0;
    const char *s = nullptr;
};
}  // namespace

std::string formatMessage(const std::string &fmt, va_list args)
{
    // Pass 1: collect each insert's type so arguments can be read in index order.
    std::map<int, std::string> specs;
    for (size_t p = 0; p < fmt.size(); ++p) {
        if (fmt[p] != '%') continue;
        Insert ins;
        if (size_t next = parseInsert(fmt, p + 1, ins)) {
            specs.emplace(ins.index, ins.spec);
            p = next - 1;
        } else {
            ++p;  // skip the escaped character
        }
    }
    int maxIndex = specs.empty() ? 0 : specs.rbegin()->first;
    std::vector<Arg> argv(size_t(maxIndex) + 1);
    va_list ap;
    va_copy(ap, args);
    for (int i = 1; i <= maxIndex; ++i) {
        auto it = specs.find(i);
        Arg &a = argv[size_t(i)];
        a.kind = it == specs.end() ? ArgKind::Int : kindOf(it->second);
        switch (a.kind) {
        case ArgKind::Int: a.i = va_arg(ap, int); break;
        case ArgKind::Long: a.i = va_arg(ap, long); break;
        case ArgKind::Double: a.d = va_arg(ap, double); break;
        case ArgKind::String: a.s = va_arg(ap, const char *); break;
        }
    }
    va_end(ap);

    // Pass 2: produce the output.
    std::string out;
    for (size_t p = 0; p < fmt.size(); ++p) {
        char c = fmt[p];
        if (c != '%' || p + 1 >= fmt.size()) {
            out.push_back(c);
            continue;
        }
        Insert ins;
        if (size_t next = parseInsert(fmt, p + 1, ins)) {
            const Arg &a = argv[size_t(ins.index)];
            std::string spec = "%" + ins.spec;
            char buf[512];
            switch (a.kind) {
            case ArgKind::String: snprintf(buf, sizeof(buf), spec.c_str(), a.s ? a.s : "(null)"); break;
            case ArgKind::Double: snprintf(buf, sizeof(buf), spec.c_str(), a.d); break;
            case ArgKind::Long: snprintf(buf, sizeof(buf), spec.c_str(), (long)a.i); break;
            case ArgKind::Int: snprintf(buf, sizeof(buf), spec.c_str(), (int)a.i); break;
            }
            out += buf;
            p = next - 1;
            continue;
        }
        char e = fmt[++p];
        switch (e) {
        case '0': return out;  // end of message, no trailing newline
        case 'n': out += "\r\n"; break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        default: out.push_back(e); break;  // %% %. %! "% " and anything else: the character itself
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// HSTRING objects

struct EngineStrings::Str {
    uint32_t magic = kMagic;
    int refs = 1;
    std::string text;
    static constexpr uint32_t kMagic = 0x53545231;  // "STR1"
};

namespace {
template <class S> S *asStr(HSTRING h)
{
    auto *s = reinterpret_cast<S *>(h);
    return (s && s->magic == S::kMagic) ? s : nullptr;
}
}

EngineStrings::EngineStrings(std::unique_ptr<PEResources> resources, std::string side)
    : m_resources(std::move(resources)), m_side(std::move(side))
{
}

EngineStrings::~EngineStrings()
{
    if (m_live) fprintf(stderr, "[%s strings] %zu string(s) were never freed\n", m_side.c_str(), m_live);
}

HSTRING EngineStrings::create(const char *text)
{
    Str *s = new Str;
    s->text = text ? text : "";
    ++m_live;
    return reinterpret_cast<HSTRING>(s);
}

HSTRING EngineStrings::format(int messageCode, va_list args)
{
    std::optional<std::string> fmt = m_resources ? m_resources->string((uint32_t)messageCode) : std::nullopt;
    if (!fmt) {
        fprintf(stderr, "[%s strings] FormatString: no string resource %d\n", m_side.c_str(), messageCode);
        return nullptr;
    }
    std::string text = formatMessage(*fmt, args);
    return create(text.c_str());
}

HSTRING EngineStrings::copy(HSTRING h)
{
    if (Str *s = asStr<Str>(h)) ++s->refs;
    return h;
}

void EngineStrings::free(HSTRING h)
{
    Str *s = asStr<Str>(h);
    if (!s) return;
    if (--s->refs == 0) {
        s->magic = 0;
        delete s;
        --m_live;
    }
}

char *EngineStrings::data(HSTRING h) const
{
    Str *s = asStr<Str>(h);
    return s ? s->text.data() : nullptr;
}

bool EngineStrings::equal(HSTRING a, HSTRING b, bool ignoreCase) const
{
    Str *sa = asStr<Str>(a), *sb = asStr<Str>(b);
    if (!sa || !sb) return sa == sb;
    return ignoreCase ? strcasecmp(sa->text.c_str(), sb->text.c_str()) == 0 : sa->text == sb->text;
}

const char *EngineStrings::textResource(const std::string &name)
{
    auto it = m_texts.find(name);
    if (it == m_texts.end()) {
        auto raw = m_resources ? m_resources->named("TEXT", name) : std::nullopt;
        if (!raw) return nullptr;
        std::vector<char> text(raw->begin(), raw->end());
        text.push_back('\0');
        it = m_texts.emplace(name, std::move(text)).first;
    }
    return it->second.data();
}

// ---------------------------------------------------------------------------
// Binding to the C function tables

namespace {
EngineStrings *g_client = nullptr;
EngineStrings *g_server = nullptr;

template <EngineStrings *&S> struct Bind {
    static HSTRING format(int code, ...)
    {
        va_list ap;
        va_start(ap, code);
        HSTRING h = S->format(code, ap);
        va_end(ap);
        return h;
    }
    static HSTRING copy(HSTRING h) { return S->copy(h); }
    static HSTRING create(char *text) { return S->create(text); }
    static void free(HSTRING h) { S->free(h); }
    static DBOOL compare(HSTRING a, HSTRING b) { return S->equal(a, b, false) ? DTRUE : DFALSE; }
    static DBOOL compareUpper(HSTRING a, HSTRING b) { return S->equal(a, b, true) ? DTRUE : DFALSE; }
    static char *data(HSTRING h) { return S->data(h); }

    template <class API> static void install(API *api)
    {
        api->FormatString = format;
        api->CopyString = copy;
        api->CreateString = create;
        api->FreeString = free;
        api->CompareStrings = compare;
        api->CompareStringsUpper = compareUpper;
        api->GetStringData = data;
    }
};
}  // namespace

void installClientStrings(ClientDE *client, EngineStrings *strings)
{
    g_client = strings;
    Bind<g_client>::install(client);
}

void installServerStrings(ServerDE *server, EngineStrings *strings)
{
    g_server = strings;
    Bind<g_server>::install(server);
}

}  // namespace shogo
