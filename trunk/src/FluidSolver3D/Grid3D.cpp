#include "Grid3D.h"

namespace FluidSolver3D
{
	Grid3D::Grid3D(double _dx, double _dy, double _dz, double _depth, double _baseT) : 
		dx(_dx), dy(_dy), dz(_dz), depth(_depth), baseT(_baseT), nodes(NULL)
	{
		grid2D = new FluidSolver2D::Grid2D(dx, dy, baseT, true, 0.0);
	}

	Grid3D::~Grid3D()
	{
		if (nodes != NULL) delete [] nodes;
		if (grid2D != NULL) delete grid2D;
	}

	NodeType Grid3D::GetType(int i, int j, int k)
	{
		return nodes[i * dimy * dimz + j * dimz + k].type;
	}

	BCtype Grid3D::GetBC_vel(int i, int j, int k)
	{
		return nodes[i * dimy * dimz + j * dimz + k].bc_vel;
	}

	BCtype Grid3D::GetBC_temp(int i, int j, int k)
	{
		return nodes[i * dimy * dimz + j * dimz + k].bc_temp;
	}

	Vec3D Grid3D::GetVel(int i, int j, int k)
	{
		return nodes[i * dimy * dimz + j * dimz + k].v;
	}

	double Grid3D::GetT(int i, int j, int k)
	{
		return nodes[i * dimy * dimz + j * dimz + k].T;
	}

	double Grid3D::GetFrameTime()
	{
		int frames = grid2D->GetFramesNum();
		double length = grid2D->GetCycleLenght();
		return (length / frames);
	}

	FluidSolver2D::Grid2D *Grid3D::GetGrid2D()
	{
		return grid2D;
	}

	void Grid3D::SetNodeVel(int i, int j, int k, Vec3D new_v)
	{
		nodes[i * dimy * dimz + j * dimz + k].v = new_v;
	}

	bool Grid3D::LoadFromFile(char *filename)
	{
		if (grid2D->LoadFromFile(filename, ""))
		{
			dimx = grid2D->dimx;
			dimy = grid2D->dimy;
			dimz = (int)ceil(depth / dz) + 1;
			nodes = new Node[dimx * dimy * dimz];
			return true;
		}
		else
			return false;
	}

	void Grid3D::Prepare(double time)
	{
		grid2D->Prepare(time);

		memset(nodes, 0, sizeof(Node) * dimx * dimy * dimz);
		for (int i = 0; i < dimx; i++)
			for (int j = 0; j < dimy; j++)
			{
				int ind;
				if (grid2D->GetType(i, j) == FluidSolver2D::CELL_OUT)
				{
					for (int k = 0; k < dimz; k++)
					{
						ind = i * dimy * dimz + j * dimz + k;
						nodes[ind].type = NODE_OUT;
					}
				}
				else
				{
					// set up & bottom bounds
					nodes[i * dimy * dimz + j * dimz + 0].type = NODE_OUT;
					nodes[i * dimy * dimz + j * dimz + dimz-1].type = NODE_OUT;

					nodes[i * dimy * dimz + j * dimz + 1].SetBound(BC_NOSLIP, BC_FREE, Vec3D(0.0, 0.0, 0.0), baseT);
					nodes[i * dimy * dimz + j * dimz + dimz-2].SetBound(BC_NOSLIP, BC_FREE, Vec3D(0.0, 0.0, 0.0), baseT);
					
					for (int k = 2; k < dimz-2; k++)
					{
						ind = i * dimy * dimz + j * dimz + k;
						switch (grid2D->GetType(i, j))
						{
						case FluidSolver2D::CELL_BOUND:
							nodes[ind].SetBound(BC_NOSLIP, BC_FREE, Vec3D(grid2D->GetData(i, j).vel.x, grid2D->GetData(i, j).vel.y, 0.0), grid2D->GetData(i, j).T);
							break;
						case FluidSolver2D::CELL_VALVE:
							if( grid2D->GetData(i, j).vel.x == 0 && grid2D->GetData(i, j).vel.y == 0 )
								nodes[ind].SetBound(BC_FREE, BC_FREE, Vec3D(grid2D->GetData(i, j).vel.x, grid2D->GetData(i, j).vel.y, 0.0), grid2D->GetData(i, j).T);
							else
								nodes[ind].SetBound(BC_NOSLIP, BC_NOSLIP, Vec3D(grid2D->GetData(i, j).vel.x, grid2D->GetData(i, j).vel.y, 0.0), grid2D->GetData(i, j).T);
							nodes[ind].type = NODE_VALVE;
							break;
						case FluidSolver2D::CELL_IN:
							nodes[ind].type = NODE_IN;
							nodes[ind].T = baseT;
							break;
						}
					}
				}
			}
	}

	void Grid3D::TestPrint(char *filename)
	{
		FILE *file = NULL;
		fopen_s(&file, filename, "w");
		fprintf(file, "grid (z-slices):\n");
		fprintf(file, "%i %i %i\n", dimx, dimy, dimz);
		for (int i = 0; i < dimx; i++)
		{
			for (int j = 0; j < dimy; j++)
			{
				for (int k = 0; k < dimz; k++)
				{
					NodeType t = GetType(i, j, k);
					switch (t)
					{
					case NODE_IN: fprintf(file, " "); break;
					case NODE_OUT: fprintf(file, "."); break;
					case NODE_BOUND: fprintf(file, "#"); break;		
					case NODE_VALVE: fprintf(file, "+"); break;		
					}
				}
				fprintf(file, "\n");
			}
			fprintf(file, "\n");
		}
		fclose(file);
	}
}