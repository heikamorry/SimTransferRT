#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <limits>

namespace {
constexpr GUID KernelThreadGuid{0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr unsigned CswitchOpcode = 36;
constexpr unsigned ReadyThreadOpcode = 50;

struct Context {
    DWORD target{};
    std::uint64_t freq{};
    std::uint64_t firstQpc = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t lastQpc = 0;
    std::uint64_t schedulerEvents = 0, selectedEvents = 0, shortRecords = 0;
};
std::uint64_t toUs(std::uint64_t qpc, std::uint64_t freq) {
    return (qpc / freq) * 1000000ULL + ((qpc % freq) * 1000000ULL) / freq;
}
std::uint32_t u32(const unsigned char* p) {
    std::uint32_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}
void writeHeader() {
    std::cout << "qpc,host_us,cpu,kind,target_tid,other_tid,header_pid,header_tid,thread_pid,old_state,old_wait_reason,ready_flags,version\n";
}
void emit(Context& ctx,const EVENT_RECORD& e,const char* kind,DWORD otherTid,
          DWORD threadPid,int oldState,int oldWaitReason,int readyFlags) {
    const auto raw = static_cast<std::uint64_t>(e.EventHeader.TimeStamp.QuadPart);
    ++ctx.selectedEvents;
    std::cout << raw << ',' << toUs(raw, ctx.freq) << ',' << GetEventProcessorIndex(&e) << ','
              << kind << ',' << ctx.target << ',' << otherTid << ',' << e.EventHeader.ProcessId << ','
              << e.EventHeader.ThreadId << ',' << threadPid << ',' << oldState << ',' << oldWaitReason
              << ',' << readyFlags << ',' << unsigned(e.EventHeader.EventDescriptor.Version) << '\n';
}
VOID WINAPI onEvent(PEVENT_RECORD e) {
    if (!e || !e->UserContext || !e->UserData || e->EventHeader.TimeStamp.QuadPart <= 0 ||
        !IsEqualGUID(e->EventHeader.ProviderId, KernelThreadGuid)) return;
    auto& ctx = *static_cast<Context*>(e->UserContext);
    const auto opcode = e->EventHeader.EventDescriptor.Opcode;
    const auto len = e->UserDataLength;
    const auto* bytes = static_cast<unsigned char*>(e->UserData);
    if (opcode == CswitchOpcode || opcode == ReadyThreadOpcode) {
        ++ctx.schedulerEvents;
        const auto qpc = static_cast<std::uint64_t>(e->EventHeader.TimeStamp.QuadPart);
        if (qpc < ctx.firstQpc) ctx.firstQpc = qpc;
        if (qpc > ctx.lastQpc) ctx.lastQpc = qpc;
    }
    if (opcode == CswitchOpcode) {
        // Thread_V2 CSwitch starts with two IDs (u32), then eight byte-sized fields.
        if (len < 16) { ++ctx.shortRecords; return; }
        const auto newTid = u32(bytes);
        const auto oldTid = u32(bytes + 4);
        if (oldTid == ctx.target)
            emit(ctx, *e, "switch_out", newTid, 0, bytes[14], bytes[12], -1);
        if (newTid == ctx.target)
            emit(ctx, *e, "switch_in", oldTid, 0, -1, -1, -1);
    } else if (opcode == ReadyThreadOpcode) {
        // ReadyThread's first field is the target TID, not EventHeader.ThreadId.
        if (len < 8) { ++ctx.shortRecords; return; }
        if (u32(bytes) == ctx.target) emit(ctx, *e, "ready_thread", e->EventHeader.ThreadId, 0, -1, -1, bytes[6]);
    } else if (opcode >= 1 && opcode <= 4) {
        // Thread_V2_TypeGroup1 event data begins with PID then TID.
        if (len < 8) { ++ctx.shortRecords; return; }
        if (u32(bytes + 4) == ctx.target) emit(ctx, *e, "thread_metadata", 0, u32(bytes), -1, -1, -1);
    }
}
int selfTest() {
    // Synthetic layout/time test exercises the same callback without recording ETW.
    Context c{123, 10000000};
    const unsigned char switchPayload[] = {48,0,0,0,123,0,0,0,8,8,0,0,4,0,5,0};
    const unsigned char readyPayload[] = {123,0,0,0,0,0,1,0};
    EVENT_RECORD e{};
    e.UserContext = &c;
    e.EventHeader.ProviderId = KernelThreadGuid;
    e.EventHeader.EventDescriptor.Version = 2;
    e.EventHeader.ThreadId = 456;
    e.BufferContext.ProcessorNumber = 3;
    writeHeader();
    e.EventHeader.EventDescriptor.Opcode = CswitchOpcode;
    e.EventHeader.TimeStamp.QuadPart = 1234010;
    e.UserData = const_cast<unsigned char*>(switchPayload);
    e.UserDataLength = sizeof(switchPayload);
    onEvent(&e);
    e.EventHeader.EventDescriptor.Opcode = ReadyThreadOpcode;
    e.EventHeader.TimeStamp.QuadPart = 1244010;
    e.UserData = const_cast<unsigned char*>(readyPayload);
    e.UserDataLength = sizeof(readyPayload);
    onEvent(&e);
    return c.selectedEvents == 2 && toUs(1234010, c.freq) == 123401 ? 0 : 1;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wcscmp(argv[1], L"--self-test") == 0) return selfTest();
    if (argc != 3) {
        std::cerr << "Usage: dump-scheduler <capture.etl> <sender-tid>\n";
        return 2;
    }
    wchar_t* end = nullptr;
    const auto tid = std::wcstoul(argv[2], &end, 10);
    if (!tid || !end || *end || argv[2][0] == L'-' || tid == ULONG_MAX) {
        std::cerr << "Expected a positive Windows thread ID.\n";
        return 2;
    }
    Context context{tid};
    EVENT_TRACE_LOGFILEW log{};
    log.LogFileName = argv[1];
    log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback = &onEvent;
    log.Context = &context;
    auto handle = OpenTraceW(&log);
    if (handle == INVALID_PROCESSTRACE_HANDLE) {
        std::cerr << "OpenTraceW error " << GetLastError() << '\n';
        return 3;
    }
    if (log.LogfileHeader.ReservedFlags != 1 || log.LogfileHeader.PerfFreq.QuadPart <= 0) {
        CloseTrace(handle);
        std::cerr << "Expected a QPC-based ETL clock (ReservedFlags=1), got "
                  << log.LogfileHeader.ReservedFlags << '\n';
        return 4;
    }
    context.freq = static_cast<std::uint64_t>(log.LogfileHeader.PerfFreq.QuadPart);
    writeHeader();
    const auto result = ProcessTrace(&handle, 1, nullptr, nullptr);
    const auto close = CloseTrace(handle);
    if (result != ERROR_SUCCESS || close != ERROR_SUCCESS) {
        std::cerr << "ProcessTrace=" << result << ", CloseTrace=" << close << '\n';
        return 5;
    }
    std::cerr << "{\"clock\":\"qpc\",\"frequency\":" << context.freq
              << ",\"events_lost\":" << log.LogfileHeader.EventsLost
              << ",\"buffers_lost\":" << log.LogfileHeader.BuffersLost
              << ",\"scheduler_events\":" << context.schedulerEvents
              << ",\"selected_events\":" << context.selectedEvents
              << ",\"short_records\":" << context.shortRecords
              << ",\"coverage_start_host_us\":" << (context.schedulerEvents ? toUs(context.firstQpc,context.freq) : 0)
              << ",\"coverage_end_host_us\":" << (context.schedulerEvents ? toUs(context.lastQpc,context.freq) : 0) << "}\n";
    return context.schedulerEvents && context.selectedEvents ? 0 : 6;
}
