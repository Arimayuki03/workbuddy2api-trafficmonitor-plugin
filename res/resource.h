// resource.h — 对话框与控件 ID（页面控件全部代码创建，资源里只有框架）。
#pragma once

#define IDD_SETTINGS            100

#define IDC_TAB                 1000

// ① 服务
#define IDC_LBL_STATE           1001
#define IDC_LBL_SUB             1002
#define IDC_EDT_DIR             1010
#define IDC_BTN_BROWSE          1011
#define IDC_EDT_PORT            1012
// 1013/1014 曾是 API Key 输入框/提示：v1.1 删除（鉴权自动读服务 config.json）
#define IDC_BTN_START           1020
#define IDC_BTN_STOP            1021
#define IDC_BTN_RESTART         1022
#define IDC_CHK_AUTOSTART       1030
#define IDC_CHK_TM              1031
#define IDC_CHK_RELAUNCH        1032
#define IDC_CHK_LOG             1033
#define IDC_EDT_POLL            1040
#define IDC_LBL_POLL            1041
#define IDC_LBL_ERR             1050
#define IDC_LBL_ACTION          1051

// ② 账户与积分
#define IDC_LST_ACC             1100
#define IDC_BTN_CRED            1110
#define IDC_LBL_CCOOL           1111
#define IDC_EDT_CINTERVAL       1112
#define IDC_LBL_CINTERVAL       1113
#define IDC_LBL_CNOTE           1114

// ③ 定时任务（每种任务的控件集合在代码里数组管理）
#define IDC_TASK_BASE           1200  // i*10+0 chk, +2 下次, +3 状态, +5 按钮, +6 时间输入框, +7 应用按钮
#define IDC_LBL_TASKWARN        1290
#define IDC_BTN_RUNALL          1291
#define IDC_LBL_TASKNOTE        1292

// ④ 显示
#define IDC_RAD_ACCOUNT         1300
#define IDC_RAD_CREDITS         1301
#define IDC_RAD_ONLY            1302
#define IDC_CHK_LIVECRD         1303
#define IDC_LBL_DEMO            1304
#define IDC_CHK_TIPFULL         1305

// ⑤ 高级
#define IDC_EDT_ADMIN           1400
#define IDC_LBL_ADMIN           1401
#define IDC_BTN_OPENCFG         1410
#define IDC_BTN_OPENSVC         1411
#define IDC_BTN_OPENLOG         1412
#define IDC_BTN_OPENPLOG        1413
#define IDC_LBL_VER             1420
#define IDC_LBL_ADMINSTAT       1421
