// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "mkldnn_shuffle_channels_node.h"

#include <ie_parallel.hpp>
#include <mkldnn_extension_utils.h>
#include <cpu/x64/jit_generator.hpp>
#include "common/blocked_desc_creator.h"

#include "common/cpu_memcpy.h"
#include "utils/general_utils.h"

#include <string>
#include <cmath>
#include <common/primitive_hashing_utils.hpp>

#define THROW_SHCH_ERROR IE_THROW() << "ShuffleChannels layer with name '" << getName() << "' "

using namespace mkldnn;
using namespace MKLDNNPlugin;
using namespace InferenceEngine;
using namespace mkldnn::impl;
using namespace mkldnn::impl::cpu::x64;

size_t MKLDNNShuffleChannelsNode::ShuffleChannelsAttributes::hash() const {
    using namespace dnnl::impl;
    using namespace dnnl::impl::primitive_hashing;

    size_t seed = 0;
    seed = hash_combine(seed, layoutType);
    seed = hash_combine(seed, dataRank);
    seed = hash_combine(seed, axis);
    seed = hash_combine(seed, spatialRank);
    seed = hash_combine(seed, group);
    seed = hash_combine(seed, dataSize);
    seed = get_vector_hash(seed, srcDims);
    seed = get_vector_hash(seed, srcBlockedDims);

    return seed;
}

bool MKLDNNShuffleChannelsNode::ShuffleChannelsAttributes::operator==(const ShuffleChannelsAttributes& rhs) const {
    bool result = layoutType == rhs.layoutType && dataRank == rhs.dataRank &&
                  axis == rhs.axis && spatialRank == rhs.spatialRank &&
                  group == rhs.group && dataSize == rhs.dataSize && srcDims == rhs.srcDims &&
                  srcBlockedDims == rhs.srcBlockedDims;
    return result;
}

bool MKLDNNShuffleChannelsNode::isSupportedOperation(const std::shared_ptr<const ngraph::Node>& op, std::string& errorMessage) noexcept {
    try {
        auto shuffleChannels = ov::as_type_ptr<const ngraph::op::v0::ShuffleChannels>(op);
        if (!shuffleChannels) {
            errorMessage = "Only opset1 ShuffleChannels operation is supported";
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

MKLDNNShuffleChannelsNode::MKLDNNShuffleChannelsNode(const std::shared_ptr<ngraph::Node>& op, const mkldnn::engine& eng, MKLDNNWeightsSharing::Ptr &cache)
        : MKLDNNNode(op, eng, cache) {
    std::string errorMessage;
    if (!isSupportedOperation(op, errorMessage)) {
        IE_THROW(NotImplemented) << errorMessage;
    }

    if (inputShapes.size() != 1 || outputShapes.size() != 1)
        THROW_SHCH_ERROR << "has incorrect number of input/output edges.";

    auto shuffleChannels = ov::as_type_ptr<const ngraph::op::v0::ShuffleChannels>(op);
    attrs.group = shuffleChannels->get_group();
    attrs.axis = shuffleChannels->get_axis();
    attrs.dataRank = getInputShapeAtPort(0).getRank();
    if (attrs.axis < 0)
        attrs.axis += attrs.dataRank;

    supportDynamicBatch = (attrs.axis != 0);
}

void MKLDNNShuffleChannelsNode::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    InferenceEngine::Precision precision = getOriginalInputPrecisionAtPort(0);
    const std::set<size_t> supported_precision_sizes = {1, 2, 4, 8, 16};
    if (supported_precision_sizes.find(precision.size()) == supported_precision_sizes.end())
        THROW_SHCH_ERROR << "has unsupported precision: " << precision.name();

    impl_desc_type impl_type;
    if (mayiuse(cpu::x64::avx512_common)) {
        impl_type = impl_desc_type::jit_avx512;
    } else if (mayiuse(cpu::x64::avx2)) {
        impl_type = impl_desc_type::jit_avx2;
    } else if (mayiuse(cpu::x64::sse41)) {
        impl_type = impl_desc_type::jit_sse42;
    } else {
        impl_type = impl_desc_type::ref;
    }

    // use ncsp as default for non-quantized networks and nspc for quantized
    auto firstCreatorType = isInQuantizedGraph ? LayoutType::nspc : LayoutType::ncsp;
    auto secondCreatorType = isInQuantizedGraph ? LayoutType::ncsp : LayoutType::nspc;

    addSupportedPrimDesc({{firstCreatorType, precision}},
                         {{firstCreatorType, precision}},
                         impl_type, supportDynamicBatch);
    addSupportedPrimDesc({{secondCreatorType, precision}},
                         {{secondCreatorType, precision}},
                         impl_type, supportDynamicBatch);
    // canUseBlocked
    if (attrs.axis != 1) {
        addSupportedPrimDesc({{LayoutType::nCsp8c, precision}},
                             {{LayoutType::nCsp8c, precision}},
                             impl_type, supportDynamicBatch);
        addSupportedPrimDesc({{LayoutType::nCsp16c, precision}},
                             {{LayoutType::nCsp16c, precision}},
                             impl_type, supportDynamicBatch);
    }
}

void MKLDNNShuffleChannelsNode::createPrimitive() {
    auto &dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    auto &srcMemPtr = getParentEdgeAt(0)->getMemoryPtr();
    if (!dstMemPtr || !dstMemPtr->GetPrimitivePtr())
        THROW_SHCH_ERROR << "has not allocated destination memory";
    if (!srcMemPtr || !srcMemPtr->GetPrimitivePtr())
        THROW_SHCH_ERROR << "has not allocated input memory";
    if (getSelectedPrimitiveDescriptor() == nullptr)
        THROW_SHCH_ERROR << "has unidentified preferable primitive descriptor";

    const auto& memoryDesc = srcMemPtr->getDesc();
    attrs.spatialRank = attrs.dataRank - attrs.axis - 1;
    attrs.dataSize = memoryDesc.getPrecision().size();
    attrs.layoutType = memoryDesc.hasLayoutType(LayoutType::nCsp16c) ? LayoutType::nCsp16c :
                       memoryDesc.hasLayoutType(LayoutType::nCsp8c) ? LayoutType::nCsp8c :
                       memoryDesc.hasLayoutType(LayoutType::nspc) ? LayoutType::nspc : LayoutType::ncsp;

    if (inputShapesDefined() && isExecutable()) {
        if (needPrepareParams())
            prepareParams();
        updateLastInputDims();
    }
}

void MKLDNNShuffleChannelsNode::prepareParams() {
    auto& srcMemPtr = getParentEdgeAt(0)->getMemoryPtr();
    auto builder = [](const ShuffleChannelsAttributes& key) -> std::shared_ptr<ShuffleChannelsExecutor> {
        return std::make_shared<ShuffleChannelsExecutor>(key);
    };
    attrs.srcDims = srcMemPtr->getStaticDims();
    attrs.srcBlockedDims = srcMemPtr->GetDescWithType<BlockedMemoryDesc>()->getBlockDims();

    auto cache = getRuntimeCache();
    auto result = cache->getOrCreate(attrs, builder);
    VERBOSE_HELPER_NODE_PREPARE_PARAMS(result.second);
    if (!result.first) {
        IE_THROW() << "ShuffleChannelsExecutor was not found for node " << getName() << ".";
    }

    execPtr = result.first;
}

MKLDNNShuffleChannelsNode::ShuffleChannelsExecutor::ShuffleChannelsExecutor(const ShuffleChannelsAttributes& attrs) {
    if (!one_of(attrs.layoutType, LayoutType::nCsp16c, LayoutType::nCsp8c, LayoutType::nspc, LayoutType::ncsp))
        IE_THROW() << "ShuffleChannels executor supports only 'nCsp16c', 'nCsp8c', 'nspc' or 'ncsp' layouts.";

    const bool isBlocked = MKLDNNPlugin::one_of(attrs.layoutType, LayoutType::nCsp16c, LayoutType::nCsp8c);
    const bool isChannelsLast = attrs.layoutType == LayoutType::nspc;
    const auto& srcDims = attrs.srcDims;
    const auto& srcBlockedDims = attrs.srcBlockedDims;

    // 2 for decomposed axis dim, 1 for composed spatial dim
    const int batchRank = attrs.axis;
    const int reshapedRank = batchRank + 2 + static_cast<int>(attrs.spatialRank != 0) + static_cast<int>(isBlocked && (attrs.spatialRank == 0));
    PermuteParams params;
    params.data_size = attrs.dataSize;
    params.order.resize(reshapedRank, 0);
    params.src_block_order.resize(reshapedRank);
    params.dst_block_order.resize(reshapedRank);
    params.dst_block_dims.resize(reshapedRank);
    params.src_block_dims.resize(reshapedRank);

    const size_t groupSize = srcDims[attrs.axis] / attrs.group;
    size_t spatialShapeSize = 1;
    if (attrs.spatialRank != 0) {
        for (int i = batchRank + 1; i < attrs.dataRank; i++) {
            spatialShapeSize *= srcDims[i];
        }
    }

    auto decomposeAndTranpose = [&](int axis) {
        params.src_block_dims[axis] = attrs.group;
        params.src_block_dims[axis + 1] = groupSize;
        params.order[axis] = axis + 1;
        params.order[axis + 1] = axis;
    };

    const int channelDim = 1;
    if (isBlocked) {
        size_t blkSize = srcBlockedDims.back();
        size_t CB = srcBlockedDims[1];
        if (attrs.axis > channelDim) {  // axis on spatial
            for (int i = 0; i < batchRank; i++) {
                params.order[i] = i;
                params.src_block_dims[i] = srcBlockedDims[i];
            }
            decomposeAndTranpose(batchRank);

            params.order[batchRank + 2] = batchRank + 2;
            params.src_block_dims[batchRank + 2] = spatialShapeSize * blkSize;
        } else { // axis on batch
            decomposeAndTranpose(0);
            spatialShapeSize = CB * blkSize;
            for (int i = 2; i < attrs.dataRank; i++) {
                spatialShapeSize *= srcDims[i];
            }
            params.order[2] = 2;
            params.src_block_dims[2] = spatialShapeSize;
        }
    } else if (isChannelsLast) {
        if (attrs.axis == channelDim) {  // axis on channel
            params.order[0] = 0;
            params.src_block_dims[0] = srcDims[0];
            params.order[1] = 1;
            params.src_block_dims[1] = spatialShapeSize;
            decomposeAndTranpose(2);
        } else if (attrs.axis > channelDim) {  // axis on spatial
            for (int i = 0; i < batchRank; i++) {
                if (i == 0) {
                    params.order[i] = i;
                    params.src_block_dims[i] = srcDims[i];
                } else if (i == 1) {
                    params.order[reshapedRank - 1] = reshapedRank - 1;
                    params.src_block_dims[params.order[reshapedRank - 1]] = srcDims[i];
                } else if (i > 1) {
                    params.order[i - 1] = i - 1;
                    params.src_block_dims[i - 1] = srcDims[i];
                }
            }
            decomposeAndTranpose(batchRank - 1);

            if (attrs.spatialRank != 0) {
                params.order[batchRank + 1] = batchRank + 1;
                params.src_block_dims[batchRank + 1] = spatialShapeSize;
            }
        } else { // axis on batch
            decomposeAndTranpose(0);
            params.order[2] = 2;
            params.src_block_dims[2] = spatialShapeSize;
        }
    } else {
        for (int i = 0; i < batchRank; i++) {
            params.src_block_dims[i] = srcDims[i];
            params.order[i] = i;
        }

        decomposeAndTranpose(batchRank);
        if (attrs.spatialRank != 0) {
            params.order[batchRank + 2] = batchRank + 2;
            params.src_block_dims[batchRank + 2] = spatialShapeSize;
        }
    }

    std::iota(params.src_block_order.begin(), params.src_block_order.end(), 0);
    std::iota(params.dst_block_order.begin(), params.dst_block_order.end(), 0);
    for (size_t i = 0; i < reshapedRank; i++)
        params.dst_block_dims[i] = params.src_block_dims[params.order[i]];

    permuteKernel = std::unique_ptr<PermuteKernel>(new PermuteKernel(params));
}

void MKLDNNShuffleChannelsNode::ShuffleChannelsExecutor::exec(const uint8_t* srcData, uint8_t* dstData, const int MB) {
    if (!permuteKernel)
        IE_THROW() << "Could not execute. Kernel for Transpose node was not compiled.";

    if (MB > 0)
        permuteKernel->execute(srcData, dstData, MB);
    else
        permuteKernel->execute(srcData, dstData);
}

void MKLDNNShuffleChannelsNode::executeDynamicImpl(mkldnn::stream strm) {
    execute(strm);
}

void MKLDNNShuffleChannelsNode::execute(mkldnn::stream strm) {
    if (!execPtr)
        THROW_SHCH_ERROR << "doesn't have a compiled executor.";

    int MB = -1;
    if (supportDynamicBatch)
        MB = isDynamicNode() ? getParentEdgeAt(0)->getMemoryPtr()->getStaticDims()[0] : batchToProcess();

    const uint8_t* srcData = reinterpret_cast<const uint8_t*>(getParentEdgeAt(0)->getMemoryPtr()->GetPtr());
    uint8_t* dstData = reinterpret_cast<uint8_t*>(getChildEdgeAt(0)->getMemoryPtr()->GetPtr());
    execPtr->exec(srcData, dstData, MB);
}

bool MKLDNNShuffleChannelsNode::created() const {
    return getType() == ShuffleChannels;
}

REG_MKLDNN_PRIM_FOR(MKLDNNShuffleChannelsNode, ShuffleChannels);
