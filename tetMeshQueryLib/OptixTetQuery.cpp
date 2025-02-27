// ======================================================================== //
// Copyright 2019-2020 Ingo Wald (NVIDIA) and Bin Wang (LSU)                //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "OptixTetQuery.h"
#include "internalTypes.h"
#include "cuda_runtime_api.h"

extern "C" char optixTetQueries_ptxCode[];

namespace owl {
  namespace tetQueries {

    /*! contains temporary data for shared-face strcuture
      computation. after the respective geom and accel have been
      built, these can be released, and are thus in a separate
      class */
    struct SharedFacesBuilder {

      SharedFacesBuilder(const vec3d *vertex, int numVertices,
                         const vec4i *index, int numIndices);
    
      std::vector<SharedFacesGeom::FaceInfo> faceInfos;
      std::vector<vec3i> faceIndices;
      std::vector<vec3f> faceVertices;
      std::map<uint64_t, int> knownFaces;
      float maxEdgeLength = 0.f;
    
      void add(int tetID, vec3i face);
    };

    SharedFacesBuilder::SharedFacesBuilder(const vec3d *vertices, int numVertices,
                                           const vec4i *indices,  int numIndices)
    {
      std::cout << "#adv: creating shared faces" << std::endl;
      for (int i=0;i<numVertices;i++)
        faceVertices.push_back(vec3f(vertices[i].x,vertices[i].y,vertices[i].z));
    
      for (int tetID = 0; tetID < numIndices; tetID++) {
        vec4i index = indices[tetID];
        if (index.x == index.y) continue;
        if (index.x == index.z) continue;
        if (index.x == index.w) continue;
        if (index.y == index.z) continue;
        if (index.y == index.w) continue;
        if (index.z == index.w) continue;

        const vec3f A = faceVertices[index.x];
        const vec3f B = faceVertices[index.y];
        const vec3f C = faceVertices[index.z];
        const vec3f D = faceVertices[index.w];

        maxEdgeLength = std::max(maxEdgeLength, length(B - A));
        maxEdgeLength = std::max(maxEdgeLength, length(C - A));
        maxEdgeLength = std::max(maxEdgeLength, length(D - A));
        maxEdgeLength = std::max(maxEdgeLength, length(C - B));
        maxEdgeLength = std::max(maxEdgeLength, length(D - B));
        maxEdgeLength = std::max(maxEdgeLength, length(D - C));

        const float volume = dot(D - A, cross(B - A, C - A));
        if (volume == 0.f) {
          // ideally, remove this tet from the input; for now, just
          // don't create any faces for it, and instead write in dummies
          // (to not mess up indices)
          continue;
        }
        else if (volume < 0.f) {
          std::swap(index.x, index.y);
        }
        //{x,0},{y,1},{z,2},{w,3}
        add(tetID, vec3i(index.x, index.y, index.z)); // 0,1,2
        add(tetID, vec3i(index.y, index.w, index.z)); // 1,3,2
        add(tetID, vec3i(index.x, index.w, index.y)); // 0,3,1
        add(tetID, vec3i(index.z, index.w, index.x)); // 2,3,0
      }

      std::cout << "#adv: maximum edge length " << maxEdgeLength << std::endl;
    }
    

    void SharedFacesBuilder::add(int tetID, vec3i face)
    {
      int front = true;
      if (face.x > face.z) { std::swap(face.x, face.z); front = !front; }
      if (face.y > face.z) { std::swap(face.y, face.z); front = !front; }
      if (face.x > face.y) { std::swap(face.x, face.y); front = !front; }
      assert(face.x < face.y && face.x < face.z);

      int faceID = -1;
      uint64_t key = ((((uint64_t)face.z << 20) | face.y) << 20) | face.x;
      auto it = knownFaces.find(key);
      if (it == knownFaces.end()) {
        faceID = faceIndices.size();
        faceIndices.push_back(face);
        faceInfos.push_back({ -1,-1 });
        knownFaces[key] = faceID;
      }
      else {
        faceID = it->second;
      }

      if (front)
        faceInfos[faceID].front = tetID;
      else
        faceInfos[faceID].back = tetID;
    }
  
    OptixTetQuery::OptixTetQuery(const vec3d *vertex, int numVertices,
                                 const vec4i *index,  int numIndices)
    {
      std::cout << "#adv: initializing owl" << std::endl;
      owl = owlContextCreate(nullptr,1);
      owlSetMaxInstancingDepth(owl,0);

      SharedFacesBuilder sharedFaces(vertex,numVertices,index,numIndices);
    
      module
        = owlModuleCreate(owl,optixTetQueries_ptxCode);

      std::cout << "#adv: creating tet mesh 'shared faces' geom type" << std::endl;
      OWLVarDecl sharedFacesGeomVars[]
        = {
           { "tetForFace", OWL_BUFPTR, OWL_OFFSETOF(SharedFacesGeom,tetForFace) },
           { /* end of list sentinel: */nullptr },
      };
      OWLGeomType facesGeomType
        = owlGeomTypeCreate(owl,OWL_GEOM_TRIANGLES,sizeof(SharedFacesGeom),
                            sharedFacesGeomVars,-1);
      owlGeomTypeSetClosestHit(facesGeomType,0,
                               module,"sharedFacesCH");

      // ------------------------------------------------------------------
      // create the triangles geom part
      // ------------------------------------------------------------------
      std::cout << "#adv: creating geom" << std::endl;
      OWLGeom facesGeom
        = owlGeomCreate(owl,facesGeomType);
      OWLBuffer faceVertexBuffer
        = owlDeviceBufferCreate(owl,OWL_FLOAT3,
                                sharedFaces.faceVertices.size(),
                                sharedFaces.faceVertices.data());
      OWLBuffer faceIndexBuffer
        = owlDeviceBufferCreate(owl,OWL_INT3,
                                sharedFaces.faceIndices.size(),
                                sharedFaces.faceIndices.data());
      OWLBuffer faceInfoBuffer
        = owlDeviceBufferCreate(owl,OWL_INT2,
                                sharedFaces.faceInfos.size(),
                                sharedFaces.faceInfos.data());
    
      owlTrianglesSetVertices(facesGeom,faceVertexBuffer,
                              sharedFaces.faceVertices.size(),
                              sizeof(sharedFaces.faceVertices[0]),0);
      owlTrianglesSetIndices(facesGeom,faceIndexBuffer,
                             sharedFaces.faceIndices.size(),
                             sizeof(sharedFaces.faceIndices[0]),0);
    
      // ------------------------------------------------------------------
      // create the group, to force accel build
      // ------------------------------------------------------------------
      // iw: todo - set disable-anyhit flag on group (needs addition to owl)d
      std::cout << "#adv: building BVH" << std::endl;
      OWLGroup faces
        = owlTrianglesGeomGroupCreate(owl,1,&facesGeom);
      owlGroupBuildAccel(faces);
      this->faceBVH = faces;

      owlBufferDestroy(faceIndexBuffer);
      owlBufferDestroy(faceVertexBuffer);

      // ------------------------------------------------------------------
      // upload/set the 'shading' data
      // ------------------------------------------------------------------
      owlGeomSetBuffer(facesGeom,"tetForFace",faceInfoBuffer);
      std::cout << "#adv: done setting up optix tet-mesh" << std::endl;

      // ------------------------------------------------------------------
      // create a raygen that we can launch for the query kernel
      // ------------------------------------------------------------------
      OWLVarDecl rayGenVars[]
        = {
           { "faces",        OWL_GROUP, OWL_OFFSETOF(RayGen,faces) },
           { "maxEdgeLength",OWL_FLOAT, OWL_OFFSETOF(RayGen,maxEdgeLength) },
           { /* sentinel */ nullptr },
      };
    
      this->rayGen = owlRayGenCreate(owl,
                                     module,
                                     "queryKernel",
                                     sizeof(RayGen),
                                     rayGenVars,-1);
      owlRayGenSetGroup(rayGen,"faces",faceBVH);
      owlRayGenSet1f(rayGen,"maxEdgeLength",sharedFaces.maxEdgeLength);

      // ------------------------------------------------------------------
      // create a dummy miss program, to make optix happy
      // ------------------------------------------------------------------
      OWLMissProg miss = owlMissProgCreate(owl,
                                           module,
                                           "miss",
                                           0,nullptr,0);
    
      // ------------------------------------------------------------------
      // have all programs, geometries, groups, etc - build the SBT
      // ------------------------------------------------------------------
      owlBuildPrograms(owl);
      owlBuildPipeline(owl);
      owlBuildSBT(owl);


      // ------------------------------------------------------------------
      // FINALLY: create a launch params that we can use to pass array
      // of queries
      // ------------------------------------------------------------------
      OWLVarDecl lpVars[]
        = {
           { "particles",    OWL_ULONG,  OWL_OFFSETOF(LaunchParams,particlesFloat) },
           { "numParticles", OWL_INT,    OWL_OFFSETOF(LaunchParams,numParticles) },
           { "isFloat",      OWL_INT,    OWL_OFFSETOF(LaunchParams,isFloat) },
           { "out_tetIDs",   OWL_ULONG, OWL_OFFSETOF(LaunchParams,out_tetIDs) },
           { /* sentinel */ nullptr },
      };
    
      launchParams
          = owlParamsCreate(owl, sizeof(LaunchParams),
              lpVars, -1);
    }



    /*! perform a _synchronous_ query with given device-side array of
      particle */
    void OptixTetQuery::query_sync(FloatParticle *d_particles, int* out_tetIDs, int numParticles)
    {
      int launchWidth  = 64*1024;
      int launchHeight = divRoundUp(numParticles,launchWidth);

      owlParamsSet1ul(launchParams,"particles",(uint64_t)d_particles);
      owlParamsSet1i(launchParams,"numParticles",numParticles);
      owlParamsSet1i(launchParams,"isFloat",1);
      owlParamsSet1l (launchParams, "out_tetIDs", (uint64_t)out_tetIDs);
      owlLaunch2D(rayGen,launchWidth,launchHeight,launchParams);
      cudaDeviceSynchronize();
    }

    /*! perform a _synchronous_ query with given device-side array of
      particle */
    void OptixTetQuery::query_sync(DoubleParticle *d_particles, int* out_tetIDs, int numParticles)
    {
      int launchWidth  = 64*1024;
      int launchHeight = divRoundUp(numParticles,launchWidth);

      owlParamsSet1ul(launchParams,"particles",(uint64_t)d_particles);
      owlParamsSet1i(launchParams,"numParticles",numParticles);
      owlParamsSet1i(launchParams,"isFloat",0);
      owlParamsSet1ul(launchParams, "out_tetIDs", (uint64_t)out_tetIDs);
      owlLaunch2D(rayGen,launchWidth,launchHeight,launchParams);
      cudaDeviceSynchronize();
    }
  
  } // ::owl::tetQueries
} // ::owl
