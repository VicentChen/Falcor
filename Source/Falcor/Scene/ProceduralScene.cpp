#include "stdafx.h"
#include "ProceduralScene.h"

namespace Falcor
{
    void ProceduralScene::setScene(const Tlas& tlas)
    {
        mTlas = tlas;
        mMeshCount = 0;
        mInstanceCount = 0;

        for (const auto& blas : mTlas)
        {
            mMeshCount += static_cast<uint>(blas.mGeometries.size());
            mInstanceCount += static_cast<uint>(blas.mInstances.size());
        }
    }

    Shader::DefineList ProceduralScene::getSceneDefines() const
    {
        Shader::DefineList defines;
        defines.add("MATERIAL_COUNT", std::to_string(1));
        defines.add("INDEXED_VERTICES", "0");
        return defines;
    }

    void ProceduralScene::preview(RenderContext* pContext, GraphicsState* pState, GraphicsVars* pVars)
    {
        logError("Preview AABBs not implemented yet.", Logger::MsgBox::ContinueAbort, false);
    }

    void ProceduralScene::raytrace(RenderContext* pContext, RtProgram* pProgram, const std::shared_ptr<RtProgramVars>& pVars, uint3 dispatchDims)
    {
        PROFILE("raytraceProceduralScene");

        auto rayTypeCount = pProgram->getHitProgramCount();
        setRaytracingShaderData(pContext, pVars->getRootVar(), rayTypeCount);

        // TODO: add support for DXR 1.0
        //// If not set yet, set geometry indices for this RtProgramVars.
        //if (pVars->getSceneForGeometryIndices().get() != this)
        //{
        //    setGeometryIndexIntoRtVars(pVars);
        //    pVars->setSceneForGeometryIndices(shared_from_this());
        //}

        // Set ray type constant.
        pVars->getRootVar()["DxrPerFrame"]["hitProgramCount"] = rayTypeCount;

        pContext->raytrace(pProgram, pVars.get(), dispatchDims.x, dispatchDims.y, dispatchDims.z);
    }

    void ProceduralScene::setRaytracingShaderData(RenderContext* pContext, const ShaderVar& var, uint32_t rayTypeCount)
    {
        // On first execution, create BLAS for each mesh.
        if (mBlasData.empty())
        {
            initGeomDesc(pContext);
            buildBlas(pContext);
        }

        // On first execution, when meshes have moved, when there's a new ray count, or when a BLAS has changed, create/update the TLAS
        //
        // TODO: The notion of "ray count" is being treated as fundamental here, and intrinsically
        // linked to the number of hit groups in the program, without checking if this matches
        // other things like the number of miss shaders. If/when we support meshes with custom
        // intersection shaders, then the assumption that number of ray types and number of
        // hit groups match will be incorrect.
        //
        // It really seems like a first-class notion of ray types (and the number thereof) is required.
        //
        auto tlasIt = mTlasCache.find(rayTypeCount);
        if (tlasIt == mTlasCache.end())
        {
            // We need a hit entry per mesh right now to pass GeometryIndex()
            buildTlas(pContext, rayTypeCount);

            // If new TLAS was just created, get it so the iterator is valid
            if (tlasIt == mTlasCache.end()) tlasIt = mTlasCache.find(rayTypeCount);
        }

        assert(tlasIt->second.pSrv);

        // Bind TLAS.
        var["gRtScene"].setSrv(tlasIt->second.pSrv);
    }

    void ProceduralScene::initGeomDesc(RenderContext* pContext)
    {
        auto convertBoundingBox2DxAABB = [&](const BoundingBox& aabb)
        {
            float3 aabbMin = aabb.center - aabb.extent;
            float3 aabbMax = aabb.center + aabb.extent;

            D3D12_RAYTRACING_AABB dxAABB = { aabbMin.x, aabbMin.y, aabbMin.z, aabbMax.x, aabbMax.y, aabbMax.z };
            return std::move(dxAABB);
        };
        
        std::vector<D3D12_RAYTRACING_AABB> dxAABBs;
        for (const auto& blas : mTlas)
            for (const auto& geometry : blas.mGeometries)
                std::transform(geometry.mPrimitives.begin(), geometry.mPrimitives.end(), std::back_inserter(dxAABBs), convertBoundingBox2DxAABB);

        mpGeometryBuffer = Buffer::create(sizeof(D3D12_RAYTRACING_AABB) * dxAABBs.size(), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, dxAABBs.data());
        mpGeometryBuffer->setName("ProceduralGeometryBuffer");
        pContext->resourceBarrier(mpGeometryBuffer.get(), Resource::State::NonPixelShader);

        uint aabbBufferOffset = 0;
        for (int i = 0; i < mTlas.size(); i++)
        {
            auto& blas = mTlas[i];
            mBlasData.emplace_back(BlasData{});
            for (const auto& geometry : blas.mGeometries)
            {
                D3D12_RAYTRACING_GEOMETRY_DESC geoDesc = {};
                geoDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
                geoDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                geoDesc.AABBs.AABBCount = geometry.mPrimitives.size();
                geoDesc.AABBs.AABBs.StartAddress = mpGeometryBuffer->getGpuAddress();
                geoDesc.AABBs.AABBs.StrideInBytes = aabbBufferOffset * sizeof(D3D12_RAYTRACING_AABB);

                mBlasData[i].geomDescs.emplace_back(geoDesc);
                aabbBufferOffset += static_cast<uint>(geometry.mPrimitives.size());
            }
        }
    }

    void ProceduralScene::buildBlas(RenderContext* pContext)
    {
        PROFILE("buildBlas");

        // On the first time, or if a full rebuild is necessary we will:
        // - Update all build inputs and prebuild info
        // - Calculate total intermediate buffer sizes
        // - Build all BLASes into an intermediate buffer
        // - Calculate total compacted buffer size
        // - Compact/clone all BLASes to their final location
        if (mRebuildBlas)
        {
            uint64_t totalMaxBlasSize = 0;
            uint64_t totalScratchSize = 0;

            for (auto& blas : mBlasData)
            {
                // Determine how BLAS build/update should be done.
                // The default choice is to compact all static BLASes and those that don't need to be rebuilt every frame. For those compaction just adds overhead.
                // TODO: Add compaction on/off switch for profiling.
                // TODO: Disable compaction for skinned meshes if update performance becomes a problem.
                blas.updateMode = mBlasUpdateMode;
                blas.useCompaction = !blas.hasSkinnedMesh || blas.updateMode != UpdateMode::Rebuild;

                // Setup build parameters.
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs = blas.buildInputs;
                inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                inputs.NumDescs = (uint32_t)blas.geomDescs.size();
                inputs.pGeometryDescs = blas.geomDescs.data();
                inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

                // Add necessary flags depending on settings.
                if (blas.useCompaction)
                {
                    inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
                }
                if (blas.hasSkinnedMesh && blas.updateMode == UpdateMode::Refit)
                {
                    inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
                }

                // Set optional performance hints.
                // TODO: Set FAST_BUILD for skinned meshes if update/rebuild performance becomes a problem.
                // TODO: Add FAST_TRACE on/off switch for profiling. It is disabled by default as it is scene-dependent.
                //if (!blas.hasSkinnedMesh)
                //{
                //    inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                //}

                // Get prebuild info.
                GET_COM_INTERFACE(gpDevice->getApiHandle(), ID3D12Device5, pDevice5);
                pDevice5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &blas.prebuildInfo);

                // Figure out the padded allocation sizes to have proper alignement.
                uint64_t paddedMaxBlasSize = align_to(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blas.prebuildInfo.ResultDataMaxSizeInBytes);
                blas.blasByteOffset = totalMaxBlasSize;
                totalMaxBlasSize += paddedMaxBlasSize;

                uint64_t scratchSize = std::max(blas.prebuildInfo.ScratchDataSizeInBytes, blas.prebuildInfo.UpdateScratchDataSizeInBytes);
                uint64_t paddedScratchSize = align_to(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, scratchSize);
                blas.scratchByteOffset = totalScratchSize;
                totalScratchSize += paddedScratchSize;
            }

            // Allocate intermediate buffers and scratch buffer.
            // The scratch buffer we'll retain because it's needed for subsequent rebuilds and updates.
            // TODO: Save memory by reducing the scratch buffer to the minimum required for the dynamic objects.
            if (mpBlasScratch == nullptr || mpBlasScratch->getSize() < totalScratchSize)
            {
                mpBlasScratch = Buffer::create(totalScratchSize, Buffer::BindFlags::UnorderedAccess, Buffer::CpuAccess::None);
                mpBlasScratch->setName("Scene::mpBlasScratch");
            }
            else
            {
                // If we didn't need to reallocate, just insert a barrier so it's safe to use.
                pContext->uavBarrier(mpBlasScratch.get());
            }

            Buffer::SharedPtr pDestBuffer = Buffer::create(totalMaxBlasSize, Buffer::BindFlags::AccelerationStructure, Buffer::CpuAccess::None);

            const size_t postBuildInfoSize = sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC);
            static_assert(postBuildInfoSize == sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE_DESC));
            Buffer::SharedPtr pPostbuildInfoBuffer = Buffer::create(mBlasData.size() * postBuildInfoSize, Buffer::BindFlags::None, Buffer::CpuAccess::Read);

            // Build the BLASes into the intermediate destination buffer.
            // We output postbuild info to a separate buffer to find out the final size requirements.
            assert(pDestBuffer && pPostbuildInfoBuffer && mpBlasScratch);
            uint64_t postBuildInfoOffset = 0;

            for (const auto& blas : mBlasData)
            {
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
                asDesc.Inputs = blas.buildInputs;
                asDesc.ScratchAccelerationStructureData = mpBlasScratch->getGpuAddress() + blas.scratchByteOffset;
                asDesc.DestAccelerationStructureData = pDestBuffer->getGpuAddress() + blas.blasByteOffset;

                // Need to find out the the postbuild compacted BLAS size to know the final allocation size.
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuildInfoDesc = {};
                postbuildInfoDesc.InfoType = blas.useCompaction ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE;
                postbuildInfoDesc.DestBuffer = pPostbuildInfoBuffer->getGpuAddress() + postBuildInfoOffset;
                postBuildInfoOffset += postBuildInfoSize;

                GET_COM_INTERFACE(pContext->getLowLevelData()->getCommandList(), ID3D12GraphicsCommandList4, pList4);
                pList4->BuildRaytracingAccelerationStructure(&asDesc, 1, &postbuildInfoDesc);
            }

            // Release scratch buffer if there is no animated content. We will not need it.
            if (!mHasSkinnedMesh) mpBlasScratch.reset();

            // Read back the calculated final size requirements for each BLAS.
            // For this purpose we have to flush and map the postbuild info buffer for readback.
            // TODO: We could copy to a staging buffer first and wait on a GPU fence for when it's ready.
            // But there is no other work to do inbetween so it probably wouldn't help. This is only done once at startup anyway.
            pContext->flush(true);
            const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC* postBuildInfo =
                (const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC*) pPostbuildInfoBuffer->map(Buffer::MapType::Read);

            uint64_t totalBlasSize = 0;
            for (size_t i = 0; i < mBlasData.size(); i++)
            {
                auto& blas = mBlasData[i];
                blas.blasByteSize = postBuildInfo[i].CompactedSizeInBytes;
                assert(blas.blasByteSize <= blas.prebuildInfo.ResultDataMaxSizeInBytes);
                uint64_t paddedBlasSize = align_to(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blas.blasByteSize);
                totalBlasSize += paddedBlasSize;
            }
            pPostbuildInfoBuffer->unmap();

            // Allocate final BLAS buffer.
            if (mpBlas == nullptr || mpBlas->getSize() < totalBlasSize)
            {
                mpBlas = Buffer::create(totalBlasSize, Buffer::BindFlags::AccelerationStructure, Buffer::CpuAccess::None);
                mpBlas->setName("Scene::mpBlas");
            }
            else
            {
                // If we didn't need to reallocate, just insert a barrier so it's safe to use.
                pContext->uavBarrier(mpBlas.get());
            }

            // Insert barriers for the intermediate buffer. This is probably not necessary since we flushed above, but it's not going to hurt.
            pContext->uavBarrier(pDestBuffer.get());

            // Compact/clone all BLASes to their final location.
            uint64_t blasOffset = 0;
            for (auto& blas : mBlasData)
            {
                GET_COM_INTERFACE(pContext->getLowLevelData()->getCommandList(), ID3D12GraphicsCommandList4, pList4);
                pList4->CopyRaytracingAccelerationStructure(
                    mpBlas->getGpuAddress() + blasOffset,
                    pDestBuffer->getGpuAddress() + blas.blasByteOffset,
                    blas.useCompaction ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);

                uint64_t paddedBlasSize = align_to(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blas.blasByteSize);
                blas.blasByteOffset = blasOffset;
                blasOffset += paddedBlasSize;
            }
            assert(blasOffset == totalBlasSize);

            // Insert barrier. The BLAS buffer is now ready for use.
            pContext->uavBarrier(mpBlas.get());

            updateRaytracingStats();
            mRebuildBlas = false;

            return;
        }

        // If we get here, all BLASes have previously been built and compacted. We will:
        // - Early out if there are no animated meshes.
        // - Update or rebuild in-place the ones that are animated.
        assert(!mRebuildBlas);
        if (mHasSkinnedMesh == false) return;

        // Insert barriers. The buffers are now ready to be written to.
        assert(mpBlas && mpBlasScratch);
        pContext->uavBarrier(mpBlas.get());
        pContext->uavBarrier(mpBlasScratch.get());

        for (const auto& blas : mBlasData)
        {
            // Skip updating BLASes not containing skinned meshes.
            if (!blas.hasSkinnedMesh) continue;

            // Build/update BLAS.
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
            asDesc.Inputs = blas.buildInputs;
            asDesc.ScratchAccelerationStructureData = mpBlasScratch->getGpuAddress() + blas.scratchByteOffset;
            asDesc.DestAccelerationStructureData = mpBlas->getGpuAddress() + blas.blasByteOffset;

            if (blas.updateMode == UpdateMode::Refit)
            {
                // Set source address to destination address to update in place.
                asDesc.SourceAccelerationStructureData = asDesc.DestAccelerationStructureData;
                asDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            }
            else
            {
                // We'll rebuild in place. The BLAS should not be compacted, check that size matches prebuild info.
                assert(blas.blasByteSize == blas.prebuildInfo.ResultDataMaxSizeInBytes);
            }

            GET_COM_INTERFACE(pContext->getLowLevelData()->getCommandList(), ID3D12GraphicsCommandList4, pList4);
            pList4->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);
        }

        // Insert barrier. The BLAS buffer is now ready for use.
        pContext->uavBarrier(mpBlas.get());
    }

    void ProceduralScene::buildTlas(RenderContext* pContext, uint32_t rayCount)
    {
        PROFILE("buildTlas");

        TlasData tlas;
        auto it = mTlasCache.find(rayCount);
        if (it != mTlasCache.end()) tlas = it->second;

        fillInstanceDesc(mInstanceDescs, rayCount);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = (uint32_t)mInstanceDescs.size();
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

        tlas.updateMode = mTlasUpdateMode;

        // On first build for the scene, create scratch buffer and cache prebuild info. As long as INSTANCE_DESC count doesn't change, we can reuse these
        if (mpTlasScratch == nullptr)
        {
            // Prebuild
            GET_COM_INTERFACE(gpDevice->getApiHandle(), ID3D12Device5, pDevice5);
            pDevice5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &mTlasPrebuildInfo);
            mpTlasScratch = Buffer::create(mTlasPrebuildInfo.ScratchDataSizeInBytes, Buffer::BindFlags::UnorderedAccess, Buffer::CpuAccess::None);
            mpTlasScratch->setName("Scene::mpTlasScratch");

            // #SCENE This isn't guaranteed according to the spec, and the scratch buffer being stored should be sized differently depending on update mode
            assert(mTlasPrebuildInfo.UpdateScratchDataSizeInBytes <= mTlasPrebuildInfo.ScratchDataSizeInBytes);
        }

        // Setup GPU buffers
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
        asDesc.Inputs = inputs;

        // If first time building this TLAS
        if (tlas.pTlas == nullptr)
        {
            assert(tlas.pInstanceDescs == nullptr); // Instance desc should also be null if no TLAS
            tlas.pTlas = Buffer::create(mTlasPrebuildInfo.ResultDataMaxSizeInBytes, Buffer::BindFlags::AccelerationStructure, Buffer::CpuAccess::None);
            tlas.pInstanceDescs = Buffer::create((uint32_t)mInstanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), Buffer::BindFlags::None, Buffer::CpuAccess::Write, mInstanceDescs.data());
        }
        // Else update instance descs and barrier TLAS buffers
        else
        {
            pContext->uavBarrier(tlas.pTlas.get());
            pContext->uavBarrier(mpTlasScratch.get());
            tlas.pInstanceDescs->setBlob(mInstanceDescs.data(), 0, inputs.NumDescs * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
            asDesc.SourceAccelerationStructureData = tlas.pTlas->getGpuAddress(); // Perform the update in-place
        }

        assert((inputs.NumDescs != 0) && tlas.pInstanceDescs->getApiHandle() && tlas.pTlas->getApiHandle() && mpTlasScratch->getApiHandle());

        asDesc.Inputs.InstanceDescs = tlas.pInstanceDescs->getGpuAddress();
        asDesc.ScratchAccelerationStructureData = mpTlasScratch->getGpuAddress();
        asDesc.DestAccelerationStructureData = tlas.pTlas->getGpuAddress();

        // Set the source buffer to update in place if this is an update
        if ((inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) > 0) asDesc.SourceAccelerationStructureData = asDesc.DestAccelerationStructureData;

        // Create TLAS
        GET_COM_INTERFACE(pContext->getLowLevelData()->getCommandList(), ID3D12GraphicsCommandList4, pList4);
        pContext->resourceBarrier(tlas.pInstanceDescs.get(), Resource::State::NonPixelShader);
        pList4->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);
        pContext->uavBarrier(tlas.pTlas.get());

        // Create TLAS SRV
        if (tlas.pSrv == nullptr)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.RaytracingAccelerationStructure.Location = tlas.pTlas->getGpuAddress();

            DescriptorSet::Layout layout;
            layout.addRange(DescriptorSet::Type::TextureSrv, 0, 1);
            DescriptorSet::SharedPtr pSet = DescriptorSet::create(gpDevice->getCpuDescriptorPool(), layout);
            gpDevice->getApiHandle()->CreateShaderResourceView(nullptr, &srvDesc, pSet->getCpuHandle(0));

            ResourceWeakPtr pWeak = tlas.pTlas;
            tlas.pSrv = std::make_shared<ShaderResourceView>(pWeak, pSet, 0, 1, 0, 1);
        }

        mTlasCache[rayCount] = tlas;
    }

    void ProceduralScene::fillInstanceDesc(std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instanceDescs, uint32_t rayCount) const
    {
        assert(mpBlas);
        instanceDescs.clear();

        uint InstanceContributionToHitGroupIndex = 0;
        for (size_t i = 0; i < mBlasData.size(); i++)
        {
            D3D12_RAYTRACING_INSTANCE_DESC desc = {};
            desc.AccelerationStructure = mpBlas->getGpuAddress() + mBlasData[i].blasByteOffset;
            desc.InstanceContributionToHitGroupIndex = InstanceContributionToHitGroupIndex;

            for (const auto& instance: mTlas[i].mInstances)
            {
                desc.Flags = instance.mFlags;
                desc.InstanceMask = instance.mMask;
                desc.InstanceID = instance.mID;

                glm::mat4 Transform = glm::transpose(instance.mTransformMtx);
                memcpy(&desc.Transform, &Transform, sizeof(desc.Transform));

                instanceDescs.emplace_back(desc);
            }

            InstanceContributionToHitGroupIndex += rayCount * static_cast<uint>(mTlas[i].mGeometries.size());
        }
    }

    void ProceduralScene::updateRaytracingStats()
    {
        auto& s = mRTStats;

        s.blasCount = mBlasData.size();
        s.blasCompactedCount = 0;
        s.blasMemoryInBytes = 0;

        for (const auto& blas : mBlasData)
        {
            if (blas.useCompaction) s.blasCompactedCount++;
            s.blasMemoryInBytes += blas.blasByteSize;
        }
    }
}
