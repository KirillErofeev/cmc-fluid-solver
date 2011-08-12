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

#include "AdiSolver3D.h"

#if( __CUDA_ARCH__ < 120 )
#define SOLVER_BLOCK_DIM	128
#else
#define SOLVER_BLOCK_DIM	256
#endif

#define SEG_BLOCK_DIM_X		32
#define SEG_BLOCK_DIM_Y		8

#include "../Common/test_util.h"

namespace FluidSolver3D
{
	struct FluidParamsGPU
	{
		FTYPE vis_dx2;
		FTYPE dt, dx, dy, dz;
		FTYPE v_T, t_phi;

		FluidParamsGPU( VarType var, DirType dir, FTYPE _dt, FTYPE _dx, FTYPE _dy, FTYPE _dz, FluidParams _params ) : 
			dt(_dt), dx(_dx), dy(_dy), dz(_dz)
		{
			switch (var)
			{
			case type_U:
			case type_V:
			case type_W:	
				switch (dir)
				{
				case X:	vis_dx2 = _params.v_vis / (dx * dx); break;
				case Y: vis_dx2 = _params.v_vis / (dy * dy); break;
				case Z: case Z_as_Y: vis_dx2 = _params.v_vis / (dz * dz); break;
				}
				break;
			case type_T:
				switch (dir)
				{
				case X:	vis_dx2 = _params.t_vis / (dx * dx); break;
				case Y: vis_dx2 = _params.t_vis / (dy * dy); break;
				case Z: case Z_as_Y:  vis_dx2 = _params.t_vis / (dz * dz); break;
				}
				break;
			}
						
			v_T = _params.v_T;
			t_phi = _params.t_phi;
		}
	};

#if 1
	// interleave matrix arrays for better memory access
	#define get(a, elem_id)			a[id + (elem_id + 1) * max_n_max_n * MAX_SEGS_PER_ROW]
#else
	// sequential layout - bad access pattern,  currently not implemented for MGPU
//	#define get(a, elem_id)			a[elem_id + id * max_n]
#endif

	template<int dir, int var>
	__device__ void apply_bc0(int i, int j, int k, int dimy, int dimz, FTYPE &b0, FTYPE &c0, FTYPE &d0, Node *nodes, SegmentType segType = BOUND)
	{
		switch (dir)
		{
		case X:
			switch (segType)
			{
			case UNBOUND:
				return;
			case BOUND_END:
				return;
			};
		}

		//b0 = c0 = d0 = 1.;
	//	return;

		int id = i * dimy * dimz;
		if (dir != Z_as_Y) id += j * dimz + k;
			else id += j * dimy + k;

		if ((var == type_T && nodes[id].bc_temp == BC_FREE) ||
			(var != type_T && nodes[id].bc_vel == BC_FREE))
		{
			// free: f(0) = 2 * f(1) - f(2)
			b0 = 2.0; 
			c0 = -1.0; 
			d0 = 0.0; 
		}
		else
		{
			// no-slip: f(0) = f(1)
			b0 = 1.0; 
			c0 = 0.0; 
			switch (var)
			{
			case type_U: d0 = nodes[id].v.x; break;
			case type_V: d0 = nodes[id].v.y; break;
			case type_W: d0 = nodes[id].v.z; break;
			case type_T: d0 = nodes[id].T; break;
			}
		}
	}

	template<int dir, int var>
	__device__ void apply_bc1(int i, int j, int k, int dimy, int dimz, FTYPE &a1, FTYPE &b1, FTYPE &d1, Node *nodes, SegmentType segType = BOUND)
	{
		switch (dir)
		{
		case X:
			switch (segType)
			{
			case UNBOUND:
				return;
			case BOUND_START:
				return;
			}
		}

		//a1 = b1 = d1 = 2.;
		//return;

		int id = i * dimy * dimz;
		if (dir != Z_as_Y) id += j * dimz + k;
			else id += j * dimy + k;

		if ((var == type_T && nodes[id].bc_temp == BC_FREE) ||
			(var != type_T && nodes[id].bc_vel == BC_FREE))
		{
			// free: f(N) = 2 * f(N-1) - f(N-2)
			a1 = -1.0; 
			b1 = 2.0; 
			d1 = 0.0;
		}
		else
		{
			// no-slip: f(N) = f(N-1)
			a1 = 0.0; 
			b1 = 1.0; 
			switch (var)
			{
			case type_U: d1 = nodes[id].v.x; break;
			case type_V: d1 = nodes[id].v.y; break;
			case type_W: d1 = nodes[id].v.z; break;
			case type_T: d1 = nodes[id].T; break;
			}
		}
	}	

	template<int dir, int var>
	__device__ void build_matrix(FluidParamsGPU params, int i, int j, int k, FTYPE *a, FTYPE *b, FTYPE *c, FTYPE *d, int n, TimeLayer3D_GPU &cur, TimeLayer3D_GPU &temp, int id, int num_seg, int max_n_max_n, int dimX, SegmentType segType = BOUND)
	{	
		int ps;
		int start = 1;
		int end = n-1;
		switch (dir)
		{
		case X:
			switch (segType)
			{
			case UNBOUND:
				start = 0; end = n;				
				break;
			case BOUND_START:
				end = n;
				break;
			case BOUND_END:
				start = 0;
				break;
			}
		}

		for (int p = start; p < end; p++)
		{
			switch (dir)
			{
			case X:		
				ps = dimX - n + p; // allign segments to the end
				get(a,ps) = - temp.elem(temp.u, i+p, j, k) / (2 * params.dx) - params.vis_dx2; 
				get(b,ps) = 3 / params.dt  +  2 * params.vis_dx2; 
				get(c,ps) = temp.elem(temp.u, i+p, j, k) / (2 * params.dx) - params.vis_dx2; 
				
				switch (var)	
				{
				case type_U: get(d,ps) = cur.elem(cur.u, i+p, j, k) * 3 / params.dt - params.v_T * temp.d_x(temp.T, params.dx, i+p, j, k); break;
				case type_V: get(d,ps) = cur.elem(cur.v, i+p, j, k) * 3 / params.dt; break;
				case type_W: get(d,ps) = cur.elem(cur.w, i+p, j, k) * 3 / params.dt; break;
				case type_T: get(d,ps) = cur.elem(cur.T, i+p, j, k) * 3 / params.dt + params.t_phi * temp.DissFuncX(params.dx, params.dy, params.dz, i+p, j, k); break;
				}	
				break;

			case Y:
				get(a,p) = - temp.elem(temp.v, i, j+p, k) / (2 * params.dy) - params.vis_dx2; 
				get(b,p) = 3 / params.dt  +  2 * params.vis_dx2; 
				get(c,p) = temp.elem(temp.v, i, j+p, k) / (2 * params.dy) - params.vis_dx2; 
				
				switch (var)	
				{
				case type_U: get(d,p) = cur.elem(cur.u, i, j+p, k) * 3 / params.dt; break;
				case type_V: get(d,p) = cur.elem(cur.v, i, j+p, k) * 3 / params.dt - params.v_T * temp.d_y(temp.T, params.dy, i, j+p, k); break;
				case type_W: get(d,p) = cur.elem(cur.w, i, j+p, k) * 3 / params.dt; break;
				case type_T: get(d,p) = cur.elem(cur.T, i, j+p, k) * 3 / params.dt + params.t_phi * temp.DissFuncY(params.dx, params.dy, params.dz, i, j+p, k); break;
				}
				break;

			case Z:
				get(a,p) = - temp.elem(temp.w, i, j, k+p) / (2 * params.dz) - params.vis_dx2; 
				get(b,p) = 3 / params.dt  +  2 * params.vis_dx2; 
				get(c,p) = temp.elem(temp.w, i, j, k+p) / (2 * params.dz) - params.vis_dx2; 
				
				switch (var)	
				{
				case type_U: get(d,p) = cur.elem(cur.u, i, j, k+p) * 3 / params.dt; break;
				case type_V: get(d,p) = cur.elem(cur.v, i, j, k+p) * 3 / params.dt; break;
				case type_W: get(d,p) = cur.elem(cur.w, i, j, k+p) * 3 / params.dt - params.v_T * temp.d_z(temp.T, params.dz, i, j, k+p); break;
				case type_T: get(d,p) = cur.elem(cur.T, i, j, k+p) * 3 / params.dt + params.t_phi * temp.DissFuncZ(params.dx, params.dy, params.dz, i, j, k+p); break;
				}
				break;

			case Z_as_Y:
				get(a,p) = - temp.elem(temp.w, i, k+p, j) / (2 * params.dz) - params.vis_dx2; 
				get(b,p) = 3 / params.dt  +  2 * params.vis_dx2; 
				get(c,p) = temp.elem(temp.w, i, k+p, j) / (2 * params.dz) - params.vis_dx2; 
				
				switch (var)	
				{
				case type_U: get(d,p) = cur.elem(cur.u, i, k+p, j) * 3 / params.dt; break;
				case type_V: get(d,p) = cur.elem(cur.v, i, k+p, j) * 3 / params.dt; break;
				case type_W: get(d,p) = cur.elem(cur.w, i, k+p, j) * 3 / params.dt - params.v_T * temp.d_y(temp.T, params.dz, i, k+p, j); break;
				case type_T: get(d,p) = cur.elem(cur.T, i, k+p, j) * 3 / params.dt + params.t_phi * temp.DissFuncZ_as_Y(params.dx, params.dz, params.dy, i, k+p, j); break;
				}
				break;
			}
		}
	}

template<int dir, int swipe>
	__device__ void solve_tridiagonal(FTYPE *a, FTYPE *b, FTYPE *c, FTYPE *d, FTYPE *x, int num, int id, int num_seg, int max_n_max_n, int dimX, SegmentType segType = BOUND)
	{
		switch (swipe)
		{
			case ALL:
			case FORWARD:
			{
				int start = 1;

				switch (dir)
				{
				case X:
						switch (segType)
						{
						case UNBOUND:
							start = 0;
							get(c,dimX-num-1) = get(c,-1);
							get(d,dimX-num-1) = get(d,-1);
							break;
						case BOUND_END:
							start = 0;
							get(c,dimX-num-1) = get(c,-1);
							get(d,dimX-num-1) = get(d,-1);
							get(c, dimX-1) = 0.0;
							break;
						case BOUND:
							get(c,dimX-1) = 0.0;
							get(c,dimX-num) = get(c,dimX-num) / get(b,dimX-num);
							get(d,dimX-num) = get(d,dimX-num) / get(b,dimX-num);
							break;
						case BOUND_START:
							get(c,dimX-num) = get(c,dimX-num) / get(b,dimX-num);
							get(d,dimX-num) = get(d,dimX-num) / get(b,dimX-num);
							break;
						}
						break;
				default:
					get(c,0) = get(c,0) / get(b,0);
					get(d,0) = get(d,0) / get(b,0);
					get(c,num-1) = 0.0;
					break;
				}

				int is;
				for (int i = start; i < num; i++)
				{
					switch (dir)
					{
					case X:
						is = dimX - num + i;
						break;
					default:
						is = i;
						break;
					}
					get(c,is) = get(c,is) / (get(b,is) - get(a,is) * get(c,is-1));
					get(d,is) = (get(d,is) - get(d,is-1) * get(a,is)) / (get(b,is) - get(a,is) * get(c,is-1));
					/*
					switch (segType)
					{
						case BOUND_START:
							if ( i ==  num - 1)
							{
								printf("BOUND_START: id = %d    get(d, %d) = %f\n",id ,is, get(d, is));
							}
							break;
						case BOUND_END:
							if ( i == start)
								printf("BOUND_END: id = %d    get(d, %d) = %f\n",id ,is, get(d, is));
							break;
					}
					/**/
				}
				break;
			}			
		}

		switch (swipe)
		{
			case ALL:
			case BACK:
			{
				int end = num - 1;

				switch (dir)
				{
				case X:
					switch (segType)
					{
					case UNBOUND:
						end = num;
						get(x,num) = get(x,dimX);
						break;
					case BOUND_START:
						end = num;
						get(x,num) = get(x,dimX);
						break;
					case BOUND_END:
						get(x, num-1) = get(d,dimX-1);
						break;
					case BOUND:
						get(x, num-1) = get(d,dimX-1);
						break;
					}
					break;
				default:
					get(x, num-1) = get(d,num-1);
					break;
				}

				int is;
				for (int i = end-1; i >= 0; i--) 
				{
					switch (dir)
					{
					case X:
						is = dimX - num + i;
						break;
					default:
						is = i;
					}
					get(x,i) = get(d,is) -  get(c,is) * get(x, i+1);
					/*
					switch (segType)
					{
						case BOUND_START:
							if ( i ==  end - 1)
							{
								printf("BOUND_START: id = %d    get(x, %d+1) = %f\n",id ,i, get(x, i+1));
							}
							break;
						case BOUND_END:
							if ( i == 0)
								printf("BOUND_END: id = %d    get(x, %d) = %f\n",id ,i, get(x, i));
							break;
					}
					/**/
				}
				break;
			}
		}
	}

	template<int dir, int var>
	__device__ void update_segment( FTYPE *x, Segment3D &seg, TimeLayer3D_GPU &layer, int id, int num_seg, int max_n_max_n )
	{
		int i = seg.posx;
		int j = seg.posy;
		int k = seg.posz;
		
		if( dir == Z_as_Y ) 
		{
			k = seg.posy;
			j = seg.posz;
		}

		for (int t = 0; t < seg.size; t++)
		{
			switch (var)
			{
			case type_U: layer.elem(layer.u, i, j, k) = get(x,t); break;
			case type_V: layer.elem(layer.v, i, j, k) = get(x,t); break;
			case type_W: layer.elem(layer.w, i, j, k) = get(x,t); break;
			case type_T: layer.elem(layer.T, i, j, k) = get(x,t); break;
			}

			switch (dir)
			{
			case X: i++; break;
			case Y: j++; break;
			case Z: k++; break;
			case Z_as_Y: j++; break;
			}
		}
	}

	template<int dir, int var, int swipe>
	__global__ void solve_segments( FluidParamsGPU p, int num_seg, Segment3D *segs, Node* nodes, TimeLayer3D_GPU cur, TimeLayer3D_GPU temp, TimeLayer3D_GPU next,
									FTYPE *a, FTYPE *b, FTYPE *c, FTYPE *d, FTYPE *x, int  max_n_max_n, int dimX, int id_shift = 0 )
	{
		// fetch current segment info
		int id = id_shift + blockIdx.x * blockDim.x + threadIdx.x;
		if( id >= num_seg + id_shift) return;
		Segment3D &seg = segs[id];

		int n = seg.size;

		int start, end;
		switch (dir)
		{
		case X:
			if (seg.skipX)
				return;
			start = dimX - n;
			end = dimX - 1;
			break;
		default:
			start = 0;
			end = n-1;
			break;
		}

		switch (swipe)
		{
		case ALL:
		case FORWARD:
			apply_bc0<dir, var>(seg.posx, seg.posy, seg.posz, cur.dimy, cur.dimz, get(b,start), get(c,start), get(d,start), nodes, seg.type);
			apply_bc1<dir, var>(seg.endx, seg.endy, seg.endz, cur.dimy, cur.dimz, get(a,end), get(b,end), get(d,end), nodes, seg.type);
		
			build_matrix<dir, var>(p, seg.posx, seg.posy, seg.posz, a, b, c, d, n, cur, temp, id, num_seg, max_n_max_n, dimX, seg.type);

		case BACK:			
			solve_tridiagonal<dir, swipe>(a, b, c, d, x, n, id, num_seg, max_n_max_n, dimX, seg.type);
			break;
		}
			
		switch (swipe)
		{
		case ALL:
		case BACK:
			update_segment<dir, var>(x, seg, next, id, num_seg, max_n_max_n);
			break;
		}
	}

	template<int dir, int var>
	__global__ void build_matrix( FluidParamsGPU p, int num_seg, Segment3D *segs, Node* nodes, TimeLayer3D_GPU cur, TimeLayer3D_GPU temp, 
								  FTYPE *a, FTYPE *b, FTYPE *c, FTYPE *d, int max_n_max_n, int dimX, int id_shift = 0 )
	{
		// fetch current segment info
		int id = id_shift + blockIdx.x * blockDim.x + threadIdx.x;
		if( id >= num_seg + id_shift) return;
		Segment3D &seg = segs[id];

		int n = seg.size;

		int start, end;
		switch (dir)
		{
		case X:
			if (seg.skipX)
				return;
			start = dimX - n;
			end = dimX - 1;
			break;
		default:
			start = 0;
			end = n-1;
		}

		apply_bc0<dir, var>(seg.posx, seg.posy, seg.posz, cur.dimy, cur.dimz, get(b,start), get(c,start), get(d,start), nodes, seg.type);
		apply_bc1<dir, var>(seg.endx, seg.endy, seg.endz, cur.dimy, cur.dimz, get(a,end), get(b,end), get(d,end), nodes, seg.type);
		
		build_matrix<dir, var>(p, seg.posx, seg.posy, seg.posz, a, b, c, d, n, cur, temp, id, num_seg, max_n_max_n, dimX);	
	}

	template<int dir, int var, int swipe>
	__global__ void solve_matrix( int num_seg, Segment3D *segs, TimeLayer3D_GPU next,
								  FTYPE *a, FTYPE *b, FTYPE *c, FTYPE *d, FTYPE *x, int max_n_max_n, int dimX, int id_shift = 0 )
	{
		// fetch current segment info
		int id = id_shift + blockIdx.x * blockDim.x + threadIdx.x;
		if( id >= num_seg + id_shift) return;
		Segment3D &seg = segs[id];

		switch (dir)
		{
		case X:
			if (seg.skipX)
				return;
		}

		int n = seg.size;

		solve_tridiagonal<dir, swipe>(a, b, c, d, x, n, id, num_seg, max_n_max_n, seg.type);
		
		switch (swipe)
		{
		case BACK:
			update_segment<dir, var>(x, seg, next, id, num_seg, max_n_max_n);
			break;
		}
	}

	template<DirType dir, VarType var>
	void LaunchSolveSegments_dir_var( FluidParamsGPU p, int *num_seg, Segment3D **segs, Node **nodes, TimeLayer3D_GPU &cur, TimeLayer3D_GPU &temp, TimeLayer3D_GPU &next,
									  FTYPE **d_a, FTYPE **d_b, FTYPE **d_c, FTYPE **d_d, FTYPE **d_x, bool decomposeOpt )
	/*
		Y and Z direction only (and X if nGPUs = 1)
	*/
	{
		GPUplan *pGPUplan = GPUplan::Instance();

		int max_n_max_n;
		int max_n = max( max( cur.dimx, cur.dimy ), cur.dimz );
		dim3 block(SOLVER_BLOCK_DIM);

		for (int i = 0; i < pGPUplan->size(); i++)
		{
			cudaSetDevice(i);
			cur.SetDevice(i); temp.SetDevice(i); next.SetDevice(i);

			int dimX = pGPUplan->node(i)->getLength1D();
			dim3 grid((num_seg[i] + block.x - 1)/block.x);

			max_n_max_n = pGPUplan->node(i)->getLength1D() * max_n;  // valid for Y and Z direction only if nGPUs > 1

			switch( decomposeOpt )
			{
			case true:
				build_matrix<dir, var><<<grid, block>>>( p, num_seg[i], segs[i], nodes[i], cur, temp, d_a[i], d_b[i], d_c[i], d_d[i], max_n_max_n, dimX );
				break;

			case false:
				//cudaFuncSetCacheConfig(solve_segments<dir, var, ALL>, cudaFuncCachePreferL1);
				solve_segments<dir, var, ALL><<<grid, block>>>( p, num_seg[i], segs[i], nodes[i], cur, temp, next, d_a[i], d_b[i], d_c[i], d_d[i], d_x[i], max_n_max_n, dimX );
				break;
			}
		}
		cudaDeviceSynchronize();

		if ( decomposeOpt )
		{
			for (int i = 0; i < pGPUplan->size(); i++)
			{
				cudaSetDevice(i);
				cur.SetDevice(i); temp.SetDevice(i); next.SetDevice(i);

				int dimX = pGPUplan->node(i)->getLength1D();
				dim3 grid((num_seg[i] + block.x - 1)/block.x);

				max_n_max_n = pGPUplan->node(i)->getLength1D() * max_n;

				solve_matrix<dir, var, ALL><<<grid, block>>>( num_seg[i], segs[i], next, d_a[i], d_b[i], d_c[i], d_d[i], d_x[i], max_n_max_n, dimX );
			}
			cudaDeviceSynchronize();
		}
		//printf("dir %d: next dd_T after forward and back calculation: %f\n", TestUtil::sumEllementsMultiGPU(next.dd_T, next.dimx * next.dimy * next.dimz, next.haloSize));
		//TestUtil::printEllementsMultiGPU<FTYPE>(next.dd_T, next.dimy, next.dimz, next.dimy*next.dimz, true);
		//fflush(stdout);
	}

	template<VarType var>
	void LaunchSolveSegments_X_var( FluidParamsGPU p, int *num_seg, Segment3D **segs, Node **nodes, TimeLayer3D_GPU &cur, TimeLayer3D_GPU &temp, TimeLayer3D_GPU &next,
									  FTYPE **d_a, FTYPE **d_b, FTYPE **d_c, FTYPE **d_d, FTYPE **d_x, bool decomposeOpt )
	/*
		X direction only
	*/
	{
		GPUplan *pGPUplan = GPUplan::Instance();
		PARAplan* pplan = PARAplan::Instance();
		int irank =  pplan->rank();
		int size = pplan->size();
		
		if (pGPUplan->size() == 1 && pplan->size() == 1)
		{
			LaunchSolveSegments_dir_var<X, var>( p, num_seg, segs, nodes, cur, temp, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt );
			return;
		}
		/**/

		int max_n_max_n;
		int max_n = max( max( cur.dimx, cur.dimy ), cur.dimz );

		int haloSize = max_n * max_n * MAX_SEGS_PER_ROW;
		int dimX;

		dim3 block(SOLVER_BLOCK_DIM);

		FTYPE *mpi_buf_1 = new FTYPE[haloSize];
		FTYPE *mpi_buf_2 = new FTYPE[haloSize];

		//***************************************
		//cur.SetDevice(0); temp.SetDevice(0); next.SetDevice(0);
		//int dimX = pGPUplan->node(0).getLength1D();
		//dim3 grid((num_seg[0] + block.x - 1)/block.x);

		//max_n_max_n = max_n * max_n;
		//solve_segments<X, var, FORWARD><<<grid, block>>>( p, num_seg[0], segs[0], nodes[0], cur, temp, next, d_a[0], d_b[0], d_c[0], d_d[0], d_x[0], max_n_max_n, dimX );
		////cudaDeviceSynchronize();
		//solve_segments<X, var, BACK><<<grid, block>>>( p, num_seg[0], segs[0], nodes[0], cur, temp, next, d_a[0], d_b[0], d_c[0], d_d[0], d_x[0], max_n_max_n, dimX );
		//cudaDeviceSynchronize();
		//return;
		//***************************************
		cudaSetDevice(0);
		paraDevRecv<FTYPE, FORWARD>(d_c[0], mpi_buf_1, haloSize, 666);
		paraDevRecv<FTYPE, FORWARD>(d_d[0], mpi_buf_2, haloSize, 667);
		for (int i = 0; i < pGPUplan->size(); i++)
		{
			cudaSetDevice(i);
			cur.SetDevice(i); temp.SetDevice(i); next.SetDevice(i);

			dimX = pGPUplan->node(i)->getLength1D();
			dim3 grid((num_seg[i] + block.x - 1)/block.x);

			max_n_max_n = max_n * max_n;

			//cudaFuncSetCacheConfig(solve_segments<dir, var, ALL>, cudaFuncCachePreferL1);
			solve_segments<X, var, FORWARD><<<grid, block>>>( p, num_seg[i], segs[i], nodes[i], cur, temp, next, d_a[i], d_b[i], d_c[i], d_d[i], d_x[i], max_n_max_n, dimX );
			if (i < pGPUplan->size() - 1) //  send to node n+1
			{
				haloMemcpyPeer<FTYPE, FORWARD>( d_c, i, haloSize, dimX * haloSize );
				haloMemcpyPeer<FTYPE, FORWARD>( d_d, i, haloSize, dimX * haloSize );
			}
		}
		//cudaDeviceSynchronize();
		cudaSetDevice(pGPUplan->size()-1);
		paraDevSend<FTYPE, FORWARD>(d_c[pGPUplan->size()-1] + haloSize + dimX * haloSize - haloSize, mpi_buf_1, haloSize, 666);
		paraDevSend<FTYPE, FORWARD>(d_d[pGPUplan->size()-1] + haloSize + dimX * haloSize - haloSize, mpi_buf_2, haloSize, 667);

		paraDevRecv<FTYPE, BACK>(d_x[pGPUplan->size()-1] + haloSize +  dimX * haloSize, mpi_buf_1, haloSize, 668);
		for (int i = pGPUplan->size() - 1; i >= 0; i--)
		{
			cudaSetDevice(i);
			cur.SetDevice(i); temp.SetDevice(i); next.SetDevice(i);

			dimX = pGPUplan->node(i)->getLength1D();
			dim3 grid((num_seg[i] + block.x - 1)/block.x);

			max_n_max_n = max_n * max_n;
			solve_segments<X, var, BACK><<<grid, block>>>( p, num_seg[i], segs[i], nodes[i], cur, temp, next, d_a[i], d_b[i], d_c[i], d_d[i], d_x[i], max_n_max_n, dimX );
			if (i > 0)
				haloMemcpyPeer<FTYPE, BACK>(d_x, i, haloSize, pGPUplan->node(i-1)->getLength1D()*haloSize);
		}
		cudaDeviceSynchronize();
		cudaSetDevice(0);
		paraDevSend<FTYPE, BACK>(d_x[0] + haloSize, mpi_buf_1, haloSize, 668);		

		delete [] mpi_buf_1;
		delete [] mpi_buf_2;
	}

	template<DirType dir>
	void LaunchSolveSegments_dir( FluidParamsGPU p, int *num_seg, Segment3D **segs, VarType var, Node **nodes, TimeLayer3D_GPU &cur, TimeLayer3D_GPU &temp, TimeLayer3D_GPU &next,
								  FTYPE **d_a, FTYPE **d_b, FTYPE **d_c, FTYPE **d_d, FTYPE **d_x, bool decomposeOpt )
	{
		switch( var )
		{
		case type_U: LaunchSolveSegments_dir_var<dir, type_U>( p, num_seg, segs, nodes, cur, temp, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		case type_V: LaunchSolveSegments_dir_var<dir, type_V>( p, num_seg, segs, nodes, cur, temp, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		case type_W: LaunchSolveSegments_dir_var<dir, type_W>( p, num_seg, segs, nodes, cur, temp, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		case type_T: LaunchSolveSegments_dir_var<dir, type_T>( p, num_seg, segs, nodes, cur, temp, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		}
	}

	void LaunchSolveSegments_X( FluidParamsGPU p, int *num_seg, Segment3D **segs, VarType var, Node **nodes, TimeLayer3D_GPU &cur, TimeLayer3D_GPU &temp, TimeLayer3D_GPU &next,
								  FTYPE **d_a, FTYPE **d_b, FTYPE **d_c, FTYPE **d_d, FTYPE **d_x, bool decomposeOpt )
	/*
		Will change later to solve variables in parallel on mGPU (more memory)
	*/
	{
		switch( var )
		{
		case type_U: LaunchSolveSegments_X_var<type_U>( p, num_seg, segs, nodes, cur, temp, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		case type_V: LaunchSolveSegments_X_var<type_V>( p, num_seg, segs, nodes, cur, temp, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		case type_W: LaunchSolveSegments_X_var<type_W>( p, num_seg, segs, nodes, cur, temp, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		case type_T: LaunchSolveSegments_X_var<type_T>( p, num_seg, segs, nodes, cur, temp, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		}
	}

	void SolveSegments_GPU( FTYPE dt, FluidParams params, int *num_seg, Segment3D **segs, VarType var, DirType dir, Node **nodes, TimeLayer3D *cur, TimeLayer3D *temp, TimeLayer3D *next,
							FTYPE **d_a, FTYPE **d_b, FTYPE **d_c, FTYPE **d_d, FTYPE **d_x, bool decomposeOpt )
	{
		TimeLayer3D_GPU d_cur( cur );
		TimeLayer3D_GPU d_temp( temp );
		TimeLayer3D_GPU d_next( next );

		FluidParamsGPU p( var, dir, dt, cur->dx, cur->dy, cur->dz, params );

		switch( dir )
		{
		case X: LaunchSolveSegments_X( p, num_seg, segs, var, nodes, d_cur, d_temp, d_next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		case Y: LaunchSolveSegments_dir<Y>( p, num_seg, segs, var, nodes, d_cur, d_temp, d_next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		case Z: LaunchSolveSegments_dir<Z>( p, num_seg, segs, var, nodes, d_cur, d_temp, d_next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		case Z_as_Y: LaunchSolveSegments_dir<Z_as_Y>( p, num_seg, segs, var, nodes, d_cur, d_temp, d_next, d_a, d_b, d_c, d_d, d_x, decomposeOpt ); break;
		}
	}
}
