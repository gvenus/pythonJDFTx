/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman, Kendra Letchworth Weaver, Deniz Gunceler
Copyright 1996-2003 Sohrab Ismail-Beigi

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#include <electronic/Everything.h>
#include <electronic/ElecMinimizer.h>
#include <electronic/ColumnBundle.h>
#include <electronic/matrix.h>
#include <electronic/operators.h>
#include <electronic/ExCorr.h>
#include <electronic/ExactExchange.h>
#include <fluid/FluidSolver.h>
#include <core/Units.h>
#include <core/ScalarFieldIO.h>
#include <cstdio>
#include <cmath>
#include <limits.h>

ElecVars::ElecVars()
: isRandom(true), initLCAO(true), HauxInitialized(false), lcaoIter(-1), lcaoTol(1e-6)
{
}

// Kernel for generating box shaped external potentials
void applyBoxPot(int i, vector3<> r, matrix3<>& R, const ElecVars::BoxPotential* bP, double* Vbox)
{	// Map lattice intervals [0,1) to [-0.5,0.5)
	r = inv(R)*r;
	for(int j=0; j<3; j++) r[j] -= floor(0.5+r[j]);
	r = R*r;
	//Set potential
	for(int j=0; j<3; j++)
	{	if((r[j] < bP->min[j]) or (r[j] > bP->max[j]))
		{	Vbox[i] = bP->Vout;
			return;
		}
	}
	Vbox[i] = bP->Vin;
}

void ElecVars::setup(const Everything &everything)
{	
	this->e = &everything;
	logPrintf("\n---------- Allocating electronic variables ----------\n"); logFlush();

	const ElecInfo &eInfo = e->eInfo;
	const IonInfo& iInfo = e->iInfo;
	const std::vector<Basis>& basis = e->basis;
	const GridInfo& gInfo = e->gInfo;

	n.resize(eInfo.nDensities);
	Vscloc.resize(n.size());
	if(eInfo.hasU)
	{	iInfo.rhoAtom_initZero(rhoAtom);
		iInfo.rhoAtom_initZero(U_rhoAtom);
	}
	
	if(VexternalFilename.size())
	{	Vexternal.resize(VexternalFilename.size());
		for(unsigned s=0; s<Vexternal.size(); s++)
		{	Vexternal[s] = ScalarFieldData::alloc(gInfo);
			logPrintf("Reading external potential from '%s'\n", VexternalFilename[s].c_str());
			loadRawBinary(Vexternal[s], VexternalFilename[s].c_str());
		}
		if(Vexternal.size()==1 && n.size()==2) //Replicate potential for second spin:
			Vexternal.push_back(Vexternal[0]->clone());
	}

	//Vexternal contributions from boxPot's.
	for(size_t j = 0; j<boxPot.size(); j++)
	{	//Create potential
		ScalarField temp; nullToZero(temp, e->gInfo);
		applyFunc_r(e->gInfo, applyBoxPot, gInfo.R, &boxPot[j], temp->data());
		temp = I(gaussConvolve(J(temp), boxPot[j].convolve_radius));
		//Add to Vexternal
		if(!Vexternal.size()) Vexternal.resize(n.size());
		for(unsigned s=0; s<n.size(); s++) Vexternal[s] += temp;
	}

	//Vexternal contributions due to external field
	if(e->coulombParams.Efield.length_squared())
	{	ScalarField temp = e->coulomb->getEfieldPotential();
		//Add to Vexternal
		if(!Vexternal.size()) Vexternal.resize(n.size());
		for(unsigned s=0; s<n.size(); s++) Vexternal[s] += temp;
	}

	if(rhoExternalFilename.length())
	{	logPrintf("Reading external charge from '%s'\n", rhoExternalFilename.c_str());
		ScalarField temp(ScalarFieldData::alloc(gInfo));
		loadRawBinary(temp, rhoExternalFilename.c_str());
		rhoExternal = J(temp);
	}
	
	
	//Initialize matrix arrays if required:
	Hsub.resize(eInfo.nStates);
	Hsub_evecs.resize(eInfo.nStates);
	Hsub_eigs.resize(eInfo.nStates);
	if(eigsFilename.length()) eInfo.read(Hsub_eigs, eigsFilename.c_str());
	if(eInfo.subspaceRotation)
	{	B.resize(eInfo.nStates);
		B_evecs.resize(eInfo.nStates);
		B_eigs.resize(eInfo.nStates);
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
			B[q] = zeroes(eInfo.nBands, eInfo.nBands); //Set to zero
	}
	grad_CdagOC.resize(eInfo.nStates);
	VdagC.resize(eInfo.nStates, std::vector<matrix>(e->iInfo.species.size()));
	
	// Initialize matrix U and its cohorts
	U.resize(eInfo.nStates);
	U_evecs.resize(eInfo.nStates);
	U_eigs.resize(eInfo.nStates);
	Umhalf.resize(eInfo.nStates);
	V.resize(eInfo.nStates);
	
	//Applies custom fillings, if present
	if(eInfo.customFillings.size())
	{	// macro to find the HOMO of a quantum-number
		
		std::vector<size_t> band_index;
		for(size_t j=0; j<eInfo.customFillings.size(); j++)
		{	int qnum = std::get<0>(eInfo.customFillings[j]);
			if(!eInfo.isMine(qnum)) continue;
			int HOMO = eInfo.findHOMO(qnum);
			size_t band = std::get<1>(eInfo.customFillings[j])+HOMO;
			if(band >= F[qnum].size() or band<0)
			{	die("\tERROR: Incorrect band index (%i) for custom fillings is specified! HOMO + %i"
					" does not exist.\n\tEither increase the number of bands or change the band index\n\n", 
					(int) band, std::get<1>(eInfo.customFillings[j]));
			}
			band_index.push_back(band);
		}
		for(size_t j=0; j<eInfo.customFillings.size(); j++)
		{	int qnum = std::get<0>(eInfo.customFillings[j]);
			if(!eInfo.isMine(qnum)) continue;
			F[qnum][band_index[j]] = std::get<2>(eInfo.customFillings[j]);
		}
		
		//Recompute electron count:
		double& nElectrons = ((ElecInfo&)eInfo).nElectrons;
		nElectrons = 0.;
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
			nElectrons += eInfo.qnums[q].weight * trace(F[q]);
		mpiUtil->allReduce(nElectrons, MPIUtil::ReduceSum, true);
	}
	
	//Read in electron (spin) density if needed
	if(e->cntrl.fixed_H)
	{	string fnamePattern = nFilenamePattern.length() ? nFilenamePattern : VFilenamePattern; //Command ensures that the pattern has a "$VAR" in it
		#define READchannel(var, suffix) \
		{	string fname = fnamePattern; \
			size_t pos = fname.find("$VAR"); \
			myassert(pos != string::npos); \
			fname.replace(pos,4, suffix); \
			logPrintf("Reading " #suffix " from file '%s' ... ", fname.c_str()); logFlush(); \
			nullToZero(var, e->gInfo); \
			loadRawBinary(var, fname.c_str()); \
			logPrintf("done\n"); logFlush(); \
		}
		#define READ(var) \
		{	var.resize(eInfo.nDensities); \
			if(var.size()==1) { READchannel(var[0], #var) } \
			else \
			{	READchannel(var[0], #var "_up") \
				READchannel(var[1], #var "_dn") \
				if(var.size()==4) \
				{	READchannel(var[2], #var "_re") \
					READchannel(var[3], #var "_im") \
				} \
			} \
		}
		#define READrhoAtom(var) \
		{	string fname = fnamePattern; \
			size_t pos = fname.find("$VAR"); \
			myassert(pos != string::npos); \
			fname.replace(pos,4, #var); \
			logPrintf("Reading " #var " from file '%s' ... ", fname.c_str()); logFlush(); \
			FILE* fp = fopen(fname.c_str(), "r"); \
			for(matrix& m: var) m.read(fp); \
			fclose(fp); \
			logPrintf("done\n"); logFlush(); \
		}
		if(nFilenamePattern.length())
		{	READ(n)
			if(e->exCorr.needsKEdensity()) READ(tau)
			if(eInfo.hasU) READrhoAtom(rhoAtom)
		}
		else
		{	READ(Vscloc)
			if(e->exCorr.needsKEdensity()) READ(Vtau)
			if(eInfo.hasU) READrhoAtom(U_rhoAtom)
		}
		#undef READ
		#undef READchannel
		//Check if fix-occupied has been specified when required (hyrbids):
		if( e->exCorr.exxFactor() && (! e->cntrl.fixOccupied) )
			die("Band-structure (fixed electron density) calculations with hybrid\n"
				"functionals require fixed occupied states (command fix-occupied)\n");
		//Check if wavefunctions have been read in if occupied orbitals are fixed:
		if(e->cntrl.fixOccupied && (! wfnsFilename.length()))
			die("Fixed occupied orbitals require wavefunctions to be read from file\n");
	}

	//Read auxilliary hamiltonian if required
	if(eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
	{	dmuContrib = diagMatrix(eInfo.nBands, 0.);
		if(HauxFilename.length())
		{	logPrintf("Reading auxilliary hamitonian from '%s'\n", HauxFilename.c_str());
			eInfo.read(B, HauxFilename.c_str());
			HauxInitialized = true;
		}
	}

	// Initialize ColumnBundle arrays for the electronic wave-functions:
	logPrintf("Initializing wave functions:  ");
	init(Y, eInfo.nStates, eInfo.nBands, &basis[0], &eInfo);
	init(C, eInfo.nStates, eInfo.nBands, &basis[0], &eInfo);

	//Initial wave functions
	int nBandsInited = 0;
	if(wfnsFilename.length())
	{	logPrintf("reading from '%s'\n", wfnsFilename.c_str()); logFlush();
		if(readConversion) readConversion->Ecut = e->cntrl.Ecut;
		read(Y, wfnsFilename.c_str(), eInfo, readConversion.get());
		nBandsInited = (readConversion && readConversion->nBandsOld) ? readConversion->nBandsOld : eInfo.nBands;
		isRandom = (nBandsInited<eInfo.nBands);
	}
	else if(initLCAO)
	{	nBandsInited = LCAO();
	}
	
	//Randomize and orthogonalize any uninitialized bands:
	if(nBandsInited < eInfo.nBands)
	{	if(nBandsInited) logPrintf("Setting upper %d bands to ", eInfo.nBands-nBandsInited);
		logPrintf("bandwidth-limited random numbers\n"); logFlush();
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		{	//randomize uninitialized bands:
			Y[q].randomize(nBandsInited, eInfo.nBands);
			//don't mix (during orthogonalization) the random columns with the init'd ones:
			if(nBandsInited)
			{	//Ensure that the initalized bands are orthonormal:
				ColumnBundle Yfixed = Y[q].getSub(0, nBandsInited);
				ColumnBundle OYfixed = O(Yfixed);
				matrix ortho = invsqrt(Yfixed^OYfixed);
				Yfixed = Yfixed * ortho;
				OYfixed = OYfixed * ortho;
				//Project out initalized band directions from the rest:
				Y[q] -= Yfixed * (OYfixed^Y[q]);
				Y[q].setSub(0, Yfixed);
			}
		}
	}
	
	//Orthogonalize initial wavefunctions:
	for(int q=eInfo.qStart; q<eInfo.qStop; q++)
	{	Y[q] = Y[q] * invsqrt(Y[q]^O(Y[q]));
		C[q] = Y[q];
		iInfo.project(C[q], VdagC[q]);
	}
	
	//Fluid setup:
	if(fluidParams.fluidType != FluidNone)
	{	logPrintf("----- createFluidSolver() ----- (Fluid-side solver setup)\n");
		fluidSolver = std::shared_ptr<FluidSolver>(createFluidSolver(*e, fluidParams));
		if(!fluidSolver) die("Failed to create fluid solver.\n");
		if(fluidInitialStateFilename.length())
		{	logPrintf("Reading fluid state from '%s'\n", fluidInitialStateFilename.c_str()); logFlush();
			fluidSolver->loadState(fluidInitialStateFilename.c_str());
		}
	}
	
	//Citations:
	Citations::add("Total energy minimization",
		"T.A. Arias, M.C. Payne and J.D. Joannopoulos, Phys. Rev. Lett. 69, 1077 (1992)");
	if(eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
		Citations::add("Direct minimization with Fermi fillings",
			"C. Freysoldt, S. Boeck, and J. Neugebauer, Phys. Rev. B 79, 241103(R) (2009)");
}

ScalarFieldArray ElecVars::get_nXC() const
{	if(e->iInfo.nCore)
	{	ScalarFieldArray nXC = clone(n);
		int nSpins = std::min(int(nXC.size()), 2); //1 for unpolarized and 2 for polarized
		for(int s=0; s<nSpins; s++) //note that off-diagonal components of spin-density matrix are excluded
			nXC[s] += (1./nSpins) * e->iInfo.nCore; //add core density
		return nXC;
	}
	else return n; //no cores
}

//Electronic density functional and gradient
void ElecVars::EdensityAndVscloc(Energies& ener, const ExCorr* alternateExCorr)
{	static StopWatch watch("EdensityAndVscloc"); watch.start();
	const ElecInfo& eInfo = e->eInfo;
	const IonInfo& iInfo = e->iInfo;
	
	ScalarFieldTilde nTilde = J(get_nTot());
	
	// Local part of pseudopotential:
	ener.E["Eloc"] = dot(nTilde, O(iInfo.Vlocps));
	ScalarFieldTilde VsclocTilde = clone(iInfo.Vlocps);
	
	// Hartree term:
	ScalarFieldTilde dH = (*e->coulomb)(nTilde); //Note: external charge and nuclear charge contribute to d_vac as well (see below)
	ener.E["EH"] = 0.5*dot(nTilde, O(dH));
	VsclocTilde += dH;

	// External charge:
	ener.E["Eexternal"] = 0.;
	if(rhoExternal)
	{	ScalarFieldTilde phiExternal = (*e->coulomb)(rhoExternal);
		ener.E["Eexternal"] += dot(nTilde + iInfo.rhoIon, O(phiExternal));
		if(rhoExternalSelfEnergy)
			ener.E["Eexternal"] += 0.5 * dot(rhoExternal, O(phiExternal));
		VsclocTilde += phiExternal;
	}
	
	//Fluid contributions
	ScalarFieldTilde VtauTilde;
	if(fluidParams.fluidType != FluidNone)
	{	//Compute n considered for cavity formation (i.e-> include chargeball and partial cores)
		ScalarFieldTilde nCavityTilde = clone(nTilde);
		if(iInfo.nCore) nCavityTilde += J(iInfo.nCore);
		if(iInfo.nChargeball) nCavityTilde += iInfo.nChargeball;

		//Net electric charge:
		ScalarFieldTilde rhoExplicitTilde = nTilde + iInfo.rhoIon + rhoExternal;
		if(!fluidSolver->k2factor) rhoExplicitTilde->setGzero(0.); //No screening => apply neutralizing background charge
		fluidSolver->set(rhoExplicitTilde, nCavityTilde);
		// If the fluid doesn't have a gummel loop, minimize it each time:
		if(!fluidSolver->needsGummel()) fluidSolver->minimizeFluid();
		
		// Compute the energy and accumulate gradients:
		ener.E["A_diel"] = fluidSolver->get_Adiel_and_grad(&d_fluid, &V_cavity);
		VsclocTilde += d_fluid;
		VsclocTilde += V_cavity;

		//Chemical-potential correction due to potential of electron in bulk fluid
		double bulkPotential = fluidSolver->bulkPotential();
		//Chemical-potential correction due to finite nuclear width in fluid interaction:
		double muCorrection = fluidSolver->ionWidthMuCorrection();
		ener.E["MuShift"] = (eInfo.nElectrons - iInfo.getZtot()) * (muCorrection - bulkPotential);
		VsclocTilde->setGzero(muCorrection - bulkPotential + VsclocTilde->getGzero());
	}

	//Atomic density-matrix contributions: (DFT+U)
	if(eInfo.hasU)
		ener.E["U"] = iInfo.rhoAtom_computeU(rhoAtom, U_rhoAtom);
	
	// Exchange and correlation, and store the real space Vscloc with the odd historic normalization factor of JdagOJ:
	ScalarFieldArray Vxc;
	const ExCorr& exCorr = alternateExCorr ? *alternateExCorr : e->exCorr;
	ener.E["Exc"] = exCorr(get_nXC(), &Vxc, false, &tau, &Vtau);
	if(!exCorr.hasEnergy() && !e->cntrl.scf)
		die("Potential functionals do not support total-energy minimization; use SCF instead.\n")
	if(exCorr.orbitalDep)
	{	if(!e->cntrl.scf)
		{	if(e->cntrl.fixed_H) die("Orbital-dependent potential functionals do not support fix-density; use fix-potential instead.\n")
			else die("Orbital-dependent potential functionals do not support total-energy minimization; use SCF instead.\n")
		}
		Vxc += exCorr.orbitalDep->getPotential();
	}
	if(VtauTilde) Vtau.resize(n.size());
	for(unsigned s=0; s<Vscloc.size(); s++)
	{	Vscloc[s] = JdagOJ(Vxc[s]);
		if(s<2) //Include all the spin-independent contributions along the diagonal alone
			Vscloc[s] += Jdag(O(VsclocTilde), true);
		//External potential contributions:
		if(Vexternal.size())
		{	ener.E["Eexternal"] += e->gInfo.dV * dot(n[s], Vexternal[s]);
			Vscloc[s] += JdagOJ(Vexternal[s]);
		}
		e->symm.symmetrize(Vscloc[s]);
		if(VtauTilde) Vtau[s] += I(VtauTilde);
		if(Vtau[s]) e->symm.symmetrize(Vtau[s]);
	}
	watch.stop();
}


//-----  Electronic energy and (preconditioned) gradient calculation ----------

double ElecVars::elecEnergyAndGrad(Energies& ener, ElecGradient* grad, ElecGradient* Kgrad, bool calc_Hsub)
{	
	const ElecInfo& eInfo = e->eInfo;
	
	//Cleanup old gradients:
	if(grad) { grad->Y.assign(eInfo.nStates, ColumnBundle()); }
	if(Kgrad) { Kgrad->Y.assign(eInfo.nStates, ColumnBundle()); }
	
	//Determine whether Hsub and hence HC needs to be calculated:
	bool need_Hsub = calc_Hsub || grad || e->cntrl.fixed_H;
	double mu = 0.0;
	
	if(not e->cntrl.scf) //Wavefunctions are already orthonormal when this function is called in SCF mode
	{
		//Orthonormalize Y to compute C, U and cohorts
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
			orthonormalize(q);
	
		//Update overlap condition number:
		double UeigMin = DBL_MAX, UeigMax = 0.;
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		{	auto extremes = std::minmax_element(U_eigs[q].begin(), U_eigs[q].end());
			UeigMin = std::min(UeigMin, *(extremes.first));
			UeigMax = std::max(UeigMax, *(extremes.second));
		}
		mpiUtil->allReduce(UeigMin, MPIUtil::ReduceMin, true);
		mpiUtil->allReduce(UeigMax, MPIUtil::ReduceMax, true);
		overlapCondition = UeigMax / UeigMin;
	
		//Update fillings if required:
		if(eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
		{	//Update nElectrons from mu, or mu from nElectrons as appropriate:
			if(std::isnan(eInfo.mu)) mu = eInfo.findMu(B_eigs, eInfo.nElectrons);
			else { mu = eInfo.mu; ((ElecInfo&)eInfo).nElectrons = eInfo.nElectronsFermi(mu, B_eigs); }
			//Compute fillings from aux hamiltonian eigenvalues:
			for(int q=eInfo.qStart; q<eInfo.qStop; q++)
				F[q] = eInfo.fermi(mu, B_eigs[q]);
			//Update TS and muN:
			eInfo.updateFillingsEnergies(F, ener);
		}
	}
	
	//Update the density and density-dependent pieces if required:
	if(!e->cntrl.fixed_H)
	{	//Calculate (spin) densities
		n = calcDensity();
		
		//Calculate kinetic energy density if required
		if(e->exCorr.needsKEdensity())
			tau = KEdensity();
		
		//Update atomic density matrix contributions for DFT+U (if necessary):
		if(eInfo.hasU)
			e->iInfo.rhoAtom_calc(F, C, rhoAtom);
		
		//Calculate density functional and its gradient:
		EdensityAndVscloc(ener);
		
		//Update Vscloc projected onto spherical functions (for ultrasoft psp's)
		if(need_Hsub) e->iInfo.augmentDensityGridGrad(Vscloc);
	}
	
	//--------- Wavefunction dependent parts -----------
	
	std::vector<ColumnBundle> HC(eInfo.nStates); //gradient w.r.t C (upto weights and fillings)
	
	//Exact exchange if required:
	ener.E["EXX"] = 0.;
	if(e->exCorr.exxFactor())
	{	double aXX = e->exCorr.exxFactor();
		double omega = e->exCorr.exxRange();
		myassert(e->exx);
		ener.E["EXX"] = (*e->exx)(aXX, omega, F, C, need_Hsub ? &HC : 0);
	}
	
	//Do the single-particle contributions one state at a time to save memory (and for better cache warmth):
	ener.E["KE"] = 0.;
	ener.E["Enl"] = 0.;
	for(int q=eInfo.qStart; q<e->eInfo.qStop; q++)
	{	diagMatrix Fq = e->cntrl.fixed_H ? eye(e->eInfo.nBands) : F[q]; //override fillings for band structure
		applyHamiltonian(q, Fq, HC[q], ener, need_Hsub);
	}
	mpiUtil->allReduce(ener.E["KE"], MPIUtil::ReduceSum);
	mpiUtil->allReduce(ener.E["Enl"], MPIUtil::ReduceSum);
	
	if(grad and eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux and std::isnan(eInfo.mu)) //contribution due to Nconstraint via the mu gradient 
	{	double dmuNumDen[2] = { 0., 0. }; //numerator and denominator of dmuContrib
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		{	diagMatrix fprime = eInfo.fermiPrime(mu, B_eigs[q]);
			dmuNumDen[0] += eInfo.qnums[q].weight * trace(fprime * (diag(Hsub[q])-B_eigs[q]));
			dmuNumDen[1] += eInfo.qnums[q].weight * trace(fprime);
		}
		mpiUtil->allReduce(dmuNumDen, 2, MPIUtil::ReduceSum);
		dmuContrib = eye(eInfo.nBands) * (dmuNumDen[0]/dmuNumDen[1]);
	}
	
	//Propagate grad_C to gradients and/or preconditioned gradients of Y, B:
	if(grad)
	{	double KErollover = 2.*ener.E["KE"] / eInfo.nElectrons;
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		{
			diagMatrix Fq = e->cntrl.fixed_H ? eye(e->eInfo.nBands) : F[q]; //override fillings for band structure
			orthonormalizeGrad(q, Fq, HC[q], grad->Y[q], KErollover, Kgrad ? &Kgrad->Y[q] : 0, &grad->B[q], Kgrad ? &Kgrad->B[q] : 0);
			HC[q].free(); // Deallocate HCq when done.
			
			//Subspace hamiltonian gradient:
			if(grad && eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
			{
				const QuantumNumber& qnum = eInfo.qnums[q];
				matrix gradF = Hsub[q]-B_eigs[q]-dmuContrib; //gradient w.r.t fillings
				grad->B[q] = qnum.weight
					* dagger_symmetrize(dagger(V[q]) * eInfo.fermiGrad(mu, B_eigs[q], gradF) * V[q]);
				if(Kgrad) //Drop the fermiPrime factors in preconditioned gradient:
					Kgrad->B[q] = (-eInfo.kT * subspaceRotationFactor * qnum.weight)
						* dagger_symmetrize(dagger(V[q]) * gradF * V[q]);
			}
		}
	}
	
	if(e->cntrl.fixed_H)
	{	//Compute band structure energy
		ener.Eband = 0;
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
			ener.Eband += eInfo.qnums[q].weight * trace(Hsub[q]).real();
		mpiUtil->allReduce(ener.Eband, MPIUtil::ReduceSum);
	}
	return relevantFreeEnergy(*e);
}

//Make phase (and degenerate-subspace rotations) of wavefunctions reproducible 
void fixPhase(matrix& evecs, const diagMatrix& eigs, const ColumnBundle& C)
{	const double tol = 1e-4;
	//Pick out the head elements:
	const std::vector<int>& head = C.basis->head;
	int nSpinor = C.spinorLength();
	matrix Chead = zeroes(head.size()*nSpinor, C.nCols());
	for(size_t n=0; n<head.size(); n++)
		for(int s=0; s<nSpinor; s++)
			callPref(eblas_zaxpy)(C.nCols(), 1.,
				C.dataPref() + n + s*C.basis->nbasis, C.colLength(), 
				Chead.dataPref() + nSpinor*n + s, Chead.nRows() );
	Chead = Chead * evecs;
	//Hamiltonian for degeneracy breaking (some definite (and unlikely to be symmetric) diagonal in the low G head)
	diagMatrix headH(Chead.nRows());
	for(int n=0; n<Chead.nRows(); n++)
		headH[n] = pow(n, M_PI);
	//Resolve degeneracies
	matrix degFix = eye(C.nCols());
	bool degFound = false;
	for(int bStart=0; bStart<C.nCols()-1;)
	{	int bStop=bStart;
		while(bStop<eigs.nCols() && eigs[bStop]<eigs[bStart]+tol) bStop++;
		if(bStop-bStart > 1) //degeneracy
		{	degFound = true;
			matrix CheadSub = Chead(0,Chead.nRows(), bStart,bStop);
			matrix degEvecs; diagMatrix degEigs;
			(dagger(CheadSub) * headH * CheadSub).diagonalize(degEvecs, degEigs);
			degFix.set(bStart,bStop, bStart,bStop, degEvecs);
		}
		bStart = bStop;
	}
	if(degFound)
	{	evecs = evecs*degFix;
		Chead = Chead*degFix;
	}
	//Fix phase:
	matrix phaseFix = eye(C.nCols());
	for(int b=0; b<C.nCols(); b++)
	{	complex phase;
		double normPrev = 0;
		for(int n=0; n<Chead.nRows(); n++)
		{	const complex c = Chead(n,b);
			if(c.norm() > normPrev)
			{	phase = c.conj()/c.abs();
				normPrev = c.norm();
			}
		}
		phaseFix.set(b,b, phase);
	}
	evecs = evecs*phaseFix;
}

void ElecVars::setEigenvectors(int qActive)
{	const ElecInfo& eInfo = e->eInfo;
	logPrintf("Setting wave functions to eigenvectors of Hamiltonian\n"); logFlush();
	for(int q=eInfo.qStart; q<eInfo.qStop; q++)
	{	if((qActive > -1) and (qActive != q)) continue;
		fixPhase(Hsub_evecs[q], Hsub_eigs[q], C[q]);
		Y[q] = (C[q] = C[q] * Hsub_evecs[q]);
		grad_CdagOC[q] =  dagger(Hsub_evecs[q]) *grad_CdagOC[q] * Hsub_evecs[q];
		for(matrix& VdagCq_sp: VdagC[q])
			if(VdagCq_sp) VdagCq_sp = VdagCq_sp * Hsub_evecs[q];
		
		if(eInfo.subspaceRotation)
		{	if(eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
				B[q] = dagger(Hsub_evecs[q]) * B_eigs[q] * Hsub_evecs[q];
			else B[q].zero();
		}
		
		//Apply corresponding changes to Hsub:
		Hsub[q] = Hsub_eigs[q]; //now diagonal
		Hsub_evecs[q] = eye(eInfo.nBands);
	}
}

int ElecVars::nOccupiedBands(int q) const
{	myassert(e->eInfo.isMine(q));
	for(unsigned i=0; i<F[q].size(); i++)
		if(F[q][i]<=e->cntrl.occupiedThreshold)
			return i; //index of first unoccupied band = number of ocucpied bands
	return F[q].size(); //all bands occupied
}

ScalarFieldArray ElecVars::KEdensity() const
{	ScalarFieldArray tau(n.size());
	//Compute KE density from valence electrons:
	for(int q=e->eInfo.qStart; q<e->eInfo.qStop; q++)
		for(int iDir=0; iDir<3; iDir++)
			tau += (0.5*C[q].qnum->weight) * diagouterI(F[q], D(C[q],iDir), tau.size(), &e->gInfo);
	for(ScalarField& tau_s: tau)
	{	nullToZero(tau_s, e->gInfo);
		e->symm.symmetrize(tau_s); //Symmetrize
		tau_s->allReduce(MPIUtil::ReduceSum);
	}
	//Add core KE density model:
	if(e->iInfo.tauCore)
	{	for(unsigned s=0; s<tau.size(); s++)
			tau[s] += (1.0/tau.size()) * e->iInfo.tauCore; //add core KE density
	}
	return tau;
}

ScalarFieldArray ElecVars::calcDensity() const
{	ScalarFieldArray density(n.size());
	//Runs over all states and accumulates density to the corresponding spin channel of the total density
	e->iInfo.augmentDensityInit();
	for(int q=e->eInfo.qStart; q<e->eInfo.qStop; q++)
	{	density += e->eInfo.qnums[q].weight * diagouterI(F[q], C[q], density.size(), &e->gInfo);
		e->iInfo.augmentDensitySpherical(e->eInfo.qnums[q], F[q], VdagC[q]); //pseudopotential contribution
	}
	e->iInfo.augmentDensityGrid(density);
	
	for(ScalarField& ns: density)
	{	nullToZero(ns, e->gInfo);
		e->symm.symmetrize(ns); //Symmetrize
		ns->allReduce(MPIUtil::ReduceSum);
	}
	return density;
}

void ElecVars::orthonormalize(int q)
{	myassert(e->eInfo.isMine(q));
	ColumnBundle projYq;
	if(e->cntrl.fixOccupied) //project out fixed wavefunctions from free ones
	{	int nOcc = nOccupiedBands(q);
		if(!nOcc) projYq = Y[q];
		else
		{	ColumnBundle fixedYq = Y[q].getSub(0, nOcc);
			projYq = Y[q] - fixedYq*(O(fixedYq)^Y[q]);
			projYq.setSub(0, fixedYq);
		}
	}
	const ColumnBundle& Yq = e->cntrl.fixOccupied ? projYq : Y[q];
	VdagC[q].clear();
	U[q] = Yq^O(Yq, &VdagC[q]); //Compute U:
	Umhalf[q] = invsqrt(U[q], &U_evecs[q], &U_eigs[q]); //Compute U^-0.5 (and retrieve U's eigensystem)
	if(e->eInfo.subspaceRotation)
	{	if(e->eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
		{	//Diagonalize auxilliary subspace hamiltonian to get V:
			B[q].diagonalize(B_evecs[q], B_eigs[q]);
			V[q] = dagger(B_evecs[q]);
		}
		else 
		{	//Old subspace rotations: Compute V = exp(i B) and retrieve B's eigensystem
			V[q] = cis(B[q], &B_evecs[q], &B_eigs[q]);
		}
	}
	matrix YtoC = e->eInfo.subspaceRotation ? Umhalf[q] * dagger(V[q]) : Umhalf[q];
	C[q] = Yq * YtoC;
	e->iInfo.project(C[q], VdagC[q], &YtoC); //update the atomic projections
}

double ElecVars::applyHamiltonian(int q, const diagMatrix& Fq, ColumnBundle& HCq, Energies& ener, bool need_Hsub)
{	myassert(e->eInfo.isMine(q));
	const QuantumNumber& qnum = e->eInfo.qnums[q];
	std::vector<matrix> HVdagCq(e->iInfo.species.size());
	
	//Propagate grad_n (Vscloc) to HCq (which is grad_Cq upto weights and fillings) if required
	if(need_Hsub)
	{	HCq += Idag_DiagV_I(C[q], Vscloc); //Accumulate Idag Diag(Vscloc) I C
		e->iInfo.augmentDensitySphericalGrad(qnum, Fq, VdagC[q], HVdagCq); //Contribution via pseudopotential density augmentation
		if(e->exCorr.needsKEdensity() && Vtau[qnum.index()]) //Contribution via orbital KE:
		{	for(int iDir=0; iDir<3; iDir++)
				HCq -= (0.5*e->gInfo.dV) * D(Idag_DiagV_I(D(C[q],iDir), Vtau), iDir);
		}
		if(e->eInfo.hasU) //Contribution via atomic density matrix projections (DFT+U)
			e->iInfo.rhoAtom_grad(C[q], U_rhoAtom, HCq);
	}

	//Kinetic energy:
	ColumnBundle minushalfLCq = -0.5*L(C[q]);
	if(HCq) HCq += minushalfLCq;
	ener.E["KE"] += qnum.weight * traceinner(Fq, C[q], minushalfLCq).real();;
	
	//Nonlocal pseudopotentials:
	ener.E["Enl"] += qnum.weight * e->iInfo.EnlAndGrad(qnum, Fq, VdagC[q], HVdagCq);
	if(HCq) e->iInfo.projectGrad(HVdagCq, C[q], HCq);
	
	//Compute subspace hamiltonian if needed:
	if(need_Hsub)
	{	Hsub[q] = C[q] ^ HCq;
		Hsub[q].diagonalize(Hsub_evecs[q], Hsub_eigs[q]);
	}
	
	if(e->cntrl.fixed_H) return qnum.weight * trace(Hsub[q]).real();
	else return 0.;
}

void ElecVars::orthonormalizeGrad(int q, const diagMatrix& Fq, const ColumnBundle& HCq, ColumnBundle& gradYq, double KErollover, ColumnBundle* KgradYq, matrix* gradBq, matrix* KgradBq)
{	myassert(e->eInfo.isMine(q));
	const QuantumNumber& qnum = e->eInfo.qnums[q];
	
	ColumnBundle OC = O(C[q]); //Note: O is not cheap for ultrasoft pseudopotentials
	ColumnBundle PbarHC = HCq - OC * Hsub[q]; //PbarHC = HC - O C C^HC
	matrix gradCtoY = e->eInfo.subspaceRotation ? V[q]*Umhalf[q] : Umhalf[q];
	
	//First term of grad_Y, proportional to F:
	gradYq = PbarHC * (Fq * gradCtoY);
	if(KgradYq) *KgradYq = precond_inv_kinetic(PbarHC * gradCtoY, KErollover);
	grad_CdagOC[q] = -(Hsub[q]*Fq);
	
	//Second term of grad_Y, proportional to [F,Hsub]:
	if(e->eInfo.subspaceRotation) // subspace rotation is on whenever [F,Hsub]!=0
	{	matrix FcommHsub = Hsub[q] * Fq - Fq * Hsub[q];
		matrix Q_FcommHsub = V[q] * sqrt_grad(dagger(V[q])*FcommHsub*V[q], U_evecs[q], U_eigs[q]);
		ColumnBundle OCQ = OC * Q_FcommHsub;
		gradYq += OCQ;
		if(KgradYq) *KgradYq += precond_inv_kinetic(OCQ, KErollover);
		grad_CdagOC[q] += Q_FcommHsub * dagger(V[q]) * U[q] * Umhalf[q];
		
		//Subspace rotation gradient:
		if(gradBq and e->eInfo.fillingsUpdate!=ElecInfo::FermiFillingsAux)
		{	*gradBq = qnum.weight * dagger_symmetrize(cis_grad(FcommHsub, B_evecs[q], B_eigs[q]));
			if(KgradBq) *KgradBq = subspaceRotationFactor * (*gradBq);
		}
	}

	//Scale wavefunction gradients by state weight:
	//Preconditioned gradient does not get multiplied by the quantum number weight (increases performance)
	gradYq *= qnum.weight;

	//Project out fixed directions from gradient (if any):
	if(e->cntrl.fixOccupied)
	{	int nOcc = nOccupiedBands(q);
		if(nOcc)
		{	//Apply projector to unoccupied orbital gradients:
			ColumnBundle fixedYq = Y[q].getSub(0, nOcc), OfixedYq = O(fixedYq);
			gradYq -= fixedYq * (OfixedYq^gradYq);
			if(KgradYq) *KgradYq -= fixedYq * (OfixedYq^(*KgradYq));
			//Zero out occupied orbital gradients:
			fixedYq.zero();
			gradYq.setSub(0, fixedYq);
			if(KgradYq) KgradYq->setSub(0, fixedYq);
		}
	}
}

double ElecVars::bandEnergyAndGrad(int q, Energies& ener, ColumnBundle* grad, ColumnBundle* Kgrad)
{	orthonormalize(q);
	diagMatrix Fq = eye(e->eInfo.nBands);
	ColumnBundle Hq;
	double Eband = applyHamiltonian(q, Fq, Hq, ener, true);
	if(grad)
	{	double KErollover = 2.*e->ener.E["KE"]/(e->eInfo.qnums[q].weight*e->eInfo.nBands);
		orthonormalizeGrad(q, Fq, Hq, *grad, KErollover, Kgrad);
	}
	Hq.free();
	
	// Calculate overlap condition
	auto extremes = std::minmax_element(U_eigs[q].begin(), U_eigs[q].end());
	overlapCondition = *extremes.second / *extremes.first;
	return Eband;
}
