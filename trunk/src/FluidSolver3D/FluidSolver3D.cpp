#include "FluidSolver3D.h"

using namespace FluidSolver3D;
using namespace Common;

void parse_cmd_params(int argc, char **argv, BackendType &backend, bool &csv, bool &transpose, bool &decompose, bool &align)
{
	for( int i = 4; i < argc; i++ )
	{
		if( !strcmp(argv[i], "GPU") ) backend = GPU;
		if( !strcmp(argv[i], "CSV") ) csv = true;
		if( !strcmp(argv[i], "transpose") ) transpose = true;
		if( !strcmp(argv[i], "decompose") ) decompose = true;
		if( !strcmp(argv[i], "align") ) align = true;
	}
}

int main(int argc, char **argv)
{
	BackendType backend = CPU;
	bool csv = false;
	bool transpose = false;
	bool decompose = false;
	bool align = false;
	parse_cmd_params(argc, argv, backend, csv, transpose, decompose, align);

	if( backend == CPU )
	{
#ifdef _OPENMP
		printf("Using OpenMP: num_proc = %i\n", omp_get_num_procs());
#else
		printf("Using single CPU\n");
#endif
	}
	else
	{
		cudaDeviceProp deviceProp;
		cudaGetDeviceProperties(&deviceProp, 0);
		printf("Using GPU: %s\n", deviceProp.name);
		cudaSetDevice(0);
	}

	printf("%s precision computations\n", (typeid(FTYPE) == typeid(float)) ?  "Single" : "Double");

	char inputPath[MAX_STR_SIZE];
	char configPath[MAX_STR_SIZE];
	char outputPath[MAX_STR_SIZE];
	char gridPath[MAX_STR_SIZE];

	FindFile(inputPath, argv[1]);
	FindFile(configPath, argv[3]);
	
	Config::Config();
	Config::LoadFromFile(configPath);

	//--------------------------------------- Initializing ---------------------------------------
	Grid3D *grid = NULL;
	if( Config::in_fmt == Shape3D ) 
	{
		grid = new Grid3D(Config::dx, Config::dy, Config::dz, Config::startT, backend);
		printf("Geometry: 3D polygons\n", grid->dimx, grid->dimy, grid->dimz);
	}
	else if( Config::in_fmt == Shape2D )
	{
		grid = new Grid3D(Config::dx, Config::dy, Config::dz, Config::depth, Config::startT, backend);
		printf("Geometry: extruded 2D shape\n");
	}
	else
	{
		grid = new Grid3D(Config::dx, Config::dy, Config::dz, Config::startT, backend, true);
		printf("Geometry: depths from NetCDF\n");
	}

	printf("Grid options:\n  align %s\n", align ? "ON" : "OFF");
	if (grid->LoadFromFile(inputPath, align))
		printf("Grid = %i x %i x %i\n", grid->dimx, grid->dimy, grid->dimz);
	grid->Prepare(0.0);

	sprintf_s(gridPath, "%s_grid_3d", argv[2]);
	grid->OutputImage(gridPath);

	if( grid->GetGrid2D() != NULL )
	{
		sprintf_s(gridPath, "%s_grid_2d.bmp", argv[2]);
		grid->GetGrid2D()->OutputImage(gridPath);
	}
	
	FluidParams *params;
	if (Config::useNormalizedParams) params = new FluidParams(Config::Re, Config::Pr, Config::lambda);
		else params = new FluidParams(Config::viscosity, Config::density, Config::R_specific, Config::k, Config::cv);

	Solver3D *solver;
	switch (Config::solverID)
	{
		case Explicit: printf("Explicit solver is not implemented yet!\n"); break;
		case Stable: printf("Stable solver is not implemented yet!\n"); break;
		case ADI: 
			solver = new AdiSolver3D(); 
			if( backend == GPU ) 
			{
				printf("Solver options:\n  transpose %s\n  decompose %s\n", transpose ? "ON" : "OFF", decompose ? "ON" : "OFF");
				dynamic_cast<AdiSolver3D*>(solver)->SetOptionsGPU(transpose, decompose);
			}
			break;
	}
	solver->Init(backend, csv, grid, *params);

	int startFrame = 0;
	
	int frames = grid->GetFramesNum();
	double length = grid->GetCycleLength();
	double dt = length / (frames * Config::calc_subframes);
	double finaltime = length * Config::cycles;

	sprintf_s(outputPath, MAX_STR_SIZE, "%s_res.txt", argv[2]);
	if( Config::in_fmt == Shape2D )
		OutputNetCDFHeader3D(outputPath, &grid->GetGrid2D()->bbox, Config::depth, dt * Config::out_subframes, finaltime, Config::outdimx, Config::outdimy, Config::outdimz);
	else
		OutputNetCDFHeader3D(outputPath, &grid->GetBBox(), dt * Config::out_subframes, finaltime, Config::outdimx, Config::outdimy, Config::outdimz);
	
	// allocate result arrays
	Vec3D *resVel = new Vec3D[Config::outdimx * Config::outdimy * Config::outdimz];
	double *resT = new double[Config::outdimx * Config::outdimy * Config::outdimz];

	//------------------------------------------ Solving ------------------------------------------
	cpu_timer timer;
	timer.start();

	int lastframe = -1;
	double t = dt;
	for (int i=0; t < finaltime; t+=dt, i++)
	{
		int currentframe = grid->GetFrame(t);
		float layer_time = grid->GetLayerTime(t);

		if (currentframe != lastframe)
		{
			lastframe = currentframe;
			i = 0;
		}

		grid->Prepare(t);
		solver->UpdateBoundaries();
		solver->TimeStep((FTYPE)dt, Config::num_global, Config::num_local);
		solver->SetGridBoundaries();

		timer.stop();
		PrintTimeStepInfo(currentframe, i, t, finaltime, timer.elapsed_sec());

		if ((i % Config::out_subframes) == 0)
		{
			float dur = (float)dt * Config::out_subframes;
			if (dur > layer_time) dur = layer_time;

			solver->GetLayer(resVel, resT, Config::outdimx, Config::outdimy, Config::outdimz);
			OutputNetCDF3D_U(outputPath, resVel, resT, Config::outdimx, Config::outdimy, Config::outdimz, 
							(i + Config::out_subframes >= Config::calc_subframes) && (currentframe == frames-1));
		}
	}
	timer.stop();
	printf("\nTotal time: %.2f sec\n", timer.elapsed_sec());

	delete solver;
	delete [] resVel;
	delete [] resT;

	delete grid;

	return 0;
}