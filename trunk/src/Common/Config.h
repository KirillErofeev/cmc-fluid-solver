#pragma once

#include <string>

#define MAX_STR_SIZE	255

namespace Common
{
	enum solver { Explicit, ADI, Stable };
	enum dimension { _2D, _3D, unknown };
	
	class Config
	{
	public:

		static dimension problem_dim;

		// grid size
		static double dx, dy, dz;

		// fluid parameters
		static double viscosity, density;
		static double Re, Pr, lambda;		// not used currently

		// boundary conditions
		static bool bc_noslip;

		// thermodynamic params
		static double R_specific, k, cv, startT;		 

		// animation params
		static int cycles, calc_subframes, out_subframes;

		// output grid
		static int outdimx, outdimy;

		// solver params
		static solver solverID;		
		static int num_global, num_local;

		Config()
		{
			// set default
			R_specific = 461.495;		// water,	287.058	for air (gas constant)
			k = 0.6;					// water (thermal conductivity)
			cv = 4200.0;				// water (specific heat capacity at constant volume)
			startT = 300.0;				// in Kelvin

			bc_noslip = true;		

			viscosity = 0.05;
			density = 1000.0;

			cycles = 1;
			calc_subframes = 50;
			out_subframes = 10;
			outdimx = outdimy = 50;

			solverID = Stable;
			num_global = 2;
			num_local = 1;

			// must specify 
			problem_dim = unknown;
			dx = -1;
			dy = -1;
			dz = -1;
		}

		static void ReadDouble(FILE *file, double &value)
		{
			float f = 0.0f;
			fscanf_s(file, "%f", &f);
			value = (double)f;
		}

		static void ReadInt(FILE *file, int &value)
		{
			fscanf_s(file, "%i", &value);
		}

		static void ReadSolver(FILE *file)
		{
			char solverStr[MAX_STR_SIZE];
			fscanf_s(file, "%s", solverStr, MAX_STR_SIZE);
			if (!strcmp(solverStr, "Explicit")) solverID = Explicit;
			if (!strcmp(solverStr, "ADI"))		solverID = ADI;
			if (!strcmp(solverStr, "Stable"))	solverID = Stable;
		}

		static void ReadBC(FILE *file)
		{
			char bcStr[MAX_STR_SIZE];
			fscanf_s(file, "%s", bcStr, MAX_STR_SIZE);
			if (!strcmp(bcStr, "NoSlip")) bc_noslip = true;
				else bc_noslip = false;
		}

		static void ReadDim(FILE *file)
		{
			char dimStr[MAX_STR_SIZE];
			fscanf_s(file, "%s", dimStr, MAX_STR_SIZE);
			if (!strcmp(dimStr, "2D")) problem_dim = _2D;
				else problem_dim = _3D;
		}

		static void LoadFromFile(char *filename)
		{
			FILE *file = NULL;
			fopen_s(&file, filename, "r");

			if (file == NULL) { printf("cannot open config file!\n"); exit(0); }

			char str[MAX_STR_SIZE];
			while (!feof(file))
			{
				fscanf_s(file, "%s", str, MAX_STR_SIZE);

				if (!strcmp(str, "dimension")) ReadDim(file);

				if (!strcmp(str, "viscosity")) ReadDouble(file, viscosity);
				if (!strcmp(str, "density")) ReadDouble(file, density);

				if (!strcmp(str, "bc_type")) ReadBC(file);

				if (!strcmp(str, "grid_dx")) ReadDouble(file, dx);
				if (!strcmp(str, "grid_dy")) ReadDouble(file, dy);

				if (!strcmp(str, "cycles")) ReadInt(file, cycles);
				if (!strcmp(str, "calc_subframes")) ReadInt(file, calc_subframes);

				if (!strcmp(str, "out_subframes")) ReadInt(file, out_subframes);
				if (!strcmp(str, "out_gridx")) ReadInt(file, outdimx);
				if (!strcmp(str, "out_gridy")) ReadInt(file, outdimy);

				if (!strcmp(str, "solver")) ReadSolver(file);
				if (!strcmp(str, "num_global")) ReadInt(file, num_global);
				if (!strcmp(str, "num_local")) ReadInt(file, num_local);
			}	

			fclose(file);

			// checking		
			if (problem_dim == unknown) { printf("must specify problem dimension!\n"); exit(0); }
			if (dx < 0) { printf("cannot find dx!\n"); exit(0); }
			if (dy < 0) { printf("cannot find dy!\n"); exit(0); }
			if (problem_dim == _3D && dz < 0) { printf("cannot find dz!\n"); exit(0); }
		}
	};

	dimension Config::problem_dim;

	double Config::dx, Config::dy, Config::dz;

	double Config::viscosity, Config::density;
	double Config::Re, Config::Pr, Config::lambda;		// not used currently

	bool Config::bc_noslip;

	double Config::R_specific, Config::k, Config::cv, Config::startT;		 

	int Config::cycles, Config::calc_subframes, Config::out_subframes;

	int Config::outdimx, Config::outdimy;

	solver Config::solverID;		
	int Config::num_global, Config::num_local;
}