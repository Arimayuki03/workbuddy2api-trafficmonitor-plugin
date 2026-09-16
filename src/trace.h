// trace.h — 排查辅助：接口调用追踪（见 trace.cpp）。随排查移除。
#pragma once

namespace wb2::trace {

// WB2API_TRACE=1 时开启；写 WB2API_TRACE_DIR\wb2api_trace.log
void Init();
bool Active();                         // Init 成功开启后为 true
void Write(const char* what);          // 只在已 Init 后生效
void ExceptRecord(const char* where, unsigned long code, void* addr);
void StackRecord(void* const* frames, unsigned count);

} // namespace wb2::trace

// 门控宏：非 trace 构建下为空操作
#ifdef WB2API_TRACE_BUILD
#define WB2API_TRACE_LOG(msg) ::wb2::trace::Write(msg)
#else
#define WB2API_TRACE_LOG(msg) ((void)0)
#endif
