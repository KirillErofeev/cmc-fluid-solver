#include "StableSolver2D.h"

namespace FluidSolver
{
	void StableSolver2D::Init(Grid2D* _grid, FluidParams &_params)
	{
		grid = _grid;

		dimx = grid->dimx;
		dimy = grid->dimy;

		params = _params;

		cur = new TimeLayer2D(grid->dimx, grid->dimy, grid->dx, grid->dy);
		next = new TimeLayer2D(grid->dimx, grid->dimy, grid->dx, grid->dy);
		
		temp = new TimeLayer2D(grid->dimx, grid->dimy, grid->dx, grid->dy);
		next_w = new TimeLayer2D(grid->dimx, grid->dimy, grid->dx, grid->dy);
		q[0] = new ScalarField2D(grid->dimx, grid->dimy, grid->dx, grid->dy);
		q[1] = new ScalarField2D(grid->dimx, grid->dimy, grid->dx, grid->dy);
		div = new ScalarField2D(grid->dimx, grid->dimy, grid->dx, grid->dy);

		for (int i = 0; i < dimx; i++)
			for (int j = 0; j < dimy; j++)
				switch(grid->GetType(i, j))
				{
				case IN:
				case BOUND:
				case VALVE:
				case OUT:
					cur->U(i, j) = grid->GetData(i, j).vel.x;
					cur->V(i, j) = grid->GetData(i, j).vel.y;
					cur->T(i, j) = grid->GetData(i, j).T;
					break;
				}

		cur->CopyAllto(grid, next);
		cur->CopyAllto(grid, temp);
	}

	void StableSolver2D::SolveU(double dt, TimeLayer2D *cur, TimeLayer2D *temp, TimeLayer2D *next)
	{
		// eval new layer
		for (int i = 0; i < dimx; i++)
			for (int j = 0; j < dimy; j++)
				if (grid->GetType(i, j) == IN)
				{
					next->U(i, j) = cur->U(i, j) + dt * ( 
						- temp->U(i, j) * temp->Ux(i, j) 
						- temp->V(i, j) * temp->Uy(i, j) 
						+ params.v_vis * (temp->Uxx(i, j) + temp->Uyy(i, j)) );
				}
	}

	void StableSolver2D::SolveV(double dt, TimeLayer2D *cur, TimeLayer2D *temp, TimeLayer2D *next)
	{
		// eval new layer
		for (int i = 0; i < dimx; i++)
			for (int j = 0; j < dimy; j++)
				if (grid->GetType(i, j) == IN)
				{
					next->V(i, j) = cur->V(i, j) + dt * ( 
						- temp->U(i, j) * temp->Vx(i, j) 
						- temp->V(i, j) * temp->Vy(i, j) 
						+ params.v_vis * (temp->Vxx(i, j) + temp->Vyy(i, j)) );
				}
	}

	void StableSolver2D::Project(double dt, TimeLayer2D *w, TimeLayer2D *proj)
	{
		// eval divergence field first (optimization)
		#pragma omp parallel default(none) firstprivate(w)
		{
			#pragma omp for
			for (int i = 0; i < dimx; i++)
				for (int j = 0; j < dimy; j++)
				{
					if (grid->GetType(i, j) == IN)
					{
						div->U(i, j) = w->Ux(i, j) + w->Vy(i, j);
					}
				}
		}

		// need to solve poisson equation : 
		//		laplas(q) = q_xx + q_yy = div(w)
		// simple discetization:
		//		(q[i+1,j] - 2q*[i,j] + q[i-1,j])/dx2 + (q[i,j+1] - 2q*[i,j] + q[i,j-1])/dy2 = div
		
		double dx2 = grid->dx * grid->dx;
		double dy2 = grid->dy * grid->dy;
		double rcp_dxdy2 = 0.5 / (dx2 + dy2);

		double err;
		int prev = 0, cur = 1;	// previous/current
		q[prev]->ClearZero();
		q[cur]->ClearZero();

		do
		{
			#pragma omp parallel default(none) firstprivate(dx2, dy2, cur, prev, w, rcp_dxdy2)
			{
				#pragma omp for
				for (int i = 0; i < dimx; i++)
					for (int j = 0; j < dimy; j++)
					{
						if (grid->GetType(i, j) == IN)
						{
							double f = w->Ux(i, j) + w->Vy(i, j);
							q[cur]->U(i, j) = rcp_dxdy2 * ((q[cur]->U(i-1, j) + q[prev]->U(i+1, j)) * dy2 + (q[prev]->U(i, j+1) + q[cur]->U(i, j-1)) * dx2 - div->U(i, j) * dx2 * dy2);
						}
					}
			}
				// check precision
				err = 0.0;
				for (int i = 0; i < dimx; i++)
					for (int j = 0; j < dimy; j++)
						if (grid->GetType(i, j) == IN)
						{
							err += abs(q[cur]->Uxx(i, j) + q[cur]->Uyy(i, j) - div->U(i, j));
						}
				//printf("err = %.4f\n", thread_err);

			int tmp = prev; prev = cur; cur = tmp;
		} while (err >= POISSON_ERR_THRESHOLD);	// exit only when the error is small enough
		cur = prev;
		
		// force divergence free field: 
		//		proj = w - grad(q)

		for (int i = 0; i < dimx; i++)
			for (int j = 0; j < dimy; j++)
				if (grid->GetType(i, j) == IN)
				{
					Vec2D g = q[cur]->grad(i, j);
					proj->U(i, j) = w->U(i, j) - g.x;
					proj->V(i, j) = w->V(i, j) - g.y;
				}
	}

	void StableSolver2D::TimeStep(double dt, int num_global, int num_local)
	{
		cur->CopyAllto(grid, temp);

		// do global iterations
		int it;
		double err = next->EvalDivError(grid);

		for (it = 0; (it < num_global) || (err > DIV_ERR_THRESHOLD); it++)
		{
			cur->CopyAllto(grid, next_w);

			// solve equations
			SolveU(dt, cur, temp, next_w);
			SolveV(dt, cur, temp, next_w);
			Project(dt, next_w, next);

			err = next->EvalDivError(grid);
			
			// update non-linear parameters
			next->MergeAllto(grid, temp, IN);
			
			if (it > MAX_GLOBAL_ITERS) 
			{
				printf("\nExceeded max number of iterations (%i)\n", MAX_GLOBAL_ITERS); 
				exit(1); 
			}

			if (err > DIV_ERR_THRESHOLD * 10)
			{
				printf("\nError is too big!\n", err);
				exit(1);
			}
		}

		ClearOutterCells();

		// output number of global iterations & error
		printf("\r%i,%.4f,", it, err);

		// copy result to current layer
		next->CopyAllto(grid, cur);
	}

	void StableSolver2D::FreeMemory()
	{
		if (cur != NULL) delete cur;
		if (next != NULL) delete next;
		
		if (temp != NULL) delete temp;
		if (next_w != NULL) delete next_w;
		if (q[0] != NULL) delete q[0];
		if (q[1] != NULL) delete q[1];
		if (div != NULL) delete div;
	}

	StableSolver2D::StableSolver2D()
	{
		grid = NULL;

		cur = NULL;
		next = NULL;
		
		temp = NULL;
		next_w = NULL;
		q[0] = NULL;
		q[1] = NULL;
		div = NULL;
	}

	StableSolver2D::~StableSolver2D()
	{
		FreeMemory();
	}
}