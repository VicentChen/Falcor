#pragma once
#include "Utils/Math/AABB.h"
#include "Core/API/Buffer.h"

namespace Falcor
{
    
    /** DXR Procedural Scene Layout:
        - BLAS Construction
            - BLAS is composed of one or more geometries.
            - Geometry is composed of one or more primitives. Indices of geometries generated by DXR can be accessed by GeometryIndex() in shaders.
            - In procedural scene, a primitive is an AABB. Indices of primitives generated by DXR can be accessed by PrimitiveIndex() in shaders.

        - TLAS Construction
            - TLAS is composed of one or more BLASs.
            - A BLAS in TLAS can have multiple instances. User-defined indices of instances can be accessed by InstanceID() in shaders.

        Index of hit group:
            InstanceContributionToHitGroupIndex + MultiplierForGeometryContributionToHitGroupIndex * GeometryContributionToHitGroupIndex + RayContributionToHitGroupIndex
             (Offset to index current instance)                                        (Offset of geometry)                                     (Offset of ray)

        Note:
         - DXR 1.0 is not supported because of lacking GeometryIndex()

        TODO:
         - preview(): show all AABBs in procedural scene
         - add support for animation
         - rebuild BLAS & TLAS after scene changed
         
    */
    class dlldecl ProceduralScene : public std::enable_shared_from_this<ProceduralScene>
    {
    public:
        using SharedPtr = std::shared_ptr<ProceduralScene>;

        using GeometryID = uint;
        using PrimitiveID = uint;
        using InstanceID = uint;
        
        using Primitive = BoundingBox;
        
        struct Geometry
        {
            std::string mName;                     ///< Name
            std::vector<Primitive> mPrimitives;    ///< Primitives in geometries
        };

        struct Instance
        {
            uint mID = 0;   ///< can be accessed by InstanceID() in shaders
            uint mMask = 0xFF;
            D3D12_RAYTRACING_INSTANCE_FLAGS mFlags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            float4x4 mTransformMtx;
        };
        
        struct Blas
        {
            std::vector<Geometry> mGeometries;     ///< Geometries in BLAS
            std::vector<Instance> mInstances;   ///< Instance-to-world transformation matrix
        };

        using Tlas = std::vector<Blas>;

        static SharedPtr create() { return SharedPtr(new ProceduralScene()); }
        
        void addBlas(const Blas& blas) { mTlas.emplace_back(blas); }
        
        void setScene(const Tlas& tlas);
        
        // #SCENE: we should get rid of this. We can't right now because we can't create a structured-buffer of materials (MaterialData contains textures)
        Shader::DefineList getSceneDefines() const;

        /** Preview the scene using the rasterizer
        */
        void preview(RenderContext* pContext, GraphicsState* pState, GraphicsVars* pVars);

        /** Render the scene using raytracing
        */
        void raytrace(RenderContext* pContext, RtProgram* pProgram, const std::shared_ptr<RtProgramVars>& pVars, uint3 dispatchDims);

        /** Set the scene ray tracing resources into a shader var.
            The acceleration structure is created lazily, which requires the render context.
            \param[in] pContext Render context.
            \param[in] var Shader variable to set data into, usually the root var.
            \param[in] rayTypeCount Number of ray types in raygen program. Not needed for DXR 1.1.
        */
        void setRaytracingShaderData(RenderContext* pContext, const ShaderVar& var, uint32_t rayTypeCount = 1);

        uint getMeshCount() const { return mMeshCount; }
        uint getInstanceCount() const { return mInstanceCount; }
        
    private:
        ProceduralScene() = default;
        
        /** Initialize geometry descs for each BLAS
        */
        void initGeomDesc(RenderContext* pContext);

        /** Generate bottom level acceleration structures for all meshes
        */
        void buildBlas(RenderContext* pContext);

        /** Generate top level acceleration structure for the scene. Automatically determines whether to build or refit.
            \param[in] rayCount Number of ray types in the shader. Required to setup how instances index into the Shader Table
        */
        void buildTlas(RenderContext* pContext, uint32_t rayCount);

        /** Generate data for creating a TLAS.
        */
        void fillInstanceDesc(std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instanceDescs, uint32_t rayCount) const;

        void updateRaytracingStats();

        // Procedural Scene Geometry
        Tlas mTlas;
        uint mMeshCount = 0;
        uint mInstanceCount = 0;
        
        // Raytracing Data
        enum class UpdateMode
        {
            Rebuild,    ///< Recreate acceleration structure when updates are needed
            Refit       ///< Update acceleration structure when updates are needed
        };

        struct RayTracingStats
        {
            // Raytracing stats
            size_t blasCount = 0;             ///< Number of BLASes.
            size_t blasCompactedCount = 0;    ///< Number of compacted BLASes.
            size_t blasMemoryInBytes = 0;     ///< Total memory in bytes used by the BLASes.
        };
        
        RayTracingStats mRTStats;    ///< Ray tracing statistics.
        
        UpdateMode mTlasUpdateMode = UpdateMode::Rebuild;    ///< How the TLAS should be updated when there are changes in the scene
        UpdateMode mBlasUpdateMode = UpdateMode::Refit;      ///< How the BLAS should be updated when there are changes to meshes

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> mInstanceDescs; ///< Shared between TLAS builds to avoid reallocating CPU memory

        struct TlasData
        {
            Buffer::SharedPtr pTlas;
            ShaderResourceView::SharedPtr pSrv;             ///< Shader Resource View for binding the TLAS
            Buffer::SharedPtr pInstanceDescs;               ///< Buffer holding instance descs for the TLAS
            UpdateMode updateMode = UpdateMode::Rebuild;    ///< Update mode this TLAS was created with.
        };

        std::unordered_map<uint32_t, TlasData> mTlasCache;  ///< Top Level Acceleration Structure for scene data cached per shader ray count
                                                            ///< Number of ray types in program affects Shader Table indexing
        Buffer::SharedPtr mpTlasScratch;                    ///< Scratch buffer used for TLAS builds. Can be shared as long as instance desc count is the same, which for now it is.
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO mTlasPrebuildInfo; ///< This can be reused as long as the number of instance descs doesn't change.

        struct BlasData
        {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs;
            std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geomDescs;

            uint64_t blasByteSize = 0;                      ///< Size of the final BLAS.
            uint64_t blasByteOffset = 0;                    ///< Offset into the BLAS buffer to where it is stored.
            uint64_t scratchByteOffset = 0;                 ///< Offset into the scratch buffer to use for updates/rebuilds.

            bool hasSkinnedMesh = false;                    ///< Whether the BLAS contains a skinned mesh, which means the BLAS may need to be updated.
            bool useCompaction = false;                     ///< Whether the BLAS should be compacted after build.
            UpdateMode updateMode = UpdateMode::Refit;      ///< Update mode this BLAS was created with.
        };

        std::vector<BlasData> mBlasData;    ///< All data related to the scene's BLASes.
        Buffer::SharedPtr mpBlas;           ///< Buffer containing all BLASes.
        Buffer::SharedPtr mpBlasScratch;    ///< Scratch buffer used for BLAS builds.
        bool mRebuildBlas = true;           ///< Flag to indicate BLASes need to be rebuilt.
        bool mHasSkinnedMesh = false;       ///< Whether the scene has a skinned mesh at all.

        Buffer::SharedPtr mpGeometryBuffer;
    };
}
