/*
 * Copyright 1993-2015 NVIDIA Corporation.  All rights reserved.
 *
 * Please refer to the NVIDIA end user license agreement (EULA) associated
 * with this source code for terms and conditions that govern your use of
 * this software. Any use, reproduction, disclosure, or distribution of
 * this software and related documentation outside the terms of the EULA
 * is strictly prohibited.
 *
 */

#include <helper_cuda.h>

#include <assert.h>
#include <math.h>
#include <memory.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>

#include <cuda_gl_interop.h>

template <typename T>
void integrateNbodySystem(DeviceData<T> *deviceData,
                          cudaGraphicsResource **pgres, unsigned int currentRead,
                          float deltaTime, float damping,
                          unsigned int numBodies, unsigned int numDevices,
                          int blockSize, bool bUsePBO);

cudaError_t setSofteningSquared(float softeningSq);
cudaError_t setSofteningSquared(double softeningSq);

template<typename T>
BodySystemCUDA<T>::BodySystemCUDA(unsigned int numBodies,
                                  unsigned int numDevices,
                                  unsigned int blockSize,
                                  bool usePBO,
                                  bool useSysMem,
                                  bool useP2P,
                                  int deviceId)
    : m_numBodies(numBodies),
      m_numDevices(numDevices),
      m_bInitialized(false),
      m_bUsePBO(usePBO),
      m_bUseSysMem(useSysMem),
      m_bUseP2P(useP2P),
      m_currentRead(0),
      m_currentWrite(1),
      m_blockSize(blockSize),
      m_devID(deviceId)
{
    m_hPos[0] = m_hPos[1] = 0;
    m_hVel = 0;

    m_deviceData = 0;

    _initialize(numBodies);
    setSoftening(0.00125f);
    setDamping(0.995f);
}

template<typename T>
BodySystemCUDA<T>::~BodySystemCUDA()
{
    _finalize();
    m_numBodies = 0;
}

template<typename T>
void BodySystemCUDA<T>::_initialize(int numBodies)
{
    assert(!m_bInitialized);

    m_numBodies = numBodies;

    unsigned int memSize = sizeof(T) * 4 * numBodies;

    m_deviceData = new DeviceData<T>[m_numDevices];

    // divide up the workload amongst Devices
    float *weights = new float[m_numDevices];
    int *numSms = new int[m_numDevices];
    float total = 0;

    for (unsigned int i = 0; i < m_numDevices; i++)
    {
        cudaDeviceProp props;
        checkCudaErrors(cudaGetDeviceProperties(&props, i));

        // Choose the weight based on the Compute Capability
        // We estimate that a CC2.0 SM is about 4.0x faster than a CC 1.x SM for
        // this application (since a 15-SM GF100 is about 2X faster than a 30-SM GT200).
        numSms[i] = props.multiProcessorCount;
        weights[i] = numSms[i] * (props.major >= 2 ? 4.f : 1.f);
        total += weights[i];

    }

    unsigned int offset = 0;
    unsigned int remaining = m_numBodies;

    for (unsigned int i = 0; i < m_numDevices; i++)
    {
        unsigned int count = (int)((weights[i] / total) * m_numBodies);
        // Rounding up to numSms[i]*256 leads to better GPU utilization _per_ GPU
        // but when using multiple devices, it will lead to the last GPUs not having any work at all
        // which means worse overall performance
        // unsigned int round = numSms[i] * 256;
        unsigned int round = 256;

        count = round * ((count + round - 1) / round);
        if (count > remaining)
        {
            count = remaining;
        }

        remaining -= count;
        m_deviceData[i].offset = offset;
        m_deviceData[i].numBodies = count;
        offset += count;

        if ((i == m_numDevices - 1) && (offset < m_numBodies-1))
        {
            m_deviceData[i].numBodies += m_numBodies - offset;
        }
    }

    delete [] weights;
    delete [] numSms;

    if (m_bUseSysMem)
    {
        checkCudaErrors(cudaHostAlloc((void **)&m_hPos[0], memSize, cudaHostAllocMapped | cudaHostAllocPortable));
        checkCudaErrors(cudaHostAlloc((void **)&m_hPos[1], memSize, cudaHostAllocMapped | cudaHostAllocPortable));
        checkCudaErrors(cudaHostAlloc((void **)&m_hVel,    memSize, cudaHostAllocMapped | cudaHostAllocPortable));

        memset(m_hPos[0], 0, memSize);
        memset(m_hPos[1], 0, memSize);
        memset(m_hVel, 0, memSize);

        for (unsigned int i = 0; i < m_numDevices; i++)
        {
            if (m_numDevices > 1)
            {
                checkCudaErrors(cudaSetDevice(i));
            }

            checkCudaErrors(cudaEventCreate(&m_deviceData[i].event));
            checkCudaErrors(cudaHostGetDevicePointer((void **)&m_deviceData[i].dPos[0], (void *)m_hPos[0], 0));
            checkCudaErrors(cudaHostGetDevicePointer((void **)&m_deviceData[i].dPos[1], (void *)m_hPos[1], 0));
            checkCudaErrors(cudaHostGetDevicePointer((void **)&m_deviceData[i].dVel, (void *)m_hVel, 0));
        }
    }
    else
    {
        m_hPos[0] = new T[m_numBodies*4];
        m_hVel = new T[m_numBodies*4];

        memset(m_hPos[0], 0, memSize);
        memset(m_hVel, 0, memSize);

        checkCudaErrors(cudaSetDevice(m_devID));
        checkCudaErrors(cudaEventCreate(&m_deviceData[0].event));

        if (m_bUsePBO)
        {
            // create the position pixel buffer objects for rendering
            // we will actually compute directly from this memory in CUDA too
            glGenBuffers(2, (GLuint *)m_pbo);

            for (int i = 0; i < 2; ++i)
            {
                glBindBuffer(GL_ARRAY_BUFFER, m_pbo[i]);
                glBufferData(GL_ARRAY_BUFFER, memSize, m_hPos[0], GL_DYNAMIC_DRAW);

                int size = 0;
                glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, (GLint *)&size);

                if ((unsigned)size != memSize)
                {
                    fprintf(stderr, "WARNING: Pixel Buffer Object allocation failed!n");
                }

                glBindBuffer(GL_ARRAY_BUFFER, 0);
                checkCudaErrors(cudaGraphicsGLRegisterBuffer(&m_pGRes[i],
                                                             m_pbo[i],
                                                             cudaGraphicsMapFlagsNone));
            }
        }
        else
        {
            checkCudaErrors(cudaMalloc((void **)&m_deviceData[0].dPos[0], memSize));
            checkCudaErrors(cudaMalloc((void **)&m_deviceData[0].dPos[1], memSize));
        }

        checkCudaErrors(cudaMalloc((void **)&m_deviceData[0].dVel, memSize));

        // At this point we already know P2P is supported
        if (m_bUseP2P)
        {
            for (unsigned int i = 1; i < m_numDevices; i++) {
                int access = 0;
                cudaError_t error;

                // Enable access for gpu_i to memory owned by gpu0
                checkCudaErrors(cudaSetDevice(i));
                if ((error = cudaDeviceEnablePeerAccess(0, 0)) != cudaErrorPeerAccessAlreadyEnabled)
                {
                    checkCudaErrors(error);
                }
                else
                {
                    // We might have already enabled P2P, so catch this and reset error code...
                    cudaGetLastError();
                }

                checkCudaErrors(cudaEventCreate(&m_deviceData[i].event));

                // Point all GPUs to the memory allocated on gpu0
                m_deviceData[i].dPos[0] = m_deviceData[0].dPos[0];
                m_deviceData[i].dPos[1] = m_deviceData[0].dPos[1];
                m_deviceData[i].dVel = m_deviceData[0].dVel;
            }
        }
 
    }

    m_bInitialized = true;
}

template<typename T>
void BodySystemCUDA<T>::_finalize()
{
    assert(m_bInitialized);

    if (m_bUseSysMem)
    {
        checkCudaErrors(cudaFreeHost(m_hPos[0]));
        checkCudaErrors(cudaFreeHost(m_hPos[1]));
        checkCudaErrors(cudaFreeHost(m_hVel));

        for (unsigned int i = 0; i < m_numDevices; i++)
        {
            cudaEventDestroy(m_deviceData[i].event);
        }
    }
    else
    {
        delete [] m_hPos[0];
        delete [] m_hPos[1];
        delete [] m_hVel;

        checkCudaErrors(cudaFree((void **)m_deviceData[0].dVel));

        if (m_bUsePBO)
        {
            checkCudaErrors(cudaGraphicsUnregisterResource(m_pGRes[0]));
            checkCudaErrors(cudaGraphicsUnregisterResource(m_pGRes[1]));
            glDeleteBuffers(2, (const GLuint *)m_pbo);
        }
        else
        {
            checkCudaErrors(cudaFree((void **)m_deviceData[0].dPos[0]));
            checkCudaErrors(cudaFree((void **)m_deviceData[0].dPos[1]));

            checkCudaErrors(cudaEventDestroy(m_deviceData[0].event));

            if (m_bUseP2P)
            {
                for (unsigned int i = 1; i < m_numDevices; i++)
                {
                    checkCudaErrors(cudaEventDestroy(m_deviceData[i].event));
                }
            }
        }
    }

    delete [] m_deviceData;

    m_bInitialized = false;
}

template <typename T>
void BodySystemCUDA<T>::loadTipsyFile(const std::string &filename)
{
    if (m_bInitialized)
        _finalize();

    std::vector< typename vec4<T>::Type > positions;
    std::vector< typename vec4<T>::Type > velocities;
    std::vector< int > ids;

    int nBodies = 0;
    int nFirst=0, nSecond=0, nThird=0;

    read_tipsy_file(positions,
                    velocities,
                    ids,
                    filename,
                    nBodies,
                    nFirst,
                    nSecond,
                    nThird);

    _initialize(nBodies);

    setArray(BODYSYSTEM_POSITION, (T *)&positions[0]);
    setArray(BODYSYSTEM_VELOCITY, (T *)&velocities[0]);
}


template<typename T>
void BodySystemCUDA<T>::setSoftening(T softening)
{
    T softeningSq = softening*softening;

    for (unsigned int i = 0; i < m_numDevices; i++)
    {
        if (m_numDevices > 1)
        {
            checkCudaErrors(cudaSetDevice(i));
        }

        checkCudaErrors(setSofteningSquared(softeningSq));
    }
}

template<typename T>
void BodySystemCUDA<T>::setDamping(T damping)
{
    m_damping = damping;
}

template<typename T>
void BodySystemCUDA<T>::update(T deltaTime)
{
    assert(m_bInitialized);

    integrateNbodySystem<T>(m_deviceData, m_pGRes, m_currentRead,
                            (float)deltaTime, (float)m_damping,
                            m_numBodies, m_numDevices,
                            m_blockSize, m_bUsePBO);

    std::swap(m_currentRead, m_currentWrite);
}

template<typename T>
T *BodySystemCUDA<T>::getArray(BodyArray array)
{
    assert(m_bInitialized);

    T *hdata = 0;
    T *ddata = 0;

    cudaGraphicsResource *pgres = NULL;

    int currentReadHost = m_bUseSysMem ? m_currentRead : 0;

    switch (array)
    {
        default:
        case BODYSYSTEM_POSITION:
            hdata = m_hPos[currentReadHost];
            ddata = m_deviceData[0].dPos[m_currentRead];

            if (m_bUsePBO)
            {
                pgres = m_pGRes[m_currentRead];
            }

            break;

        case BODYSYSTEM_VELOCITY:
            hdata = m_hVel;
            ddata = m_deviceData[0].dVel;
            break;
    }

    if (!m_bUseSysMem)
    {
        if (pgres)
        {
            checkCudaErrors(cudaGraphicsResourceSetMapFlags(pgres, cudaGraphicsMapFlagsReadOnly));
            checkCudaErrors(cudaGraphicsMapResources(1, &pgres, 0));
            size_t bytes;
            checkCudaErrors(cudaGraphicsResourceGetMappedPointer((void **)&ddata, &bytes, pgres));
        }

        checkCudaErrors(cudaMemcpy(hdata, ddata,
                                   m_numBodies*4*sizeof(T), cudaMemcpyDeviceToHost));

        if (pgres)
        {
            checkCudaErrors(cudaGraphicsUnmapResources(1, &pgres, 0));
        }
    }

    return hdata;
}

template<typename T>
void BodySystemCUDA<T>::setArray(BodyArray array, const T *data)
{
    assert(m_bInitialized);

    m_currentRead = 0;
    m_currentWrite = 1;

    switch (array)
    {
        default:
        case BODYSYSTEM_POSITION:
            {
                if (m_bUsePBO)
                {
                    glBindBuffer(GL_ARRAY_BUFFER, m_pbo[m_currentRead]);
                    glBufferSubData(GL_ARRAY_BUFFER, 0, 4 * sizeof(T) * m_numBodies, data);

                    int size = 0;
                    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, (GLint *)&size);

                    if ((unsigned)size != 4 * (sizeof(T) * m_numBodies))
                    {
                        fprintf(stderr, "WARNING: Pixel Buffer Object download failed!n");
                    }

                    glBindBuffer(GL_ARRAY_BUFFER, 0);
                }
                else
                {
                    if (m_bUseSysMem)
                    {
                        memcpy(m_hPos[m_currentRead], data, m_numBodies * 4 * sizeof(T));
                    }
                    else
                        checkCudaErrors(cudaMemcpy(m_deviceData[0].dPos[m_currentRead], data,
                                                   m_numBodies * 4 * sizeof(T),
                                                   cudaMemcpyHostToDevice));
                }
            }
            break;

        case BODYSYSTEM_VELOCITY:
            if (m_bUseSysMem)
            {
                memcpy(m_hVel, data, m_numBodies * 4 * sizeof(T));
            }
            else
                checkCudaErrors(cudaMemcpy(m_deviceData[0].dVel, data, m_numBodies * 4 * sizeof(T),
                                           cudaMemcpyHostToDevice));

            break;
    }
}
