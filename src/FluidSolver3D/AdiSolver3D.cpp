#include "AdiSolver3D.h"
#include "..\Common\Algorithms.h"

namespace FluidSolver3D
{
	AdiSolver3D::AdiSolver3D()
	{
		grid = NULL;

		cur = NULL;
		temp = NULL;
		half1 = NULL;
		half2 = NULL;
		next = NULL;

		curT = NULL;
		tempT = NULL;

		a = d_a = NULL;
		b = d_b = NULL;
		c = d_c = NULL;
		d = d_d = NULL;
		x = d_x = NULL;

		h_listX = d_listX = NULL;
		h_listY = d_listY = NULL;
		h_listZ = d_listZ = NULL;
	}

	void AdiSolver3D::FreeMemory()
	{
		if (cur != NULL) delete cur;
		if (temp != NULL) delete temp;
		if (half1 != NULL) delete half1;
		if (half2 != NULL) delete half2;
		if (next != NULL) delete next;

		if (curT != NULL) delete curT;
		if (tempT != NULL) delete tempT;

		if (a != NULL) delete [] a;
		if (b != NULL) delete [] b;
		if (c != NULL) delete [] c;
		if (d != NULL) delete [] d;
		if (x != NULL) delete [] x;

		if (h_listX != NULL) delete [] h_listX;
		if (h_listY != NULL) delete [] h_listY;
		if (h_listZ != NULL) delete [] h_listZ;

		if (d_a != NULL) cudaFree(d_a);
		if (d_b != NULL) cudaFree(d_b);
		if (d_c != NULL) cudaFree(d_c);
		if (d_d != NULL) cudaFree(d_d);
		if (d_x != NULL) cudaFree(d_x);

		if (d_listX != NULL) cudaFree(d_listX);
		if (d_listY != NULL) cudaFree(d_listY);
		if (d_listZ != NULL) cudaFree(d_listZ);
	}

	AdiSolver3D::~AdiSolver3D()
	{
		prof.PrintTimings(csvFormat);

		FreeMemory();
	}

	void AdiSolver3D::SetOptionsGPU(bool _transposeOpt, bool _decomposeOpt)
	{
		transposeOpt = _transposeOpt;
		decomposeOpt = _decomposeOpt;
	}

	void AdiSolver3D::Init(BackendType _backend, bool _csv, Grid3D* _grid, FluidParams &_params)
	{
		grid = _grid;
		backend = _backend;
		csvFormat = _csv;

		dimx = grid->dimx;
		dimy = grid->dimy;
		dimz = grid->dimz;

		params = _params;

		int n = max(dimx, max(dimy, dimz));
		a = new FTYPE[n * n * n];
		b = new FTYPE[n * n * n];
		c = new FTYPE[n * n * n];
		d = new FTYPE[n * n * n];
		x = new FTYPE[n * n * n];

		h_listX = new Segment3D[grid->dimy * grid->dimz * MAX_SEGS_PER_ROW];
		h_listY = new Segment3D[grid->dimx * grid->dimz * MAX_SEGS_PER_ROW];
		h_listZ = new Segment3D[grid->dimx * grid->dimy * MAX_SEGS_PER_ROW];

		if( backend == GPU )
		{
			// create GPU matrices
			cudaMalloc( &d_a, sizeof(FTYPE) * n * n * n );
			cudaMalloc( &d_b, sizeof(FTYPE) * n * n * n );
			cudaMalloc( &d_c, sizeof(FTYPE) * n * n * n );
			cudaMalloc( &d_d, sizeof(FTYPE) * n * n * n );
			cudaMalloc( &d_x, sizeof(FTYPE) * n * n * n );
						
			cudaMalloc( &d_listX, sizeof(Segment3D) * grid->dimy * grid->dimz * MAX_SEGS_PER_ROW );
			cudaMalloc( &d_listY, sizeof(Segment3D) * grid->dimx * grid->dimz * MAX_SEGS_PER_ROW );
			cudaMalloc( &d_listZ, sizeof(Segment3D) * grid->dimx * grid->dimy * MAX_SEGS_PER_ROW );
		}
		else
		{
			transposeOpt = false;
			decomposeOpt = false;
		}

		cur = new TimeLayer3D(backend, grid);
		half1 = new TimeLayer3D(backend, grid->dimx, grid->dimy, grid->dimz, (FTYPE)grid->dx, (FTYPE)grid->dy, (FTYPE)grid->dz);
		half2 = new TimeLayer3D(backend, grid->dimx, grid->dimy, grid->dimz, (FTYPE)grid->dx, (FTYPE)grid->dy, (FTYPE)grid->dz);
		next = new TimeLayer3D(backend, grid->dimx, grid->dimy, grid->dimz, (FTYPE)grid->dx, (FTYPE)grid->dy, (FTYPE)grid->dz);
		temp = new TimeLayer3D(backend, grid->dimx, grid->dimy, grid->dimz, (FTYPE)grid->dx, (FTYPE)grid->dy, (FTYPE)grid->dz);

		curT = new TimeLayer3D(backend, grid->dimx, grid->dimz, grid->dimy, (FTYPE)grid->dx, (FTYPE)grid->dz, (FTYPE)grid->dy);
		tempT = new TimeLayer3D(backend, grid->dimx, grid->dimz, grid->dimy, (FTYPE)grid->dx, (FTYPE)grid->dz, (FTYPE)grid->dy);
	}

	void AdiSolver3D::TimeStep(FTYPE dt, int num_global, int num_local)
	{	
		CreateSegments();

		// setup non-linear layer
		prof.StartEvent();
		cur->CopyLayerTo(temp);
		prof.StopEvent("CopyLayer");

		// create transposed cur array if opt is enabled
		if (transposeOpt)
		{
			prof.StartEvent();
			cur->Transpose(curT);
			prof.StopEvent("Transpose");
		}

		// do global iterations
		for (int it = 0; it < num_global; it++)
		{
			// alternating directions
			SolveDirection(Z, dt, num_local, h_listZ, d_listZ, cur, temp, half1);		
			SolveDirection(Y, dt, num_local, h_listY, d_listY, half1, temp, half2);
			SolveDirection(X, dt, num_local, h_listX, d_listX, half2, temp, next);

			// update non-linear layer
			prof.StartEvent();
			next->MergeLayerTo(grid, temp, NODE_IN);
			prof.StopEvent("MergeLayer");
		}

		// compute error
		prof.StartEvent();
		double err = next->EvalDivError(grid);
		prof.StopEvent("EvalDivError");

		// check & output error
		if (err > ERR_THRESHOLD) {
			printf("\nError is too big!\n", err);
			exit(1);
		}
		else
			printf("\rerr = %.8f,", err);

		// clear cells for dynamic grid update
		prof.StartEvent();
		ClearOutterCells();
		prof.StopEvent("ClearLayer");

		// swap current/next pointers 
		TimeLayer3D *tmp = next;
		next = cur;
		cur = tmp;
	}

	template<DirType dir>
	void AdiSolver3D::CreateListSegments(int &numSeg, Segment3D *h_list, Segment3D *d_list, int dim1, int dim2, int dim3)
	{	
		numSeg = 0;
		for (int i = 0; i < dim2; i++)
			for (int j = 0; j < dim3; j++)
			{
				Segment3D seg, new_seg;
				int state = 0, incx, incy, incz;
				switch (dir)
				{
				case X:	
					seg.posx = 0; seg.posy = i; seg.posz = j; 
					incx = 1; incy = 0; incz = 0;
					break;
				case Y: 
					seg.posx = i; seg.posy = 0; seg.posz = j; 
					incx = 0; incy = 1; incz = 0;
					break;
				case Z: 
					seg.posx = i; seg.posy = j; seg.posz = 0; 
					incx = 0; incy = 0; incz = 1; 
					break;
				}
				seg.dir = (DirType)dir;

				while ((seg.posx + incx < dimx) && (seg.posy + incy < dimy) && (seg.posz + incz < dimz))
				{
					if (grid->GetType(seg.posx + incx, seg.posy + incy, seg.posz + incz) == NODE_IN)
					{
						if (state == 0) 
							new_seg = seg;
						state = 1;
					}
					else
					{
						if (state == 1)
						{
							new_seg.endx = seg.posx + incx;
							new_seg.endy = seg.posy + incy;
							new_seg.endz = seg.posz + incz;

							new_seg.size = (new_seg.endx - new_seg.posx) + (new_seg.endy - new_seg.posy) + (new_seg.endz - new_seg.posz) + 1;

							h_list[numSeg] = new_seg;
							numSeg++;
							state = 0;
						}
					}
					
					seg.posx += incx;
					seg.posy += incy;
					seg.posz += incz;
				}
			}

		if( backend == GPU )
		{
			// copy segments info to GPU as well
			cudaMemcpy( d_list, h_list, sizeof(Segment3D) * numSeg, cudaMemcpyHostToDevice );
		}
	}

	void AdiSolver3D::CreateSegments()
	{
		prof.StartEvent();

		CreateListSegments<X>(numSegs[X], h_listX, d_listX, dimx, dimy, dimz);
		CreateListSegments<Y>(numSegs[Y], h_listY, d_listY, dimy, dimx, dimz);
		CreateListSegments<Z>(numSegs[Z], h_listZ, d_listZ, dimz, dimx, dimy);

		prof.StopEvent("CreateSegments");
	}

	void AdiSolver3D::SolveDirection(DirType dir, FTYPE dt, int num_local, Segment3D *h_list, Segment3D *d_list, TimeLayer3D *cur, TimeLayer3D *temp, TimeLayer3D *next)
	{
		for (int it = 0; it < num_local; it++)
		{
			// transpose non-linear layer if opt is enabled
			if( transposeOpt )
			{
				prof.StartEvent();			
				temp->Transpose(tempT);
				prof.StopEvent("Transpose");
			}

			prof.StartEvent();

			switch( backend )
			{
			case CPU:
				#pragma omp parallel default(none) firstprivate(dt, dir) shared(h_list, cur, temp, next)
				{
					#pragma omp for
					for (int s = 0; s < numSegs[dir]; s++)
					{		
						SolveSegment(dt, s, h_list[s], type_U, dir, cur, temp, next);
						SolveSegment(dt, s, h_list[s], type_V, dir, cur, temp, next);
						SolveSegment(dt, s, h_list[s], type_W, dir, cur, temp, next);
						SolveSegment(dt, s, h_list[s], type_T, dir, cur, temp, next);			
					}
				}
				break;
			
			case GPU:
				DirType dir_new = dir;
				TimeLayer3D *cur_new = cur;
				TimeLayer3D *temp_new = temp;

				if( transposeOpt && dir == Z )
				{
					// set transposed direction
					dir_new = Z_as_Y;
					cur_new = curT;
					temp_new = tempT;
				}

				SolveSegments_GPU(dt, params, numSegs[dir], d_list, type_U, dir_new, grid->GetNodesGPU(), cur_new, temp_new, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt);	
				SolveSegments_GPU(dt, params, numSegs[dir], d_list, type_V, dir_new, grid->GetNodesGPU(), cur_new, temp_new, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt);
				SolveSegments_GPU(dt, params, numSegs[dir], d_list, type_W, dir_new, grid->GetNodesGPU(), cur_new, temp_new, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt);
				SolveSegments_GPU(dt, params, numSegs[dir], d_list, type_T, dir_new, grid->GetNodesGPU(), cur_new, temp_new, next, d_a, d_b, d_c, d_d, d_x, decomposeOpt);
				
				break;
			}

			switch( dir )
			{
			case X: prof.StopEvent("SolveSegments_X"); break;
			case Y: prof.StopEvent("SolveSegments_Y"); break;
			case Z: prof.StopEvent("SolveSegments_Z"); break;
			}

			// update non-linear layer
			prof.StartEvent();
			next->MergeLayerTo(grid, temp, NODE_IN);
			prof.StopEvent("MergeLayer");
		}
	}

	void AdiSolver3D::SolveSegment(FTYPE dt, int id, Segment3D seg, VarType var, DirType dir, TimeLayer3D *cur, TimeLayer3D *temp, TimeLayer3D *next)
	{
		int n = seg.size;

		int max_n = max(dimx, max(dimy, dimz));
		FTYPE *a = this->a + id * max_n;
		FTYPE *b = this->b + id * max_n;
		FTYPE *c = this->c + id * max_n;
		FTYPE *d = this->d + id * max_n;
		FTYPE *x = this->x + id * max_n;

		ApplyBC0(seg.posx, seg.posy, seg.posz, var, b[0], c[0], d[0]);
		ApplyBC1(seg.endx, seg.endy, seg.endz, var, a[n-1], b[n-1], d[n-1]);
		BuildMatrix(dt, seg.posx, seg.posy, seg.posz, var, dir, a, b, c, d, n, cur, temp);
		
		SolveTridiagonal(a, b, c, d, x, n);
		
		UpdateSegment(x, seg, var, next);
	}

	void AdiSolver3D::UpdateSegment(FTYPE *x, Segment3D seg, VarType var, TimeLayer3D *layer)
	{
		int i = seg.posx;
		int j = seg.posy;
		int k = seg.posz;

		for (int t = 0; t < seg.size; t++)
		{
			switch (var)
			{
			case type_U: layer->U->elem(i, j, k) = x[t]; break;
			case type_V: layer->V->elem(i, j, k) = x[t]; break;
			case type_W: layer->W->elem(i, j, k) = x[t]; break;
			case type_T: layer->T->elem(i, j, k) = x[t]; break;
			}

			switch (seg.dir)
			{
			case X: i++; break;
			case Y: j++; break;
			case Z: k++; break;
			}
		}
	}

	void AdiSolver3D::BuildMatrix(FTYPE dt, int i, int j, int k, VarType var, DirType dir, FTYPE *a, FTYPE *b, FTYPE *c, FTYPE *d, int n, TimeLayer3D *cur, TimeLayer3D *temp)
	{
		FTYPE vis_dx2, vis_dy2, vis_dz2;
		FTYPE dx = cur->dx;
		FTYPE dy = cur->dy;
		FTYPE dz = cur->dz;

		switch (var)
		{
		case type_U:
		case type_V:
		case type_W:
			vis_dx2 = params.v_vis / (dx * dx);
			vis_dy2 = params.v_vis / (dy * dy);
			vis_dz2 = params.v_vis / (dz * dz);
			break;
		case type_T:
			vis_dx2 = params.t_vis / (dx * dx);
			vis_dy2 = params.t_vis / (dy * dy);
			vis_dz2 = params.t_vis / (dz * dz);
			break;
		}
		
		for (int p = 1; p < n-1; p++)
		{
			switch (dir)
			{
			case X:		
				a[p] = - temp->U->elem(i+p, j, k) / (2 * dx) - vis_dx2; 
				b[p] = 3 / dt  +  2 * vis_dx2; 
				c[p] = temp->U->elem(i+p, j, k) / (2 * dx) - vis_dx2; 
				
				switch (var)	
				{
				case type_U: d[p] = cur->U->elem(i+p, j, k) * 3 / dt - params.v_T * temp->T->d_x(i+p, j, k); break;
				case type_V: d[p] = cur->V->elem(i+p, j, k) * 3 / dt; break;
				case type_W: d[p] = cur->W->elem(i+p, j, k) * 3 / dt; break;
				case type_T: d[p] = cur->T->elem(i+p, j, k) * 3 / dt + params.t_phi * temp->DissFuncX(i+p, j, k); break;
				}	
				break;

			case Y:
				a[p] = - temp->V->elem(i, j+p, k) / (2 * dy) - vis_dy2; 
				b[p] = 3 / dt  +  2 * vis_dy2; 
				c[p] = temp->V->elem(i, j+p, k) / (2 * dy) - vis_dy2; 
				
				switch (var)	
				{
				case type_U: d[p] = cur->U->elem(i, j+p, k) * 3 / dt; break;
				case type_V: d[p] = cur->V->elem(i, j+p, k) * 3 / dt - params.v_T * temp->T->d_y(i, j+p, k); break;
				case type_W: d[p] = cur->W->elem(i, j+p, k) * 3 / dt; break;
				case type_T: d[p] = cur->T->elem(i, j+p, k) * 3 / dt + params.t_phi * temp->DissFuncY(i, j+p, k); break;
				}
				break;

			case Z:
				a[p] = - temp->W->elem(i, j, k+p) / (2 * dz) - vis_dz2; 
				b[p] = 3 / dt  +  2 * vis_dz2; 
				c[p] = temp->W->elem(i, j, k+p) / (2 * dz) - vis_dz2; 
				
				switch (var)	
				{
				case type_U: d[p] = cur->U->elem(i, j, k+p) * 3 / dt; break;
				case type_V: d[p] = cur->V->elem(i, j, k+p) * 3 / dt; break;
				case type_W: d[p] = cur->W->elem(i, j, k+p) * 3 / dt - params.v_T * temp->T->d_z(i, j, k+p); break;
				case type_T: d[p] = cur->T->elem(i, j, k+p) * 3 / dt + params.t_phi * temp->DissFuncZ(i, j, k+p); break;
				}
				break;
			}
		}
	}

	void AdiSolver3D::ApplyBC0(int i, int j, int k, VarType var, FTYPE &b0, FTYPE &c0, FTYPE &d0)
	{
		if ((var == type_T && grid->GetBC_temp(i, j, k) == BC_FREE) ||
			(var != type_T && grid->GetBC_vel(i, j, k) == BC_FREE))
		{
			// free
			b0 = 1.0; 
			c0 = -1.0; 
			d0 = 0.0; 
		}
		else
		{
			// no-slip
			b0 = 1.0; 
			c0 = 0.0; 
			switch (var)
			{
			case type_U: d0 = (FTYPE)grid->GetVel(i, j, k).x; break;
			case type_V: d0 = (FTYPE)grid->GetVel(i, j, k).y; break;
			case type_W: d0 = (FTYPE)grid->GetVel(i, j, k).z; break;
			case type_T: d0 = (FTYPE)grid->GetT(i, j, k); break;
			}
		}
	}

	void AdiSolver3D::ApplyBC1(int i, int j, int k, VarType var, FTYPE &a1, FTYPE &b1, FTYPE &d1)
	{
		if ((var == type_T && grid->GetBC_temp(i, j, k) == BC_FREE) ||
			(var != type_T && grid->GetBC_vel(i, j, k) == BC_FREE))
		{
			// free
			a1 = 1.0; 
			b1 = -1.0; 
			d1 = 0.0;
		}
		else
		{
			// no-slip
			a1 = 0.0; 
			b1 = 1.0; 
			switch (var)
			{
			case type_U: d1 = (FTYPE)grid->GetVel(i, j, k).x; break;
			case type_V: d1 = (FTYPE)grid->GetVel(i, j, k).y; break;
			case type_W: d1 = (FTYPE)grid->GetVel(i, j, k).z; break;
			case type_T: d1 = (FTYPE)grid->GetT(i, j, k); break;
			}
		}
	}	
}