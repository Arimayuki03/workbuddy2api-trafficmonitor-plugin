// http.cpp — WinHTTP 实现。错误文案区分三类，状态机按类别决定 保持旧态/判停/判异常：
//   - 拒绝连接/网络不可达 → "服务不在"（可判 STOPPED）
//   - 超时 → "慢/无响应"（保持旧态，防抖）
//   - 其他（DNS、代理等）→ 异常（保持旧态）
#include "http.h"
#include "common.h"
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace wb2 {
namespace {

const wchar_t* WinHttpErrText(DWORD code)
{
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT:            return L"请求超时";
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_INTERNAL_ERROR:     return L"连接被拒绝（端口未监听）";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:  return L"地址解析失败";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return L"响应格式异常";
    default: return nullptr;
    }
}

// 查询自定义头（WINHTTP_QUERY_CUSTOM 要求显式头名）。
std::wstring QueryHeaderCustom(HINTERNET req, LPCWSTR name)
{
    wchar_t buf[256]{};
    DWORD size = sizeof buf;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, buf, &size, nullptr)) return L"";
    return TrimW(buf);
}

} // namespace

HttpResponse HttpJson(LPCWSTR method, const std::wstring& url,
    const std::string& bearer, const std::string& body_utf8, DWORD timeout_ms)
{
    HttpResponse out;
    // URL 拆解
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[128]{}, path[512]{}, extra[128]{};
    uc.lpszHostName = host; uc.dwHostNameLength = 127;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 511;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 127;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        out.err = L"URL 解析失败";
        return out;
    }

    HINTERNET session = WinHttpOpen(L"WorkBuddy2ApiTMPlugin/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { out.err = L"WinHTTP 初始化失败"; return out; }
    WinHttpSetTimeouts(session, 2000, 2000, static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));

    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) {
        out.err = L"无法建立连接";
        WinHttpCloseHandle(session);
        return out;
    }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    std::wstring full_path = path + std::wstring(extra);
    HINTERNET req = WinHttpOpenRequest(conn, method, full_path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        out.err = L"无法创建请求";
        WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return out;
    }

    bool sent = false;
    if (bearer.empty() && body_utf8.empty()) {
        sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    } else {
        std::wstring headers;
        if (!bearer.empty()) headers += L"Authorization: Bearer " + Utf8ToWide(bearer) + L"\r\n";
        if (!body_utf8.empty()) headers += L"Content-Type: application/json; charset=utf-8\r\n";
        const void* body = body_utf8.empty() ? WINHTTP_NO_REQUEST_DATA : body_utf8.data();
        DWORD blen = static_cast<DWORD>(body_utf8.size());
        sent = WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(headers.size()),
            const_cast<void*>(body), blen, blen, 0);
    }

    DWORD we = 0;
    if (sent) sent = WinHttpReceiveResponse(req, nullptr);
    if (!sent) {
        we = GetLastError();
        std::wstring text = WinHttpErrText(we) ? WinHttpErrText(we) : WideFormat(L"网络错误 %lu", we);
        out.err = text + L" <- " + method + L" " + url;
        // 记录"拒绝连接"类供状态机判定：err 前缀即语义（中文文案被 UI 直接使用）。
        out.status = 0;
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return out;
    }

    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    out.status = status;
    out.x_service = QueryHeaderCustom(req, L"X-Service");

    // 读满响应体（上限 4MB 防御）
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        if (out.body.size() > 4u << 20) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
        chunk.resize(got);
        out.body += chunk;
    }

    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
    return out;
}

bool HttpErrIsUnreachable(const std::wstring& err)
{
    return err.find(L"连接被拒绝") != std::wstring::npos ||
        err.find(L"无法建立连接") != std::wstring::npos;
}

} // namespace wb2
