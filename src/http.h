// http.h — WinHTTP 同步封装（只允许在 worker/action 线程调用，UI 线程禁止）。
// 目标恒为本机 http://127.0.0.1:port —— 超时收紧、无 TLS、无连接复用（每次 30s 一级，
// 会话重建开销可忽略，换来零跨线程句柄共享）。
#pragma once
#include <string>
#include <windows.h>

namespace wb2 {

// 传输层失败分类（状态机据此判停/防抖；与展示文案 err 严格分离，改文案不再影响分类）。
enum class TransportError {
    None,         // 拿到了 HTTP 状态码（含 4xx/5xx），没有传输层失败
    Unreachable,  // 连接被拒/无法建立连接：端口没监听（可判"服务不在"）
    Timeout,      // 请求超时：进程慢或僵死（保持旧态防抖）
    Other,        // 其余（DNS、URL 非法、代理等）：保持旧态
};

struct HttpResponse {
    DWORD status = 0;        // HTTP 状态码；0 = 传输层失败
    std::string body;        // UTF-8 响应体（503/4xx 也照常有 JSON——调用方先 parse 再判码）
    std::wstring x_service;  // X-Service 头（服务身份校验用）
    std::wstring err;        // 传输层错误的中文描述（status=0 时有值）
    // 传输层失败分类（状态机用；err 只是给人看的文案）
    TransportError transport = TransportError::None;

    bool ok() const { return status != 0; }
};

// method：L"GET"/L"POST"/L"PATCH"；bearer 空则不发鉴权头；body_utf8 空则无请求体。
// timeout_ms：连接/发送/接收共用。
HttpResponse HttpJson(LPCWSTR method, const std::wstring& url,
    const std::string& bearer, const std::string& body_utf8, DWORD timeout_ms);

} // namespace wb2
