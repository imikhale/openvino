// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "behavior/ov_executable_network/exec_network_base.hpp"

using namespace ov::test::behavior;
namespace {
const std::vector<ov::AnyMap> configs = {
        {},
};

const std::vector<ov::AnyMap> autoBatchConfigs = {
        // explicit batch size 4 to avoid fallback to no auto-batching (i.e. plain GPU)
        {{CONFIG_KEY(AUTO_BATCH_DEVICE_CONFIG) , std::string(CommonTestUtils::DEVICE_GPU) + "(4)"},
                // no timeout to avoid increasing the test time
                {CONFIG_KEY(AUTO_BATCH_TIMEOUT) , "0 "}}
};

INSTANTIATE_TEST_SUITE_P(smoke_BehaviorTests, OVExecutableNetworkBaseTest,
                        ::testing::Combine(
                                ::testing::Values(CommonTestUtils::DEVICE_GPU),
                                ::testing::ValuesIn(configs)),
                        OVExecutableNetworkBaseTest::getTestCaseName);

INSTANTIATE_TEST_SUITE_P(smoke_AutoBatchBehaviorTests, OVExecutableNetworkBaseTest,
                         ::testing::Combine(
                                 ::testing::Values(CommonTestUtils::DEVICE_BATCH),
                                 ::testing::ValuesIn(autoBatchConfigs)),
                         OVExecutableNetworkBaseTest::getTestCaseName);
}  // namespace