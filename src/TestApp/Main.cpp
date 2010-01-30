#include "FluidSolver.h"
#include "Timer.h"

const double dx = 0.0014;
const double dy = 0.0014;

const double dt = 0.000001;

const double Re = 50.0;
const double Pr = 0.82;
const double lambda = 1.4;

const int num_global = 4;
const int num_local = 1;

const int cycles = 1;
const int frames = 25;
const int subframes = 100;
const int subsub = 10;

const int outdimx = 50;
const int outdimy = 50;

const float timeValue = 0.035f;

enum solvers { Explicit, ADI };
const int solverID = Explicit;		

using namespace FluidSolver;

int main(int argc, char **argv)
{
	char dataPath[MAX_PATH];
	char resPath[MAX_PATH];

	sprintf_s(dataPath, "..\\..\\data\\%s_ns.txt", argv[1]);
	sprintf_s(resPath, "..\\..\\data\\%s_res.txt", argv[1]);

	//--------------------------------------- Initializing ---------------------------------------
	Grid2D grid(dx, dy);
	if (grid.LoadFromFile(dataPath) == OK)
	{
		printf("dx,dy,dimx,dimy,dt,Re,Pr,lambda\n");
		printf("%f,%f,%i,%i,%.3f,%f,%f,%f\n", dx, dy, grid.dimx, grid.dimy, dt, Re, Pr, lambda);
	}
	grid.Prepare(0, 0);
	//grid.TestPrint();
	
	FluidParams params(Re, Pr, lambda);

	Solver2D *solver;
	switch (solverID)
	{
		case Explicit: solver = new ExplicitSolver2D(); break;
		case ADI: solver = new AdiSolver2D(); break;
	}
	solver->Init(&grid, params);

	printf("Starting from the beginning\n");
	int startFrame = 0;
	FILE *resFile = NULL;
	fopen_s(&resFile, resPath, "w");
	OutputResultHeader(resFile, &grid.bbox, outdimx, outdimy);
	
	Vec2D *resVel = new Vec2D[outdimx * outdimy];
	double *resT = new double[outdimx * outdimy];

	//------------------------------------------ Solving ------------------------------------------
	cpu_timer timer;
	timer.start();
	for (int i = startFrame, end = frames * cycles; i < end; i++)
	{	
		fprintf(resFile, "Frame %i\n", i % frames);

		for (int j = 0; j < subframes; j++)
		{
			grid.Prepare(i, (double)j / subframes);
			solver->UpdateBoundaries();
 			solver->TimeStep(dt, num_global, num_local);
			solver->SetGridBoundaries();

			timer.stop();
			PrintTimeStepInfo(i, j, frames, subframes, cycles, timer.elapsed_sec());

			if ((j % subsub) == 0)
			{
				solver->GetLayer(resVel, resT, outdimx, outdimy);
				OutputResult(resFile, resVel, resT, outdimx, outdimy, timeValue);
			}
		}
	}
	printf("\n");

	delete solver;
	delete [] resVel;
	delete [] resT;

	fclose(resFile);
	return 0;
}