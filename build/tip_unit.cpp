// tip_unit.cpp — FitTipBudget/JoinLines/TipUidHidden 单元测试（匿名 namespace 函数，经 include worker.cpp 访问）。
// Base() 总长 269；预算分档：265=删模型用量区 / 210=再删积分汇总 / 100=只剩骨架。
#include "../src/worker.cpp"
#include <cstdio>

using namespace wb2;

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else printf("pass: %s\n", msg); \
} while (0)

static std::vector<std::wstring> Base()
{
    // 模拟 BuildDisplayLocked 的行序：骨架 + 各区
    return {
        L"WorkBuddy2API：运行中（PID 1234）",        // 骨架
        L"地址：http://127.0.0.1:7863",              // 骨架
        L"健康 6/8 · 冷却 1 · 禁用 1 · 粘性会话 0",  // 骨架
        L"—— 账户 ——",
        L"  张三 (cn) 正常 实时:12,345",
        L"  李四 (cn) 请求中 glm-4.6×2 实时:2,345",
        L"—— 定时任务 ——",
        L"  签到[启用 下次09:00]",
        L"实时积分：总剩 14,690（14:00 查询；下次可查 14:10）",
        L"—— 模型用量（本次运行累计）——",
        L"  glm-4.6：12.3分 / 45次（均 0.27/次）",
        L"单击此栏位打开设置",                       // 骨架尾注
    };
}

int main()
{
    // 0. 当前预算常量 = 700（与原版完整形态对齐）
    CHECK(kTipBudget == 700, "kTipBudget == 700");

    // 1. 预算内原样保留
    {
        auto lines = Base();
        std::wstring out = FitTipBudget(lines, 10000);
        CHECK(out.find(L"—— 模型用量") != std::wstring::npos, "预算内不删任何行（含末区）");
        CHECK(out.find(L"glm-4.6：12.3分") != std::wstring::npos, "预算内末区明细保留");
        CHECK(out.find(L"…") == std::wstring::npos, "预算内无省略提示");
    }
    // 2. 预算 265（总长 281 超出）：删尾部模型用量区（明细+标题=46），积分汇总保留
    {
        auto lines = Base();
        std::wstring out = FitTipBudget(lines, 265);
        CHECK(out.size() <= 265, "[265] 收敛到预算内");
        CHECK(out.find(L"WorkBuddy2API：") != std::wstring::npos, "[265] 状态头保留");
        CHECK(out.find(L"地址：") != std::wstring::npos, "[265] 地址行保留");
        CHECK(out.find(L"单击此栏位打开设置") != std::wstring::npos, "[265] 尾注保留");
        CHECK(out.find(L"glm-4.6：12.3分") == std::wstring::npos, "[265] 模型用量明细被删");
        CHECK(out.find(L"—— 模型用量") == std::wstring::npos, "[265] 模型用量标题随删");
        CHECK(out.find(L"实时积分：总剩") != std::wstring::npos, "[265] 积分汇总保留");
        CHECK(out.find(L"…") != std::wstring::npos, "[265] 有省略提示");
    }
    // 3. 预算 210：再删积分汇总（35）→ 200，定时任务保留
    {
        auto lines = Base();
        std::wstring out = FitTipBudget(lines, 210);
        CHECK(out.size() <= 210, "[210] 收敛到预算内");
        CHECK(out.find(L"实时积分：总剩") == std::wstring::npos, "[210] 积分汇总被删");
        CHECK(out.find(L"签到") != std::wstring::npos, "[210] 定时任务保留");
    }
    // 4. 预算 100：删到只剩骨架（账户区成员删完后标题随删，不留悬空标题）
    {
        auto lines = Base();
        std::wstring out = FitTipBudget(lines, 100);
        CHECK(out.size() <= 100, "[100] 收敛到预算内");
        CHECK(out.find(L"WorkBuddy2API：") != std::wstring::npos, "[100] 状态头保留");
        CHECK(out.find(L"健康 6/8") != std::wstring::npos, "[100] 健康概要保留");
        CHECK(out.find(L"单击此栏位打开设置") != std::wstring::npos, "[100] 尾注保留");
        CHECK(out.find(L"——") == std::wstring::npos, "[100] 无悬空节标题");
        CHECK(out.find(L"张三") == std::wstring::npos && out.find(L"签到") == std::wstring::npos,
            "[100] 全部明细舍弃");
    }
    // 5. 只有骨架仍超预算 → 硬截兜底（构造一个超长骨架行）
    {
        std::vector<std::wstring> lines = {
            L"WorkBuddy2API：" + std::wstring(600, L'长'),
            L"单击此栏位打开设置",
        };
        std::wstring out = FitTipBudget(lines, 100);
        CHECK(out.size() <= 100, "[硬截] 超长骨架收敛到预算内");
        CHECK(!out.empty() && out.back() == L'…', "[硬截] 以省略号结尾");
    }
    // 6. JoinLines 用 \r\n
    {
        std::wstring out = JoinLines({ L"a", L"b" });
        CHECK(out == L"a\r\nb", "JoinLines 用 \\r\\n 连接");
    }
    // 7. TipUidHidden：单账户显隐查找
    {
        Settings st;
        st.tip_hidden_uids = { L"abcd1234", L"efef5678" };
        CHECK(TipUidHidden(st, L"abcd1234"), "TipUidHidden 命中列表内 uid");
        CHECK(!TipUidHidden(st, L"other9999"), "TipUidHidden 列表外 uid 不命中");
        Settings empty;
        CHECK(!TipUidHidden(empty, L"abcd1234"), "TipUidHidden 空列表不命中");
    }
    printf(fails ? "RESULT: %d FAIL\n" : "RESULT: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
