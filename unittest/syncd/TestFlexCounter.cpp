#include "FlexCounter.h"
#include "MockableSaiInterface.h"
#include "MockHelper.h"
#include "VirtualObjectIdManager.h"
#include "NumberOidIndexGenerator.h"
#include <string>
#include <gtest/gtest.h>

using namespace saimeta;
using namespace sairedis;
using namespace syncd;
using namespace std;

std::string join(const std::vector<std::string>& input)
{
    SWSS_LOG_ENTER();

    if (input.empty())
    {
        return "";
    }
    std::ostringstream ostream;
    auto iter = input.begin();
    ostream << *iter;
    while (++iter != input.end())
    {
        ostream << "," << *iter;
    }
    return ostream.str();
}

template <typename T>
std::string toOid(T value)
{
    SWSS_LOG_ENTER();

    std::ostringstream ostream;
    ostream << "oid:0x" << std::hex << value;
    return ostream.str();
}

std::shared_ptr<MockableSaiInterface> sai(new MockableSaiInterface());
typedef std::function<void(swss::Table &countersTable, const std::string& key, const std::vector<std::string>& counterIdNames, const std::vector<std::string>& expectedValues)> VerifyStatsFunc;

std::vector<sai_object_id_t> generateOids(
        unsigned int numOid,
        sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    std::vector<sai_object_id_t> object_ids;
    if (!numOid)
        return object_ids;

    auto scc = std::make_shared<SwitchConfigContainer>();
    for (unsigned int i = 0; i < numOid; i++){
        auto hw_info = "asic" + std::to_string(i);
        scc->insert(std::make_shared<SwitchConfig>(i, hw_info));
    }

    auto vidManager = VirtualObjectIdManager(0, scc, std::make_shared<NumberOidIndexGenerator>());
    if (object_type == SAI_OBJECT_TYPE_SWITCH)
    {
        for (unsigned int i = 0; i < numOid; i++){
            auto hw_info = "asic" + std::to_string(i);
            object_ids.push_back(vidManager.allocateNewSwitchObjectId(hw_info));
        }
    }
    else
    {
        auto sid = vidManager.allocateNewSwitchObjectId("asic0");
        for (unsigned int i = 0; i < numOid; i++){
            object_ids.push_back(vidManager.allocateNewObjectId(object_type, sid));
        }
    }
    return object_ids;
}

void testAddRemoveCounter(
        unsigned int numOid,
        sai_object_type_t object_type,
        const std::string& counterIdFieldName,
        const std::vector<std::string>& counterIdNames,
        const std::vector<std::string>& expectedValues,
        VerifyStatsFunc verifyFunc,
        bool autoRemoveDbEntry,
        const std::string statsMode = STATS_MODE_READ)
{
    SWSS_LOG_ENTER();

    FlexCounter fc("test", sai, "COUNTERS_DB");

    test_syncd::mockVidManagerObjectTypeQuery(object_type);

    std::vector<sai_object_id_t> object_ids = generateOids(numOid, object_type);
    EXPECT_EQ(object_ids.size(), numOid);

    std::vector<swss::FieldValueTuple> values;
    values.emplace_back(POLL_INTERVAL_FIELD, "1000");
    values.emplace_back(FLEX_COUNTER_STATUS_FIELD, "enable");
    values.emplace_back(STATS_MODE_FIELD, statsMode);
    fc.addCounterPlugin(values);

    values.clear();
    values.emplace_back(counterIdFieldName, join(counterIdNames));
    for (auto object_id : object_ids)
    {
        fc.addCounter(object_id, object_id, values);
    }

    EXPECT_EQ(fc.isEmpty(), false);

    usleep(1000*1050);
    swss::DBConnector db("COUNTERS_DB", 0);
    swss::RedisPipeline pipeline(&db);
    swss::Table countersTable(&pipeline, COUNTERS_TABLE, false);

    std::vector<std::string> keys;
    countersTable.getKeys(keys);
    EXPECT_EQ(keys.size(), object_ids.size());

    for (size_t i = 0; i < object_ids.size(); i++)
    {
        std::string expectedKey = toOid(object_ids[i]);
        verifyFunc(countersTable, expectedKey, counterIdNames, expectedValues);
    }

    for (auto object_id : object_ids)
    {
        fc.removeCounter(object_id);
        if (!autoRemoveDbEntry)
        {
            countersTable.del(toOid(object_id));
        }
    }
    EXPECT_EQ(fc.isEmpty(), true);

    countersTable.getKeys(keys);
    ASSERT_TRUE(keys.empty());
}

TEST(FlexCounter, addRemoveCounter)
{
    sai->mock_getStatsExt = [](sai_object_type_t, sai_object_id_t, uint32_t number_of_counters, const sai_stat_id_t *, sai_stats_mode_t, uint64_t *counters) {
        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            counters[i] = (i + 1) * 100;
        }
        return SAI_STATUS_SUCCESS;
    };
    sai->mock_getStats = [](sai_object_type_t, sai_object_id_t, uint32_t number_of_counters, const sai_stat_id_t *, uint64_t *counters) {
        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            counters[i] = (i + 1) * 100;
        }
        return SAI_STATUS_SUCCESS;
    };
    sai->mock_queryStatsCapability = [](sai_object_id_t switch_id, sai_object_type_t object_type, sai_stat_capability_list_t *stats_capability) {
        // For now, just return failure to make test simple, will write a singe test to cover querySupportedCounters
        return SAI_STATUS_FAILURE;
    };

    sai->mock_bulkGetStats = [](sai_object_id_t, sai_object_type_t, uint32_t, const sai_object_key_t *, uint32_t, const sai_stat_id_t *, sai_stats_mode_t, sai_status_t *, uint64_t *)
    {
        return SAI_STATUS_FAILURE;
    };

    auto counterVerifyFunc = [] (swss::Table &countersTable, const std::string& key, const std::vector<std::string>& counterIdNames, const std::vector<std::string>& expectedValues)
    {
        std::string value;
        for (size_t i = 0; i < counterIdNames.size(); i++)
        {
            countersTable.hget(key, counterIdNames[i], value);
            EXPECT_EQ(value, expectedValues[i]);
        }
    };

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_COUNTER,
        FLOW_COUNTER_ID_LIST,
        {"SAI_COUNTER_STAT_PACKETS", "SAI_COUNTER_STAT_BYTES"},
        {"100", "200"},
        counterVerifyFunc,
        true);

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_MACSEC_FLOW,
        MACSEC_FLOW_COUNTER_ID_LIST,
        {"SAI_MACSEC_FLOW_STAT_CONTROL_PKTS", "SAI_MACSEC_FLOW_STAT_PKTS_UNTAGGED"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_MACSEC_SA,
        MACSEC_SA_COUNTER_ID_LIST,
        {"SAI_MACSEC_SA_STAT_OCTETS_ENCRYPTED", "SAI_MACSEC_SA_STAT_OCTETS_PROTECTED"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_PORT,
        PORT_COUNTER_ID_LIST,
        {"SAI_PORT_STAT_IF_IN_OCTETS", "SAI_PORT_STAT_IF_IN_UCAST_PKTS"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_PORT,
        PORT_DEBUG_COUNTER_ID_LIST,
        {"SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS", "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_1_DROPPED_PKTS"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    bool clearCalled = false;
    sai->mock_clearStats = [&] (sai_object_type_t object_type, sai_object_id_t object_id, uint32_t number_of_counters, const sai_stat_id_t *counter_ids) {
        clearCalled = true;
        return SAI_STATUS_SUCCESS;
    };

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_QUEUE,
        QUEUE_COUNTER_ID_LIST,
        {"SAI_QUEUE_STAT_PACKETS", "SAI_QUEUE_STAT_BYTES"},
        {"100", "200"},
        counterVerifyFunc,
        false,
        STATS_MODE_READ_AND_CLEAR);
    EXPECT_EQ(true, clearCalled);

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP,
        PG_COUNTER_ID_LIST,
        {"SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS", "SAI_INGRESS_PRIORITY_GROUP_STAT_BYTES"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_ROUTER_INTERFACE,
        RIF_COUNTER_ID_LIST,
        {"SAI_ROUTER_INTERFACE_STAT_IN_OCTETS", "SAI_ROUTER_INTERFACE_STAT_IN_PACKETS"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_SWITCH,
        SWITCH_DEBUG_COUNTER_ID_LIST,
        {"SAI_SWITCH_STAT_IN_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS", "SAI_SWITCH_STAT_IN_CONFIGURED_DROP_REASONS_1_DROPPED_PKTS"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_TUNNEL,
        TUNNEL_COUNTER_ID_LIST,
        {"SAI_TUNNEL_STAT_IN_OCTETS", "SAI_TUNNEL_STAT_IN_PACKETS"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        1,
        (sai_object_type_t)SAI_OBJECT_TYPE_ENI,
        ENI_COUNTER_ID_LIST,
        {"SAI_ENI_STAT_FLOW_CREATED", "SAI_ENI_STAT_FLOW_CREATE_FAILED", "SAI_ENI_STAT_FLOW_DELETED", "SAI_ENI_STAT_FLOW_DELETE_FAILED"},
        {"100", "200", "300", "400"},
        counterVerifyFunc,
        false);

    clearCalled = false;
    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_BUFFER_POOL,
        BUFFER_POOL_COUNTER_ID_LIST,
        {"SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES", "SAI_BUFFER_POOL_STAT_WATERMARK_BYTES"},
        {"100", "200"},
        counterVerifyFunc,
        false);
    EXPECT_EQ(true, clearCalled);

    sai->mock_get = [] (sai_object_type_t objectType, sai_object_id_t objectId, uint32_t attr_count, sai_attribute_t *attr_list) {
        for (uint32_t i = 0; i < attr_count; i++)
        {
            if (attr_list[i].id == SAI_QUEUE_ATTR_PAUSE_STATUS)
            {
                attr_list[i].value.booldata = false;
            }
        }
        return SAI_STATUS_SUCCESS;
    };

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_QUEUE,
        QUEUE_ATTR_ID_LIST,
        {"SAI_QUEUE_ATTR_PAUSE_STATUS"},
        {"false"},
        counterVerifyFunc,
        false);

    sai->mock_get = [] (sai_object_type_t objectType, sai_object_id_t objectId, uint32_t attr_count, sai_attribute_t *attr_list) {
        for (uint32_t i = 0; i < attr_count; i++)
        {
            if (attr_list[i].id == SAI_INGRESS_PRIORITY_GROUP_ATTR_PORT)
            {
                attr_list[i].value.oid = 1;
            }
        }
        return SAI_STATUS_SUCCESS;
    };

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP,
        PG_ATTR_ID_LIST,
        {"SAI_INGRESS_PRIORITY_GROUP_ATTR_PORT"},
        {"oid:0x1"},
        counterVerifyFunc,
        false);

    sai->mock_get = [] (sai_object_type_t objectType, sai_object_id_t objectId, uint32_t attr_count, sai_attribute_t *attr_list) {
        for (uint32_t i = 0; i < attr_count; i++)
        {
            if (attr_list[i].id == SAI_MACSEC_SA_ATTR_CONFIGURED_EGRESS_XPN)
            {
                attr_list[i].value.u64 = 0;
            }
            else if (attr_list[i].id == SAI_MACSEC_SA_ATTR_AN)
            {
                attr_list[i].value.u8 = 1;
            }
        }
        return SAI_STATUS_SUCCESS;
    };

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_MACSEC_SA,
        MACSEC_SA_ATTR_ID_LIST,
        {"SAI_MACSEC_SA_ATTR_CONFIGURED_EGRESS_XPN", "SAI_MACSEC_SA_ATTR_AN"},
        {"0", "1"},
        counterVerifyFunc,
        false);

    sai->mock_get = [] (sai_object_type_t objectType, sai_object_id_t objectId, uint32_t attr_count, sai_attribute_t *attr_list) {
        for (uint32_t i = 0; i < attr_count; i++)
        {
            if (attr_list[i].id == SAI_ACL_COUNTER_ATTR_PACKETS)
            {
                attr_list[i].value.u64 = 1000;
            }
        }
        return SAI_STATUS_SUCCESS;
    };

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_ACL_COUNTER,
        ACL_COUNTER_ATTR_ID_LIST,
        {"SAI_ACL_COUNTER_ATTR_PACKETS"},
        {"1000"},
        counterVerifyFunc,
        false);
}

TEST(FlexCounter, queryCounterCapability)
{
    sai->mock_queryStatsCapability = [](sai_object_id_t switch_id, sai_object_type_t object_type, sai_stat_capability_list_t *stats_capability) {
        if (stats_capability->count == 0)
        {
            stats_capability->count = 1;
            return SAI_STATUS_BUFFER_OVERFLOW;
        }
        else
        {
            stats_capability->list[0].stat_enum = SAI_PORT_STAT_IF_IN_OCTETS;
            stats_capability->list[0].stat_modes = SAI_STATS_MODE_READ | SAI_STATS_MODE_READ_AND_CLEAR;
            return SAI_STATUS_SUCCESS;
        }
    };

    sai->mock_getStats = [](sai_object_type_t, sai_object_id_t, uint32_t number_of_counters, const sai_stat_id_t *, uint64_t *counters) {
        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            counters[i] = 1000;
        }
        return SAI_STATUS_SUCCESS;
    };

    sai->mock_clearStats = [&] (sai_object_type_t object_type, sai_object_id_t object_id, uint32_t number_of_counters, const sai_stat_id_t *counter_ids) {
        return SAI_STATUS_SUCCESS;
    };

    sai->mock_bulkGetStats = [](sai_object_id_t, sai_object_type_t, uint32_t, const sai_object_key_t *, uint32_t, const sai_stat_id_t *, sai_stats_mode_t, sai_status_t *, uint64_t *)
    {
        return SAI_STATUS_FAILURE;
    };

    auto counterVerifyFunc = [] (swss::Table &countersTable, const std::string& key, const std::vector<std::string>& counterIdNames, const std::vector<std::string>& expectedValues)
    {
        std::string value;
        countersTable.hget(key, "SAI_PORT_STAT_IF_IN_OCTETS", value);
        EXPECT_EQ(value, "1000");
        // SAI_PORT_STAT_IF_IN_UCAST_PKTS is not supported, shall not in countersTable
        bool ret = countersTable.hget(key, "SAI_PORT_STAT_IF_IN_UCAST_PKTS", value);
        EXPECT_EQ(false, ret);
    };

    testAddRemoveCounter(
        1,
        SAI_OBJECT_TYPE_PORT,
        PORT_COUNTER_ID_LIST,
        {"SAI_PORT_STAT_IF_IN_OCTETS", "SAI_PORT_STAT_IF_IN_UCAST_PKTS"},
        {},
        counterVerifyFunc,
        false);
}

TEST(FlexCounter, noSupportedCounters)
{
    sai->mock_queryStatsCapability = [](sai_object_id_t switch_id, sai_object_type_t object_type, sai_stat_capability_list_t *stats_capability) {
        return SAI_STATUS_FAILURE;
    };

    sai->mock_getStats = [](sai_object_type_t, sai_object_id_t, uint32_t number_of_counters, const sai_stat_id_t *, uint64_t *counters) {
        return SAI_STATUS_FAILURE;
    };

    FlexCounter fc("test", sai, "COUNTERS_DB");
    std::vector<swss::FieldValueTuple> values;
    values.emplace_back(PORT_COUNTER_ID_LIST, "SAI_PORT_STAT_IF_IN_OCTETS,SAI_PORT_STAT_IF_IN_UCAST_PKTS");

    test_syncd::mockVidManagerObjectTypeQuery(SAI_OBJECT_TYPE_PORT);

    fc.addCounter(sai_object_id_t(0x1000000000000), sai_object_id_t(0x1000000000000), values);
    // No supported counter, this object shall not be queried
    EXPECT_EQ(fc.isEmpty(), true);
}

void testAddRemovePlugin(const std::string& pluginFieldName)
{
    SWSS_LOG_ENTER();

    FlexCounter fc("test", sai, "COUNTERS_DB");

    std::vector<swss::FieldValueTuple> values;
    values.emplace_back(pluginFieldName, "dummy_sha_strings");
    fc.addCounterPlugin(values);
    EXPECT_EQ(fc.isEmpty(), false);

    fc.removeCounterPlugins();
    EXPECT_EQ(fc.isEmpty(), true);
}

TEST(FlexCounter, addRemoveCounterPlugin)
{
    std::string fields[] = {QUEUE_PLUGIN_FIELD,
                            PG_PLUGIN_FIELD,
                            PORT_PLUGIN_FIELD,
                            RIF_PLUGIN_FIELD,
                            BUFFER_POOL_PLUGIN_FIELD,
                            TUNNEL_PLUGIN_FIELD,
                            FLOW_COUNTER_PLUGIN_FIELD};
    for (auto &field : fields)
    {
        testAddRemovePlugin(field);
    }
}

TEST(FlexCounter, addRemoveCounterForPort)
{
    FlexCounter fc("test", sai, "COUNTERS_DB");

    sai_object_id_t counterVid{0x1000000000000};
    sai_object_id_t counterRid{0x1000000000000};
    std::vector<swss::FieldValueTuple> values;
    values.emplace_back(PORT_COUNTER_ID_LIST, "SAI_PORT_STAT_IF_IN_OCTETS,SAI_PORT_STAT_IF_IN_ERRORS");

    test_syncd::mockVidManagerObjectTypeQuery(SAI_OBJECT_TYPE_PORT);
    sai->mock_getStats = [](sai_object_type_t, sai_object_id_t, uint32_t number_of_counters, const sai_stat_id_t *ids, uint64_t *counters) {
        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            if (ids[i] == SAI_PORT_STAT_IF_IN_OCTETS)
            {
                counters[i] = 100;
            }
            else if (ids[i] == SAI_PORT_STAT_IF_IN_ERRORS)
            {
                counters[i] = 200;
            }
            else
            {
                return SAI_STATUS_FAILURE;
            }
        }
        return SAI_STATUS_SUCCESS;
    };

    fc.addCounter(counterVid, counterRid, values);
    EXPECT_EQ(fc.isEmpty(), false);

    values.clear();
    values.emplace_back(POLL_INTERVAL_FIELD, "1000");
    values.emplace_back(FLEX_COUNTER_STATUS_FIELD, "enable");
    values.emplace_back(STATS_MODE_FIELD, STATS_MODE_READ);
    fc.addCounterPlugin(values);

    usleep(1000*1000);
    swss::DBConnector db("COUNTERS_DB", 0);
    swss::RedisPipeline pipeline(&db);
    swss::Table countersTable(&pipeline, COUNTERS_TABLE, false);

    std::vector<std::string> keys;
    countersTable.getKeys(keys);
    EXPECT_EQ(keys.size(), size_t(1));
    std::string expectedKey = toOid(counterVid);
    EXPECT_EQ(keys[0], expectedKey);

    std::string value;
    countersTable.hget(expectedKey, "SAI_PORT_STAT_IF_IN_OCTETS", value);
    EXPECT_EQ(value, "100");
    countersTable.hget(expectedKey, "SAI_PORT_STAT_IF_IN_ERRORS", value);
    EXPECT_EQ(value, "200");

    fc.removeCounter(counterVid);
    EXPECT_EQ(fc.isEmpty(), true);
    countersTable.del(expectedKey);
    countersTable.getKeys(keys);
    ASSERT_TRUE(keys.empty());

    // Test again with queryStatsCapability support
    sai->mock_queryStatsCapability = [](sai_object_id_t, sai_object_type_t, sai_stat_capability_list_t *capability) {
        if (capability->count < 2)
        {
            capability->count = 2;
            return SAI_STATUS_BUFFER_OVERFLOW;
        }

        capability->list[0].stat_enum = SAI_PORT_STAT_IF_IN_OCTETS;
        capability->list[0].stat_modes = SAI_STATS_MODE_READ | SAI_STATS_MODE_READ_AND_CLEAR;
        capability->list[1].stat_enum = SAI_PORT_STAT_IF_IN_ERRORS;
        capability->list[1].stat_modes = SAI_STATS_MODE_READ | SAI_STATS_MODE_READ_AND_CLEAR;
        return SAI_STATUS_SUCCESS;
    };

    values.clear();
    values.emplace_back(PORT_COUNTER_ID_LIST, "SAI_PORT_STAT_IF_IN_OCTETS,SAI_PORT_STAT_IF_IN_ERRORS");
    fc.addCounter(counterVid, counterRid, values);
    EXPECT_EQ(fc.isEmpty(), false);

    usleep(1000*1000);
    countersTable.hget(expectedKey, "SAI_PORT_STAT_IF_IN_OCTETS", value);
    EXPECT_EQ(value, "100");
    countersTable.hget(expectedKey, "SAI_PORT_STAT_IF_IN_ERRORS", value);
    EXPECT_EQ(value, "200");

    fc.removeCounter(counterVid);
    EXPECT_EQ(fc.isEmpty(), true);
    countersTable.del(expectedKey);
    countersTable.getKeys(keys);
    ASSERT_TRUE(keys.empty());
}

TEST(FlexCounter, bulkCounter)
{
    sai->mock_getStatsExt = [&](sai_object_type_t, sai_object_id_t, uint32_t number_of_counters, const sai_stat_id_t *, sai_stats_mode_t, uint64_t *counters) {
        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            counters[i] = (i + 1) * 10;
        }
        return SAI_STATUS_SUCCESS;
    };
    sai->mock_getStats = [&](sai_object_type_t, sai_object_id_t, uint32_t number_of_counters, const sai_stat_id_t *, uint64_t *counters) {
        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            counters[i] = (i + 1) * 10;
        }
        return SAI_STATUS_SUCCESS;
    };
    sai->mock_queryStatsCapability = [&](sai_object_id_t switch_id, sai_object_type_t object_type, sai_stat_capability_list_t *stats_capability) {
        // For now, just return failure to make test simple, will write a singe test to cover querySupportedCounters
        return SAI_STATUS_FAILURE;
    };

    bool clearCalled = false;
    sai->mock_bulkGetStats = [&](sai_object_id_t,
                                sai_object_type_t,
                                uint32_t object_count,
                                const sai_object_key_t *object_keys,
                                uint32_t number_of_counters,
                                const sai_stat_id_t *counter_ids,
                                sai_stats_mode_t mode,
                                sai_status_t *object_status,
                                uint64_t *counters)
    {
        EXPECT_TRUE(mode == SAI_STATS_MODE_BULK_READ_AND_CLEAR || mode == SAI_STATS_MODE_BULK_READ);
        if (mode == SAI_STATS_MODE_BULK_READ_AND_CLEAR)
        {
            clearCalled = true;
        }
        for (uint32_t i = 0; i < object_count; i++)
        {
            object_status[i] = SAI_STATUS_SUCCESS;
            for (uint32_t j = 0; j < number_of_counters; j++)
            {
                counters[i * number_of_counters + j] = (j + 1) * 100;
            }
        }
        return SAI_STATUS_SUCCESS;
    };

    auto counterVerifyFunc = [] (swss::Table &countersTable, const std::string& key, const std::vector<std::string>& counterIdNames, const std::vector<std::string>& expectedValues)
    {
        std::string value;
        for (size_t i = 0; i < counterIdNames.size(); i++)
        {
            countersTable.hget(key, counterIdNames[i], value);
            ASSERT_EQ(value, expectedValues[i]);
        }
    };

    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_COUNTER,
        FLOW_COUNTER_ID_LIST,
        {"SAI_COUNTER_STAT_PACKETS", "SAI_COUNTER_STAT_BYTES"},
        {"100", "200"},
        counterVerifyFunc,
        true);

    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_MACSEC_FLOW,
        MACSEC_FLOW_COUNTER_ID_LIST,
        {"SAI_MACSEC_FLOW_STAT_CONTROL_PKTS", "SAI_MACSEC_FLOW_STAT_PKTS_UNTAGGED"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_MACSEC_SA,
        MACSEC_SA_COUNTER_ID_LIST,
        {"SAI_MACSEC_SA_STAT_OCTETS_ENCRYPTED", "SAI_MACSEC_SA_STAT_OCTETS_PROTECTED"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_PORT,
        PORT_COUNTER_ID_LIST,
        {"SAI_PORT_STAT_IF_IN_OCTETS", "SAI_PORT_STAT_IF_IN_UCAST_PKTS"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_PORT,
        PORT_DEBUG_COUNTER_ID_LIST,
        {"SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS", "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_1_DROPPED_PKTS"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_QUEUE,
        QUEUE_COUNTER_ID_LIST,
        {"SAI_QUEUE_STAT_PACKETS", "SAI_QUEUE_STAT_BYTES"},
        {"100", "200"},
        counterVerifyFunc,
        false,
        STATS_MODE_READ_AND_CLEAR);
    EXPECT_EQ(true, clearCalled);

    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP,
        PG_COUNTER_ID_LIST,
        {"SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS", "SAI_INGRESS_PRIORITY_GROUP_STAT_BYTES"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_ROUTER_INTERFACE,
        RIF_COUNTER_ID_LIST,
        {"SAI_ROUTER_INTERFACE_STAT_IN_OCTETS", "SAI_ROUTER_INTERFACE_STAT_IN_PACKETS"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_SWITCH,
        SWITCH_DEBUG_COUNTER_ID_LIST,
        {"SAI_SWITCH_STAT_IN_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS", "SAI_SWITCH_STAT_IN_CONFIGURED_DROP_REASONS_1_DROPPED_PKTS"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_TUNNEL,
        TUNNEL_COUNTER_ID_LIST,
        {"SAI_TUNNEL_STAT_IN_OCTETS", "SAI_TUNNEL_STAT_IN_PACKETS"},
        {"100", "200"},
        counterVerifyFunc,
        false);

    testAddRemoveCounter(
        2,
        (sai_object_type_t)SAI_OBJECT_TYPE_ENI,
        ENI_COUNTER_ID_LIST,
        {"SAI_ENI_STAT_FLOW_CREATED", "SAI_ENI_STAT_FLOW_CREATE_FAILED", "SAI_ENI_STAT_FLOW_DELETED", "SAI_ENI_STAT_FLOW_DELETE_FAILED"},
        {"100", "200", "300", "400"},
        counterVerifyFunc,
        false);

    clearCalled = false;
    testAddRemoveCounter(
        2,
        SAI_OBJECT_TYPE_BUFFER_POOL,
        BUFFER_POOL_COUNTER_ID_LIST,
        {"SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES", "SAI_BUFFER_POOL_STAT_WATERMARK_BYTES"},
        {"10", "20"},
        counterVerifyFunc,
        false);
    // buffer pool stats does not support bulk
    EXPECT_EQ(false, clearCalled);
}

TEST(FlexCounter, counterIdChange)
{
    sai->mock_getStats = [&](sai_object_type_t, sai_object_id_t, uint32_t number_of_counters, const sai_stat_id_t *, uint64_t *counters) {
        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            counters[i] = (i + 1) * 10;
        }
        return SAI_STATUS_SUCCESS;
    };
    sai->mock_bulkGetStats = [&](sai_object_id_t,
                                sai_object_type_t,
                                uint32_t object_count,
                                const sai_object_key_t *object_keys,
                                uint32_t number_of_counters,
                                const sai_stat_id_t *counter_ids,
                                sai_stats_mode_t mode,
                                sai_status_t *object_status,
                                uint64_t *counters)
    {
        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            switch(counter_ids[i])
            {
                case SAI_PORT_STAT_IF_IN_OCTETS:
                case SAI_PORT_STAT_IF_IN_UCAST_PKTS:
                    break;
                default:
                    return SAI_STATUS_NOT_SUPPORTED;
            }
        }

        for (uint32_t i = 0; i < object_count; i++)
        {
            object_status[i] = SAI_STATUS_SUCCESS;
            for (uint32_t j = 0; j < number_of_counters; j++)
            {
                counters[i * number_of_counters + j] = (j + 1) * 100;
            }
        }
        return SAI_STATUS_SUCCESS;
    };
    auto counterVerifyFunc = [] (swss::Table &countersTable, const std::string& key, const std::vector<std::string>& counterIdNames, const std::vector<std::string>& expectedValues)
    {
        std::string value;
        for (size_t i = 0; i < counterIdNames.size(); i++)
        {
            countersTable.hget(key, counterIdNames[i], value);
            ASSERT_EQ(value, expectedValues[i]);
        }
    };

    FlexCounter fc("test", sai, "COUNTERS_DB");

    test_syncd::mockVidManagerObjectTypeQuery(SAI_OBJECT_TYPE_PORT);

    std::vector<swss::FieldValueTuple> values;
    values.emplace_back(POLL_INTERVAL_FIELD, "1000");
    values.emplace_back(FLEX_COUNTER_STATUS_FIELD, "enable");
    values.emplace_back(STATS_MODE_FIELD, STATS_MODE_READ);
    fc.addCounterPlugin(values);

    values.clear();
    values.emplace_back(PORT_COUNTER_ID_LIST, "SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS,SAI_PORT_STAT_IF_IN_DISCARDS");
    sai_object_id_t oid{0x1000000000000};
    fc.addCounter(oid, oid, values);

    usleep(1000*1050);
    swss::DBConnector db("COUNTERS_DB", 0);
    swss::RedisPipeline pipeline(&db);
    swss::Table countersTable(&pipeline, COUNTERS_TABLE, false);

    std::vector<std::string> keys;
    countersTable.getKeys(keys);
    EXPECT_EQ(keys.size(),1);
    std::string expectedKey = toOid(oid);
    counterVerifyFunc(countersTable,
                      expectedKey,
                      {"SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS", "SAI_PORT_STAT_IF_IN_DISCARDS"},
                      {"10", "20"});

    // not support bulk to support bulk
    values.clear();
    values.emplace_back(PORT_COUNTER_ID_LIST, "SAI_PORT_STAT_IF_IN_OCTETS,SAI_PORT_STAT_IF_IN_UCAST_PKTS");
    fc.addCounter(oid, oid, values);

    usleep(1000*1050);
    counterVerifyFunc(countersTable,
                      expectedKey,
                      {"SAI_PORT_STAT_IF_IN_OCTETS", "SAI_PORT_STAT_IF_IN_UCAST_PKTS"},
                      {"100", "200"});

    // support bulk but counter id changes
    values.clear();
    values.emplace_back(PORT_COUNTER_ID_LIST, "SAI_PORT_STAT_IF_IN_OCTETS");
    fc.addCounter(oid, oid, values);

    usleep(1000*1050);
    counterVerifyFunc(countersTable,
                      expectedKey,
                      {"SAI_PORT_STAT_IF_IN_OCTETS"},
                      {"100"});

    // support bulk with different counter id
    sai_object_id_t oid1{0x1000000000001};
    values.clear();
    values.emplace_back(PORT_COUNTER_ID_LIST, "SAI_PORT_STAT_IF_IN_OCTETS,SAI_PORT_STAT_IF_IN_UCAST_PKTS");
    fc.addCounter(oid1, oid1, values);

    usleep(1000*1050);
    counterVerifyFunc(countersTable,
                      toOid(oid1),
                      {"SAI_PORT_STAT_IF_IN_OCTETS", "SAI_PORT_STAT_IF_IN_UCAST_PKTS"},
                      {"100", "200"});

    // support bulk to not support bulk
    values.clear();
    values.emplace_back(PORT_COUNTER_ID_LIST, "SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS,SAI_PORT_STAT_IF_IN_UCAST_PKTS");
    fc.addCounter(oid, oid, values);

    usleep(1000*1050);
    counterVerifyFunc(countersTable,
                      expectedKey,
                      {"SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS", "SAI_PORT_STAT_IF_IN_UCAST_PKTS"},
                      {"10", "20"});

    // not support bulk but counter id changes
    values.clear();
    values.emplace_back(PORT_COUNTER_ID_LIST, "SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS,SAI_PORT_STAT_IF_IN_DISCARDS");
    fc.addCounter(oid, oid, values);
    usleep(1000*1050);
    counterVerifyFunc(countersTable,
                      expectedKey,
                      {"SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS", "SAI_PORT_STAT_IF_IN_DISCARDS"},
                      {"10", "20"});

    // verify oid1 is still using bulk
    counterVerifyFunc(countersTable,
                      toOid(oid1),
                      {"SAI_PORT_STAT_IF_IN_OCTETS", "SAI_PORT_STAT_IF_IN_UCAST_PKTS"},
                      {"100", "200"});

    fc.removeCounter(oid);
    countersTable.del(expectedKey);
    fc.removeCounter(oid1);
    countersTable.del(toOid(oid1));
}
