/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman

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

#ifndef JDFTX_FLUID_IDEALGASPSIALPHA_H
#define JDFTX_FLUID_IDEALGASPSIALPHA_H

#include <fluid/IdealGasPomega.h>

//! IdealGas for polyatomic molecules with the effective potential 'psi_alpha' independent variables
class IdealGasPsiAlpha : public IdealGasPomega
{
public:
	//!Initialize and associate with excess functional fex (and its fluid mixture)
	//!Also specify the orientation quadrature and translation operator used for the orientation integrals
	IdealGasPsiAlpha(const FluidMixture*, const FluidComponent*, const SO3quad& quad, const TranslationOperator& trans);

	void initState(const ScalarField* Vex, ScalarField* psi, double scale, double Elo, double Ehi) const;

protected:
	string representationName() const;
	void initState_o(int o, const matrix3<>& rot, double scale, const ScalarField& Eo, ScalarField* psi) const;
	void getDensities_o(int o, const matrix3<>& rot, const ScalarField* psi, ScalarField& logPomega_o) const;
	void convertGradients_o(int o, const matrix3<>& rot, const ScalarField& Phi_logPomega_o, ScalarField* Phi_psi) const;
};

#endif // JDFTX_FLUID_IDEALGASPSIALPHA_H
