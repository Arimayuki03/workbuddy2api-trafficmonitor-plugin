#include "settings.h"
#include "common.h"
#include "logger.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

namespace wb2 {

using nlohmann::json;

namespace {

// 落盘用的原子写：tmp + MOVEFILE_REPLACE_EXISTING（与 wb2api 服务端同风格）。
// 临时名拼入当前线程 id：worker 线程 SetUserStopped 与 UI 线程列头排序点击都会在
// 锁外调 Modify 落盘，固定 tmp 名 + 独占打开会让后到者 CreateFileW 直接失败、
// 丢一次持久化；线程 id 区分并发写方后各写各的 tmp，最终仍靠 REPLACE_EXISTING
// 原子落到同一目标（后写覆盖先写，属预期）。
bool WriteFileAtomicW(const std::wstring& path, const std::string& content)
{
    std::wstring tmp = WideFormat(L"%s.tmp%lu", path.c_str(), GetCurrentThreadId());
    _wremove(tmp.c_str()); // 不存在即正常，返回值可忽略
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    DWORD written = 0;
    ok = WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
        written == content.size();
    ok = CloseHandle(h) && ok;
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) _wremove(tmp.c_str());
    return ok;
}

Settings ParseSettings(const json& j)
{
    Settings s;
    auto get_str = [&j](const char* k, std::wstring& dst) {
        if (j.contains(k) && j[k].is_string()) dst = Utf8ToWide(j[k].get<std::string>());
    };
    auto get_int = [&j](const char* k, int& dst) {
        if (j.contains(k) && j[k].is_number_integer()) dst = j[k].get<int>();
    };
    auto get_bool = [&j](const char* k, bool& dst) {
        if (j.contains(k) && j[k].is_boolean()) dst = j[k].get<bool>();
    };
    auto get_utf8 = [&j](const char* k, std::string& dst) {
        if (j.contains(k) && j[k].is_string()) dst = j[k].get<std::string>();
    };
    {
        int dm = static_cast<int>(s.deploy_mode);
        get_int("deploy_mode", dm);
        if (dm == DM_DOCKER) s.deploy_mode = DM_DOCKER;
        else if (dm != DM_LOCAL) {
            LogW(WideFormat(L"settings: deploy_mode=%d 非法，回落本地模式", dm));
            s.deploy_mode = DM_LOCAL;
        }
    }
    get_str("server_url", s.server_url);
    get_utf8("api_key", s.api_key);
    get_str("auths_dir", s.auths_dir);
    get_str("service_dir", s.service_dir);
    get_int("port", s.port);
    get_int("poll_interval_sec", s.poll_interval_sec);
    get_int("admin_poll_sec", s.admin_poll_sec);
    get_int("credits_refresh_interval_min", s.credits_refresh_interval_min);
    get_int("show_mode", s.show_mode);
    get_bool("show_live_credits", s.show_live_credits);
    get_bool("show_concurrency", s.show_concurrency);
    get_bool("tooltip_full", s.tooltip_full);
    get_bool("tooltip_accounts", s.tooltip_accounts);
    get_bool("tooltip_tasks", s.tooltip_tasks);
    // tip_hidden_uids：字符串数组（可选，旧配置没有即空列表=全部显示）。
    // 去重 + 丢空串：手改 json 或异常路径不会留下重复/无效条目。
    if (j.contains("tip_hidden_uids") && j["tip_hidden_uids"].is_array())
        for (const auto& e : j["tip_hidden_uids"]) {
            if (!e.is_string()) continue;
            std::wstring uid = Utf8ToWide(e.get<std::string>());
            if (uid.empty()) continue;
            if (std::find(s.tip_hidden_uids.begin(), s.tip_hidden_uids.end(), uid) ==
                s.tip_hidden_uids.end())
                s.tip_hidden_uids.push_back(std::move(uid));
        }
    get_bool("autostart_task", s.autostart_task);
    get_bool("start_with_tm", s.start_with_tm);
    get_bool("auto_relaunch", s.auto_relaunch);
    get_bool("logging", s.logging);
    get_bool("user_stopped", s.user_stopped);
    // acc_sort_col/acc_sort_dir：账户表列头排序持久化（可选，旧配置没有即 -1=未排序）。
    // 防呆：列号超出当前列范围（手改 json / 列序调整后的旧配置）回落未排序；
    // 方向非 0/1 回落升序。上界用 kAccSortColMax（settings.h 单一来源，与账户表列数同步）。
    get_int("acc_sort_col", s.acc_sort_col);
    get_int("acc_sort_dir", s.acc_sort_dir);
    if (s.acc_sort_col < -1 || s.acc_sort_col > kAccSortColMax) s.acc_sort_col = -1;
    if (s.acc_sort_dir != 0 && s.acc_sort_dir != 1) s.acc_sort_dir = 0;
    // 数值归一（用户手改 json 防呆）：全部回落默认，不给 0/负值留运行期除零或疯狂轮询的口子。
    if (s.port <= 0 || s.port > 65535) s.port = 7863;
    if (s.poll_interval_sec < 10) s.poll_interval_sec = 30;
    if (s.poll_interval_sec > 600) s.poll_interval_sec = 600;
    if (s.admin_poll_sec < 15) s.admin_poll_sec = 60;
    if (s.credits_refresh_interval_min < 0)
        s.credits_refresh_interval_min = 0; // 负值=关：钳到 0，不意外打开自动查询（审查修复）
    else if (s.credits_refresh_interval_min != 0 && s.credits_refresh_interval_min < 1)
        s.credits_refresh_interval_min = 1; // 下限 1 分钟（服务端冷却兜底防风控）
    if (s.credits_refresh_interval_min > 1440) s.credits_refresh_interval_min = 1440;
    if (s.show_mode < 0 || s.show_mode > 2) s.show_mode = SM_STATE_ACCOUNT;
    // 目录/URL/密钥统一走共享归一函数（与设置对话框保存同一份实现）。
    s.service_dir = NormalizeLocalDir(std::move(s.service_dir));
    s.auths_dir = NormalizeLocalDir(std::move(s.auths_dir));
    s.server_url = NormalizeServiceUrl(std::move(s.server_url));
    s.api_key = SanitizeApiKey(s.api_key);
    return s;
}

json SerializeSettings(const Settings& s)
{
    json j;
    j["deploy_mode"] = static_cast<int>(s.deploy_mode);
    j["server_url"] = WideToUtf8(s.server_url);
    j["api_key"] = s.api_key;
    j["auths_dir"] = WideToUtf8(s.auths_dir);
    j["service_dir"] = WideToUtf8(s.service_dir);
    j["port"] = s.port;
    j["poll_interval_sec"] = s.poll_interval_sec;
    j["admin_poll_sec"] = s.admin_poll_sec;
    j["credits_refresh_interval_min"] = s.credits_refresh_interval_min;
    j["show_mode"] = s.show_mode;
    j["show_live_credits"] = s.show_live_credits;
    j["show_concurrency"] = s.show_concurrency;
    j["tooltip_full"] = s.tooltip_full;
    j["tooltip_accounts"] = s.tooltip_accounts;
    j["tooltip_tasks"] = s.tooltip_tasks;
    {
        json arr = json::array();
        for (const auto& u : s.tip_hidden_uids) arr.push_back(WideToUtf8(u));
        j["tip_hidden_uids"] = arr;
    }
    j["autostart_task"] = s.autostart_task;
    j["start_with_tm"] = s.start_with_tm;
    j["auto_relaunch"] = s.auto_relaunch;
    j["logging"] = s.logging;
    j["user_stopped"] = s.user_stopped;
    j["acc_sort_col"] = s.acc_sort_col;
    j["acc_sort_dir"] = s.acc_sort_dir;
    return j;
}

} // namespace

std::wstring NormalizeLocalDir(std::wstring v)
{
    v = TrimW(std::move(v));
    // 只剥成对引号：资源管理器"复制为路径"会给整串加引号；半截引号保留原样，
    // 让 PathFileExists 失败时用户能看到真实输入，而不是被悄悄改坏。
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"')
        v = v.substr(1, v.size() - 2);
    // 盘符根（D:\）保留尾分隔符：去掉会变成 "D:"，语义从"根目录"变成
    // "D 盘当前目录"，PathFileExists/子目录拼接全都会错。
    const bool drive_root = v.size() == 3 && v[1] == L':' &&
        (v[2] == L'\\' || v[2] == L'/');
    if (!drive_root)
        while (!v.empty() && (v.back() == L'\\' || v.back() == L'/'))
            v.pop_back();
    return TrimW(std::move(v));
}

std::wstring NormalizeServiceUrl(std::wstring v)
{
    v = TrimW(std::move(v));
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"')
        v = v.substr(1, v.size() - 2);
    v = TrimW(std::move(v));
    // 粘贴常见的前导斜杠（//host:port）先剥掉，再补 scheme。
    while (!v.empty() && (v.front() == L'/' || v.front() == L'\\'))
        v.erase(0, 1);
    if (v.empty()) return L"";
    // 裸空白/控制字符直接判非法：它们会破坏 URL 或请求头，且合法 URL 本就不该含。
    for (wchar_t ch : v)
        if (ch <= 0x20 || ch == 0x7f) return L"";
    if (v.find(L"://") == std::wstring::npos) v = L"http://" + v;

    std::wstring low = v;
    std::transform(low.begin(), low.end(), low.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    size_t host_start = 0;
    if (low.rfind(L"http://", 0) == 0) host_start = 7;
    else if (low.rfind(L"https://", 0) == 0) host_start = 8;
    else return L""; // 只支持 http/https（WinHTTP 也只处理这两种）
    while (v.size() > host_start && (v.back() == L'/' || v.back() == L'\\'))
        v.pop_back();
    if (v.size() <= host_start) return L""; // 只有 "http://" 没有主机
    return v;
}

std::string SanitizeApiKey(const std::string& key)
{
    // 该值会原样拼进 Authorization: Bearer <key> 头（http.cpp）。内部 CR/LF 会
    // 拆出额外头（头注入），NUL/其他控制字符会让请求畸形——统一剔除。
    std::wstring w = Utf8ToWide(key);
    w.erase(std::remove_if(w.begin(), w.end(),
        [](wchar_t ch) { return ch < 0x20 || ch == 0x7f; }), w.end());
    return WideToUtf8(TrimW(std::move(w)));
}

std::wstring ServiceBaseUrl(const Settings& s)
{
    // Docker/远程模式绝不回落到本机：未填地址时返回空串，由调用点提示并短路。
    if (s.deploy_mode == DM_DOCKER) return s.server_url;
    return WideFormat(L"http://127.0.0.1:%d", s.port);
}

std::wstring AuthsDirectory(const Settings& s)
{
    if (s.deploy_mode == DM_DOCKER) {
        std::wstring dir = s.auths_dir;
        // 相对路径按插件配置目录解析（而不是 TrafficMonitor 的 CWD）：否则同一个
        // 配置启动方式不同就会扫到不同目录，且失败后毫无线索。
        if (!dir.empty() && PathIsRelativeW(dir.c_str())) {
            if (s.config_dir.empty()) return L"";
            dir = s.config_dir + L"\\" + dir;
        }
        return dir;
    }
    if (s.service_dir.empty()) return L"";
    return s.service_dir + L"\\auths";
}

SettingsStore& SettingsStore::Instance()
{
    static SettingsStore inst;
    return inst;
}

void SettingsStore::Init(const std::wstring& config_dir)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!path_.empty()) return; // 只认第一次注入的目录（TM 单次生命周期内稳定）
    cur_.config_dir = config_dir;
    path_ = config_dir + L"\\WorkBuddy2ApiPlugin.json";
    std::ifstream f(path_, std::ios::binary);
    if (f) {
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        try {
            json j = json::parse(raw);
            cur_ = ParseSettings(j);
            cur_.config_dir = config_dir;
            return;
        } catch (const std::exception& e) {
            // 坏 json：备份再回落默认，不静默吞掉用户配置
            std::wstring bad = path_ + L".bad";
            // MoveFileExW 可能因 .bad 被占用而失败：此时绝不能继续走到底部用默认
            // 配置覆盖用户的原 json。raw 还在内存里，退而把原文写进 .bad 保底，
            // 磁盘上保留用户原文件（内存用默认值即可运行）。
            if (!MoveFileExW(path_.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                if (!WriteFileAtomicW(bad, raw))
                    LogE(L"settings: 坏 json 备份 .bad 失败，用户原配置保留在磁盘未动");
                else
                    LogW(L"settings: 坏 json 备份 .bad 被占用，已将原文写入 .bad");
                LogW(L"settings: json 解析失败，回落默认配置: " + Utf8ToWide(e.what()));
                return; // 磁盘保留用户原文件，内存用默认值
            }
            LogW(L"settings: json 解析失败，已备份为 .bad 并回落默认: " + Utf8ToWide(e.what()));
        }
    }
    cur_.config_dir = config_dir;
    std::string dump = SerializeSettings(cur_).dump(2);
    // 兜底默认配置写回：失败只记日志，不影响内存默认值生效
    if (!WriteFileAtomicW(path_, dump))
        LogE(L"settings: 默认配置落盘失败");
}

Settings SettingsStore::Get() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return cur_;
}

void SettingsStore::Update(const Settings& s)
{
    Settings copy;
    std::wstring p;
    {
        std::lock_guard<std::mutex> lk(mu_);
        copy = s; // 以入参为基底（旧写法 copy = cur_ = s 右结合：cur_ 先被入参污染，config_dir 保护失效）
        copy.config_dir = cur_.config_dir; // 运行期注入字段以现值为准（入参可能是裸默认结构）
        cur_ = copy;
        p = path_;
    }
    if (p.empty()) return;
    // 锁外原子落盘；失败只记日志（内存值已生效，下次 Update 重试）
    if (!WriteFileAtomicW(p, SerializeSettings(copy).dump(2)))
        LogE(L"settings: 配置落盘失败");
}

Settings SettingsStore::Modify(const std::function<void(Settings&)>& fn)
{
    Settings copy;
    std::wstring p;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn(cur_);
        copy = cur_;
        p = path_;
    }
    if (p.empty()) return copy;
    if (!WriteFileAtomicW(p, SerializeSettings(copy).dump(2)))
        LogE(L"settings: 配置落盘失败");
    return copy;
}

std::string SettingsStore::ReadApiKeyLocked(const std::wstring& file)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    FILETIME mt{};
    BOOL mt_ok = GetFileTime(h, nullptr, nullptr, &mt);
    std::string content;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
        content.append(buf, got);
    CloseHandle(h);
    if (mt_ok && mt.dwLowDateTime == key_mtime_.dwLowDateTime &&
        mt.dwHighDateTime == key_mtime_.dwHighDateTime && file == key_file_)
        return key_cached_; // mtime 未变：吃缓存

    std::string key;
    try {
        json j = json::parse(content);
        if (j.contains("api_key") && j["api_key"].is_string())
            key = j["api_key"].get<std::string>();
    } catch (...) {
        key.clear();
    }
    key = SanitizeApiKey(key);
    key_file_ = file;
    key_mtime_ = mt;
    key_cached_ = key;
    return key;
}

std::string SettingsStore::CurrentApiKey()
{
    std::lock_guard<std::mutex> lk(mu_);
    // Docker/远程模式：容器内 config.json 不在本机，只能用手动填写的 key。
    // 留空时返回空串——/healthz 与 /status 若服务端未配 key 仍可用；配了 key
    // 则会得到 401，由各调用点给出"检查 API Key"提示。
    if (cur_.deploy_mode == DM_DOCKER) return SanitizeApiKey(cur_.api_key);
    if (cur_.service_dir.empty()) return {};
    return ReadApiKeyLocked(cur_.service_dir + L"\\config.json");
}

std::string SettingsStore::CurrentApiKey(const Settings& s)
{
    // 调用方已在同一份快照上算过 base URL：这里必须用同一份 s，不能再读全局
    // cur_，否则模式切换窗口会出现 base 与 key 来自不同部署方式。
    std::lock_guard<std::mutex> lk(mu_);
    if (s.deploy_mode == DM_DOCKER) return SanitizeApiKey(s.api_key);
    if (s.service_dir.empty()) return {};
    return ReadApiKeyLocked(s.service_dir + L"\\config.json");
}

} // namespace wb2
