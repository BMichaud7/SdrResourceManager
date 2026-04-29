#pragma once
#include "Types.hpp"
#include <string>
#include <optional>
#include <vector>

namespace sdr {

class MessageCodec {
public:
    static std::string             peekMsgType(const std::string& json);
    static std::optional<TaskRequest> decode(const std::string& json);
    static std::string encodeTaskResponse(const TaskResponse& r);
    static std::string encodeTaskStatus(const TaskRecord& rec,
                                        const std::vector<StreamMetrics>& m);
    static std::string encodeSnapshotResult(const std::string& req_id,
                                            const SnapshotResult& r);

    struct DevHealthEntry {
        std::string device_id, driver, uri, coherency_group;
        bool   online=false; int active_tasks=0;
        double cf_hz=0, rate_sps=0, alloc_bw_hz=0, free_bw_hz=0, temp_c=0;
    };
    static std::string encodeDeviceHealth(const std::string& req_id,
                                          const std::vector<DevHealthEntry>& e);
    static std::string encodeControllerHealth(const std::string& req_id,
        int tot_dev, int on_dev, int sched, int pend, int run,
        int tot_tasks, int ports_used, int ports_free, int64_t uptime_sec);
    static std::string encodeHealthQueryResponse(const std::string& req_id,
        const std::vector<DevHealthEntry>& devs,
        int tot_dev,int on_dev,int sched,int pend,int run,
        int tot_tasks,int ports_used,int ports_free,int64_t uptime_sec);
};

} // namespace sdr
