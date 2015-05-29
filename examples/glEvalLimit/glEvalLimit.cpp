//
//   Copyright 2013 Pixar
//
//   Licensed under the Apache License, Version 2.0 (the "Apache License")
//   with the following modification; you may not use this file except in
//   compliance with the Apache License and the following modification to it:
//   Section 6. Trademarks. is deleted and replaced with:
//
//   6. Trademarks. This License does not grant permission to use the trade
//      names, trademarks, service marks, or product names of the Licensor
//      and its affiliates, except as required to comply with Section 4(c) of
//      the License and to reproduce the content of the NOTICE file.
//
//   You may obtain a copy of the Apache License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the Apache License with the above modification is
//   distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
//   KIND, either express or implied. See the Apache License for the specific
//   language governing permissions and limitations under the Apache License.
//

#if defined(__APPLE__)
    #if defined(OSD_USES_GLEW)
        #include <GL/glew.h>
    #else
        #include <OpenGL/gl3.h>
    #endif
    #define GLFW_INCLUDE_GL3
    #define GLFW_NO_GLU
#else
    #include <stdlib.h>
    #include <GL/glew.h>
    #if defined(WIN32)
        #include <GL/wglew.h>
    #endif
#endif

#include <GLFW/glfw3.h>
GLFWwindow* g_window=0;
GLFWmonitor* g_primary=0;

#include <osd/cpuEvaluator.h>
#include <osd/cpuVertexBuffer.h>
#include <osd/cpuPatchTable.h>
#include <osd/cpuGLVertexBuffer.h>
#include <osd/mesh.h>

#ifdef OPENSUBDIV_HAS_TBB
    #include <osd/tbbEvaluator.h>
#endif

#ifdef OPENSUBDIV_HAS_OPENMP
    #include <osd/ompEvaluator.h>
#endif

#ifdef OPENSUBDIV_HAS_CUDA
    #include <osd/cudaEvaluator.h>
    #include <osd/cudaVertexBuffer.h>
    #include <osd/cudaGLVertexBuffer.h>
    #include <osd/cudaPatchTable.h>
    #include "../common/cudaDeviceContext.h"

    CudaDeviceContext g_cudaDeviceContext;
#endif

#ifdef OPENSUBDIV_HAS_OPENCL
    #include <osd/clVertexBuffer.h>
    #include <osd/clGLVertexBuffer.h>
    #include <osd/clEvaluator.h>
    #include <osd/clPatchTable.h>
    #include "../common/clDeviceContext.h"
    CLDeviceContext g_clDeviceContext;
#endif

#ifdef OPENSUBDIV_HAS_GLSL_TRANSFORM_FEEDBACK
    #include <osd/glXFBEvaluator.h>
    #include <osd/glVertexBuffer.h>
    #include <osd/glPatchTable.h>
#endif

#ifdef OPENSUBDIV_HAS_GLSL_COMPUTE
    #include <osd/glComputeEvaluator.h>
    #include <osd/glVertexBuffer.h>
    #include <osd/glPatchTable.h>
#endif

#include <far/gregoryBasis.h>
#include <far/endCapGregoryBasisPatchFactory.h>
#include <far/topologyRefiner.h>
#include <far/stencilTableFactory.h>
#include <far/patchTableFactory.h>

#include <far/error.h>

#include "../../regression/common/vtr_utils.h"
#include "../common/stopwatch.h"
#include "../common/simple_math.h"
#include "../common/glHud.h"
#include "../common/glUtils.h"

#include "init_shapes.h"
#include "particles.h"

#include <cfloat>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdlib.h>

using namespace OpenSubdiv;

//------------------------------------------------------------------------------
enum KernelType { kCPU = 0,
                  kOPENMP = 1,
                  kTBB = 2,
                  kCUDA = 3,
                  kCL = 4,
                  kGLXFB = 5,
                  kGLCompute = 6 };

enum EndCap      { kEndCapBSplineBasis,
                   kEndCapGregoryBasis };

enum DrawMode { kUV,
                kVARYING,
                kNORMAL,
                kSHADE,
                kFACEVARYING };

std::vector<float> g_orgPositions,
                   g_positions,
                   g_varyingColors;

int g_currentShape = 0,
    g_level = 3,
    g_kernel = kCPU,
    g_endCap = kEndCapBSplineBasis,
    g_numElements = 3;

std::vector<int>   g_coarseEdges;
std::vector<float> g_coarseEdgeSharpness;
std::vector<float> g_coarseVertexSharpness;

int   g_running = 1,
      g_width = 1024,
      g_height = 1024,
      g_fullscreen = 0,
      g_drawCageEdges = 1,
      g_drawCageVertices = 1,
      g_drawMode = kUV,
      g_prev_x = 0,
      g_prev_y = 0,
      g_mbutton[3] = {0, 0, 0},
      g_frame=0,
      g_freeze=0,
      g_repeatCount;

float g_rotate[2] = {0, 0},
      g_dolly = 5,
      g_pan[2] = {0, 0},
      g_center[3] = {0, 0, 0},
      g_size = 0,
      g_moveScale = 0.0f;

GLuint g_transformUB = 0,
       g_transformBinding = 0;

struct Transform {
    float ModelViewMatrix[16];
    float ProjectionMatrix[16];
    float ModelViewProjectionMatrix[16];
} g_transformData;


// performance
float g_evalTime = 0;
float g_computeTime = 0;
Stopwatch g_fpsTimer;

//------------------------------------------------------------------------------
int g_nParticles = 65536;

bool g_randomStart = true;//false;

GLuint g_cageEdgeVAO = 0,
       g_cageEdgeVBO = 0,
       g_cageVertexVAO = 0,
       g_cageVertexVBO = 0,
       g_samplesVAO=0;

GLhud g_hud;

//------------------------------------------------------------------------------
struct Program {
    GLuint program;
    GLuint uniformModelViewMatrix;
    GLuint uniformProjectionMatrix;
    GLuint uniformDrawMode;
    GLuint attrPosition;
    GLuint attrColor;
    GLuint attrTangentU;
    GLuint attrTangentV;
    GLuint attrPatchCoord;
} g_defaultProgram;

//------------------------------------------------------------------------------
static void
createRandomColors(int nverts, int stride, float * colors) {

    // large Pell prime number
    srand( static_cast<int>(2147483647) );

    for (int i=0; i<nverts; ++i) {
        colors[i*stride+0] = (float)rand()/(float)RAND_MAX;
        colors[i*stride+1] = (float)rand()/(float)RAND_MAX;
        colors[i*stride+2] = (float)rand()/(float)RAND_MAX;
    }
}

//------------------------------------------------------------------------------
static void
createCoarseMesh(OpenSubdiv::Far::TopologyRefiner const & refiner) {

    typedef OpenSubdiv::Far::ConstIndexArray IndexArray;

    // save coarse topology (used for coarse mesh drawing)
    OpenSubdiv::Far::TopologyLevel const & refBaseLevel = refiner.GetLevel(0);

    int nedges = refBaseLevel.GetNumEdges(),
        nverts = refBaseLevel.GetNumVertices();

    g_coarseEdges.resize(nedges*2);
    g_coarseEdgeSharpness.resize(nedges);
    g_coarseVertexSharpness.resize(nverts);

    for(int i=0; i<nedges; ++i) {
        IndexArray verts = refBaseLevel.GetEdgeVertices(i);
        g_coarseEdges[i*2  ]=verts[0];
        g_coarseEdges[i*2+1]=verts[1];
        g_coarseEdgeSharpness[i]=refBaseLevel.GetEdgeSharpness(i);
    }

    for(int i=0; i<nverts; ++i) {
        g_coarseVertexSharpness[i]=refBaseLevel.GetVertexSharpness(i);
    }

    // assign a randomly generated color for each vertex ofthe mesh
    g_varyingColors.resize(nverts*3);
    createRandomColors(nverts, 3, &g_varyingColors[0]);
}

//------------------------------------------------------------------------------
Far::PatchTable const * g_patchTable = NULL;

// input and output vertex data
class EvalOutputBase {
public:
    virtual ~EvalOutputBase() {}
    virtual GLuint BindVertexData() const = 0;
    virtual GLuint BindDerivatives() const = 0;
    virtual GLuint BindPatchCoords() const = 0;
    virtual void UpdateData(const float *src, int startVertex, int numVertices) = 0;
    virtual void UpdateVaryingData(const float *src, int startVertex, int numVertices) = 0;
    virtual void Refine() = 0;
    virtual void EvalPatches() = 0;
    virtual void EvalPatchesWithDerivatives() = 0;
    virtual void EvalPatchesVarying() = 0;
    virtual void UpdatePatchCoords(
        std::vector<Osd::PatchCoord> const &patchCoords) = 0;
};

// note: Since we don't have a class for device-patchcoord container in osd,
// we cheat to use vertexbuffer as a patch-coord (5int) container.
//
// Please don't follow the pattern in your actual application.
//
template<typename SRC_VERTEX_BUFFER, typename EVAL_VERTEX_BUFFER,
         typename STENCIL_TABLE, typename PATCH_TABLE, typename EVALUATOR,
         typename DEVICE_CONTEXT = void>
class EvalOutput : public EvalOutputBase {
public:
    typedef OpenSubdiv::Osd::EvaluatorCacheT<EVALUATOR> EvaluatorCache;

    EvalOutput(Far::StencilTable const *vertexStencils,
               Far::StencilTable const *varyingStencils,
               int numCoarseVerts, int numTotalVerts, int numParticles,
               Far::PatchTable const *patchTable,
               EvaluatorCache *evaluatorCache = NULL,
               DEVICE_CONTEXT *deviceContext = NULL)
        : _srcDesc(       /*offset*/ 0, /*length*/ 3, /*stride*/ 3),
          _srcVaryingDesc(/*offset*/ 0, /*length*/ 3, /*stride*/ 3),
          _vertexDesc(    /*offset*/ 0, /*legnth*/ 3, /*stride*/ 6),
          _varyingDesc(   /*offset*/ 3, /*legnth*/ 3, /*stride*/ 6),
          _duDesc(        /*offset*/ 0, /*legnth*/ 3, /*stride*/ 6),
          _dvDesc(        /*offset*/ 3, /*legnth*/ 3, /*stride*/ 6),
          _deviceContext(deviceContext) {
        _srcData = SRC_VERTEX_BUFFER::Create(3, numTotalVerts, _deviceContext);
        _srcVaryingData = SRC_VERTEX_BUFFER::Create(3, numTotalVerts, _deviceContext);
        _vertexData = EVAL_VERTEX_BUFFER::Create(6, numParticles, _deviceContext);
        _derivatives = EVAL_VERTEX_BUFFER::Create(6, numParticles, _deviceContext);
        _patchTable = PATCH_TABLE::Create(patchTable, _deviceContext);
        _patchCoords = NULL;
        _numCoarseVerts = numCoarseVerts;
        _vertexStencils =
            Osd::convertToCompatibleStencilTable<STENCIL_TABLE>(vertexStencils, _deviceContext);
        _varyingStencils =
            Osd::convertToCompatibleStencilTable<STENCIL_TABLE>(varyingStencils, _deviceContext);
        _evaluatorCache = evaluatorCache;
    }
    ~EvalOutput() {
        delete _srcData;
        delete _srcVaryingData;
        delete _vertexData;
        delete _derivatives;
        delete _patchTable;
        delete _patchCoords;
        delete _vertexStencils;
        delete _varyingStencils;
    }
    virtual GLuint BindVertexData() const {
        return _vertexData->BindVBO();
    }
    virtual GLuint BindDerivatives() const {
        return _derivatives->BindVBO();
    }
    virtual GLuint BindPatchCoords() const {
        return _patchCoords->BindVBO();
    }
    virtual void UpdateData(const float *src, int startVertex, int numVertices) {
        _srcData->UpdateData(src, startVertex, numVertices, _deviceContext);
    }
    virtual void UpdateVaryingData(const float *src, int startVertex, int numVertices) {
        _srcVaryingData->UpdateData(src, startVertex, numVertices, _deviceContext);
    }
    virtual void Refine() {
        Osd::BufferDescriptor dstDesc = _srcDesc;
        dstDesc.offset += _numCoarseVerts * _srcDesc.stride;

        EVALUATOR const *evalInstance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
            _evaluatorCache, _srcDesc, dstDesc, _deviceContext);

        EVALUATOR::EvalStencils(_srcData, _srcDesc,
                                _srcData, dstDesc,
                                _vertexStencils,
                                evalInstance,
                                _deviceContext);

        dstDesc = _srcVaryingDesc;
        dstDesc.offset += _numCoarseVerts * _srcVaryingDesc.stride;
        evalInstance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
            _evaluatorCache, _srcVaryingDesc, dstDesc, _deviceContext);

        EVALUATOR::EvalStencils(_srcVaryingData, _srcVaryingDesc,
                                _srcVaryingData, dstDesc,
                                _varyingStencils,
                                evalInstance,
                                _deviceContext);
    }
    virtual void EvalPatches() {
        EVALUATOR const *evalInstance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
            _evaluatorCache, _srcDesc, _vertexDesc, _deviceContext);

        EVALUATOR::EvalPatches(
            _srcData, _srcDesc,
            _vertexData, _vertexDesc,
            _patchCoords->GetNumVertices(),
            _patchCoords,
            _patchTable, evalInstance, _deviceContext);
    }
    virtual void EvalPatchesWithDerivatives() {
        EVALUATOR const *evalInstance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
            _evaluatorCache, _srcDesc, _vertexDesc, _duDesc, _dvDesc, _deviceContext);
        EVALUATOR::EvalPatches(
            _srcData, _srcDesc,
            _vertexData, _vertexDesc,
            _derivatives, _duDesc,
            _derivatives, _dvDesc,
            _patchCoords->GetNumVertices(),
            _patchCoords,
            _patchTable, evalInstance, _deviceContext);
    }
    virtual void EvalPatchesVarying() {
        EVALUATOR const *evalInstance = OpenSubdiv::Osd::GetEvaluator<EVALUATOR>(
            _evaluatorCache, _srcVaryingDesc, _varyingDesc, _deviceContext);

        EVALUATOR::EvalPatches(
            _srcVaryingData, _srcVaryingDesc,
            // varyingdata is interleved in vertexData.
            _vertexData, _varyingDesc,
            _patchCoords->GetNumVertices(),
            _patchCoords,
            _patchTable, evalInstance, _deviceContext);
    }
    virtual void UpdatePatchCoords(
        std::vector<Osd::PatchCoord> const &patchCoords) {
        if (_patchCoords and
            _patchCoords->GetNumVertices() != (int)patchCoords.size()) {
            delete _patchCoords;
            _patchCoords = NULL;
        }
        if (not _patchCoords) {
            _patchCoords = EVAL_VERTEX_BUFFER::Create(5,
                                                      (int)patchCoords.size(),
                                                      _deviceContext);
        }
        _patchCoords->UpdateData((float*)&patchCoords[0], 0, (int)patchCoords.size(), _deviceContext);
    }
private:
    SRC_VERTEX_BUFFER *_srcData;
    SRC_VERTEX_BUFFER *_srcVaryingData;
    EVAL_VERTEX_BUFFER *_vertexData;
    EVAL_VERTEX_BUFFER *_derivatives;
    EVAL_VERTEX_BUFFER *_varyingData;
    EVAL_VERTEX_BUFFER *_patchCoords;
    PATCH_TABLE *_patchTable;
    Osd::BufferDescriptor _srcDesc;
    Osd::BufferDescriptor _srcVaryingDesc;
    Osd::BufferDescriptor _vertexDesc;
    Osd::BufferDescriptor _varyingDesc;
    Osd::BufferDescriptor _duDesc;
    Osd::BufferDescriptor _dvDesc;
    int _numCoarseVerts;

    STENCIL_TABLE const *_vertexStencils;
    STENCIL_TABLE const *_varyingStencils;

    EvaluatorCache *_evaluatorCache;
    DEVICE_CONTEXT *_deviceContext;
};

EvalOutputBase *g_evalOutput = NULL;

STParticles * g_particles=0;

//------------------------------------------------------------------------------
static void
updateGeom() {
    int nverts = (int)g_orgPositions.size() / 3;

    const float *p = &g_orgPositions[0];

    float r = sin(g_frame*0.001f) * g_moveScale;

    for (int i = 0; i < nverts; ++i) {
        //float move = 0.05f*cosf(p[0]*20+g_frame*0.01f);
        float ct = cos(p[2] * r);
        float st = sin(p[2] * r);
        g_positions[i*3+0] = p[0]*ct + p[1]*st;
        g_positions[i*3+1] = -p[0]*st + p[1]*ct;
        g_positions[i*3+2] = p[2];
        p+=3;
    }

    // Run Compute pass to pose the control vertices ---------------------------
    Stopwatch s;
    s.Start();

    // update coarse vertices
    g_evalOutput->UpdateData(&g_positions[0], 0, nverts);

    // update coarse varying
    if (g_drawMode == kVARYING) {
        g_evalOutput->UpdateVaryingData(&g_varyingColors[0], 0, nverts);

    }

    // Refine
    g_evalOutput->Refine();

    s.Stop();
    g_computeTime = float(s.GetElapsed() * 1000.0f);


    // Run Eval pass to get the samples locations ------------------------------

    s.Start();

    // Apply 'dynamics' update
    assert(g_particles);

    g_particles->Update(g_evalTime); // XXXX g_evalTime is not really elapsed time...

    std::vector<OpenSubdiv::Osd::PatchCoord> const &patchCoords
        = g_particles->GetPatchCoords();

    // update patchcoord to be evaluated
    g_evalOutput->UpdatePatchCoords(patchCoords);

    // Evaluate the positions of the samples on the limit surface
    if (g_drawMode == kNORMAL || g_drawMode == kSHADE) {
        // evaluate positions and derivatives
        g_evalOutput->EvalPatchesWithDerivatives();
    } else {
        // evaluate positions
        g_evalOutput->EvalPatches();
    }

    // color
    if (g_drawMode == kVARYING) {
        // XXX: is this really varying?
        g_evalOutput->EvalPatchesVarying();
    }

    s.Stop();

    g_evalTime = float(s.GetElapsed());
}

//------------------------------------------------------------------------------
static void
createOsdMesh(ShapeDesc const & shapeDesc, int level) {


    Shape * shape = Shape::parseObj(shapeDesc.data.c_str(), shapeDesc.scheme);

    // create Vtr mesh (topology)
    OpenSubdiv::Sdc::SchemeType sdctype = GetSdcType(*shape);
    OpenSubdiv::Sdc::Options sdcoptions = GetSdcOptions(*shape);

    Far::TopologyRefiner *topologyRefiner =
        OpenSubdiv::Far::TopologyRefinerFactory<Shape>::Create(*shape,
            OpenSubdiv::Far::TopologyRefinerFactory<Shape>::Options(sdctype, sdcoptions));

    g_orgPositions=shape->verts;
    g_positions.resize(g_orgPositions.size(), 0.0f);

    delete shape;

    float speed = g_particles ? g_particles->GetSpeed() : 0.2f;

    createCoarseMesh(*topologyRefiner);

    Far::StencilTable const * vertexStencils = NULL;
    Far::StencilTable const * varyingStencils = NULL;
    int nverts=0;
    {
        // Apply feature adaptive refinement to the mesh so that we can use the
        // limit evaluation API features.
        Far::TopologyRefiner::AdaptiveOptions options(level);
        topologyRefiner->RefineAdaptive(options);

        // Generate stencil table to update the bi-cubic patches control
        // vertices after they have been re-posed (both for vertex & varying
        // interpolation)
        Far::StencilTableFactory::Options soptions;
        soptions.generateOffsets=true;
        soptions.generateIntermediateLevels=true;

        vertexStencils =
            Far::StencilTableFactory::Create(*topologyRefiner, soptions);

        soptions.interpolationMode = Far::StencilTableFactory::INTERPOLATE_VARYING;

        varyingStencils =
            Far::StencilTableFactory::Create(*topologyRefiner, soptions);

        // Generate bi-cubic patch table for the limit surface
        Far::PatchTableFactory::Options poptions;
        if (g_endCap == kEndCapBSplineBasis) {
            poptions.SetEndCapType(
                Far::PatchTableFactory::Options::ENDCAP_BSPLINE_BASIS);
        } else {
            poptions.SetEndCapType(
                Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
        }

        Far::PatchTable const * patchTable =
            Far::PatchTableFactory::Create(*topologyRefiner, poptions);

        // append endcap stencils
        if (Far::StencilTable const *endCapVertexStencilTable =
            patchTable->GetEndCapVertexStencilTable()) {
            Far::StencilTable const *table =
                Far::StencilTableFactory::AppendEndCapStencilTable(
                    *topologyRefiner,
                    vertexStencils, endCapVertexStencilTable);
            delete vertexStencils;
            vertexStencils = table;
        }
        if (Far::StencilTable const *endCapVaryingStencilTable =
            patchTable->GetEndCapVaryingStencilTable()) {
            Far::StencilTable const *table =
                Far::StencilTableFactory::AppendEndCapStencilTable(
                    *topologyRefiner,
                    varyingStencils, endCapVaryingStencilTable);
            delete varyingStencils;
            varyingStencils = table;
        }

        // total number of vertices = coarse verts + refined verts + gregory basis verts
        nverts = vertexStencils->GetNumControlVertices() +
            vertexStencils->GetNumStencils();

        if (g_patchTable) delete g_patchTable;
        g_patchTable = patchTable;
    }

    // note that for patch eval we need coarse+refined combined buffer.
    int nCoarseVertices = topologyRefiner->GetLevel(0).GetNumVertices();

    delete g_evalOutput;
    if (g_kernel == kCPU) {
        g_evalOutput = new EvalOutput<Osd::CpuVertexBuffer,
                                      Osd::CpuGLVertexBuffer,
                                      Far::StencilTable,
                                      Osd::CpuPatchTable,
                                      Osd::CpuEvaluator>
            (vertexStencils, varyingStencils,
             nCoarseVertices, nverts, g_nParticles, g_patchTable);
#ifdef OPENSUBDIV_HAS_OPENMP
    } else if (g_kernel == kOPENMP) {
        g_evalOutput = new EvalOutput<Osd::CpuVertexBuffer,
                                      Osd::CpuGLVertexBuffer,
                                      Far::StencilTable,
                                      Osd::CpuPatchTable,
                                      Osd::OmpEvaluator>
            (vertexStencils, varyingStencils,
            nCoarseVertices, nverts, g_nParticles, g_patchTable);
#endif
#ifdef OPENSUBDIV_HAS_TBB
    } else if (g_kernel == kTBB) {
        g_evalOutput = new EvalOutput<Osd::CpuVertexBuffer,
                                      Osd::CpuGLVertexBuffer,
                                      Far::StencilTable,
                                      Osd::CpuPatchTable,
                                      Osd::TbbEvaluator>
            (vertexStencils, varyingStencils,
            nCoarseVertices, nverts, g_nParticles, g_patchTable);
#endif
#ifdef OPENSUBDIV_HAS_CUDA
    } else if (g_kernel == kCUDA) {
        g_evalOutput = new EvalOutput<Osd::CudaVertexBuffer,
                                      Osd::CudaGLVertexBuffer,
                                      Osd::CudaStencilTable,
                                      Osd::CudaPatchTable,
                                      Osd::CudaEvaluator>
            (vertexStencils, varyingStencils,
            nCoarseVertices, nverts, g_nParticles, g_patchTable);
#endif
#ifdef OPENSUBDIV_HAS_OPENCL
    } else if (g_kernel == kCL) {
        static Osd::EvaluatorCacheT<Osd::CLEvaluator> clEvaluatorCache;
        g_evalOutput = new EvalOutput<Osd::CLVertexBuffer,
                                      Osd::CLGLVertexBuffer,
                                      Osd::CLStencilTable,
                                      Osd::CLPatchTable,
                                      Osd::CLEvaluator,
                                      CLDeviceContext>
            (vertexStencils, varyingStencils,
            nCoarseVertices, nverts, g_nParticles, g_patchTable,
            &clEvaluatorCache, &g_clDeviceContext);
#endif
#ifdef OPENSUBDIV_HAS_GLSL_TRANSFORM_FEEDBACK
    } else if (g_kernel == kGLXFB) {
        static Osd::EvaluatorCacheT<Osd::GLXFBEvaluator> glXFBEvaluatorCache;
        g_evalOutput = new EvalOutput<Osd::GLVertexBuffer,
                                      Osd::GLVertexBuffer,
                                      Osd::GLStencilTableTBO,
                                      Osd::GLPatchTable,
                                      Osd::GLXFBEvaluator>
            (vertexStencils, varyingStencils,
             nCoarseVertices, nverts, g_nParticles, g_patchTable,
             &glXFBEvaluatorCache);
#endif
#ifdef OPENSUBDIV_HAS_GLSL_COMPUTE
    } else if (g_kernel == kGLCompute) {
        static Osd::EvaluatorCacheT<Osd::GLComputeEvaluator> glComputeEvaluatorCache;
        g_evalOutput = new EvalOutput<Osd::GLVertexBuffer,
                                      Osd::GLVertexBuffer,
                                      Osd::GLStencilTableSSBO,
                                      Osd::GLPatchTable,
                                      Osd::GLComputeEvaluator>
            (vertexStencils, varyingStencils,
             nCoarseVertices, nverts, g_nParticles, g_patchTable,
             &glComputeEvaluatorCache);
#endif
    }

    // Create the 'uv particles' manager - this class manages the limit
    // location samples (ptex face index, (s,t) and updates them between frames.
    // Note: the number of limit locations can be entirely arbitrary
    delete g_particles;
    g_particles = new STParticles(*topologyRefiner, g_patchTable,
                                  g_nParticles, !g_randomStart);
    g_nParticles = g_particles->GetNumParticles();
    g_particles->SetSpeed(speed);

    updateGeom();

    delete topologyRefiner;
}

//------------------------------------------------------------------------------
static void
checkGLErrors(std::string const & where = "") {
    GLuint err;
    while ((err = glGetError()) != GL_NO_ERROR) {

        std::cerr << "GL error: "
                  << (where.empty() ? "" : where + " ")
                  << err << "\n";
    }
}

//------------------------------------------------------------------------------
static bool
linkDefaultProgram() {

#if defined(GL_ARB_tessellation_shader) || defined(GL_VERSION_4_0)
    #define GLSL_VERSION_DEFINE "#version 400\n"
#else
    #define GLSL_VERSION_DEFINE "#version 150\n"
#endif

    static const char *vsSrc =
        GLSL_VERSION_DEFINE
        "in vec3 position;\n"
        "in vec3 color;\n"
        "in vec3 tangentU;\n"
        "in vec3 tangentV;\n"
        "in vec2 patchCoord;\n"
        "out vec4 fragColor;\n"
        "uniform mat4 ModelViewMatrix;\n"
        "uniform mat4 ProjectionMatrix;\n"
        "uniform int DrawMode;\n"
        "void main() {\n"
        "  vec3 normal = (ModelViewMatrix * "
        "               vec4(normalize(cross(tangentU, tangentV)), 0)).xyz;\n"
        "  gl_Position = ProjectionMatrix * ModelViewMatrix * "
        "                  vec4(position, 1);\n"
        "  if (DrawMode == 0) {\n" // UV
        "    fragColor = vec4(patchCoord.x, patchCoord.y, 0, 1);\n"
        "  } else if (DrawMode == 2) {\n"
        "    fragColor = vec4(normal*0.5+vec3(0.5), 1);\n"
        "  } else if (DrawMode == 3) {\n"
        "    fragColor = vec4(vec3(1)*dot(normal, vec3(0,0,1)), 1);\n"
        "  } else if (DrawMode == 4) {\n"  // face varying
        "    fragColor = vec4(1);\n"
        "  } else {\n" // varying
        "    fragColor = vec4(color, 1);\n"
        "  }\n"
        "}\n";

    static const char *fsSrc =
        GLSL_VERSION_DEFINE
        "in vec4 fragColor;\n"
        "out vec4 color;\n"
        "void main() {\n"
        "  color = fragColor;\n"
        "}\n";

    GLuint program = glCreateProgram();
    GLuint vertexShader = GLUtils::CompileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fragmentShader = GLUtils::CompileShader(GL_FRAGMENT_SHADER, fsSrc);

    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);

    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "color");
    glBindAttribLocation(program, 2, "tangentU");
    glBindAttribLocation(program, 3, "tangentV");
    glBindAttribLocation(program, 4, "patchCoord");
    glBindFragDataLocation(program, 0, "color");

    glLinkProgram(program);

    GLint status;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        GLint infoLogLength;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLogLength);
        char *infoLog = new char[infoLogLength];
        glGetProgramInfoLog(program, infoLogLength, NULL, infoLog);
        printf("%s\n", infoLog);
        delete[] infoLog;
        exit(1);
    }

    g_defaultProgram.program = program;
    g_defaultProgram.uniformModelViewMatrix =
        glGetUniformLocation(program, "ModelViewMatrix");
    g_defaultProgram.uniformProjectionMatrix =
        glGetUniformLocation(program, "ProjectionMatrix");
    g_defaultProgram.uniformDrawMode =
        glGetUniformLocation(program, "DrawMode");
    g_defaultProgram.attrPosition = glGetAttribLocation(program, "position");
    g_defaultProgram.attrColor = glGetAttribLocation(program, "color");
    g_defaultProgram.attrTangentU = glGetAttribLocation(program, "tangentU");
    g_defaultProgram.attrTangentV = glGetAttribLocation(program, "tangentV");
    g_defaultProgram.attrPatchCoord = glGetAttribLocation(program, "patchCoord");

    return true;
}

//------------------------------------------------------------------------------
static inline void
setSharpnessColor(float s, float *r, float *g, float *b) {
    //  0.0       2.0       4.0
    // green --- yellow --- red
    *r = std::min(1.0f, s * 0.5f);
    *g = std::min(1.0f, 2.0f - s*0.5f);
    *b = 0;
}

//------------------------------------------------------------------------------
static void
drawCageEdges() {

    glUseProgram(g_defaultProgram.program);
    glUniformMatrix4fv(g_defaultProgram.uniformModelViewMatrix,
                       1, GL_FALSE, g_transformData.ModelViewMatrix);
    glUniformMatrix4fv(g_defaultProgram.uniformProjectionMatrix,
                       1, GL_FALSE, g_transformData.ProjectionMatrix);
    glUniform1i(g_defaultProgram.uniformDrawMode, 0);

    std::vector<float> vbo;
    vbo.reserve(g_coarseEdges.size() * 6);
    float r, g, b;
    for (int i = 0; i < (int)g_coarseEdges.size(); i+=2) {
        setSharpnessColor(g_coarseEdgeSharpness[i/2], &r, &g, &b);
        for (int j = 0; j < 2; ++j) {
            vbo.push_back(g_positions[g_coarseEdges[i+j]*3]);
            vbo.push_back(g_positions[g_coarseEdges[i+j]*3+1]);
            vbo.push_back(g_positions[g_coarseEdges[i+j]*3+2]);
            vbo.push_back(r);
            vbo.push_back(g);
            vbo.push_back(b);
        }
    }

    glBindVertexArray(g_cageEdgeVAO);

    glBindBuffer(GL_ARRAY_BUFFER, g_cageEdgeVBO);
    glBufferData(GL_ARRAY_BUFFER, (int)vbo.size() * sizeof(float), &vbo[0],
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(g_defaultProgram.attrPosition);
    glEnableVertexAttribArray(g_defaultProgram.attrColor);
    glDisableVertexAttribArray(g_defaultProgram.attrTangentU);
    glDisableVertexAttribArray(g_defaultProgram.attrTangentV);
    glDisableVertexAttribArray(g_defaultProgram.attrPatchCoord);
    glVertexAttribPointer(g_defaultProgram.attrPosition,
                          3, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 6, 0);
    glVertexAttribPointer(g_defaultProgram.attrColor,
                          3, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 6, (void*)12);

    glDrawArrays(GL_LINES, 0, (int)g_coarseEdges.size());

    glBindVertexArray(0);
    glUseProgram(0);
}

//------------------------------------------------------------------------------
static void
drawCageVertices() {

    glUseProgram(g_defaultProgram.program);
    glUniformMatrix4fv(g_defaultProgram.uniformModelViewMatrix,
                       1, GL_FALSE, g_transformData.ModelViewMatrix);
    glUniformMatrix4fv(g_defaultProgram.uniformProjectionMatrix,
                       1, GL_FALSE, g_transformData.ProjectionMatrix);
    glUniform1i(g_defaultProgram.uniformDrawMode, 0);

    int numPoints = (int)g_positions.size()/3;
    std::vector<float> vbo;
    vbo.reserve(numPoints*6);
    float r, g, b;
    for (int i = 0; i < numPoints; ++i) {

        switch (g_drawMode) {

            case kVARYING : { r=g_varyingColors[i*3+0];
                              g=g_varyingColors[i*3+1];
                              b=g_varyingColors[i*3+2];
                            } break;

            case kUV      : { setSharpnessColor(g_coarseVertexSharpness[i], &r, &g, &b);
                            } break;

            default : break;
        }

        vbo.push_back(g_positions[i*3+0]);
        vbo.push_back(g_positions[i*3+1]);
        vbo.push_back(g_positions[i*3+2]);
        vbo.push_back(r);
        vbo.push_back(g);
        vbo.push_back(b);
    }

    glBindVertexArray(g_cageVertexVAO);

    glBindBuffer(GL_ARRAY_BUFFER, g_cageVertexVBO);
    glBufferData(GL_ARRAY_BUFFER, (int)vbo.size() * sizeof(float), &vbo[0],
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(g_defaultProgram.attrPosition);
    glEnableVertexAttribArray(g_defaultProgram.attrColor);
    glDisableVertexAttribArray(g_defaultProgram.attrTangentU);
    glDisableVertexAttribArray(g_defaultProgram.attrTangentV);
    glDisableVertexAttribArray(g_defaultProgram.attrPatchCoord);
    glVertexAttribPointer(g_defaultProgram.attrPosition,
                          3, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 6, 0);
    glVertexAttribPointer(g_defaultProgram.attrColor,
                          3, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 6, (void*)12);

    glPointSize(10.0f);
    glDrawArrays(GL_POINTS, 0, numPoints);
    glPointSize(1.0f);

    glBindVertexArray(0);
    glUseProgram(0);
}

//------------------------------------------------------------------------------
static void
drawSamples() {
    glUseProgram(g_defaultProgram.program);

    glUniformMatrix4fv(g_defaultProgram.uniformModelViewMatrix,
                       1, GL_FALSE, g_transformData.ModelViewMatrix);
    glUniformMatrix4fv(g_defaultProgram.uniformProjectionMatrix,
                       1, GL_FALSE, g_transformData.ProjectionMatrix);
    glUniform1i(g_defaultProgram.uniformDrawMode, g_drawMode);

    glBindVertexArray(g_samplesVAO);

    glEnableVertexAttribArray(g_defaultProgram.attrPosition);
    glEnableVertexAttribArray(g_defaultProgram.attrColor);
    glEnableVertexAttribArray(g_defaultProgram.attrTangentU);
    glEnableVertexAttribArray(g_defaultProgram.attrTangentV);

    glBindBuffer(GL_ARRAY_BUFFER, g_evalOutput->BindVertexData());
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 6, 0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 6, (float*)12);

    glBindBuffer(GL_ARRAY_BUFFER, g_evalOutput->BindDerivatives());
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 6, 0);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 6, (float*)12);

    glBindBuffer(GL_ARRAY_BUFFER, g_evalOutput->BindPatchCoords());
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 5, (float*)12);

    glEnableVertexAttribArray(g_defaultProgram.attrPosition);
    glEnableVertexAttribArray(g_defaultProgram.attrColor);
    glEnableVertexAttribArray(g_defaultProgram.attrTangentU);
    glEnableVertexAttribArray(g_defaultProgram.attrTangentV);
    glEnableVertexAttribArray(g_defaultProgram.attrPatchCoord);

    glPointSize(2.0f);
    int nPatchCoords = (int)g_particles->GetPatchCoords().size();
    glDrawArrays(GL_POINTS, 0, nPatchCoords);
    glPointSize(1.0f);

    glDisableVertexAttribArray(g_defaultProgram.attrPosition);
    glDisableVertexAttribArray(g_defaultProgram.attrColor);
    glDisableVertexAttribArray(g_defaultProgram.attrTangentU);
    glDisableVertexAttribArray(g_defaultProgram.attrTangentV);
    glDisableVertexAttribArray(g_defaultProgram.attrPatchCoord);

    glBindVertexArray(0);

    glUseProgram(0);
}

//------------------------------------------------------------------------------
static void
display() {

    g_hud.GetFrameBuffer()->Bind();

    Stopwatch s;
    s.Start();

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glViewport(0, 0, g_width, g_height);

    double aspect = g_width/(double)g_height;
    identity(g_transformData.ModelViewMatrix);
    translate(g_transformData.ModelViewMatrix, -g_pan[0], -g_pan[1], -g_dolly);
    rotate(g_transformData.ModelViewMatrix, g_rotate[1], 1, 0, 0);
    rotate(g_transformData.ModelViewMatrix, g_rotate[0], 0, 1, 0);
    rotate(g_transformData.ModelViewMatrix, -90, 1, 0, 0);
    translate(g_transformData.ModelViewMatrix,
              -g_center[0], -g_center[1], -g_center[2]);
    perspective(g_transformData.ProjectionMatrix,
                45.0f, (float)aspect, 0.01f, 500.0f);
    multMatrix(g_transformData.ModelViewProjectionMatrix,
               g_transformData.ModelViewMatrix,
               g_transformData.ProjectionMatrix);

    glEnable(GL_DEPTH_TEST);

    s.Stop();
    float drawCpuTime = float(s.GetElapsed() * 1000.0f);
    s.Start();
    glFinish();
    s.Stop();
    float drawGpuTime = float(s.GetElapsed() * 1000.0f);

    drawSamples();

    if (g_drawCageEdges)
        drawCageEdges();

    if (g_drawCageVertices)
        drawCageVertices();

    g_hud.GetFrameBuffer()->ApplyImageShader();

    if (g_hud.IsVisible()) {
        g_fpsTimer.Stop();
        double fps = 1.0/g_fpsTimer.GetElapsed();
        g_fpsTimer.Start();

        int nPatchCoords = (int)g_particles->GetPatchCoords().size();

        g_hud.DrawString(10, -150, "Particle Speed ([) (]): %.1f", g_particles->GetSpeed());
        g_hud.DrawString(10, -120, "# Samples  : (%d / %d)", nPatchCoords, g_nParticles);
        g_hud.DrawString(10, -100, "Compute    : %.3f ms", g_computeTime);
        g_hud.DrawString(10, -80,  "Eval       : %.3f ms", g_evalTime * 1000.f);
        g_hud.DrawString(10, -60,  "GPU Draw   : %.3f ms", drawGpuTime);
        g_hud.DrawString(10, -40,  "CPU Draw   : %.3f ms", drawCpuTime);
        g_hud.DrawString(10, -20,  "FPS        : %3.1f", fps);

        if (g_drawMode==kFACEVARYING) {
            static char msg[] = "Face-varying interpolation not implemented yet";
            g_hud.DrawString(g_width/2-20/2*8, g_height/2, msg);
        }

        g_hud.Flush();
    }

    glFinish();

    checkGLErrors("display leave");
}

//------------------------------------------------------------------------------
static void
idle() {

    if (not g_freeze)
        g_frame++;

    updateGeom();

    if (g_repeatCount != 0 and g_frame >= g_repeatCount)
        g_running = 0;
}

//------------------------------------------------------------------------------
static void
motion(GLFWwindow *, double dx, double dy) {

    int x=(int)dx, y=(int)dy;

    if (g_mbutton[0] && !g_mbutton[1] && !g_mbutton[2]) {
        // orbit
        g_rotate[0] += x - g_prev_x;
        g_rotate[1] += y - g_prev_y;
    } else if (!g_mbutton[0] && !g_mbutton[1] && g_mbutton[2]) {
        // pan
        g_pan[0] -= g_dolly*(x - g_prev_x)/g_width;
        g_pan[1] += g_dolly*(y - g_prev_y)/g_height;
    } else if ((g_mbutton[0] && !g_mbutton[1] && g_mbutton[2]) or
               (!g_mbutton[0] && g_mbutton[1] && !g_mbutton[2])) {
        // dolly
        g_dolly -= g_dolly*0.01f*(x - g_prev_x);
        if(g_dolly <= 0.01) g_dolly = 0.01f;
    }

    g_prev_x = x;
    g_prev_y = y;
}

//------------------------------------------------------------------------------
static void
mouse(GLFWwindow *, int button, int state, int /* mods */) {

    if (button == 0 && state == GLFW_PRESS && g_hud.MouseClick(g_prev_x, g_prev_y))
        return;

    if (button < 3) {
        g_mbutton[button] = (state == GLFW_PRESS);
    }
}

//------------------------------------------------------------------------------
static void
reshape(GLFWwindow *, int width, int height) {

    g_width = width;
    g_height = height;

    int windowWidth = g_width, windowHeight = g_height;

    // window size might not match framebuffer size on a high DPI display
    glfwGetWindowSize(g_window, &windowWidth, &windowHeight);

    g_hud.Rebuild(windowWidth, windowHeight, width, height);
}

//------------------------------------------------------------------------------
void windowClose(GLFWwindow*) {
    g_running = false;
}

//------------------------------------------------------------------------------
static void
setSamples(bool add) {
    if (add) {
        g_nParticles = g_nParticles * 2;
    } else {
        g_nParticles = std::max(1, g_nParticles / 2);
    }

    createOsdMesh(g_defaultShapes[g_currentShape], g_level);
}

//------------------------------------------------------------------------------
static void
keyboard(GLFWwindow *, int key, int /* scancode */, int event, int /* mods */) {

    if (event == GLFW_RELEASE) return;
    if (g_hud.KeyDown(tolower(key))) return;

    switch (key) {
        case 'Q': g_running = 0; break;

        case '=': setSamples(true); break;

        case '-': setSamples(false); break;

        case '[': if (g_particles) {
                      g_particles->SetSpeed(g_particles->GetSpeed()-0.1f);
                  } break;
        case ']': if (g_particles) {
                      g_particles->SetSpeed(g_particles->GetSpeed()+0.1f);
                  } break;

        case GLFW_KEY_ESCAPE: g_hud.SetVisible(!g_hud.IsVisible()); break;
    }
}

//------------------------------------------------------------------------------
static void
callbackError(OpenSubdiv::Far::ErrorType err, const char *message) {
    printf("Error: %d\n", err);
    printf("%s", message);
}

//------------------------------------------------------------------------------
static void
callbackModel(int m) {
    if (m < 0)
        m = 0;

    if (m >= (int)g_defaultShapes.size())
        m = (int)g_defaultShapes.size() - 1;

    g_currentShape = m;
    createOsdMesh(g_defaultShapes[g_currentShape], g_level);
}

//------------------------------------------------------------------------------
static void
callbackEndCap(int endCap) {
    g_endCap = endCap;
    createOsdMesh(g_defaultShapes[g_currentShape], g_level);
}

//------------------------------------------------------------------------------
static void
callbackKernel(int k) {

    g_kernel = k;

#ifdef OPENSUBDIV_HAS_OPENCL
    if (g_kernel == kCL and (not g_clDeviceContext.IsInitialized())) {
        if (g_clDeviceContext.Initialize() == false) {
            printf("Error in initializing OpenCL\n");
            exit(1);
        }
    }
#endif
#ifdef OPENSUBDIV_HAS_CUDA
    if (g_kernel == kCUDA and (not g_cudaDeviceContext.IsInitialized())) {
        if (g_cudaDeviceContext.Initialize() == false) {
            printf("Error in initializing Cuda\n");
            exit(1);
        }
    }
#endif

    createOsdMesh(g_defaultShapes[g_currentShape], g_level);

}

//------------------------------------------------------------------------------
static void
callbackLevel(int l) {
    g_level = l;
    createOsdMesh(g_defaultShapes[g_currentShape], g_level);
}

//------------------------------------------------------------------------------
static void
callbackAnimate(bool checked, int /* m */) {
    g_moveScale = checked * 3.0f;
}

//------------------------------------------------------------------------------
static void
callbackFreeze(bool checked, int /* f */) {
    g_freeze = checked;
}

//------------------------------------------------------------------------------
static void
callbackCentered(bool checked, int /* f */) {
    g_randomStart = checked;
    createOsdMesh(g_defaultShapes[g_currentShape], g_level);
}

//------------------------------------------------------------------------------
static void
callbackDisplayCageVertices(bool checked, int /* d */) {
    g_drawCageVertices = checked;
}

//------------------------------------------------------------------------------
static void
callbackDisplayCageEdges(bool checked, int /* d */) {
    g_drawCageEdges = checked;
}

//------------------------------------------------------------------------------
static void
callbackDisplayVaryingColors(int mode) {
    g_drawMode = mode;
    createOsdMesh(g_defaultShapes[g_currentShape], g_level);
}


//------------------------------------------------------------------------------
static void
initHUD() {
    int windowWidth = g_width, windowHeight = g_height,
        frameBufferWidth = g_width, frameBufferHeight = g_height;

    // window size might not match framebuffer size on a high DPI display
    glfwGetWindowSize(g_window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(g_window, &frameBufferWidth, &frameBufferHeight);

    g_hud.Init(windowWidth, windowHeight, frameBufferWidth, frameBufferHeight);

    g_hud.SetFrameBuffer(new GLFrameBuffer);

    g_hud.AddCheckBox("Cage Edges (H)", true, 10, 10, callbackDisplayCageEdges, 0, 'h');
    g_hud.AddCheckBox("Cage Verts (J)", true, 10, 30, callbackDisplayCageVertices, 0, 'j');
    g_hud.AddCheckBox("Animate vertices (M)", g_moveScale != 0, 10, 50, callbackAnimate, 0, 'm');
    g_hud.AddCheckBox("Freeze (spc)", false, 10, 70, callbackFreeze, 0, ' ');

    g_hud.AddCheckBox("Random Start", g_randomStart, 10, 120, callbackCentered, 0);

    int compute_pulldown = g_hud.AddPullDown("Compute (K)", 475, 10, 300,
                                             callbackKernel, 'k');
    g_hud.AddPullDownButton(compute_pulldown, "CPU", kCPU);
#ifdef OPENSUBDIV_HAS_OPENMP
    g_hud.AddPullDownButton(compute_pulldown, "OPENMP", kOPENMP);
#endif
#ifdef OPENSUBDIV_HAS_TBB
    g_hud.AddPullDownButton(compute_pulldown, "TBB", kTBB);
#endif
#ifdef OPENSUBDIV_HAS_CUDA
    g_hud.AddPullDownButton(compute_pulldown, "CUDA", kCUDA);
#endif
#ifdef OPENSUBDIV_HAS_OPENCL
    g_hud.AddPullDownButton(compute_pulldown, "OpenCL", kCL);
#endif
#ifdef OPENSUBDIV_HAS_GLSL_TRANSFORM_FEEDBACK
    g_hud.AddPullDownButton(compute_pulldown, "GL XFB", kGLXFB);
#endif
#ifdef OPENSUBDIV_HAS_GLSL_COMPUTE
    if (GLUtils::GL_ARBComputeShaderOrGL_VERSION_4_3()) {
        g_hud.AddPullDownButton(compute_pulldown, "GL Compute", kGLCompute);
    }
#endif

    int endcap_pulldown = g_hud.AddPullDown("End cap (E)", 10, 140, 200,
                                            callbackEndCap, 'e');
    g_hud.AddPullDownButton(endcap_pulldown, "BSpline",
        kEndCapBSplineBasis,
        g_endCap == kEndCapBSplineBasis);
    g_hud.AddPullDownButton(endcap_pulldown, "GregoryBasis",
        kEndCapGregoryBasis,
        g_endCap == kEndCapGregoryBasis);

    int shading_pulldown = g_hud.AddPullDown("Shading (W)", 250, 10, 250, callbackDisplayVaryingColors, 'w');
    g_hud.AddPullDownButton(shading_pulldown, "(u,v)", kUV, g_drawMode==kUV);
    g_hud.AddPullDownButton(shading_pulldown, "Varying", kVARYING, g_drawMode==kVARYING);
    g_hud.AddPullDownButton(shading_pulldown, "Normal", kNORMAL, g_drawMode==kNORMAL);
    g_hud.AddPullDownButton(shading_pulldown, "Shade", kSHADE, g_drawMode==kSHADE);
    g_hud.AddPullDownButton(shading_pulldown, "FaceVarying", kFACEVARYING, g_drawMode==kFACEVARYING);

    for (int i = 1; i < 11; ++i) {
        char level[16];
        sprintf(level, "Lv. %d", i);
        g_hud.AddRadioButton(3, level, i==g_level, 10, 170+i*20, callbackLevel, i, '0'+(i%10));
    }

    int pulldown_handle = g_hud.AddPullDown("Shape (N)", -300, 10, 300, callbackModel, 'n');
    for (int i = 0; i < (int)g_defaultShapes.size(); ++i) {
        g_hud.AddPullDownButton(pulldown_handle, g_defaultShapes[i].name.c_str(),i);
    }

    g_hud.Rebuild(windowWidth, windowHeight, frameBufferWidth, frameBufferHeight);
}

//------------------------------------------------------------------------------
static void
initGL() {

    glClearColor(0.1f, 0.1f, 0.1f, 0.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glCullFace(GL_BACK);
    glEnable(GL_CULL_FACE);

    glGenVertexArrays(1, &g_cageVertexVAO);
    glGenVertexArrays(1, &g_cageEdgeVAO);
    glGenVertexArrays(1, &g_samplesVAO);
    glGenBuffers(1, &g_cageVertexVBO);
    glGenBuffers(1, &g_cageEdgeVBO);
}

//------------------------------------------------------------------------------
static void
uninitGL() {

    glDeleteBuffers(1, &g_cageVertexVBO);
    glDeleteBuffers(1, &g_cageEdgeVBO);
    glDeleteVertexArrays(1, &g_cageVertexVAO);
    glDeleteVertexArrays(1, &g_cageEdgeVAO);
    glDeleteVertexArrays(1, &g_samplesVAO);
}

//------------------------------------------------------------------------------
static void
callbackErrorGLFW(int error, const char* description) {
    fprintf(stderr, "GLFW Error (%d) : %s\n", error, description);
}

//------------------------------------------------------------------------------
static void
setGLCoreProfile() {

    #define glfwOpenWindowHint glfwWindowHint
    #define GLFW_OPENGL_VERSION_MAJOR GLFW_CONTEXT_VERSION_MAJOR
    #define GLFW_OPENGL_VERSION_MINOR GLFW_CONTEXT_VERSION_MINOR

    glfwOpenWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if not defined(__APPLE__)
    glfwOpenWindowHint(GLFW_OPENGL_VERSION_MAJOR, 4);
    glfwOpenWindowHint(GLFW_OPENGL_VERSION_MINOR, 2);
#else
    glfwOpenWindowHint(GLFW_OPENGL_VERSION_MAJOR, 3);
    glfwOpenWindowHint(GLFW_OPENGL_VERSION_MINOR, 2);
#endif
    glfwOpenWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
}

//------------------------------------------------------------------------------
int main(int argc, char **argv) {

    bool fullscreen = false;

    std::string str;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-f"))
            fullscreen = true;
        else {
            std::ifstream ifs(argv[1]);
            if (ifs) {
                std::stringstream ss;
                ss << ifs.rdbuf();
                ifs.close();
                str = ss.str();
                g_defaultShapes.push_back(ShapeDesc(argv[1], str.c_str(), kCatmark));
            }
        }
    }

    Far::SetErrorCallback(callbackError);

    initShapes();

    glfwSetErrorCallback(callbackErrorGLFW);
    if (not glfwInit()) {
        printf("Failed to initialize GLFW\n");
        return 1;
    }

    static const char windowTitle[] = "OpenSubdiv glEvalLimit " OPENSUBDIV_VERSION_STRING;

#define CORE_PROFILE
#ifdef CORE_PROFILE
    setGLCoreProfile();
#endif

    if (fullscreen) {

        g_primary = glfwGetPrimaryMonitor();

        // apparently glfwGetPrimaryMonitor fails under linux : if no primary,
        // settle for the first one in the list
        if (not g_primary) {
            int count=0;
            GLFWmonitor ** monitors = glfwGetMonitors(&count);

            if (count)
                g_primary = monitors[0];
        }

        if (g_primary) {
            GLFWvidmode const * vidmode = glfwGetVideoMode(g_primary);
            g_width = vidmode->width;
            g_height = vidmode->height;
        }
    }

    if (not (g_window=glfwCreateWindow(g_width, g_height, windowTitle,
                                       fullscreen and g_primary ? g_primary : NULL, NULL))) {
        printf("Failed to open window.\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(g_window);

    // accommodate high DPI displays (e.g. mac retina displays)
    glfwGetFramebufferSize(g_window, &g_width, &g_height);
    glfwSetFramebufferSizeCallback(g_window, reshape);

    glfwSetKeyCallback(g_window, keyboard);
    glfwSetCursorPosCallback(g_window, motion);
    glfwSetMouseButtonCallback(g_window, mouse);
    glfwSetWindowCloseCallback(g_window, windowClose);

#if defined(OSD_USES_GLEW)
#ifdef CORE_PROFILE
    // this is the only way to initialize glew correctly under core profile context.
    glewExperimental = true;
#endif
    if (GLenum r = glewInit() != GLEW_OK) {
        printf("Failed to initialize glew. Error = %s\n", glewGetErrorString(r));
        exit(1);
    }
#ifdef CORE_PROFILE
    // clear GL errors which was generated during glewInit()
    glGetError();
#endif
#endif

    //std::string & data = g_defaultShapes[ g_currentShape ].data;
    //Scheme scheme = g_defaultShapes[ g_currentShape ].scheme;

    //createOsdMesh( data, g_level, scheme );

    initGL();
    linkDefaultProgram();

    glfwSwapInterval(0);

    initHUD();
    callbackModel(g_currentShape);

    while (g_running) {
        idle();
        display();

        glfwPollEvents();
        glfwSwapBuffers(g_window);

        glFinish();
    }

    uninitGL();
    glfwTerminate();
}
