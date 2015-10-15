#include <electronic/Dump.h>
#include <electronic/Everything.h>
#include <electronic/Dump_internal.h>
#include <core/WignerSeitz.h>
#include <core/Operators.h>
#include <core/ScalarFieldIO.h>
#include <core/LatticeUtils.h>
#include <core/Coulomb_internal.h>

//-------------------------- Slab epsilon ----------------------------------

inline void planarAvg_sub(size_t iStart, size_t iStop, const vector3<int>& S, int iDir, complex* data)
{	int jDir = (iDir+1)%3;
	int kDir = (iDir+2)%3;
	THREAD_halfGspaceLoop( if(iG[jDir] || iG[kDir]) data[i] = 0.; )
}
void planarAvg(ScalarFieldTilde& X, int iDir)
{	threadLaunch(planarAvg_sub, X->gInfo.nG, X->gInfo.S, iDir, X->data());
}

inline void fixBoundary_sub(size_t iStart, size_t iStop, const vector3<int>& S, int iDir, int iBoundary, double* eps)
{	iBoundary = positiveRemainder(iBoundary, S[iDir]);
	THREAD_rLoop(
		int dist = iv[iDir] - iBoundary;
		if(dist*2 > S[iDir]) dist -= S[iDir];
		if(dist*2 < -S[iDir]) dist += S[iDir];
		if(abs(dist)<=2) eps[i] = 1.;
	)
}

void SlabEpsilon::dump(const Everything& e, ScalarField d_tot) const
{	string fname = e.dump.getFilename("slabEpsilon");
	logPrintf("Dumping '%s' ... ", fname.c_str()); logFlush();
	//Read reference Dtot:
	ScalarField d_totRef(ScalarFieldData::alloc(e.gInfo));
	loadRawBinary(d_totRef, dtotFname.c_str());
	//Calculate inverse of epsilon:
	int iDir = e.coulombParams.iDir;
	vector3<> zHat = e.gInfo.R.column(iDir);
	zHat *= 1./zHat.length(); //unit vector along slab normal
	double dE = dot(zHat, e.coulombParams.Efield - Efield);
	if(!dE) die("\nThe applied electric fields in the reference and present calculations are equal.\n");
	//--- calculate field using central-difference derivative:
	ScalarFieldTilde tPlus(ScalarFieldTildeData::alloc(e.gInfo)), tMinus(ScalarFieldTildeData::alloc(e.gInfo));
	initTranslation(tPlus, e.gInfo.h[iDir]);
	initTranslation(tMinus, -e.gInfo.h[iDir]);
	double h = e.gInfo.h[iDir].length();
	ScalarFieldTilde epsInvTilde = (1./(dE * 2.*h)) * (tPlus - tMinus) * J(d_tot - d_totRef);
	planarAvg(epsInvTilde, iDir);
	//Fix values of epsilon near truncation boundary to 1:
	ScalarField epsInv = I(epsInvTilde);
	threadLaunch(fixBoundary_sub, e.gInfo.nr, e.gInfo.S, iDir, e.coulomb->ivCenter[iDir] + e.gInfo.S[iDir]/2, epsInv->data());
	//Apply smoothing:
	epsInv = I(gaussConvolve(J(epsInv), sigma));
	//Write file:
	FILE* fp = fopen(fname.c_str(), "w");
	fprintf(fp, "#distance[bohr]  epsilon\n");
	vector3<int> iR;
	double* epsInvData = epsInv->data();
	for(iR[iDir]=0; iR[iDir]<e.gInfo.S[iDir]; iR[iDir]++)
		fprintf(fp, "%lf %lf\n", h*iR[iDir], 1./epsInvData[e.gInfo.fullRindex(iR)]);
	fclose(fp);
	logPrintf("done\n"); logFlush();
}

//-------------------------- Charged defects ----------------------------------

struct SlabPeriodicSolver : public LinearSolvable<ScalarFieldTilde>
{	int iDir; //truncated direction
	const ScalarField& epsilon;
	const GridInfo& gInfo;
	RealKernel Ksqrt, Kinv;
	const ScalarField epsInv;
	double K0; //G=0 component of slab kernel
	
	static inline void setKernels_sub(size_t iStart, size_t iStop, const GridInfo* gInfo, int iDir, double epsMean, double kRMS, double* Ksqrt, double* Kinv)
	{	const vector3<int>& S = gInfo->S;
		CoulombSlab_calc slabCalc(iDir, 0.5*gInfo->R.column(iDir).length());
		THREAD_halfGspaceLoop
		(	double K = slabCalc(iG, gInfo->GGT) / (4*M_PI);
			Kinv[i] = (fabs(K)>1e-12) ? 1./K : 0.;
			//Kinv[i] = gInfo->GGT.metric_length_squared(iG);
			Ksqrt[i] = (Kinv[i] || kRMS) ? 1./(epsMean*sqrt(Kinv[i] + kRMS*kRMS)) : 0.;
		)
	}
	
	SlabPeriodicSolver(int iDir, const ScalarField& epsilon)
	: iDir(iDir), epsilon(epsilon), gInfo(epsilon->gInfo), Ksqrt(gInfo), Kinv(gInfo), epsInv(inv(epsilon))
	{
		threadLaunch(setKernels_sub, gInfo.nG, &gInfo, iDir, integral(epsilon)/gInfo.detR, 0., Ksqrt.data, Kinv.data);
		K0 = (4*M_PI)/Kinv.data[0];
		Kinv.data[0] = 0.;
		Ksqrt.data[0] = 0.;
		Ksqrt.set(); Kinv.set();
		nullToZero(state, gInfo);
	}
	
	ScalarFieldTilde hessian(const ScalarFieldTilde& phiTilde) const
	{	ScalarFieldTilde rhoTilde = -(Kinv * phiTilde); //vacuum term
		rhoTilde += divergence(J((epsilon-1.) * I(gradient(phiTilde))));  //dielectric term
		return (-1./(4*M_PI)) * rhoTilde;
	}
	
	ScalarFieldTilde precondition(const ScalarFieldTilde& rTilde) const
	{	return Ksqrt*(J(epsInv*I(Ksqrt*rTilde)));
	}

	double getEnergy(const ScalarFieldTilde& rho, ScalarFieldTilde& phi)
	{	MinimizeParams mp;
		mp.nDim = gInfo.nr;
		mp.nIterations = 20;
		mp.knormThreshold = 1e-11;
		mp.fpLog = globalLog;
		mp.linePrefix = "\tSlabPeriodicCG: ";
		mp.energyFormat = "%+.15lf";

		zeroNyquist((ScalarFieldTilde&)rho);
		solve(rho, mp);
		
		phi = state;
		phi->setGzero(K0 * rho->getGzero());
		return -0.5*dot(phi, O(hessian(phi))) + dot(phi, O(rho)); //first-order correct estimate of final answer
	}
};

struct SlabIsolatedSolver : public LinearSolvable<matrix>
{
	
};

//Get the averaged field in direction iDir between planes iCenter +/- iDist
double getEfield(ScalarField V, int iDir, int iCenter, int iDist)
{	const GridInfo& gInfo = V->gInfo;
	//Planarly average:
	ScalarFieldTilde Vtilde = J(V);
	planarAvg(Vtilde, iDir);
	ScalarField Vavg = I(Vtilde);
	//Extract field:
	myassert(2*iDist < gInfo.S[iDir]);
	vector3<int> iRm, iRp;
	iRm[iDir] = positiveRemainder(iCenter - iDist, gInfo.S[iDir]);
	iRp[iDir] = positiveRemainder(iCenter + iDist, gInfo.S[iDir]);
	double Vm = Vavg->data()[gInfo.fullRindex(iRm)];
	double Vp = Vavg->data()[gInfo.fullRindex(iRp)];
	double dx = (2*iDist) * gInfo.h[iDir].length();
	return (Vm - Vp) / dx;
}

//Add electric field in direction iDir to given data
//x0 in lattice coordinates specifies where the added potential is zero
void addEfield_sub(size_t iStart, size_t iStop, const GridInfo* gInfo, int iDir, double Efield, vector3<> x0, double* V)
{	const vector3<int>& S = gInfo->S;
	double h = gInfo->h[iDir].length();
	double L = h * S[iDir];
	double r0 = x0[iDir] * L;
	THREAD_rLoop
	(	double r = h * iv[iDir] - r0;
		r -= L * floor(0.5 + r/L); //wrap to [-L/2,+L/2)
		V[i] += -Efield*r;
	)
}

void ChargedDefect::dump(const Everything& e, ScalarField d_tot) const
{	logPrintf("Calculating charged defect correction:\n"); logFlush();
	if(!center.size())
		die("\tNo model charges specified (using command charged-defect).\n");
	
	//Construct model charge on plane-wave grid:
	ScalarFieldTilde rhoModel;
	double qTot = 0.;
	for(const Center& cdc: center)
	{	ScalarFieldTilde trans(ScalarFieldTildeData::alloc(e.gInfo));
		initTranslation(trans, e.gInfo.R * cdc.pos);
		rhoModel += gaussConvolve((cdc.q/e.gInfo.detR)*trans, cdc.sigma);
		qTot += cdc.q;
	}
	
	//Find center of defects for alignment calculations:
	logSuspend(); WignerSeitz ws(e.gInfo.R); logResume();
	vector3<> pos0 = e.coulombParams.geometry==CoulombParams::Periodic ? center[0].pos : e.coulomb->xCenter;
	vector3<> posMean = pos0;
	for(const Center& cdc: center)
		posMean += (1./center.size()) * ws.restrict(cdc.pos - pos0);
	
	//Read reference Dtot and calculate electrostatic potential difference within DFT:
	ScalarField d_totRef(ScalarFieldData::alloc(e.gInfo));
	loadRawBinary(d_totRef, dtotFname.c_str());
	ScalarField Vdft = d_tot - d_totRef; //electrostatic potential of defect from DFT
	
	//Calculate isolated and periodic self-energy (and potential) of model charge
	ScalarField Vmodel; double Emodel=0., EmodelIsolated=0.;
	switch(e.coulombParams.geometry)
	{	case CoulombParams::Periodic: //Bulk defect
		{	//Periodic potential and energy:
			ScalarFieldTilde dModel = (*e.coulomb)(rhoModel) * (1./bulkEps); //assuming uniform dielectric
			Emodel = 0.5*dot(rhoModel, O(dModel));
			Vmodel = I(dModel);
			//Isolated energy:
			for(const Center& cdc: center)
				EmodelIsolated += 0.5*std::pow(cdc.q,2)/(cdc.sigma*sqrt(M_PI)*bulkEps); //self energy of Gaussian
			break;
		}
		case CoulombParams::Slab: //Surface defect
		{	int iDir = e.coulombParams.iDir;
			if(!e.coulombParams.embed)
				die("\tCoulomb truncation must be embedded for charged-defect correction in slab geometry.\n");
			rhoModel = e.coulomb->embedExpand(rhoModel); //switch to embedding grid
			//Create dielectric model for slab:
			ScalarField epsSlab; nullToZero(epsSlab, e.gInfo);
			double* epsSlabData = epsSlab->data();
			std::ifstream ifs(slabEpsFname.c_str());
			if(!ifs.is_open()) die("\tCould not open slab dielectric model file '%s' for reading.\n", slabEpsFname.c_str());
			string commentLine; getline(ifs, commentLine); //get and ignore comment line
			vector3<int> iR;
			double h = e.gInfo.h[iDir].length();
			for(iR[iDir]=0; iR[iDir]<e.gInfo.S[iDir]; iR[iDir]++)
			{	double dExpected = h*iR[iDir];
				double d; ifs >> d >> epsSlabData[e.gInfo.fullRindex(iR)];
				if(fabs(d-dExpected) > symmThreshold)
					die("\tGeometry mismatch in '%s': expecting distance %lg on line %d; found %lg instead.\n",
						slabEpsFname.c_str(), dExpected, iR[iDir]+2, d);
			}
			ScalarFieldTilde epsSlabMinus1tilde = J(epsSlab) * (e.gInfo.nr / e.gInfo.S[iDir]); //multiply by number of points per plane (to account for average below)
			epsSlabMinus1tilde->setGzero(epsSlabMinus1tilde->getGzero() - 1.); //subtract 1
			planarAvg(epsSlabMinus1tilde, iDir); //now contains a planarly-uniform version of epsSlab-1
			epsSlab = 1. + I(e.coulomb->embedExpand(epsSlabMinus1tilde)); //switch to embedding grid (note embedding eps-1 (instead of eps) since it is zero in vacuum)
			
			//Periodic potential and energy:
			ScalarFieldTilde dModel;
			Emodel = SlabPeriodicSolver(iDir, epsSlab).getEnergy(rhoModel, dModel);
			//--- fix up net electric field in output potential (increases accuracy of alignment):
			Vmodel = I(dModel);
			double EfieldDft = getEfield(Vdft, iDir, e.coulomb->ivCenter[iDir], e.gInfo.S[iDir]/2-2);
			double EfieldModel = getEfield(Vmodel, iDir, 0., e.gInfo.S[iDir]/2-2);
			vector3<> posMeanEmbed = Diag(e.coulomb->embedScale) * ws.restrict(posMean - e.coulomb->xCenter);
			threadLaunch(addEfield_sub, Vmodel->gInfo.nr, &(Vmodel->gInfo), iDir, EfieldDft-EfieldModel, posMeanEmbed, Vmodel->data());
			Vmodel = I(e.coulomb->embedShrink(J(Vmodel)));
			
			//Isolated energy:
			//TODO
			break;
		}
		default: die("\tCoulomb-interaction geometry must be either slab or periodic for charged-defect correction.\n");
	}
	logPrintf("\tEmodelIsolated: %.8lf\n", EmodelIsolated);
	logPrintf("\tEmodelPeriodic: %.8lf\n", Emodel);
	
	//Calculate alignment potential:
	ScalarField Varr[2] = { Vdft, Vmodel };
	vector3<> rCenter = e.gInfo.R*posMean;
	std::vector< std::vector<double> > hist = sphericalize(Varr, 2, 1., &rCenter);
	const std::vector<double>& rRadial = hist[0];
	const std::vector<double>& VdftRadial = hist[1];
	const std::vector<double>& VmodelRadial = hist[2];
	const std::vector<double>& wRadial = hist[3];
	//--- calculate alignment scalar:
	double deltaV = 0.; double wSum = 0.;
	for(size_t i=0; i<rRadial.size(); i++)
	{	double r = rRadial[i];
		double w = wRadial[i] * 0.5*erfc((rMin-r)/rSigma);
		deltaV += (VmodelRadial[i] - VdftRadial[i])*w;
		wSum += w;
	}
	deltaV /= wSum;
	logPrintf("\tDeltaV:         %.8lf (average over %lg grid points)\n", deltaV, wSum);
	logPrintf("\tNet correction: %.8lf (= EmodelIsolated - EmodelPeriodic + q deltaV)\n", EmodelIsolated - Emodel + qTot*deltaV);
	//--- write alignment data (spherical)
	string fname = e.dump.getFilename("chargedDefectDeltaV");
	logPrintf("\tWriting %s (spherically-averaged; plot to check DeltaV manually) ... ", fname.c_str()); logFlush();
	if(mpiUtil->isHead())
	{	FILE* fp = fopen(fname.c_str(), "w");
		if(!fp) die("\tError opening %s for writing.\n", fname.c_str())
		fprintf(fp, "#r DeltaV Vmodel Vdft weight\n");
		for(size_t i=0; i<rRadial.size(); i++)
			fprintf(fp, "%lf %le %le %le %le\n", rRadial[i], VmodelRadial[i]-VdftRadial[i],
				VmodelRadial[i], VdftRadial[i], (wRadial[i] * 0.5*erfc((rMin-rRadial[i])/rSigma)) / wSum);
		fclose(fp);
	}
	logPrintf("done.\n");
	//--- write alignment data (planar average)
	for(int iDir=0; iDir<3; iDir++)
	{	string fname = e.dump.getFilename(string("chargedDefectDeltaV") + "xyz"[iDir]);
		logPrintf("\tWriting %s (planarly-averaged normal to lattice direction# %d) ... ", fname.c_str(), iDir); logFlush();
		ScalarField Vavg[2]; double* VavgData[2];
		for(int k=0; k<2; k++)
		{	ScalarFieldTilde Vtilde = J(Varr[k]);
			planarAvg(Vtilde, iDir);
			Vavg[k] = I(Vtilde);
			VavgData[k] = Vavg[k]->data();
		}
		if(mpiUtil->isHead())
		{	FILE* fp = fopen(fname.c_str(), "w");
			if(!fp) die("\tError opening %s for writing.\n", fname.c_str())
			fprintf(fp, "#x[bohr] DeltaV Vmodel Vdft\n");
			vector3<int> iR;
			double h = e.gInfo.h[iDir].length();
			for(iR[iDir]=0; iR[iDir]<e.gInfo.S[iDir]; iR[iDir]++)
			{	size_t i = e.gInfo.fullRindex(iR);
				fprintf(fp, "%lf %le %le %le\n", iR[iDir]*h,
					VavgData[1][i]-VavgData[0][i], VavgData[0][i], VavgData[1][i]);
			}
			fclose(fp);
		}
		logPrintf("done.\n");
	}
}
