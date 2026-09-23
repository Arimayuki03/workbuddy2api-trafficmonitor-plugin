#include "settings.h"
#include "common.h"
#include "logger.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

namespace wb2 {

using nlohmann::json;

namespace {

// 落盘用的原子写：tmp + MOVEFILE_REPLACE_EXISTING（与 wb2api 服务端同风格）。
bool WriteFileAtomicW(const std::wstring& path, const std::string& content)
{
    std::wstring tmp = path + L".tmp";
    _wremove(tmp.c_str());
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
    get_str("service_dir", s.service_dir);
    get_int("port", s.port);
    get_int("poll_interval_sec", s.poll_interval_sec);
    get_int("admin_poll_sec", s.admin_poll_sec);
    get_int("credits_refresh_interval_min", s.credits_refresh_interval_min);
    get_int("show_mode", s.show_mode);
    get_bool("show_live_credits", s.show_live_credits);
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
    // 方向非 0/1 回落升序。
    get_int("acc_sort_col", s.acc_sort_col);
    get_int("acc_sort_dir", s.acc_sort_dir);
    if (s.acc_sort_col < -1 || s.acc_sort_col > 5) s.acc_sort_col = -1;
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
    s.service_dir = TrimW(s.service_dir);
    // 去掉可能带上的引号与结尾反斜杠
    if (!s.service_dir.empty() && s.service_dir.front() == L'"') {
        if (s.service_dir.back() == L'"') s.service_dir.pop_back();
        s.service_dir.erase(0, 1);
    }
    while (!s.service_dir.empty() && (s.service_dir.back() == L'\\' || s.service_dir.back() == L'/'))
        s.service_dir.pop_back();
    return s;
}

json SerializeSettings(const Settings& s)
{
    json j;
    j["service_dir"] = WideToUtf8(s.service_dir);
    j["port"] = s.port;
    j["poll_interval_sec"] = s.poll_interval_sec;
    j["admin_poll_sec"] = s.admin_poll_sec;
    j["credits_refresh_interval_min"] = s.credits_refresh_interval_min;
    j["show_mode"] = s.show_mode;
    j["show_live_credits"] = s.show_live_credits;
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
            MoveFileExW(path_.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
            LogW(L"settings: json 解析失败，已备份为 .bad 并回落默认: " + Utf8ToWide(e.what()));
        }
    }
    cur_.config_dir = config_dir;
    std::string dump = SerializeSettings(cur_).dump(2);
    WriteFileAtomicW(path_, dump);
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

std::string SettingsStore::CurrentApiKey()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_.service_dir.empty()) return {};
    std::wstring file = cur_.service_dir + L"\\config.json";
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
    key_file_ = file;
    key_mtime_ = mt;
    key_cached_ = key;
    return key;
}

} // namespace wb2
