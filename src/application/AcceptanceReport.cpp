#include "application/AcceptanceReport.h"
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>
#include <vector>

namespace rt {
    namespace {
        struct Row {
            std::string id,requirement,measurement,threshold,actual,status,reason;
            std::vector<std::string> evidence;
        };
        std::string number(std::uint64_t value) {return std::to_string(value);}
        std::string decimal(long double value) {
            std::ostringstream out;out.imbue(std::locale::classic());
            out<<std::fixed<<std::setprecision(3)<<value;return out.str();
        }
        const char* label(const std::string& status) {
            return status=="pass"?"通过":status=="fail"?"失败":"未检测";
        }
        std::string verdict(bool observed,bool failed) {
            return failed?"fail":observed?"pass":"not_measured";
        }
        std::string distribution(const Distribution& value) {
            if(!value.count)return "未测：没有符合口径的观测";
            return "样本="+number(value.count)+"；最大="+number(value.maxUs)+"µs；P99="+
                number(value.p99Us)+"µs；P99.9="+number(value.p999Us)+"µs";
        }
        std::string quote(const std::string& value) {
            static const char hex[]="0123456789abcdef";
            std::string out="\"";
            for(unsigned char c:value) {
                if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}
                else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
                else out+=static_cast<char>(c);
            }
            return out+'"';
        }
        std::string cell(const std::string& value) {
            std::string out;
            for(char c:value) {
                if(c=='|')out+="\\|";
                else if(c=='\\')out+="\\\\";
                else if(c=='\n'||c=='\r')out+="<br>";
                else if(c=='<')out+="&lt;";
                else if(c=='>')out+="&gt;";
                else if(c=='&')out+="&amp;";
                else out+=c;
            }
            return out;
        }
        std::vector<Row> rows(const Snapshot& s) {
            const auto& a=s.stats;
            const bool samples=a.sampled>=2&&s.samplingIntervals.count==a.sampled-1&&
                s.samplingDeviation.count==a.sampled-1&&a.samplingSpanUs>0;
            const bool jitterFailure=(s.samplingDeviation.count&&s.samplingDeviation.maxUs>=1000)||a.samplingIntervalViolations;
            const bool initialObserved=s.initialDuration.count>0&&s.initialDuration.count==a.initialFullySubmittedFrames;
            const bool initialFailure=a.actualInitialDeadlineMisses||(s.initialDuration.count&&s.initialDuration.maxUs>InitialWindowUs);
            const bool completionFailure=a.initialIncompleteFrames||a.unsentExpired||a.unsentAtStop||a.sendErrors||
                (!s.running&&a.built&&(a.initialFinishedFrames!=a.built||a.initialFullySubmittedFrames!=a.built||a.initialPendingFragments));
            const bool complete=a.built>0&&a.initialFinishedFrames==a.built&&a.initialFullySubmittedFrames==a.built&&!a.initialPendingFragments;
            const bool gaps=s.initialPacketGaps.count>0&&s.initialPacketGapDeviation.count==s.initialPacketGaps.count&&
                a.initialFirstCopySendCalls>=a.initialGapSegments&&
                s.initialPacketGaps.count==a.initialFirstCopySendCalls-a.initialGapSegments&&
                a.minInitialNominalGapUs>0&&a.maxInitialNominalGapUs>=a.minInitialNominalGapUs;
            const bool gapFailure=a.initialPacketGapViolations||
                (s.initialPacketGapDeviation.count&&s.initialPacketGapDeviation.maxUs>=1000);
            const bool sizes=a.socketSendCalls>0;
            const bool sizeFailure=sizes&&(a.minObservedDatagramBytes<Overhead+1||
                a.maxObservedDatagramBytes>MaxDatagram||a.minObservedDatagramBytes>a.maxObservedDatagramBytes);
            const bool socketObserved=sizes&&!s.transport.empty()&&s.localPort!=0;
            std::vector<Row> result;
            auto add=[&](Row row){result.push_back(std::move(row));};
            add({"sampling_period","每20ms采样，目标50Hz","主机实际采样间隔；同时检查跳采和最大周期偏差",
                "20000µs目标；无跳采；最大绝对偏差<1000µs",
                samples?"平均="+decimal(static_cast<long double>(a.samplingSpanUs)/(a.sampled-1))+"µs；实际频率="+
                    decimal(1000000.0L*(a.sampled-1)/a.samplingSpanUs)+"Hz；跳采="+number(a.skippedOverrun):"未测：至少需要两个采样且间隔群体完整",
                verdict(samples,jitterFailure||a.skippedOverrun),
                "平均50Hz不能代替最大偏差验收；这里测量主机采样，未测硬件触发或线上出包。",
                {"stats.sampled","stats.samplingSpanUs","stats.skippedOverrun","timing.sampling_interval","timing.abs_interval_minus_20000us"}});
            add({"sampling_jitter","采样周期偏差<1ms","全部实际相邻采样间隔减20000µs后取绝对值",
                "最大值<1000µs；样本数=sampled−1",distribution(s.samplingDeviation)+"；超限次数="+number(a.samplingIntervalViolations),
                verdict(samples,jitterFailure),"与GUI的最大采样调度迟到不同；本文档的发包间隔口径另列，不以P99代替最大值。",
                {"timing.abs_interval_minus_20000us","stats.sampled","stats.samplingIntervalViolations"}});
            add({"initial_window","首轮分片在15ms内完成","采样TS至最后首轮send调用完成；仅完整本地提交的帧",
                "最大值≤15000µs；实测超期次数=0",distribution(s.initialDuration)+"；实测超期="+number(a.actualInitialDeadlineMisses),
                verdict(initialObserved,initialFailure),"未完成帧不在时长分布内，必须同时检查首轮完整性；不代表网卡已发完。",
                {"timing.sample_to_last_initial_submit","stats.actualInitialDeadlineMisses","stats.initialFullySubmittedFrames"}});
            add({"initial_completeness","每个已构建帧完成首轮本地提交","所有built帧与完整首发计数核对",
                "完整首发=built；未完成/遗留未发/发送错误均为0",
                a.built?"构建="+number(a.built)+"；完整首发="+number(a.initialFullySubmittedFrames)+"；未完成="+number(a.initialIncompleteFrames)+
                    "；替换未发片="+number(a.unsentExpired)+"；停止未发片="+number(a.unsentAtStop)+"；仍待首发="+number(a.initialPendingFragments)+"；发送错误="+number(a.sendErrors):"未测：尚未构建逻辑帧",
                verdict(complete,completionFailure),
                "接收端后来补齐也不能抵消首轮不完整。故意丢片的功能用例不等于无丢片实时验收；运行中未结束的首轮为待测。",
                {"stats.built","stats.initialFinishedFrames","stats.initialFullySubmittedFrames","stats.initialIncompleteFrames","stats.unsentExpired","stats.unsentAtStop","stats.initialPendingFragments","stats.sendErrors"}});
            add({"packet_gap_jitter","分片平滑发送及发包间隔偏差<1ms","同一帧内相邻首份实际send调用的间隔相对名义片间隔的绝对偏差",
                "最大偏差<1000µs；firstCopyCalls=segments+gap样本数",
                distribution(s.initialPacketGapDeviation)+(gaps?"；名义间隔="+number(a.minInitialNominalGapUs)+"–"+number(a.maxInitialNominalGapUs)+"µs":"")+
                    "；超限次数="+number(a.initialPacketGapViolations),
                verdict(gaps,gapFailure),
                "这是本地调用口径，未测网卡出包。跨帧空闲、补发和重复副本另计；注入丢片断开相邻段，缺样本不能判通过。",
                {"timing.initial_packet_gap","timing.initial_packet_gap_abs_deviation","stats.initialFirstCopySendCalls","stats.initialGapSegments","stats.initialPacketGapViolations","stats.minInitialNominalGapUs","stats.maxInitialNominalGapUs"}});
            add({"datagram_size","UDP应用层数据包不超过1472字节","每次实际send调用处观察的DATA数据报长度",
                "37–1472字节（36字节固定开销加非空载荷）",
                sizes?"调用="+number(a.socketSendCalls)+"；最小="+number(a.minObservedDatagramBytes)+"字节；最大="+number(a.maxObservedDatagramBytes)+"字节":"未测：没有实际send调用",
                verdict(sizes,sizeFailure),"包含本地失败、重复和补发调用；不包含未调用send的注入丢弃，不代表线上MTU或分片抓包验证。",
                {"stats.socketSendCalls","stats.minObservedDatagramBytes","stats.maxObservedDatagramBytes"}});
            add({"df_configuration","发送socket配置DF","已发送socket的本地DF配置结果","DF启用",
                socketObserved?(s.dfEnabled?"本地DF已启用":"本地未确认DF启用"):"未测：尚无已发送socket配置证据",
                verdict(socketObserved,socketObserved&&!s.dfEnabled),"只验证本地选项；实际线上DF位与IP分片行为仍需抓包。",
                {"df_enabled","local_port","transport","stats.socketSendCalls"}});
            add({"wire_crc","IEEE CRC32及线上包格式正确","实际线上数据包CRC与完整字段验证","每包CRC与协议字段均正确",
                "未测：发送端本地报告不含逐包线上校验结果","not_measured","单元回归是独立证据，不能填作本轮运行的线上通过。",{}});
            add({"receiver_delivery","全部预期逻辑帧完整接收","接收端完成/作废/整帧缺失清单与发送端built核对","完成=built；缺失/作废/在途为0",
                "未测：没有配对接收端报告","not_measured","UDP本地send成功没有送达确认；请使用配对诊断报告。",{"receiver_delivery","built_manifest"}});
            add({"receiver_reassembly_timing","接收重组与补片等待受控","逐帧接收/完成与缺片请求时序","需按协议重试计时；不可用发送耗时代替",
                "未测：缺少接收时序观测","not_measured","本地NACK和补发总数不能推导每片请求间隔或重组延迟分布。",{"stats.nacks","stats.retransmissions"}});
            add({"hardware_timestamp","采样瞬间读取统一硬件TS","真实采集硬件计数器与触发时刻","硬件采样计数器；单位µs",
                "当前使用模拟主机单调时钟","not_measured","共享序列化TS不等于接入外部采集硬件。",{"timestamp_source"}});
            add({"intermodule_skew","同帧多模块采样时差建议<1ms","各模块物理触发时间的最大差值","建议<1000µs",
                "未测：没有每模块硬件触发时间","not_measured","同帧共用TS和模拟模板不能证明真实采样同步。",{"timestamp_source","initial_modules"}});
            add({"wire_send_timing","15ms线上发送及线上间隔","网卡实际发包时间戳或时间一致的抓包","整帧≤15ms；线上间隔按协议定义",
                "未测：没有网卡/链路时戳","not_measured","send调用完成和socket缓冲区接受均不是物理链路发送完成。",{}});
            add({"receive_buffer_window","接收显示缓冲为1帧约20ms","接收队列容量与实际停留时间","1帧；名义20ms",
                "未测：发送端未观测接收显示队列","not_measured","重组缓存与显示缓冲不是同一个测量对象。",{}});
            add({"render_synchronization","丢包/乱序下界面保持多模块同步","接收端渲染时间、模块偏差及拥塞压力观察","按硬件TS同步渲染；压力测试需有实际界面证据",
                "未测：没有接收端同步渲染观测","not_measured","发送端分片监视界面与协议接收渲染界面不同。",{}});
            add({"packet_rate","典型1MiB/50Hz约36550包每秒","本地接受的数据报计数除以运行时间（包含补发与收尾）","按帧长和负载配置解释；不以均值代替时限",
                a.datagrams&&a.elapsedUs?decimal(1000000.0L*a.datagrams/a.elapsedUs)+"包/s；数据报="+number(a.datagrams):"未测：无数据报或运行时长",
                "not_measured","显示推导的本地平均值；没有独立线上包速率验收或最大容量测试。",{"stats.datagrams","stats.elapsedUs"}});
            add({"wire_bandwidth","典型1MiB约450Mbit/s；链路足够15ms窗口","本地UDP应用字节均值与声明的链路容量","实际物理带宽需独立测量；2MiB@1Gbps不能15ms发完",
                a.bytesSent&&a.elapsedUs?"应用层="+decimal(8.0L*a.bytesSent/a.elapsedUs)+"Mbit/s；声明链路="+number(s.config.linkMbps)+"Mbit/s":"未测：无发送字节或运行时长",
                "not_measured","应用层均值不含以太网/IP/UDP开销；声明容量不是网卡测速，也不是实际出线带宽。",{"stats.bytesSent","stats.elapsedUs","link_mbps_capacity_input"}});
            add({"module_hotplug","模块在线/离线动态兼容","真实模块健康状态及mask变化处理","变化应用于后续帧且旧帧保持原内容",
                "仅有配置模块和最近缓存帧的mask","not_measured","模拟SetMask及单元回归不等于实际设备热插拔检测；未导出完整变化历史。",{"initial_modules","latest_frame_geometry"}});
            add({"frame_boundaries","1MiB/2MiB切片与重组边界","指定边界帧的完整发送及接收对账","1MiB=731片；2MiB=1461片且满足相应链路预算",
                s.frames.empty()?"未测：没有帧几何快照":"仅保留最近"+number(s.frames.size())+"个帧几何快照",
                "not_measured","最近几何或格式回归不能证明本轮已完成两种大小的接收/时序边界测试。",{"latest_frame_geometry","stats.buildRejected"}});
            add({"cache_retention","重传缓存保留最近3帧","缓存策略与跨回绕的保留行为","文档同时要求最近3帧和logic_id%3放置，回绕处需澄清",
                s.config.cachePolicy==CachePolicy::SpecModulo?"使用文档modulo规则":"使用显式FIFO替代规则",
                "not_measured","FFFFFFFF与0映射同一modulo槽；策略名不能证明全程最近3帧保留或60ms可重传。",{"cache_policy","latest_frame_geometry","cache_miss_events"}});
            add({"ts_order","接收端按TS单调顺序消费完整帧","接收队列实际输出TS顺序","单调递增；正确跳过作废帧",
                "未测：没有接收端消费时序","not_measured","发送端TS与重组完成计数不等于按序消费或渲染。",{"timestamp_source"}});
            return result;
        }
        std::string overall(const std::vector<Row>& values) {
            bool missing=false;
            for(const auto& row:values){if(row.status=="fail")return "fail";if(row.status!="pass")missing=true;}
            return missing||values.empty()?"not_measured":"pass";
        }
    }
    std::string acceptanceTableJson(const Snapshot& snapshot) {
        const auto values=rows(snapshot);const auto status=overall(values);
        std::ostringstream out;
        out<<"{\"schema_version\":1,\"scope\":\"sender-local\",\"overall_status\":"<<quote(status)
           <<",\"overall_status_label\":"<<quote(label(status))<<",\"rows\":[";
        for(std::size_t i=0;i<values.size();++i) {
            if(i)out<<',';
            const auto& row=values[i];
            out<<"{\"id\":"<<quote(row.id)<<",\"requirement\":"<<quote(row.requirement)
               <<",\"measurement\":"<<quote(row.measurement)<<",\"threshold\":"<<quote(row.threshold)
               <<",\"actual\":"<<quote(row.actual)<<",\"status\":"<<quote(row.status)
               <<",\"status_label\":"<<quote(label(row.status))<<",\"reason\":"<<quote(row.reason)<<",\"evidence\":[";
            for(std::size_t j=0;j<row.evidence.size();++j){if(j)out<<',';out<<quote(row.evidence[j]);}
            out<<"]}";
        }
        out<<"]}";return out.str();
    }
    std::string acceptanceTableMarkdown(const Snapshot& snapshot) {
        const auto values=rows(snapshot);const auto status=overall(values);
        std::ostringstream out;
        out<<"# 发送端本地验收表\n\n运行标识："<<cell(snapshot.runId.empty()?"未提供":snapshot.runId)
           <<"\n\n总体状态：**"<<label(status)<<"**。范围：发送端本地观测。"
           <<(snapshot.running?"当前仍在运行，仅代表截至此快照的观测。":"当前为停止后的快照。")
           <<" 未检测项目不作为通过，任一已测失败会使总体为失败。\n\n"
           <<"| 要求 | 测量口径 | 阈值 | 实测值 | 结果 | 原因 |\n"
           <<"|---|---|---|---|---|---|\n";
        for(const auto& row:values) {
            out<<"| "<<cell(row.requirement)<<" | "<<cell(row.measurement)<<" | "<<cell(row.threshold)
               <<" | "<<cell(row.actual)<<" | "<<label(row.status)<<" | "<<cell(row.reason)<<" |\n";
        }
        out<<"\n分位数为100µs直方图桶上界，最大值保留原始观测；完整首发时长分布排除未完成、注入丢弃和本地发送失败帧。"
           <<"这张表不是原始运行日志或逐包记录；JSON仍保留计数、分布和有限异常窗口，证据字段路径位于acceptance_table.rows[].evidence。\n\n"
           <<"关闭trace不关闭计数及直方图；trace只保留有限窗口，不能单独证明操作系统根因。"
           <<"完整异常窗口在正常停止后导出；仍在运行或进程异常中止时可能不完整。\n";
        return out.str();
    }
}
