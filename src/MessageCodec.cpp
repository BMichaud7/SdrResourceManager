#include "MessageCodec.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>

namespace sdr {
using json = nlohmann::json;
using namespace std::chrono;

static int64_t nowMs() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string MessageCodec::peekMsgType(const std::string& body) {
    try { return json::parse(body).value("msg_type",""); } catch(...) { return ""; }
}

std::optional<TaskRequest> MessageCodec::decode(const std::string& body) {
    try {
        auto j = json::parse(body);
        TaskRequest req;
        req.msg_type       = j.value("msg_type","");
        req.schema_version = j.value("schema_version","");
        req.request_id     = j.value("request_id","");
        req.correlation_id = j.value("correlation_id","");
        req.timestamp_ms   = j.value("timestamp_ms",int64_t{0});
        if (req.request_id.empty()) { spdlog::warn("MessageCodec: missing request_id"); return {}; }

        // STOP / CANCEL
        if (req.msg_type=="TASK_STOP"||req.msg_type=="TASK_CANCEL") {
            req.task_id = j.value("task_id","");
            req.reason  = j.value("reason","");
            return req;
        }
        if (req.msg_type=="HEALTH_QUERY") return req;

        req.task_type = taskTypeFromString(j.value("task_type","UNKNOWN"));
        req.priority  = j.value("priority",0);

        // Schedule
        if (j.contains("schedule")) {
            auto& s=j["schedule"];
            req.schedule_mode = scheduleModeFromString(s.value("mode","SCHEDULED"));
            req.start_time_ms = s.value("start_time_epoch_ms",int64_t{0});
            req.end_time_ms   = s.value("end_time_epoch_ms",TIME_INFINITE);
            req.duration_ms   = s.value("duration_ms",int64_t{0});
        } else {
            if (req.msg_type=="TASK_REQUEST_CONTINUOUS") req.schedule_mode=ScheduleMode::CONTINUOUS;
            else if (req.msg_type=="TASK_REQUEST_IMMEDIATE") req.schedule_mode=ScheduleMode::IMMEDIATE;
        }

        // Snapshot
        if (req.msg_type=="TASK_REQUEST_SNAPSHOT") {
            if (!j.contains("snapshot")) return {};
            auto& s=j["snapshot"]; SnapshotParams sp;
            sp.center_freq_hz  =s.value("center_freq_hz",0.0);
            sp.bandwidth_hz    =s.value("bandwidth_hz",0.0);
            sp.sample_rate_sps =s.value("sample_rate_sps",0.0);
            sp.fft_size        =s.value("fft_size",4096);
            sp.n_averages      =s.value("n_averages",16);
            sp.preferred_device=s.value("preferred_device","");
            req.snapshot_params=sp; req.task_type=TaskType::SNAPSHOT; return req;
        }

        // Calibration
        if (req.msg_type=="TASK_REQUEST_CALIBRATION") {
            if (!j.contains("calibration")) return {};
            auto& c=j["calibration"]; CalibrationParams cp;
            cp.center_freq_hz     =c.value("center_freq_hz",0.0);
            cp.bandwidth_hz       =c.value("bandwidth_hz",0.0);
            cp.sample_rate_sps    =c.value("sample_rate_sps",0.0);
            cp.duration_ms        =c.value("duration_ms",5000);
            cp.rx_count_per_device=c.value("rx_count_per_device",2);
            cp.coherency_group    =c.value("coherency_group","");
            if (c.contains("devices")) for (auto& d:c["devices"]) cp.devices.push_back(d.get<std::string>());
            req.cal_params=cp; req.task_type=TaskType::CALIBRATION;
            if (j.contains("streaming")) {
                req.streaming.dest_ip=j["streaming"].value("dest_ip","");
                if (j["streaming"].contains("dest_ports"))
                    for (auto& p:j["streaming"]["dest_ports"]) req.streaming.dest_ports.push_back(p.get<int>());
            }
            return req;
        }

        // RF block
        if (j.contains("rf")) {
            auto& r=j["rf"];
            req.rf.center_freq_hz  =r.value("center_freq_hz",0.0);
            req.rf.bandwidth_hz    =r.value("bandwidth_hz",0.0);
            req.rf.sample_rate_sps =r.value("sample_rate_sps",0.0);
            req.rf.rx_count        =r.value("rx_count",1);
            req.rf.tx_count        =r.value("tx_count",0);
            req.rf.preferred_device=r.value("preferred_device","");
            req.rf.coherency_group  =r.value("coherency_group","");
            if (r.contains("rx_gain_db")) for (auto& v:r["rx_gain_db"]) req.rf.rx_gain_db.push_back(v.get<double>());
            if (r.contains("rx_agc"))     for (auto& v:r["rx_agc"])     req.rf.rx_agc.push_back(v.get<bool>());
            if (r.contains("tx_atten_db"))for (auto& v:r["tx_atten_db"])req.rf.tx_atten_db.push_back(v.get<double>());
        }
        if (j.contains("streaming")) {
            req.streaming.dest_ip=j["streaming"].value("dest_ip","");
            if (j["streaming"].contains("dest_ports"))
                for (auto& p:j["streaming"]["dest_ports"]) req.streaming.dest_ports.push_back(p.get<int>());
        }

        // Scan
        if (req.msg_type=="TASK_REQUEST_SCAN") {
            if (!j.contains("scan")) return {};
            auto& s=j["scan"]; ScanParams sp;
            sp.repeat=s.value("repeat",true);
            req.rf.rx_count=s.value("rx_count",1); req.rf.tx_count=s.value("tx_count",0);
            req.rf.preferred_device=s.value("preferred_device","");
            if (s.contains("rx_gain_db")) for (auto& v:s["rx_gain_db"]) req.rf.rx_gain_db.push_back(v.get<double>());
            if (s.contains("entries")) {
                for (auto& e:s["entries"]) {
                    ScanEntry se;
                    se.step=e.value("step",0); se.center_freq_hz=e.value("center_freq_hz",0.0);
                    se.bandwidth_hz=e.value("bandwidth_hz",0.0); se.sample_rate_sps=e.value("sample_rate_sps",0.0);
                    se.dwell_ms=e.value("dwell_ms",1000); sp.entries.push_back(se);
                }
            }
            req.scan_params=sp; req.task_type=TaskType::SCAN; return req;
        }

        // Triggered
        if (req.msg_type=="TASK_REQUEST_TRIGGERED") {
            TriggerParams tp;
            if (j.contains("trigger")) {
                auto& t=j["trigger"];
                tp.trigger_type   =t.value("type","POWER_THRESHOLD");
                tp.threshold_dbfs =t.value("threshold_dbfs",-60.0);
                tp.pre_trigger_ms =t.value("pre_trigger_ms",50);
                tp.post_trigger_ms=t.value("post_trigger_ms",200);
                tp.max_captures   =t.value("max_captures",0);
            }
            req.trigger_params=tp; req.task_type=TaskType::TRIGGERED; return req;
        }

        // task_params
        if (j.contains("task_params")) {
            auto& tp=j["task_params"];
            if (tp.contains("df")) {
                DfParams p; auto& d=tp["df"];
                p.algorithm=d.value("algorithm","MUSIC"); p.num_sources=d.value("num_sources",1);
                p.snapshot_count=d.value("snapshot_count",1024); p.angular_res_deg=d.value("angular_res_deg",1.0);
                req.df_params=p;
            }
            if (tp.contains("narrowband")) {
                NarrowbandParams p; auto& n=tp["narrowband"];
                p.demod=n.value("demod","FM"); p.squelch_dbfs=n.value("squelch_dbfs",-80.0);
                p.output_rate_sps=n.value("output_rate_sps",48000); req.nb_params=p;
            }
            if (tp.contains("wideband")) {
                WidebandParams p; auto& w=tp["wideband"];
                p.record_raw_iq=w.value("record_raw_iq",true);
                p.detect_threshold_dbfs=w.value("detect_threshold_dbfs",-60.0);
                p.fft_size=w.value("fft_size",2048); req.wb_params=p;
            }
        }
        return req;
    } catch (const std::exception& ex) {
        spdlog::error("MessageCodec::decode: {}", ex.what()); return {};
    }
}

std::string MessageCodec::encodeTaskResponse(const TaskResponse& r) {
    json j={
        {"msg_type","TASK_RESPONSE"},{"schema_version",SCHEMA_VERSION},
        {"timestamp_ms",nowMs()},{"request_id",r.request_id},
        {"correlation_id",r.correlation_id},
        {"status",r.accepted?"ACCEPTED":"REJECTED"},
        {"task_id",r.task_id},{"schedule_mode",r.schedule_mode},
        {"actual_start_epoch_ms",r.actual_start_ms},
        {"actual_stop_epoch_ms",r.actual_stop_ms},
        {"reject_code",rejectCodeToString(r.reject_code)},
        {"reject_reason",r.reject_reason}
    };
    if (r.accepted) {
        json streams=json::array();
        for (auto& s:r.streams) streams.push_back({
            {"stream_id",s.stream_id},{"device_id",s.device_id},
            {"channel_type",s.channel_type},{"channel_index",s.channel_index},
            {"udp_ip",s.udp_ip},{"udp_port",s.udp_port},
            {"center_freq_hz",s.center_freq_hz},{"slice_offset_hz",s.slice_offset_hz},
            {"slice_bw_hz",s.slice_bw_hz},{"sample_rate_sps",s.sample_rate_sps},
            {"format",s.format}
        });
        j["streams"]=streams;
    }
    return j.dump();
}

std::string MessageCodec::encodeTaskStatus(const TaskRecord& rec, const std::vector<StreamMetrics>& m) {
    json devs=json::array();
    for (auto& a:rec.allocations) devs.push_back(a.device_id);
    json streams=json::array();
    for (auto& s:m) streams.push_back({
        {"stream_id",s.stream_id},{"channel_type",s.channel_type},
        {"channel_index",s.channel_index},{"udp_port",s.udp_port},
        {"metrics",{{"samples_total",s.samples_total},{"packets_sent",s.packets_sent},
                    {"overflows",s.overflows},{"rssi_dbfs",s.rssi_dbfs},
                    {"throughput_mbps",s.throughput_mbps}}}
    });
    json j={
        {"msg_type","TASK_STATUS"},{"schema_version",SCHEMA_VERSION},
        {"timestamp_ms",nowMs()},
        {"request_id","status-"+rec.task_id.substr(0,8)},
        {"task_id",rec.task_id},{"state",taskStateToString(rec.state)},
        {"task_type",taskTypeToString(rec.task_type)},
        {"schedule_mode",scheduleModeToString(rec.schedule_mode)},
        {"device_ids",devs},
        {"actual_start_epoch_ms",rec.start_time_ms},
        {"actual_stop_epoch_ms",rec.stop_time_ms==TIME_INFINITE?0LL:rec.stop_time_ms},
        {"streams",streams}
    };
    return j.dump();
}

std::string MessageCodec::encodeSnapshotResult(const std::string& req_id, const SnapshotResult& r) {
    return json{
        {"msg_type","SNAPSHOT_RESULT"},{"schema_version",SCHEMA_VERSION},
        {"timestamp_ms",nowMs()},{"request_id",req_id},
        {"status",r.success?"COMPLETED":"FAILED"},
        {"device_id",r.device_id},{"center_freq_hz",r.center_freq_hz},
        {"bandwidth_hz",r.bandwidth_hz},{"sample_rate_sps",r.sample_rate_sps},
        {"fft_size",r.fft_size},{"n_averages",r.n_averages},
        {"freq_resolution_hz",r.freq_resolution_hz},
        {"freq_axis_start_hz",r.freq_axis_start_hz},
        {"freq_axis_step_hz",r.freq_axis_step_hz},
        {"power_bins",r.power_bins},{"error_msg",r.error_msg}
    }.dump();
}

std::string MessageCodec::encodeDeviceHealth(const std::string& req_id, const std::vector<DevHealthEntry>& e) {
    json devs=json::array();
    for (auto& d:e) devs.push_back({
        {"device_id",d.device_id},{"online",d.online},{"active_tasks",d.active_tasks},
        {"center_freq_hz",d.cf_hz},{"sample_rate_sps",d.rate_sps},
        {"rx_allocated_bw_hz",d.alloc_bw_hz},{"rx_free_bw_hz",d.free_bw_hz},
        {"temperature_c",d.temp_c},{"driver",d.driver},{"uri",d.uri},
        {"coherency_group",d.coherency_group}
    });
    return json{{"msg_type","DEVICE_HEALTH"},{"schema_version",SCHEMA_VERSION},
                {"timestamp_ms",nowMs()},{"request_id",req_id},{"devices",devs}}.dump();
}

std::string MessageCodec::encodeControllerHealth(const std::string& req_id,
    int tot_dev,int on_dev,int sched,int pend,int run,
    int tot_tasks,int ports_used,int ports_free,int64_t uptime_sec)
{
    return json{{"msg_type","CONTROLLER_HEALTH"},{"schema_version",SCHEMA_VERSION},
                {"timestamp_ms",nowMs()},{"request_id",req_id},
                {"version",APP_VERSION},{"uptime_sec",uptime_sec},
                {"total_devices",tot_dev},{"online_devices",on_dev},
                {"scheduled_tasks",sched},{"pending_tasks",pend},
                {"running_tasks",run},{"total_tasks",tot_tasks},
                {"udp_ports_in_use",ports_used},{"udp_ports_free",ports_free}}.dump();
}

std::string MessageCodec::encodeHealthQueryResponse(const std::string& req_id,
    const std::vector<DevHealthEntry>& devs,
    int tot_dev,int on_dev,int sched,int pend,int run,
    int tot_tasks,int ports_used,int ports_free,int64_t uptime_sec)
{
    auto ctrl=json::parse(encodeControllerHealth(req_id,tot_dev,on_dev,sched,pend,run,tot_tasks,ports_used,ports_free,uptime_sec));
    auto devh=json::parse(encodeDeviceHealth(req_id,devs));
    return json{{"msg_type","HEALTH_QUERY_RESPONSE"},{"schema_version",SCHEMA_VERSION},
                {"timestamp_ms",nowMs()},{"request_id",req_id},
                {"controller",ctrl},{"devices",devh["devices"]}}.dump();
}

} // namespace sdr
