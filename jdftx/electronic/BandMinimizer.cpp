/*-------------------------------------------------------------------
Copyright 2013 Deniz Gunceler

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

#include <electronic/BandMinimizer.h>
#include <electronic/Everything.h>

BandMinimizer::BandMinimizer(Everything& e, int qActive, bool precond):
qActive(qActive), e(e), eVars(e.eVars), eInfo(e.eInfo),
precond(precond)
{	myassert(e.cntrl.fixed_H); // Check whether the electron Hamiltonian is fixed
	e.elecMinParams.energyLabel = relevantFreeEnergyName(e);
}

void BandMinimizer::step(const ColumnBundle& dir, double alpha)
{	myassert(dir.nCols() == e.eVars.Y[qActive].nCols());
	axpy(alpha, dir, e.eVars.Y[qActive]);
}

double BandMinimizer::compute(ColumnBundle* grad)
{	return e.eVars.bandEnergyAndGrad(qActive, e.ener, grad, &Kgrad);
}

ColumnBundle BandMinimizer::precondition(const ColumnBundle& grad)
{	return precond ? Kgrad : grad;
}

bool BandMinimizer::report(int iter)
{
	// Overlap check for orthogonalization
	if(e.cntrl.overlapCheckInterval
		&& (iter % e.cntrl.overlapCheckInterval == 0)
		&& (eVars.overlapCondition > e.cntrl.overlapConditionThreshold) )
	{
		logPrintf("%s\tCondition number of orbital overlap matrix (%lg) exceeds threshold (%lg): ",
			e.elecMinParams.linePrefix, eVars.overlapCondition, e.cntrl.overlapConditionThreshold);
		eVars.setEigenvectors(qActive);
		return true;
	}
	
	//Dumps at every electronic step of each band, if asked for
	e.dump(DumpFreq_Electronic, iter);
	
	return false;
}

void BandMinimizer::constrain(ColumnBundle& dir)
{	if(e.cntrl.fixOccupied)
	{	//Project out occupied directions:
		int nOcc = eVars.nOccupiedBands(qActive);
		if(nOcc)
			callPref(eblas_zero)(dir.colLength()*nOcc, dir.dataPref());
	}

}