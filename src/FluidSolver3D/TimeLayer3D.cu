/*
 *  Copyright 2010-2011 Nikolai Sakharnykh
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

// GPU implementation of TimeLayer3D

#include "Grid3D.h"

#define COPY_BLOCK_DIM_X		32
#define COPY_BLOCK_DIM_Y		8

#define TRANSPOSE_SMEM_TILE_DIM			16
#define TRANSPOSE_SMEM_BLOCK_ROWS		16

#define TRANSPOSE_CACHE_TILE_DIM		32
#define TRANSPOSE_CACHE_BLOCK_ROWS		8

using namespace FluidSolver3D;

__global__ void copy_grid(int dimx, int dimy, int dimz, FTYPE *u, FTYPE *v, FTYPE *w, FTYPE *T, Node *nodes, NodeType target)
{
	int k = blockIdx.x * blockDim.x + threadIdx.x;
	int j = blockIdx.y * blockDim.y + threadIdx.y;

	if( k >= dimz || j >= dimy ) return;

	for (int i = 0; i < dimx; i++)
	{
		int id = i * dimy * dimz + j * dimz + k;
		if( nodes[id].type == target )
		{
			u[id] = nodes[id].v.x;
			v[id] = nodes[id].v.y;
			w[id] = nodes[id].v.z;
			T[id] = nodes[id].T;
		}
	}
}

__global__ void clear(int dimx, int dimy, int dimz, FTYPE *u, FTYPE *v, FTYPE *w, FTYPE *T, Node *nodes, NodeType target, FTYPE const_u, FTYPE const_v, FTYPE const_w, FTYPE const_T)
{
	int k = blockIdx.x * blockDim.x + threadIdx.x;
	int j = blockIdx.y * blockDim.y + threadIdx.y;

	if( k >= dimz || j >= dimy ) return;

	for (int i = 0; i < dimx; i++)
	{
		int id = i * dimy * dimz + j * dimz + k;
		if( nodes[id].type == target )
		{
			u[id] = const_u;
			v[id] = const_v;
			w[id] = const_w;
			T[id] = const_T;
		}
	}
}

__global__ void copy(int dimx, int dimy, int dimz, FTYPE *src, FTYPE *dest, Node *nodes, NodeType target)
{
	int k = blockIdx.x * blockDim.x + threadIdx.x;
	int j = blockIdx.y * blockDim.y + threadIdx.y;

	if( k >= dimz || j >= dimy ) return;

	for (int i = 0; i < dimx; i++)
	{
		int id = i * dimy * dimz + j * dimz + k;
		if( nodes[id].type == target )
			dest[id] = src[id];
	}
}

__global__ void merge(int dimx, int dimy, int dimz, FTYPE *src, FTYPE *dest, Node *nodes, NodeType target)
{
	int k = blockIdx.x * blockDim.x + threadIdx.x;
	int j = blockIdx.y * blockDim.y + threadIdx.y;

	if( k >= dimz || j >= dimy ) return;

	for (int i = 0; i < dimx; i++)
	{
		int id = i * dimy * dimz + j * dimz + k;
		if( nodes[id].type == target )
			dest[id] = ( dest[id] + src[id] ) / 2;
	}
}

__global__ void transpose_shared(int dimx, int dimy, int dimz, FTYPE *src, FTYPE *dest)
{
	__shared__ FTYPE tile[TRANSPOSE_SMEM_TILE_DIM][TRANSPOSE_SMEM_TILE_DIM+1];

    // read the tile from global memory into shared memory
	int k0 = blockIdx.x * TRANSPOSE_SMEM_TILE_DIM + threadIdx.x;
	int j0 = blockIdx.y * TRANSPOSE_SMEM_TILE_DIM + threadIdx.y;

	int j1 = blockIdx.y * TRANSPOSE_SMEM_TILE_DIM + threadIdx.x;
    int k1 = blockIdx.x * TRANSPOSE_SMEM_TILE_DIM + threadIdx.y;

	int base_u0 = k0 + j0 * dimz;	
    int base_u1 = j1 + k1 * dimy;
	
	for (int i = 0; i < dimx; i++)
	{	
		// read tile from global to shared memory
		for (int row = 0; row < TRANSPOSE_SMEM_TILE_DIM; row += TRANSPOSE_SMEM_BLOCK_ROWS)
			tile[threadIdx.y + row][threadIdx.x] = src[base_u0 + row * dimz];
		base_u0 += dimz * dimy;
	
		__syncthreads();
	
		// write the transposed tile to global memory 
		for (int row = 0; row < TRANSPOSE_SMEM_TILE_DIM; row += TRANSPOSE_SMEM_BLOCK_ROWS)
			dest[base_u1 + row * dimy] = tile[threadIdx.x][threadIdx.y + row];
		base_u1 += dimz * dimy;
	}
}

__global__ void transpose_cache(int dimx, int dimy, int dimz, FTYPE *src, FTYPE *dest)
{
	int i = blockIdx.y / ((dimy + TRANSPOSE_CACHE_TILE_DIM - 1)/TRANSPOSE_CACHE_TILE_DIM);
	int blkY = blockIdx.y % ((dimy + TRANSPOSE_CACHE_TILE_DIM - 1)/TRANSPOSE_CACHE_TILE_DIM);

	int j1 = blkY * TRANSPOSE_CACHE_TILE_DIM + threadIdx.x;
    int k1 = blockIdx.x * TRANSPOSE_CACHE_TILE_DIM + threadIdx.y;

	int base_src = blockIdx.x * TRANSPOSE_CACHE_TILE_DIM + blkY * TRANSPOSE_CACHE_TILE_DIM * dimz;
	int base_dst = j1 + k1 * dimy;
	
	base_src += dimz * dimy * i;
	base_dst += dimz * dimy * i;

	// load directly from global memory filling up L1
	for (int row = 0; row < TRANSPOSE_CACHE_TILE_DIM; row += TRANSPOSE_CACHE_BLOCK_ROWS)
		dest[base_dst + row * dimy] = src[base_src + threadIdx.x * dimz + threadIdx.y + row];
}

void CopyFromGrid_GPU(int dimx, int dimy, int dimz, FTYPE **u, FTYPE **v, FTYPE **w, FTYPE **T, Node **nodes, NodeType target, int haloSize = 0)
{
	dim3 block(COPY_BLOCK_DIM_X, COPY_BLOCK_DIM_Y);
	dim3 grid((dimz + block.x - 1)/block.x, (dimy + block.y - 1)/block.y);
	GPUplan *pGPUplan = GPUplan::Instance();
	for (int i = 0; i < pGPUplan->size(); i++)
	{	
		cudaSetDevice(i);
		copy_grid<<<grid, block>>>(pGPUplan->node(i)->getLength1D(), dimy, dimz, u[i] + haloSize, v[i] + haloSize, w[i] + haloSize, T[i] + haloSize, nodes[i], target);
	}
	cudaDeviceSynchronize();
}

void CopyFieldTo_GPU(int dimx, int dimy, int dimz, FTYPE **src, FTYPE **dest, Node **nodes, NodeType target, int haloSize = 0)
{
	dim3 block(COPY_BLOCK_DIM_X, COPY_BLOCK_DIM_Y);
	dim3 grid((dimz + block.x - 1)/block.x, (dimy + block.y - 1)/block.y);
	GPUplan *pGPUplan = GPUplan::Instance();
	for (int i = 0; i < pGPUplan->size(); i++)
	{	
		cudaSetDevice(i);
		copy<<<grid, block>>>(pGPUplan->node(i)->getLength1D(), dimy, dimz, src[i] + haloSize, dest[i] + haloSize, nodes[i], target);
	}
	cudaDeviceSynchronize();
}

void MergeFieldTo_GPU(int dimx, int dimy, int dimz, FTYPE **src, FTYPE **dest, Node **nodes, NodeType target,  int haloSize = 0)
{
	dim3 block(COPY_BLOCK_DIM_X, COPY_BLOCK_DIM_Y);
	dim3 grid((dimz + block.x - 1)/block.x, (dimy + block.y - 1)/block.y);
	GPUplan *pGPUplan = GPUplan::Instance();
	for (int i = 0; i < pGPUplan->size(); i++)
	{	
		cudaSetDevice(i);
		merge<<<grid, block>>>(pGPUplan->node(i)->getLength1D(), dimy, dimz, src[i] + haloSize, dest[i] + haloSize, nodes[i], target);
	}
	cudaDeviceSynchronize();
}

void Clear_GPU(int dimx, int dimy, int dimz, FTYPE **u, FTYPE **v, FTYPE **w, FTYPE **T, Node **nodes, NodeType target, FTYPE const_u, FTYPE const_v, FTYPE const_w, FTYPE const_T, int haloSize = 0)
{
	dim3 block(COPY_BLOCK_DIM_X, COPY_BLOCK_DIM_Y);
	dim3 grid((dimz + block.x - 1)/block.x, (dimy + block.y - 1)/block.y);
	GPUplan *pGPUplan = GPUplan::Instance();
	for (int i = 0; i < pGPUplan->size(); i++)
	{	
		cudaSetDevice(i);
		clear<<<grid, block>>>(pGPUplan->node(i)->getLength1D(), dimy, dimz, u[i] + haloSize, v[i] + haloSize, w[i] + haloSize, T[i] + haloSize, nodes[i], target, const_u, const_v, const_w, const_T);
	}
	cudaDeviceSynchronize();
}

void Transpose_GPU_shared(int dimx, int dimy, int dimz, FTYPE **u, FTYPE **dest_u, int haloSize = 0)
{
	dim3 block(TRANSPOSE_SMEM_TILE_DIM, TRANSPOSE_SMEM_BLOCK_ROWS);
	dim3 grid((dimz + TRANSPOSE_SMEM_TILE_DIM - 1)/TRANSPOSE_SMEM_TILE_DIM, (dimy + TRANSPOSE_SMEM_TILE_DIM - 1)/TRANSPOSE_SMEM_TILE_DIM);
	GPUplan *pGPUplan = GPUplan::Instance();
	for (int i = 0; i < pGPUplan->size(); i++)
	{	
		cudaSetDevice(i);
		cudaFuncSetCacheConfig(transpose_shared, cudaFuncCachePreferL1);
		transpose_shared<<<grid, block>>>(pGPUplan->node(i)->getLength1D(), dimy, dimz, u[i] + haloSize, dest_u[i] + haloSize);
	}
	cudaDeviceSynchronize();
}

void Transpose_GPU_cache(int dimx, int dimy, int dimz, FTYPE **u, FTYPE **dest_u, int haloSize = 0)
{
	dim3 block(TRANSPOSE_CACHE_TILE_DIM, TRANSPOSE_CACHE_BLOCK_ROWS);	
	GPUplan *pGPUplan = GPUplan::Instance();
	for (int i = 0; i < pGPUplan->size(); i++)
	{	
		cudaSetDevice(i);
		dim3 grid((dimz + TRANSPOSE_CACHE_TILE_DIM - 1)/TRANSPOSE_CACHE_TILE_DIM, pGPUplan->node(i)->getLength1D() * ((dimy + TRANSPOSE_CACHE_TILE_DIM - 1)/TRANSPOSE_CACHE_TILE_DIM));
		cudaFuncSetCacheConfig(transpose_cache, cudaFuncCachePreferL1);
		transpose_cache<<<grid, block>>>(pGPUplan->node(i)->getLength1D(), dimy, dimz, u[i] + haloSize, dest_u[i] + haloSize);
	}
	cudaDeviceSynchronize();
}

void Transpose_GPU(int dimx, int dimy, int dimz, FTYPE **u, FTYPE **dest_u, int haloSize = 0)
{
#if 1
	Transpose_GPU_shared(dimx, dimy, dimz, u, dest_u, haloSize);
#else
	Transpose_GPU_cache(dimx, dimy, dimz, u, dest_u, haloSize);
#endif
}
