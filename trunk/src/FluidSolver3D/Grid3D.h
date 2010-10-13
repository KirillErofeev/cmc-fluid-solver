#pragma once

#include "..\Common\Geometry.h"
#include "..\Common\IO.h"

#include "..\FluidSolver2D\Grid2D.h"

#include <cuda_runtime.h>

using namespace Common;

namespace FluidSolver3D
{
	enum BackendType { CPU, GPU };

	enum NodeType { 
		NODE_IN, 
		NODE_OUT, 
		NODE_BOUND, 
		NODE_VALVE 
	};
	
	enum BCtype { 
		BC_NOSLIP, 
		BC_FREE 
	};

	struct Node
	{
		NodeType type;

		BCtype bc_vel, bc_temp;
		Vec3D v;
		FTYPE T;

		void SetBound(BCtype _bc_vel, BCtype _bc_temp, Vec3D _v, FTYPE _T)
		{
			type = NODE_BOUND;
			bc_vel = _bc_vel;
			bc_temp = _bc_temp;
			v = _v;
			T = _T;
		}
	};

	struct Grid3D
	{
		int dimx, dimy, dimz;
		double dx, dy, dz;

		double baseT;

		Grid3D(double _dx, double _dy, double _dz, double _depth, double _baseT, BackendType _backend);		// 2D with constant depth
		Grid3D(double _dx, double _dy, double _dz, double _baseT, BackendType _backend);					// polygons
		~Grid3D();

		NodeType GetType(int i, int j, int k);
		BCtype GetBC_vel(int i, int j, int k);
		BCtype GetBC_temp(int i, int j, int k);
		Vec3D GetVel(int i, int j, int k);
		FTYPE GetT(int i, int j, int k);

		// return all nodes info as an array
		Node *GetNodesCPU();							 
		Node *GetNodesGPU(bool transposed = false);		

		void SetNodeVel(int i, int j, int k, Vec3D new_v);

		double GetFrameTime();
		FluidSolver2D::Grid2D *GetGrid2D();
		
		bool LoadFromFile(char *filename, bool align = false);
		void Prepare(double time);

		void TestPrint(char *filename);

	protected:
		BackendType backend;

		Node*		nodes;		// all grid nodes
		Node*		d_nodes;	// same nodes stored on GPU
		Node*		d_nodesT;	// transposed nodes on GPU

		bool use3Dshape;

		BBox3D bbox;
		FrameInfo3D* frames;
		int num_frames;

		void Init(bool align);

		// helper function for 2D shape update
		void Prepare2D(double time);

		FluidSolver2D::Grid2D *grid2D;		// 2D helper grid for borders
		double depth;						// depth
		int active_dimz;					// dimz before align
	};
}