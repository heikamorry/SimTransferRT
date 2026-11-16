#include "ui/FrameTableModel.h"
#include "application/StreamRunner.h"
#include "application/AcceptanceReport.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <iostream>
#include <stdexcept>

namespace {
    void require(bool condition,const char* message) {
        if(!condition)throw std::runtime_error(message);
    }
    QJsonObject acceptance(const rt::Snapshot& snapshot) {
        QJsonParseError error;
        const auto value=QJsonDocument::fromJson(QByteArray::fromStdString(rt::acceptanceTableJson(snapshot)),&error);
        require(error.error==QJsonParseError::NoError&&value.isObject(),"acceptance table JSON invalid");
        return value.object();
    }
    QJsonObject row(const QJsonObject& table,const QString& id) {
        for(const auto& value:table.value("rows").toArray()) {
            const auto item=value.toObject();
            if(item.value("id").toString()==id)return item;
        }
        throw std::runtime_error("acceptance row missing");
    }
    rt::Snapshot measured() {
        rt::Snapshot s;
        auto& a=s.stats;
        a.sampled=3;a.built=3;a.samplingSpanUs=40000;a.samplingActiveUs=60000;a.elapsedUs=60000;
        a.initialFinishedFrames=3;a.initialFullySubmittedFrames=3;
        a.socketSendCalls=6;a.initialFirstCopySendCalls=6;a.initialGapSegments=3;a.datagrams=6;a.bytesSent=600;
        a.minObservedDatagramBytes=100;a.maxObservedDatagramBytes=100;
        a.minInitialNominalGapUs=20;a.maxInitialNominalGapUs=20;
        s.samplingIntervals.count=2;s.samplingIntervals.maxUs=20999;
        s.samplingDeviation.count=2;s.samplingDeviation.maxUs=999;
        s.initialDuration.count=3;s.initialDuration.maxUs=15000;
        s.initialPacketGaps.count=3;s.initialPacketGaps.maxUs=20;
        s.initialPacketGapDeviation.count=3;s.initialPacketGapDeviation.maxUs=0;
        s.localPort=5500;s.transport="test-local";s.dfEnabled=true;
        return s;
    }
    void checkAcceptance() {
        const auto empty=acceptance(rt::Snapshot{});
        require(empty.value("schema_version").toInt()==1&&empty.value("scope").toString()=="sender-local","acceptance schema missing");
        require(empty.value("overall_status").toString()=="not_measured","idle snapshot falsely accepted");
        for(const auto& value:empty.value("rows").toArray()) {
            const auto item=value.toObject();
            require(item.value("status").toString()=="not_measured","no observations became a passing/failing metric");
            require(item.value("evidence").isArray()&&item.value("actual").isString(),"acceptance columns changed types");
            require(!item.value("requirement").toString().isEmpty()&&!item.value("reason").toString().isEmpty(),"readable acceptance explanation missing");
        }
        auto s=measured();s.config.timingTraceEnabled=false;
        const auto local=acceptance(s);
        for(const auto* id:{"sampling_period","sampling_jitter","initial_window","initial_completeness","packet_gap_jitter","datagram_size","df_configuration"})
            require(row(local,id).value("status").toString()=="pass","observed local metric did not pass independently of trace");
        require(local.value("overall_status").toString()=="not_measured","missing hardware/receiver evidence became full acceptance");
        for(const auto* id:{"wire_crc","receiver_delivery","hardware_timestamp","intermodule_skew","wire_send_timing","render_synchronization"})
            require(row(local,id).value("status").toString()=="not_measured","unobserved physical/receiver metric accepted");
        s.config.timingTraceEnabled=true;
        require(acceptance(s)==local,"trace toggle changed metric acceptance");
        s.samplingDeviation.maxUs=1000;s.stats.samplingIntervalViolations=1;
        s.initialDuration.maxUs=15001;s.stats.actualInitialDeadlineMisses=1;
        s.initialPacketGapDeviation.maxUs=1000;s.stats.initialPacketGapViolations=1;
        s.stats.maxObservedDatagramBytes=1473;s.dfEnabled=false;
        s.stats.initialIncompleteFrames=1;s.stats.initialFullySubmittedFrames=2;
        const auto failed=acceptance(s);
        require(failed.value("overall_status").toString()=="fail","known failures did not fail overall acceptance");
        for(const auto* id:{"sampling_period","sampling_jitter","initial_window","initial_completeness","packet_gap_jitter","datagram_size","df_configuration"})
            require(row(failed,id).value("status").toString()=="fail","acceptance boundary or completeness failure missed");
        auto insufficient=measured();insufficient.samplingDeviation.count=1;
        require(row(acceptance(insufficient),"sampling_jitter").value("status").toString()=="not_measured","partial sampling population accepted");
        insufficient=measured();insufficient.initialPacketGaps.count=0;insufficient.initialPacketGapDeviation.count=0;
        require(row(acceptance(insufficient),"packet_gap_jitter").value("status").toString()=="not_measured","absent packet gaps treated as zero jitter");
        insufficient=measured();insufficient.running=true;insufficient.stats.initialPendingFragments=1;
        require(row(acceptance(insufficient),"initial_completeness").value("status").toString()=="not_measured","live pending frame treated as final result");
        s.runId="run|name\n<script>&\\\"";
        const auto markdown=QString::fromStdString(rt::acceptanceTableMarkdown(s));
        require(markdown.contains(QStringLiteral("| 要求 | 测量口径 | 阈值 | 实测值 | 结果 | 原因 |")),"readable table columns missing");
        require(markdown.contains(QStringLiteral("失败"))&&markdown.contains(QStringLiteral("未检测")),"readable verdicts missing");
        require(markdown.contains("run\\|name<br>&lt;script&gt;&amp;")&&!markdown.contains("<script>"),"Markdown identity escaping unsafe");
        require(markdown.contains("acceptance_table.rows[].evidence")&&markdown.contains(QStringLiteral("100µs")),"readable evidence/quantile limits missing");
    }
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        checkAcceptance();
        rt::FrameTableModel model;
        rt::Snapshot detail;
        rt::FrameStatus frame;
        frame.id=7;frame.timestampUs=1000;frame.count=1;frame.bytes=100;
        frame.fragments.push_back({0,100,0,2000,"Sent locally (unconfirmed)"});
        detail.frames.push_back(frame);
        model.apply(detail);
        require(model.rowCount()==1&&model.data(model.index(0,0)).toUInt()==7,"detailed frame not displayed");
        rt::Snapshot light;
        light.running=true;light.detailsIncluded=false;light.stats.built=8;
        model.apply(light);
        require(model.rowCount()==1&&model.data(model.index(0,0)).toUInt()==7,"heartbeat erased previous detail");
        const auto lightJson=QJsonDocument::fromJson(QByteArray::fromStdString(rt::snapshotJson(light))).object();
        require(!lightJson.value("details_included").toBool(true),"live report lost explicit detail omission");
        require(!lightJson.value("timing_trace").toObject().value("exported").toBool(),"live report pretended to export trace");
        model.apply(rt::Snapshot{});
        require(model.rowCount()==0,"new session retained prior frame rows");
        rt::TimingTrace trace;
        trace.record(rt::TraceKind::Sample,1000,7,900);
        trace.trigger(rt::TraceReason::SamplingDeviation,1000,7,1000);
        trace.record(rt::TraceKind::WaitEnd,2000,7,1000);
        trace.finish();
        detail.timingTrace=trace.report();
        detail.initialDuration.p999Us=14900;
        const auto bytes=QByteArray::fromStdString(rt::snapshotJson(detail));
        QJsonParseError error;
        const auto document=QJsonDocument::fromJson(bytes,&error);
        require(error.error==QJsonParseError::NoError,"final timeline JSON is invalid");
        const auto timeline=document.object().value("timing_trace").toObject();
        require(timeline.value("exported").toBool()&&timeline.value("total_triggers").toInt()==1,"final trace missing");
        const auto windows=timeline.value("windows").toArray();
        require(windows.size()==1&&windows[0].toObject().value("events").toArray().size()==2,"trace window corrupted in JSON");
        require(!windows[0].toObject().value("post_window_complete").toBool(true),"partial post-window mislabeled complete");
        require(document.object().value("timing").toObject().value("sample_to_last_initial_submit").toObject().value("p999_us").toInt()==14900,"P999 missing");
        require(document.object().value("acceptance_table").toObject().value("scope").toString()=="sender-local","unified acceptance table absent from JSON export");
        std::cout<<"PASS UI detailed/light/new-session compatibility and final JSON timeline\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
