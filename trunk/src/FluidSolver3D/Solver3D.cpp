#include "Solver3D.h"

namespace FluidSolver3D
{
	void Solver3D::GetLayer(Vec3D *v, double *T, int outdimx, int outdimy, int outdimz)
	{
		for (int i = 0; i < dimx; i++)
			for (int j = 0; j < dimy; j++)
				for (int k = 0; k < dimz; k++)
					if (grid->GetType(i, j, k) == NODE_OUT)
					{
						next->U->elem(i, j, k) = MISSING_VALUE;
						next->V->elem(i, j, k) = MISSING_VALUE;
						next->W->elem(i, j, k) = MISSING_VALUE;
						next->T->elem(i, j, k) = MISSING_VALUE;
					}

		next->FilterToArrays(v, T, outdimx, outdimy, outdimz);
	}

	void Solver3D::UpdateBoundaries()
	{
		cur->CopyFromGrid(grid, NODE_BOUND);
		cur->CopyFromGrid(grid, NODE_VALVE);
		cur->CopyLayerTo(grid, next, NODE_BOUND);
		cur->CopyLayerTo(grid, next, NODE_VALVE);
	}

	void Solver3D::SetGridBoundaries()
	{
		cur->CopyToGrid(grid);
	}

	void Solver3D::ClearOutterCells()
	{
		next->Clear(grid, NODE_OUT, 0.0, 0.0, 0.0, (FTYPE)grid->baseT);
	}
}